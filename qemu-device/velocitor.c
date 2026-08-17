/*
 * VELOCITOR -- fictional PCIe matrix accelerator.
 * QEMU device model.
 *
 * Reference: velocitor-device-spec.md v0.6.3.  The model is the "other side"
 * of the contract described there (spec section 0.6); annex D lists the
 * obligations that do not follow from the section text.
 *
 * SCOPE IMPLEMENTED TODAY -- steps 2 and 3 of spec section 13:
 *
 *   - PCI identity and capability layout      (spec 2.1, 3)
 *   - the three BARs, with contractual type and size, so lspci and the
 *     driver see the final layout             (spec 3)
 *   - the BAR0 identity block and SCRATCH     (spec 4.1)
 *   - the counter block, CNT_SNAP and CNT_RESET
 *                                             (spec 4.5)
 *   - MSI-X on BAR4, six vectors, and IRQ_STATUS / IRQ_MASK / IRQ_ACK
 *                                             (spec 3.3, 4.1)
 *   - ERR_INJECT bit 2, the firmware-crash injection, as the one way to
 *     make a vector go up before there is a firmware
 *                                             (spec 9)
 *   - the BAR0 access rules: 32-bit aligned only, reserved offsets read 0
 *                                             (spec 4)
 *
 * Everything else in BAR0 answers as reserved and logs under LOG_UNIMP with
 * the spec section that will implement it.  BAR2 is declared but empty.
 * Nothing here starts a firmware or moves data; the qualified error block
 * of section 4.4 is not implemented, so an injected crash raises vector 5
 * without an ERR_CODE to go with it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/pci/msix.h"
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

    /* Interrupt state, spec sections 3.3 and 4.1 */
    uint32_t irq_status;    /* 0x028 -- bits 0 and 5 only                 */
    uint32_t irq_mask;      /* 0x02C -- 1 = masked                        */
    uint32_t fw_status;     /* 0x020                                      */

    /* Error injection, spec section 9 */
    uint32_t err_inject;    /* 0x040                                      */
    uint32_t err_inject_arg;/* 0x044                                      */
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
/* Interrupts -- spec sections 3.3 and 4.1                             */
/* ------------------------------------------------------------------ */

/*
 * Raise one MSI-X vector.
 *
 * CNT_NOTIFY_TX counts the moment the device *decides* to notify, before
 * anything downstream can swallow the message.  Spec 4.5 is explicit about
 * this: a suppressed interrupt must still be counted, otherwise the two
 * sides agree and the loss is invisible -- which would empty the central
 * demonstration of the project of its content.
 *
 * Only vectors 0 and 5 latch a bit in IRQ_STATUS.  The four queue vectors
 * acknowledge nothing (spec 3.3), so their handlers need no MMIO at all.
 *
 * IRQ_MASK suppresses the message but not the latch: the driver can still
 * see what happened by reading IRQ_STATUS.  Unmasking does not replay a
 * suppressed message -- the spec settles neither point, so both are
 * decisions for section 16.
 */
static void velocitor_raise(VelocitorState *s, unsigned vector)
{
    PCIDevice *pdev = PCI_DEVICE(s);

    s->cnt[VEL_CNT_INDEX(VEL_REG_CNT_NOTIFY_TX)]++;

    if ((1u << vector) & VEL_IRQ_LATCHED) {
        s->irq_status |= 1u << vector;
    }

    if (s->irq_mask & (1u << vector)) {
        return;
    }

    /*
     * msix_enabled() is false until the guest has written the capability's
     * enable bit, which is exactly the case under qtest: the latch above is
     * still observable, the message simply goes nowhere.
     */
    if (msix_enabled(pdev)) {
        msix_notify(pdev, vector);
    }
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

    case VEL_REG_FW_STATUS:
        return s->fw_status;
    case VEL_REG_IRQ_STATUS:
        return s->irq_status;
    case VEL_REG_IRQ_MASK:
        return s->irq_mask;
    case VEL_REG_ERR_INJECT:
        return s->err_inject;
    case VEL_REG_ERR_INJECT_ARG:
        return s->err_inject_arg;

    case VEL_REG_IRQ_ACK:
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

    case VEL_REG_IRQ_MASK:
        s->irq_mask = (uint32_t)val & VEL_IRQ_LATCHED;
        return;

    case VEL_REG_IRQ_ACK:
        /* Write the bits to clear (spec 4.1); only 0 and 5 ever latch. */
        s->irq_status &= ~((uint32_t)val & VEL_IRQ_LATCHED);
        return;

    case VEL_REG_ERR_INJECT_ARG:
        s->err_inject_arg = (uint32_t)val;
        return;

    case VEL_REG_ERR_INJECT:
        s->err_inject = (uint32_t)val;
        /*
         * Bit 2 is the only injection wired today (spec 9): the firmware
         * crashes and vector 5 goes up.  It is here because step 3 needs
         * some way to make an interrupt happen, and this is the cheapest
         * trigger the spec already defines -- it also serves section 12
         * item 6 later.  The error is raised but not yet *qualified*: the
         * ERR_CODE block of section 4.4 is still unimplemented.
         */
        if (s->err_inject & VEL_ERR_INJECT_FW_CRASH) {
            s->fw_status = VEL_FW_STATUS_CRASHED;
            velocitor_raise(s, VEL_IRQ_VEC_ERROR);
        }
        return;

    case VEL_REG_MAGIC:
    case VEL_REG_VERSION:
    case VEL_REG_CAPS:
    case VEL_REG_MEM_SIZE:
    case VEL_REG_TOPOLOGY:
    case VEL_REG_DMA_BITS:
    case VEL_REG_FW_STATUS:
    case VEL_REG_IRQ_STATUS:
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

    int ret;

    /* MSI-X only (spec section 3.3): six vectors, no INTx line. */
    pdev->config[PCI_INTERRUPT_PIN] = 0x00;

    if (pcie_endpoint_cap_init(pdev, VEL_PCI_CAP_EXPRESS) < 0) {
        error_setg(errp, "velocitor: failed to add the PCI Express capability");
        return;
    }

    memory_region_init_io(&s->bar0, OBJECT(s), &velocitor_bar0_ops, s,
                          "velocitor-bar0", VEL_BAR0_SIZE);
    memory_region_init_io(&s->bar2, OBJECT(s), &velocitor_stub_ops,
                          (void *)"BAR2", "velocitor-bar2", VEL_BAR2_SIZE);
    /*
     * BAR4 is a plain container, not an I/O region: msix_init() installs
     * the table and PBA into it as subregions.  It stays VEL_BAR4_SIZE
     * because the size is contractual (spec 3) -- which is also why
     * msix_init_exclusive_bar() is not used here, as it hardcodes a 4 KiB
     * BAR of its own choosing.
     */
    memory_region_init(&s->bar4, OBJECT(s), "velocitor-bar4", VEL_BAR4_SIZE);

    pci_register_bar(pdev, VEL_BAR0_INDEX,
                     PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);
    pci_register_bar(pdev, VEL_BAR2_INDEX,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &s->bar2);
    pci_register_bar(pdev, VEL_BAR4_INDEX,
                     PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar4);

    ret = msix_init(pdev, VEL_MSIX_VECTORS,
                    &s->bar4, VEL_BAR4_INDEX, VEL_MSIX_TABLE_OFF,
                    &s->bar4, VEL_BAR4_INDEX, VEL_MSIX_PBA_OFF,
                    VEL_PCI_CAP_MSIX, errp);
    if (ret < 0) {
        return;
    }

    /*
     * Every vector is claimed up front.  msix_notify() returns silently for
     * a vector that was never "used", so without this the model would look
     * like it raised an interrupt and the driver would wait forever -- the
     * exact failure mode this project exists to make impossible.
     */
    for (unsigned v = 0; v < VEL_MSIX_VECTORS; v++) {
        msix_vector_use(pdev, v);
    }
}

static void velocitor_exit(PCIDevice *pdev)
{
    VelocitorState *s = VELOCITOR(pdev);

    msix_uninit(pdev, &s->bar4, &s->bar4);
}

static void velocitor_reset(DeviceState *dev)
{
    VelocitorState *s = VELOCITOR(dev);

    s->scratch = 0;
    memset(s->cnt, 0, sizeof(s->cnt));
    memset(s->cnt_snap, 0, sizeof(s->cnt_snap));

    s->irq_status = 0;
    s->irq_mask = 0;
    s->fw_status = VEL_FW_STATUS_RESET;
    s->err_inject = 0;
    s->err_inject_arg = 0;

    msix_reset(PCI_DEVICE(s));
}

static const VMStateDescription vmstate_velocitor = {
    .name = "velocitor",
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, VelocitorState),
        VMSTATE_UINT32(scratch, VelocitorState),
        VMSTATE_UINT32_ARRAY(cnt, VelocitorState, VEL_CNT_COUNT),
        VMSTATE_UINT32_ARRAY(cnt_snap, VelocitorState, VEL_CNT_COUNT),
        VMSTATE_UINT32(irq_status, VelocitorState),
        VMSTATE_UINT32(irq_mask, VelocitorState),
        VMSTATE_UINT32(fw_status, VelocitorState),
        VMSTATE_UINT32(err_inject, VelocitorState),
        VMSTATE_UINT32(err_inject_arg, VelocitorState),
        VMSTATE_MSIX(parent_obj, VelocitorState),
        VMSTATE_END_OF_LIST()
    },
};

static void velocitor_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = velocitor_realize;
    k->exit = velocitor_exit;
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
