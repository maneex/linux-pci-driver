/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VELOCITOR -- firmware image generator.
 *
 * Produces the ELF that Linux hands to remoteproc at step 6 of spec section
 * 13.  Two things live in it, and nothing else:
 *
 *   - the firmware header of spec section 6.6, at VEL_FW_HDR_DA, which the
 *     model verifies when RESET is released.  Without it the load would be
 *     ceremonial: the model runs C that does not depend on the bytes that
 *     were copied, so step 6 would pass with an entirely broken `load`.
 *     The magic is what makes the load falsifiable.
 *   - the resource table of spec section 6.3, which the remoteproc core
 *     parses out of the .resource_table section: one carveout for the heap,
 *     and two vdevs of two vrings each.
 *
 * There is no code in it.  This firmware is never executed by anything --
 * the QEMU model plays the part of the running processor -- so the image
 * declares EM_NONE rather than pretend to target an architecture that does
 * not exist.  What it must be is a *well-formed* ELF, because the core's
 * parsing is real: rproc_elf_sanity_check(), rproc_elf_load_segments() and
 * find_table() all run against these exact bytes.
 *
 * Emitted byte by byte rather than compiled from a linker script.  The whole
 * image is four structures and its layout is the contract; a generator that
 * shares velocitor_hw.h with the model and the driver cannot drift from
 * them, where a cross-toolchain for a processor nobody built would be a lot
 * of machinery in exchange for less control.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "velocitor_hw.h"

/* ---- ELF32, little endian: only what this image uses ---- */

#define ET_EXEC         2u
#define EM_NONE         0u
#define EV_CURRENT      1u
#define ELFCLASS32      1u
#define ELFDATA2LSB     1u
#define PT_LOAD         1u
#define PF_R            4u
#define PF_W            2u
#define SHT_PROGBITS    1u
#define SHT_STRTAB      3u
#define SHF_ALLOC       2u

#define EHDR_SIZE       52u
#define PHDR_SIZE       32u
#define SHDR_SIZE       40u

/* ---- remoteproc resource table ABI, include/linux/remoteproc.h ---- */

#define RSC_CARVEOUT    0u
#define RSC_VDEV        3u
#define FW_RSC_ADDR_ANY 0xFFFFFFFFu

#define VIRTIO_ID_RPMSG   7u
#define VIRTIO_RPMSG_F_NS 0u

/*
 * Where the trace ring of spec section 6.6 sits.  Any address inside the
 * fixed aperture would do: the model reads the choice out of the firmware
 * header instead of assuming one, which is the whole point of the field.
 * 64 KiB in leaves the header and the resource table room below it.
 */
#define FW_TRACE_DA     0x00010000u

/*
 * File offset of device address 0.  The ELF and program headers come first
 * and the payload starts on a round number after them, so that a hex dump of
 * the image is readable.
 */
#define PAYLOAD_OFF     128u

static unsigned char img[8192];
static unsigned int img_len;

static void put16(unsigned int off, uint16_t v)
{
    img[off + 0] = (unsigned char)(v & 0xff);
    img[off + 1] = (unsigned char)(v >> 8);
}

static void put32(unsigned int off, uint32_t v)
{
    img[off + 0] = (unsigned char)(v & 0xff);
    img[off + 1] = (unsigned char)((v >> 8) & 0xff);
    img[off + 2] = (unsigned char)((v >> 16) & 0xff);
    img[off + 3] = (unsigned char)(v >> 24);
}

/*
 * One vring descriptor inside a vdev entry.  da stays FW_RSC_ADDR_ANY: spec
 * section 6.2 makes that the contract, because it is the only value for
 * which the core's consistency check against the pre-registered carveout is
 * vacuous -- and the driver allocates these vrings itself.
 */
static unsigned int emit_vring(unsigned int off)
{
    put32(off + 0,  FW_RSC_ADDR_ANY);   /* da       */
    put32(off + 4,  VEL_VRING_ALIGN);   /* align    */
    put32(off + 8,  VEL_VRING_NUM);     /* num      */
    put32(off + 12, 0);                 /* notifyid: filled in by the core */
    put32(off + 16, 0);                 /* pa       */
    return off + 20;
}

/*
 * A vdev entry: header, then its two vrings.  No virtio config space --
 * spec section 6.3 keeps capabilities on the rpmsg control plane rather than
 * opening a third configuration channel through the shadow table.
 */
static unsigned int emit_vdev(unsigned int off, uint32_t id, uint32_t dfeatures)
{
    put32(off + 0,  RSC_VDEV);
    put32(off + 4,  id);
    put32(off + 8,  0);                 /* notifyid: filled in by the core */
    put32(off + 12, dfeatures);
    put32(off + 16, 0);                 /* gfeatures: written by the host  */
    put32(off + 20, 0);                 /* config_len                      */
    img[off + 24] = 0;                  /* status: written by the host     */
    img[off + 25] = 2;                  /* num_of_vrings                   */
    img[off + 26] = 0;                  /* reserved                        */
    img[off + 27] = 0;

    off = emit_vring(off + 28);
    off = emit_vring(off);
    return off;
}

/*
 * The heap carveout.  The name is contractual (spec section 6.3): the core
 * looks for a pre-registered carveout by that exact name, and creates one
 * with its own DMA allocator -- in *host* memory -- if it does not find it.
 * For the heap that would defeat the entire project, since residency in
 * device-local memory is the subject.
 */
static unsigned int emit_heap(unsigned int off)
{
    put32(off + 0,  RSC_CARVEOUT);
    put32(off + 4,  VEL_APERTURE_SIZE);                   /* da       */
    put32(off + 8,  0);                                   /* pa       */
    put32(off + 12, VEL_MEM_SIZE - VEL_APERTURE_SIZE);    /* len      */
    put32(off + 16, 0);                                   /* flags    */
    put32(off + 20, 0);                                   /* reserved */
    memset(&img[off + 24], 0, 32);
    memcpy(&img[off + 24], "heap", 4);                    /* name[32] */
    return off + 24 + 32;
}

int main(int argc, char **argv)
{
    unsigned int rsc_off, rsc_da, rsc_end;
    unsigned int e1, e2, e3;
    unsigned int shstrtab_off, shstrtab_len;
    unsigned int shoff, payload_len;
    FILE *out;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <output.elf>\n", argv[0]);
        return 2;
    }

    /* ---- payload: firmware header at device address 0 ---- */

    put32(PAYLOAD_OFF + VEL_FW_HDR_OFF_MAGIC,     VEL_FW_MAGIC);
    put32(PAYLOAD_OFF + VEL_FW_HDR_OFF_ABI,       VEL_FW_ABI);
    put32(PAYLOAD_OFF + VEL_FW_HDR_OFF_TRACE_DA,  FW_TRACE_DA);
    put32(PAYLOAD_OFF + VEL_FW_HDR_OFF_TRACE_LEN, VEL_TRACE_SIZE);

    /* ---- payload: resource table, right after the header ---- */

    rsc_off = PAYLOAD_OFF + VEL_FW_HDR_SIZE;
    rsc_da  = VEL_FW_HDR_DA + VEL_FW_HDR_SIZE;

    /* Three entries, so a header of 4 + 4 + 8 + 3 * 4 bytes. */
    e1 = rsc_off + 16 + 3 * 4;
    e2 = emit_heap(e1);
    e3 = emit_vdev(e2, VIRTIO_ID_RPMSG, 1u << VIRTIO_RPMSG_F_NS);

    /*
     * dfeatures is zero for the data vdev in v1 (spec section 8.1): step 8
     * brings the split ring up bare, and features arrive one at a time at
     * step 11.
     */
    rsc_end = emit_vdev(e3, VEL_VIRTIO_ID, 0);

    put32(rsc_off + 0,  1);             /* ver: the core accepts only 1 */
    put32(rsc_off + 4,  3);             /* num                          */
    put32(rsc_off + 8,  0);             /* reserved[0]                  */
    put32(rsc_off + 12, 0);             /* reserved[1]                  */
    put32(rsc_off + 16, e1 - rsc_off);  /* offset[0]: heap              */
    put32(rsc_off + 20, e2 - rsc_off);  /* offset[1]: rpmsg vdev        */
    put32(rsc_off + 24, e3 - rsc_off);  /* offset[2]: data vdev         */

    payload_len = rsc_end - PAYLOAD_OFF;

    /* ---- section name table ---- */

    shstrtab_off = rsc_end;
    img[shstrtab_off] = '\0';
    memcpy(&img[shstrtab_off + 1], ".shstrtab", 10);
    memcpy(&img[shstrtab_off + 11], ".resource_table", 16);
    shstrtab_len = 27;

    /* ---- section headers, on a four-byte boundary ---- */

    shoff = (shstrtab_off + shstrtab_len + 3u) & ~3u;

    /* [0] the mandatory null section */
    memset(&img[shoff], 0, SHDR_SIZE);

    /* [1] .resource_table -- what find_table() looks for, by name */
    put32(shoff + SHDR_SIZE + 0,  11);              /* sh_name             */
    put32(shoff + SHDR_SIZE + 4,  SHT_PROGBITS);    /* sh_type             */
    put32(shoff + SHDR_SIZE + 8,  SHF_ALLOC);       /* sh_flags            */
    put32(shoff + SHDR_SIZE + 12, rsc_da);          /* sh_addr             */
    put32(shoff + SHDR_SIZE + 16, rsc_off);         /* sh_offset           */
    put32(shoff + SHDR_SIZE + 20, rsc_end - rsc_off); /* sh_size           */
    put32(shoff + SHDR_SIZE + 24, 0);               /* sh_link             */
    put32(shoff + SHDR_SIZE + 28, 0);               /* sh_info             */
    put32(shoff + SHDR_SIZE + 32, 4);               /* sh_addralign        */
    put32(shoff + SHDR_SIZE + 36, 0);               /* sh_entsize          */

    /* [2] .shstrtab */
    put32(shoff + 2 * SHDR_SIZE + 0,  1);
    put32(shoff + 2 * SHDR_SIZE + 4,  SHT_STRTAB);
    put32(shoff + 2 * SHDR_SIZE + 8,  0);
    put32(shoff + 2 * SHDR_SIZE + 12, 0);
    put32(shoff + 2 * SHDR_SIZE + 16, shstrtab_off);
    put32(shoff + 2 * SHDR_SIZE + 20, shstrtab_len);
    put32(shoff + 2 * SHDR_SIZE + 24, 0);
    put32(shoff + 2 * SHDR_SIZE + 28, 0);
    put32(shoff + 2 * SHDR_SIZE + 32, 1);
    put32(shoff + 2 * SHDR_SIZE + 36, 0);

    img_len = shoff + 3 * SHDR_SIZE;

    /* ---- ELF header ---- */

    img[0] = 0x7f;
    img[1] = 'E';
    img[2] = 'L';
    img[3] = 'F';
    img[4] = ELFCLASS32;
    img[5] = ELFDATA2LSB;
    img[6] = EV_CURRENT;
    /* [7..15] OS ABI, ABI version and padding stay zero */

    put16(16, ET_EXEC);
    put16(18, EM_NONE);
    put32(20, EV_CURRENT);
    put32(24, 0);                       /* e_entry: nothing to enter    */
    put32(28, EHDR_SIZE);               /* e_phoff                      */
    put32(32, shoff);                   /* e_shoff                      */
    put32(36, 0);                       /* e_flags                      */
    put16(40, EHDR_SIZE);
    put16(42, PHDR_SIZE);
    put16(44, 1);                       /* e_phnum                      */
    put16(46, SHDR_SIZE);
    put16(48, 3);                       /* e_shnum                      */
    put16(50, 2);                       /* e_shstrndx                   */

    /* ---- the single loadable segment ---- */

    /*
     * p_paddr is the device address: rproc_elf_load_segments() passes it to
     * da_to_va(), which covers the fixed aperture only.  p_memsz reaches
     * past the payload to cover the trace ring, so that the core zeroes it
     * for us with memset_io() -- a firmware that shipped a pre-filled ring
     * would be lying about what it had written.
     */
    put32(EHDR_SIZE + 0,  PT_LOAD);
    put32(EHDR_SIZE + 4,  PAYLOAD_OFF);              /* p_offset          */
    put32(EHDR_SIZE + 8,  VEL_FW_HDR_DA);            /* p_vaddr           */
    put32(EHDR_SIZE + 12, VEL_FW_HDR_DA);            /* p_paddr           */
    put32(EHDR_SIZE + 16, payload_len);              /* p_filesz          */
    put32(EHDR_SIZE + 20, FW_TRACE_DA + VEL_TRACE_SIZE); /* p_memsz       */
    put32(EHDR_SIZE + 24, PF_R | PF_W);              /* p_flags           */
    put32(EHDR_SIZE + 28, 4096);                     /* p_align           */

    out = fopen(argv[1], "wb");
    if (!out) {
        perror(argv[1]);
        return 1;
    }
    if (fwrite(img, 1, img_len, out) != img_len) {
        perror(argv[1]);
        fclose(out);
        return 1;
    }
    fclose(out);

    printf("%s: %u bytes, resource table %u bytes at file offset %u"
           " (device address 0x%x)\n",
           argv[1], img_len, rsc_end - rsc_off, rsc_off, rsc_da);
    return 0;
}
