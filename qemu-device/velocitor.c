/*
 * VELOCITOR -- fictional PCIe matrix accelerator.
 * QEMU device model.
 *
 * Reference: velocitor-device-spec.md v0.6.3.  The model is the "other side"
 * of the contract described there (spec section 0.6); annex D lists the
 * obligations that do not follow from the section text.
 *
 * SCOPE IMPLEMENTED TODAY -- deliberately narrow, matching a driver that has
 * only a probe (step 2 of spec section 13 and below):
 *
 *   - PCI identity and capability layout      (spec 2.1, 3)
 *   - the three BARs, with contractual type and size, so lspci and the
 *     driver see the final layout             (spec 3)
 *   - the BAR0 identity block and SCRATCH     (spec 4.1)
 *   - the counter block, CNT_SNAP and CNT_RESET
 *                                             (spec 4.5)
 *   - the BAR0 access rules: 32-bit aligned only, reserved offsets read 0
 *                                             (spec 4)
 *
 * Everything else in BAR0 answers as reserved and logs under LOG_UNIMP with
 * the spec section that will implement it.  BAR2 and BAR4 are declared but
 * empty.  Nothing here starts a firmware, moves data, or raises an
 * interrupt; there is no state machine to get wrong yet.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/pci/pci.h"
#include "hw/pci/pcie.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qom/object.h"

#include "velocitor_hw.h"

#define TYPE_VELOCITOR "velocitor"
OBJECT_DECLARE_SIMPLE_TYPE(VelocitorState, VELOCITOR)

struct VelocitorState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    MemoryRegion bar0;      /* control registers, spec section 4          */
    MemoryRegion bar2;      /* fixed aperture + sliding window, spec 3.1  */
    MemoryRegion bar4;      /* MSI-X tables, spec section 3               */

    uint32_t scratch;       /* 0x00C -- mapping probe, spec section 4.1   */

    /*
     * Counters, spec section 4.5.  Two copies on purpose: cnt[] is what the
     * engines increment, cnt_snap[] is what reads answer.  Writing CNT_SNAP
     * copies one to the other, which is what makes a series of reads
     * mutually consistent without any read having a side effect (annex A.3),
     * and what lets VEL_IOC_STATS and the debugfs "counters" file coexist.
     */
    uint32_t cnt[VEL_CNT_COUNT];
    uint32_t cnt_snap[VEL_CNT_COUNT];
};

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

/*
 * Name the BAR0 block an offset falls in, so an access to something not
 * implemented yet says which part of the spec is missing rather than just
 * "unhandled".  Bring-up across two implementations is the whole point of
 * this device (spec section 0.6): a log line that names the section saves
 * the other side a grep.
 */
static const char *velocitor_bar0_block(hwaddr addr)
{
    if (addr <= VEL_BLK_CTRL_END) {
        return "control, spec 4.1";
    }
    if (addr >= VEL_BLK_ERR_BASE && addr <= VEL_BLK_ERR_END) {
        return "qualified error, spec 4.4";
    }
    if (addr >= VEL_BLK_DBGDMA_BASE && addr <= VEL_BLK_DBGDMA_END) {
        return "bring-up DMA, spec 4.3";
    }
    if (addr >= VEL_BLK_CNT_BASE && addr <= VEL_BLK_CNT_END) {
        return "counters, spec 4.5";
    }
    if (addr >= VEL_BLK_RSC_BASE && addr <= VEL_BLK_RSC_END) {
        return "shadow resource table, spec 4.2";
    }
    if (addr >= VEL_BLK_VQ_BASE && addr <= VEL_BLK_VQ_END) {
        return "queue configuration, spec 4.2";
    }
    return "reserved";
}

/*
 * Spec section 4: "Accès 32 bits uniquement.  Tout accès non aligné ou de
 * taille différente est ignoré en écriture et retourne 0xFFFFFFFF en
 * lecture."
 *
 * The MemoryRegionOps below therefore accept every width and let the
 * handlers reject: having QEMU split or merge accesses on our behalf would
 * hide exactly the driver bug this rule exists to catch.
 */
static bool velocitor_access_ok(const char *what, hwaddr addr, unsigned size)
{
    if (size == 4 && (addr & 3) == 0) {
        return true;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "velocitor: %s of %u byte(s) at 0x%" HWADDR_PRIx
                  " -- only aligned 32-bit accesses are defined (spec 4)\n",
                  what, size, addr);
    return false;
}

/* ------------------------------------------------------------------ */
/* BAR0 -- control registers, spec section 4                           */
/* ------------------------------------------------------------------ */

static uint64_t velocitor_bar0_read(void *opaque, hwaddr addr, unsigned size)
{
    VelocitorState *s = opaque;

    if (!velocitor_access_ok("BAR0 read", addr, size)) {
        /* All ones; the memory core truncates to the access width, so a
         * 32-bit read gets the 0xFFFFFFFF the spec names. */
        return ~0ULL;
    }

    /*
     * Counters answer from the snapshot, never from the live values.  A
     * driver that has not written CNT_SNAP therefore reads zeroes rather
     * than a torn set -- including for the four 64-bit counters, whose LO
     * and HI halves cannot drift apart between two reads.
     */
    if (addr >= VEL_CNT_FIRST && addr <= VEL_CNT_LAST) {
        return s->cnt_snap[VEL_CNT_INDEX(addr)];
    }

    switch (addr) {
    case VEL_REG_MAGIC:
        return VEL_MAGIC;
    case VEL_REG_VERSION:
        return VEL_VERSION;
    case VEL_REG_CAPS:
        return VEL_CAP_FP32 | VEL_CAP_BF16 | VEL_CAP_TRANSPOSE;
    case VEL_REG_SCRATCH:
        /* Bitwise inverted read-back: proves the write reached the device
         * and came back, which a stuck-at-zero mapping cannot fake. */
        return ~s->scratch;
    case VEL_REG_MEM_SIZE:
        return VEL_MEM_SIZE;
    case VEL_REG_TOPOLOGY:
        return VEL_TOPOLOGY;
    case VEL_REG_DMA_BITS:
        return VEL_DMA_BITS;

    case VEL_REG_CNT_RESET:
    case VEL_REG_CNT_SNAP:
        /*
         * Write-only with a side effect (annex A.3): the model answers all
         * ones so that a driver reading them is caught on the spot rather
         * than silently getting a plausible zero.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "velocitor: read of write-only register 0x%" HWADDR_PRIx
                      " (spec annex A.3) -- returns all ones\n", addr);
        return ~0U;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "velocitor: BAR0 read at 0x%" HWADDR_PRIx
                      " (%s) -- not implemented, reads 0\n",
                      addr, velocitor_bar0_block(addr));
        return 0;
    }
}

static void velocitor_bar0_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    VelocitorState *s = opaque;

    if (!velocitor_access_ok("BAR0 write", addr, size)) {
        return;
    }

    /* Counters are the device's own truth: read-only to the driver. */
    if (addr >= VEL_CNT_FIRST && addr <= VEL_CNT_LAST) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "velocitor: write 0x%08x to read-only counter"
                      " 0x%" HWADDR_PRIx " (spec 4.5) -- ignored\n",
                      (uint32_t)val, addr);
        return;
    }

    switch (addr) {
    case VEL_REG_SCRATCH:
        s->scratch = (uint32_t)val;
        return;

    case VEL_REG_CNT_SNAP:
        /*
         * Freeze every counter at once (spec 4.5).  The spec says "write 1";
         * acting on bit 0 keeps a driver that writes a wider flag word from
         * silently doing nothing.
         */
        if (val & 1) {
            memcpy(s->cnt_snap, s->cnt, sizeof(s->cnt));
        }
        return;

    case VEL_REG_CNT_RESET:
        if (val & 1) {
            memset(s->cnt, 0, sizeof(s->cnt));
            memset(s->cnt_snap, 0, sizeof(s->cnt_snap));
        }
        return;

    case VEL_REG_MAGIC:
    case VEL_REG_VERSION:
    case VEL_REG_CAPS:
    case VEL_REG_MEM_SIZE:
    case VEL_REG_TOPOLOGY:
    case VEL_REG_DMA_BITS:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "velocitor: write 0x%08x to read-only register"
                      " 0x%" HWADDR_PRIx " -- ignored\n",
                      (uint32_t)val, addr);
        return;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "velocitor: BAR0 write 0x%08x at 0x%" HWADDR_PRIx
                      " (%s) -- not implemented, ignored\n",
                      (uint32_t)val, addr, velocitor_bar0_block(addr));
        return;
    }
}

static const MemoryRegionOps velocitor_bar0_ops = {
    .read = velocitor_bar0_read,
    .write = velocitor_bar0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
};

/* ------------------------------------------------------------------ */
/* BAR2 and BAR4 -- declared, not populated                            */
/* ------------------------------------------------------------------ */

/*
 * BAR2 carries the fixed aperture and the sliding window (spec 3.1); BAR4
 * carries the MSI-X tables and will be handed to msix_init() at step 3.
 * Both are registered now because the BAR *numbering* is contractual: BAR2
 * is 64-bit and therefore eats slot 3, which is why MSI-X lives in BAR4
 * (spec section 3).  Declaring them costs six lines and settles the layout
 * the driver will map against.
 */
static uint64_t velocitor_stub_read(void *opaque, hwaddr addr, unsigned size)
{
    const char *name = opaque;

    qemu_log_mask(LOG_UNIMP,
                  "velocitor: %s read at 0x%" HWADDR_PRIx
                  " -- region not populated yet, reads 0\n", name, addr);
    return 0;
}

static void velocitor_stub_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    const char *name = opaque;

    qemu_log_mask(LOG_UNIMP,
                  "velocitor: %s write 0x%" PRIx64 " at 0x%" HWADDR_PRIx
                  " -- region not populated yet, ignored\n", name, val, addr);
}

static const MemoryRegionOps velocitor_stub_ops = {
    .read = velocitor_stub_read,
    .write = velocitor_stub_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
};

/* ------------------------------------------------------------------ */
/* Device life cycle                                                   */
/* ------------------------------------------------------------------ */

static void velocitor_realize(PCIDevice *pdev, Error **errp)
{
    VelocitorState *s = VELOCITOR(pdev);

    /*
     * MSI-X only (spec section 3.3): six vectors, no INTx.  QEMU already
     * leaves the interrupt pin at zero unless a device sets it, so this
     * write changes nothing today -- it states the intent, and holds when
     * msix_init() lands at step 3 next to devices that do declare a line.
     */
    pdev->config[PCI_INTERRUPT_PIN] = 0x00;

    if (pcie_endpoint_cap_init(pdev, VEL_PCI_CAP_EXPRESS) < 0) {
        error_setg(errp, "velocitor: failed to add the PCI Express capability");
        return;
    }

    memory_region_init_io(&s->bar0, OBJECT(s), &velocitor_bar0_ops, s,
                          "velocitor-bar0", VEL_BAR0_SIZE);
    memory_region_init_io(&s->bar2, OBJECT(s), &velocitor_stub_ops,
                          (void *)"BAR2", "velocitor-bar2", VEL_BAR2_SIZE);
    memory_region_init_io(&s->bar4, OBJECT(s), &velocitor_stub_ops,
                          (void *)"BAR4", "velocitor-bar4", VEL_BAR4_SIZE);

    pci_register_bar(pdev, VEL_BAR0_INDEX,
                     PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);
    pci_register_bar(pdev, VEL_BAR2_INDEX,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &s->bar2);
    pci_register_bar(pdev, VEL_BAR4_INDEX,
                     PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar4);
}

static void velocitor_reset(DeviceState *dev)
{
    VelocitorState *s = VELOCITOR(dev);

    s->scratch = 0;
    memset(s->cnt, 0, sizeof(s->cnt));
    memset(s->cnt_snap, 0, sizeof(s->cnt_snap));
}

static const VMStateDescription vmstate_velocitor = {
    .name = "velocitor",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, VelocitorState),
        VMSTATE_UINT32(scratch, VelocitorState),
        VMSTATE_UINT32_ARRAY(cnt, VelocitorState, VEL_CNT_COUNT),
        VMSTATE_UINT32_ARRAY(cnt_snap, VelocitorState, VEL_CNT_COUNT),
        VMSTATE_END_OF_LIST()
    },
};

static void velocitor_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = velocitor_realize;
    k->vendor_id = VEL_PCI_VENDOR_ID;
    k->device_id = VEL_PCI_DEVICE_ID;
    k->revision = VEL_PCI_REVISION;
    k->class_id = VEL_PCI_CLASS;

    dc->desc = "VELOCITOR matrix accelerator (fictional)";
    dc->reset = velocitor_reset;
    dc->vmsd = &vmstate_velocitor;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

/*
 * Declaring INTERFACE_PCIE_DEVICE and *not* INTERFACE_CONVENTIONAL_PCI_DEVICE
 * is what makes this a PCI Express endpoint: pci_qdev_realize() sets
 * QEMU_PCI_CAP_EXPRESS from exactly that pair, which is why the device needs
 * no is_express flag of its own (PCIDeviceClass has no such member in 7.2).
 *
 * Note that QEMU 7.2 does not refuse this device on a conventional PCI bus:
 * -M pc still enumerates it, Express capability and all, on a bus with no
 * PCIe semantics.  q35 is therefore a constraint we impose (see
 * devtools/config.defaults), not one the tooling enforces for us.
 */
static const TypeInfo velocitor_info = {
    .name          = TYPE_VELOCITOR,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(VelocitorState),
    .class_init    = velocitor_class_init,
    .interfaces    = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { },
    },
};

static void velocitor_register_types(void)
{
    type_register_static(&velocitor_info);
}

type_init(velocitor_register_types)
