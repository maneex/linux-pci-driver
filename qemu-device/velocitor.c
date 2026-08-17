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
 *   - device-local memory behind BAR2: fixed aperture, sliding window, and
 *     the WIN_BASE read-back                  (spec 3.1, 9)
 *   - the bring-up DMA block, asynchronous, with the 42-bit address trap
 *                                             (spec 4.3, 9.1, annex D.1/D.2)
 *   - the shadow resource table registers, and the shallow check the model
 *     makes of the table when RSC_VALID goes up
 *                                             (spec 4.2, 6.4)
 *   - the firmware life cycle: RESET, the firmware header check that makes
 *     the load falsifiable, FW_STATUS through 1 and 2, FW_ABI, GENERATION
 *     and an initialised trace ring
 *                                             (spec 4.1, 6.1, 6.5, 6.6)
 *   - ERR_INJECT bits 2 and 6, the firmware crash and the swallowed
 *     WIN_BASE write                          (spec 9)
 *   - the BAR0 access rules: 32-bit aligned only, reserved offsets read 0
 *                                             (spec 4)
 *
 * Everything else in BAR0 answers as reserved and logs under LOG_UNIMP with
 * the spec section that will implement it.  Nothing here starts a firmware
 * or moves data outside the bring-up block.  Of the qualified error block of
 * section 4.4 only ERR_CODE and ERR_INFO_* exist, filled by the DMA path;
 * an injected firmware crash still raises vector 5 without an ERR_CODE to go
 * with it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
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
    MemoryRegion bar2;      /* container: aperture + window, spec 3.1     */
    MemoryRegion bar4;      /* MSI-X tables, spec section 3               */

    uint32_t scratch;       /* 0x00C -- mapping probe, spec section 4.1   */

    /*
     * Device-local memory, spec section 2.  BAR2 cannot map it directly --
     * 256 MiB behind a 32 MiB BAR -- so two aliases look into it: the low one
     * fixed, the high one movable (spec 3.1).
     *
     * Aliases rather than I/O callbacks on purpose.  Callbacks would turn
     * every four-byte access into a VM exit, and the step 4 criterion is a
     * sweep of the whole memory: 64 million exits, which is not a test anyone
     * runs twice.  As aliases both halves run at RAM speed, and the only MMIO
     * left on the path is WIN_BASE itself.
     */
    MemoryRegion mem;
    MemoryRegion aperture;
    MemoryRegion window;

    /*
     * Sliding window, spec 3.1 and 9.  Two values: what the driver wrote, and
     * what the device acts on.  Reading WIN_BASE is what promotes one to the
     * other -- the read-back the spec makes mandatory.
     */
    uint32_t win_base;      /* effective; what the alias currently shows   */
    uint32_t win_pending;   /* written, not yet confirmed by a read        */

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

    /*
     * Firmware life cycle, spec sections 4.1, 6.1 and 6.5.  RESET is the
     * only one of these the driver writes; the other three are what the
     * model answers about the boot it just did or refused to do.
     *
     * The 1 -> 2 transition runs from a timer for the same reason the DMA
     * does (annex D.1): a driver that polls FW_STATUS after releasing RESET
     * has to see "verified" before "running", or the wait in ops->start()
     * would be a formality that never actually waits for anything.
     */
    uint32_t reset;         /* 0x01C -- 1 = asserted                      */
    uint32_t fw_status;     /* 0x020                                      */
    uint32_t fw_abi;        /* 0x038 -- 0 until the header checks out     */
    uint32_t generation;    /* 0x03C -- bumped on every entry into 2      */
    uint32_t trace_da;      /* where the firmware header says the ring is */
    uint32_t trace_len;
    QEMUTimer *boot_timer;

    /*
     * Bring-up DMA, spec section 4.3.  The copy runs from a virtual-clock
     * timer, never from the MMIO callback: annex D.1 forbids doing work in
     * the callback, and a driver at step 5 has no interrupts yet, so it polls
     * DBG_DMA_STATUS through 1 and then 2.  Copying inline would make the
     * busy state unobservable and hide the asynchrony the driver has to cope
     * with.
     *
     * A virtual timer rather than a bottom half, for two reasons.  It is
     * deterministic, which section 9 requires of everything here; and it is
     * steppable from qtest, where there is no CPU to run a bottom half at
     * all -- layer 1 of section 13.1 could otherwise never see a transfer
     * complete.
     */
    QEMUTimer *dma_timer;
    uint32_t dma_addr_lo, dma_addr_hi;
    uint32_t dma_dev;
    uint32_t dma_len;
    uint32_t dma_ctl;
    uint32_t dma_status;

    /*
     * Shadow resource table, spec sections 4.2 and 6.4.  The device never
     * writes it: the table belongs to Linux, and this is only where the
     * driver says it put its copy.  Reading it is what will let the model
     * see the negotiated features rather than the offered ones -- the
     * distinction section 8.1 insists on.
     */
    uint32_t rsc_addr_lo, rsc_addr_hi;
    uint32_t rsc_len;
    uint32_t rsc_valid;

    /* Qualified error, spec section 4.4 -- only what step 5 can raise */
    uint32_t err_code;
    uint32_t err_info_lo, err_info_hi;

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
/* Sliding window -- spec sections 3.1 and 9                           */
/* ------------------------------------------------------------------ */

/*
 * Promote the pending base to the effective one.  Driven by *reading*
 * WIN_BASE, never by writing it: spec section 9 makes the read-back
 * mandatory, so a driver that skips it keeps reading perfectly valid data
 * from the wrong place -- the failure this artefact exists to teach.
 *
 * CNT_WIN_MOVE counts effective moves, not writes.  Counting writes would
 * make the counter agree with a driver that never reads back, and the point
 * is precisely that the two must disagree.
 */
static void velocitor_window_promote(VelocitorState *s)
{
    if (s->win_pending == s->win_base) {
        return;
    }

    s->win_base = s->win_pending;
    memory_region_set_alias_offset(&s->window, s->win_base);
    s->cnt[VEL_CNT_INDEX(VEL_REG_CNT_WIN_MOVE)]++;
}

static void velocitor_window_write_base(VelocitorState *s, uint32_t base)
{
    /*
     * ERR_INJECT bit 6, spec section 9: swallow this write and say nothing.
     * The bit is consumed, matching "la prochaine ecriture" -- a driver that
     * compares its read-back recovers on the next attempt, one that does not
     * compare stays wrong for good.
     */
    if (s->err_inject & VEL_ERR_INJECT_WIN_IGNORE) {
        s->err_inject &= ~VEL_ERR_INJECT_WIN_IGNORE;
        return;
    }

    if ((base & (VEL_WINDOW_SIZE - 1)) != 0 ||
        base > VEL_MEM_SIZE - VEL_WINDOW_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "velocitor: WIN_BASE 0x%08x unaligned or out of range"
                      " (spec 3.1) -- ignored\n", base);
        s->cnt[VEL_CNT_INDEX(VEL_REG_CNT_ERR_RANGE)]++;
        return;
    }

    s->win_pending = base;
}

/*
 * Every 32-bit word holds its own offset.  Nothing in the spec says the
 * device powers up with any particular content; this is a model artefact,
 * and it is what makes the step 4 sweep falsifiable.  A driver that forgets
 * the read-back reads the first window sixteen times over, and the offset of
 * the first mismatch names the window that failed to move.
 *
 * The driver cannot produce this oracle itself: writing the pattern through
 * the same faulty addressing it later reads with would cancel the two errors
 * and the test would pass.
 */
static void velocitor_mem_fill_pattern(VelocitorState *s)
{
    uint32_t *mem = memory_region_get_ram_ptr(&s->mem);
    uint32_t off;

    for (off = 0; off < VEL_MEM_SIZE; off += 4) {
        mem[off / 4] = cpu_to_le32(off);
    }
}

/* ------------------------------------------------------------------ */
/* Bring-up DMA -- spec section 4.3, annex D.1 and D.2                 */
/* ------------------------------------------------------------------ */

/* Simulated transfer latency, virtual time.  Arbitrary but fixed: what
 * matters is that the busy state exists and lasts a knowable while. */
#define VEL_DMA_LATENCY_NS 1000

/*
 * Qualify an error, spec section 4.4.  Split out from the DMA path because
 * the shadow table below reports through the same three registers without
 * having a transfer status to set.
 */
static void velocitor_error_set(VelocitorState *s, uint32_t code, uint64_t info)
{
    s->err_code = code;
    s->err_info_lo = (uint32_t)info;
    s->err_info_hi = (uint32_t)(info >> 32);
}

static void velocitor_dma_fail(VelocitorState *s, uint32_t code, uint64_t info)
{
    velocitor_error_set(s, code, info);
    s->dma_status = VEL_DMA_STATUS_ERROR;
}

static void velocitor_dma_run(void *opaque)
{
    VelocitorState *s = opaque;
    PCIDevice *pdev = PCI_DEVICE(s);
    uint64_t iova = ((uint64_t)s->dma_addr_hi << 32) | s->dma_addr_lo;
    uint8_t *mem = memory_region_get_ram_ptr(&s->mem);
    MemTxResult res;

    /*
     * The 42-bit trap, spec section 9 and annex D.2: checked before any
     * access, so the device never touches an address it has just declared
     * out of its reach.  A driver with a correct mask never sees this; one
     * built with dma_bits_override=64 sees it every time, which is the whole
     * point of section 9.1.
     */
    if (iova >= (1ULL << VEL_DMA_BITS)) {
        velocitor_dma_fail(s, VEL_ERR_DMA_WIDTH, iova);
        return;
    }

    if ((uint64_t)s->dma_dev + s->dma_len > VEL_MEM_SIZE) {
        velocitor_dma_fail(s, VEL_ERR_OUT_OF_BOUNDS, s->dma_dev);
        s->cnt[VEL_CNT_INDEX(VEL_REG_CNT_ERR_RANGE)]++;
        return;
    }

    /*
     * Host memory is reached through the PCI DMA address space, never
     * through guest RAM directly (annex D.2).  That is what makes the vIOMMU
     * test of section 9.1 measure anything at all.
     */
    if (s->dma_ctl == VEL_DBG_DMA_H2D) {
        res = pci_dma_read(pdev, iova, mem + s->dma_dev, s->dma_len);
        s->cnt[VEL_CNT_INDEX(VEL_REG_CNT_DMA_RD)]++;
        s->cnt[VEL_CNT_INDEX(VEL_REG_CNT_BYTES_RD_LO)] += s->dma_len;
    } else {
        res = pci_dma_write(pdev, iova, mem + s->dma_dev, s->dma_len);
        s->cnt[VEL_CNT_INDEX(VEL_REG_CNT_DMA_WR)]++;
        s->cnt[VEL_CNT_INDEX(VEL_REG_CNT_BYTES_WR_LO)] += s->dma_len;
    }

    if (res != MEMTX_OK) {
        velocitor_dma_fail(s, VEL_ERR_DMA_WIDTH, iova);
        return;
    }

    s->dma_status = VEL_DMA_STATUS_DONE;
}

static void velocitor_dma_start(VelocitorState *s, uint32_t ctl)
{
    if (ctl != VEL_DBG_DMA_H2D && ctl != VEL_DBG_DMA_D2H) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "velocitor: DBG_DMA_CTL = %u is neither H2D nor D2H"
                      " (spec 4.3) -- ignored\n", ctl);
        return;
    }

    s->dma_ctl = ctl;
    s->err_code = VEL_ERR_NONE;
    s->dma_status = VEL_DMA_STATUS_BUSY;
    timer_mod(s->dma_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + VEL_DMA_LATENCY_NS);
}

/* ------------------------------------------------------------------ */
/* Firmware life cycle -- spec sections 4.1, 6.1, 6.5 and 6.6          */
/* ------------------------------------------------------------------ */

/*
 * Simulated boot time, virtual clock.  Long enough next to the DMA latency
 * that the two states are told apart by a driver that polls, short enough
 * that nobody waits for it.
 */
#define VEL_FW_BOOT_LATENCY_NS 100000

static uint32_t velocitor_mem_read32(VelocitorState *s, uint32_t off)
{
    const uint32_t *mem = memory_region_get_ram_ptr(&s->mem);

    return le32_to_cpu(mem[off / 4]);
}

static void velocitor_mem_write32(VelocitorState *s, uint32_t off, uint32_t val)
{
    uint32_t *mem = memory_region_get_ram_ptr(&s->mem);

    mem[off / 4] = cpu_to_le32(val);
}

/*
 * The boot completes here, one virtual tick after the header checked out.
 *
 * GENERATION moves on every entry into "running" (spec 6.5), and that is the
 * whole of the ABA defence: the allocator starts over at each boot, so a
 * handle from the previous generation must not silently name someone else's
 * allocation.  A driver that composes {generation, handle} pairs sees the
 * change; one that keeps bare handles across a crash does not, which is the
 * failure the project exists to make visible.
 */
static void velocitor_boot_done(void *opaque)
{
    VelocitorState *s = opaque;

    s->fw_status = VEL_FW_STATUS_RUNNING;
    s->generation++;

    /*
     * Publish an empty, well-formed trace ring (spec 6.6).  The loader has
     * already zeroed this region -- it is past the end of the ELF's file
     * data, so the core memset_io()s it -- which leaves head, tail and
     * dropped correct; only the entry size has to be stated.  A driver
     * reading the ring before a single entry exists then finds an empty
     * ring rather than having to guess whether it is empty or garbage.
     */
    velocitor_mem_write32(s, s->trace_da + VEL_TRACE_OFF_ENTRY_SIZE,
                          VEL_TRACE_ENTRY);
}

/*
 * Verify the firmware header, spec section 6.6, and start the boot.
 *
 * This is what stops step 6 from being ceremonial.  The model executes C
 * that does not depend on a single byte of the image, so without a check on
 * the loaded bytes a completely broken `load` would boot just as well as a
 * correct one.  Here the magic has to be in device memory, at the device
 * address the header contract names, for the firmware to run at all.
 */
static void velocitor_fw_verify(VelocitorState *s)
{
    uint32_t magic = velocitor_mem_read32(s, VEL_FW_HDR_DA +
                                          VEL_FW_HDR_OFF_MAGIC);
    uint32_t abi = velocitor_mem_read32(s, VEL_FW_HDR_DA +
                                        VEL_FW_HDR_OFF_ABI);
    uint32_t trace_da = velocitor_mem_read32(s, VEL_FW_HDR_DA +
                                             VEL_FW_HDR_OFF_TRACE_DA);
    uint32_t trace_len = velocitor_mem_read32(s, VEL_FW_HDR_DA +
                                              VEL_FW_HDR_OFF_TRACE_LEN);

    /*
     * Releasing RESET starts from nothing, whatever the previous boot ended
     * as.  Spec 4.1 says a header that does not check out leaves FW_STATUS
     * at 0, and that has to hold coming out of a crash as much as coming out
     * of a cold start.
     */
    s->fw_status = VEL_FW_STATUS_RESET;
    s->fw_abi = 0;

    if (magic != VEL_FW_MAGIC || abi != VEL_FW_ABI) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "velocitor: firmware header at device address 0x%x has "
                      "magic 0x%08x, ABI %u -- expected 0x%08x and %u "
                      "(spec 6.6); staying in reset\n",
                      VEL_FW_HDR_DA, magic, abi, VEL_FW_MAGIC, VEL_FW_ABI);
        velocitor_error_set(s, VEL_ERR_FW_HEADER, magic);
        velocitor_raise(s, VEL_IRQ_VEC_ERROR);
        return;
    }

    /*
     * The ring is read by the driver through the fixed aperture (spec 6.2),
     * so a firmware that puts it anywhere else has announced a buffer the
     * host cannot reach.
     */
    if (trace_len != VEL_TRACE_SIZE ||
        (uint64_t)trace_da + trace_len > VEL_APERTURE_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "velocitor: firmware trace buffer at 0x%08x + %u is not "
                      "%u bytes inside the fixed aperture (spec 6.6); "
                      "staying in reset\n",
                      trace_da, trace_len, VEL_TRACE_SIZE);
        velocitor_error_set(s, VEL_ERR_FW_HEADER, trace_da);
        velocitor_raise(s, VEL_IRQ_VEC_ERROR);
        return;
    }

    s->fw_abi = abi;
    s->trace_da = trace_da;
    s->trace_len = trace_len;
    s->fw_status = VEL_FW_STATUS_VERIFIED;

    timer_mod(s->boot_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + VEL_FW_BOOT_LATENCY_NS);
}

static void velocitor_reset_write(VelocitorState *s, uint32_t val)
{
    s->reset = val & 1;

    if (s->reset) {
        /* Annex D.3: deferred work goes before anything else. */
        timer_del(s->boot_timer);
        s->fw_status = VEL_FW_STATUS_RESET;
        s->fw_abi = 0;
        s->trace_da = 0;
        s->trace_len = 0;
        return;
    }

    /*
     * Spec section 6.1 has ops->start() publish RSC_ADDR_* and only then
     * release RESET.  The other order is not refused -- nothing in the boot
     * itself needs the table yet -- but it is a driver bug waiting for step
     * 7, where the device reads the notifyids out of it.
     */
    if (!s->rsc_valid) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "velocitor: RESET released with no valid shadow "
                      "resource table (spec 6.1) -- the notifyids will not be "
                      "readable\n");
    }

    velocitor_fw_verify(s);
}

/* ------------------------------------------------------------------ */
/* Shadow resource table -- spec sections 4.2 and 6.4                  */
/* ------------------------------------------------------------------ */

/*
 * A resource table starts with ver, num and two reserved words; the array
 * of entry offsets follows.  Anything shorter than that header cannot be a
 * table at all, whatever it points at.
 */
#define VEL_RSC_HDR_SIZE  16u

/*
 * Read one 32-bit word out of the shadow table.
 *
 * Through the PCI DMA address space like every other host access (annex
 * D.2), and behind the same 42-bit trap as the bring-up DMA: a table
 * published above the device's reach is exactly the failure section 9.1
 * exists to produce, and it would be absurd for it to be caught on the data
 * path but not on the control path.
 *
 * Step 7 reads the notifyids through here, step 8 the negotiated features.
 */
static bool velocitor_rsc_read32(VelocitorState *s, uint32_t off, uint32_t *val)
{
    PCIDevice *pdev = PCI_DEVICE(s);
    uint64_t base = ((uint64_t)s->rsc_addr_hi << 32) | s->rsc_addr_lo;
    uint32_t raw;

    if ((uint64_t)off + sizeof(raw) > s->rsc_len) {
        velocitor_error_set(s, VEL_ERR_OUT_OF_BOUNDS, off);
        return false;
    }

    if (base + off >= (1ULL << VEL_DMA_BITS)) {
        velocitor_error_set(s, VEL_ERR_DMA_WIDTH, base + off);
        return false;
    }

    if (pci_dma_read(pdev, base + off, &raw, sizeof(raw)) != MEMTX_OK) {
        velocitor_error_set(s, VEL_ERR_DMA_WIDTH, base + off);
        return false;
    }

    *val = le32_to_cpu(raw);
    return true;
}

/*
 * RSC_VALID going up is the driver saying "the table is readable now".  The
 * model takes it at its word only after reading the two words that make a
 * resource table one: a driver that publishes the wrong buffer, or the right
 * buffer at the wrong address, is caught here rather than three steps later
 * when a notifyid comes back as garbage.
 *
 * The check is deliberately shallow -- ver and num, nothing else.  Parsing
 * the entries belongs to the step that consumes them, and duplicating the
 * core's own validation would only create a second opinion to disagree with.
 */
static void velocitor_rsc_publish(VelocitorState *s, uint32_t val)
{
    uint32_t ver = 0;
    uint32_t num = 0;

    if (!(val & 1)) {
        s->rsc_valid = 0;
        return;
    }

    if (s->rsc_len < VEL_RSC_HDR_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "velocitor: RSC_VALID raised with RSC_LEN = %u, too "
                      "short for a resource table header (spec 6.4)\n",
                      s->rsc_len);
        velocitor_error_set(s, VEL_ERR_BAD_DESC, s->rsc_len);
        velocitor_raise(s, VEL_IRQ_VEC_ERROR);
        return;
    }

    if (!velocitor_rsc_read32(s, 0, &ver) ||
        !velocitor_rsc_read32(s, 4, &num)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "velocitor: shadow resource table at 0x%08x%08x is not "
                      "readable by the device (spec 6.4)\n",
                      s->rsc_addr_hi, s->rsc_addr_lo);
        velocitor_raise(s, VEL_IRQ_VEC_ERROR);
        return;
    }

    if (ver != 1 || num == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "velocitor: shadow resource table has ver = %u, num = %u"
                      " -- expected version 1 and at least one entry"
                      " (spec 6.3)\n", ver, num);
        velocitor_error_set(s, VEL_ERR_BAD_DESC, ver);
        velocitor_raise(s, VEL_IRQ_VEC_ERROR);
        return;
    }

    s->rsc_valid = 1;
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

    case VEL_REG_WIN_BASE:
        /* The read is the commit point, spec section 9. */
        velocitor_window_promote(s);
        return s->win_base;

    case VEL_REG_DBG_DMA_ADDR_LO:
        return s->dma_addr_lo;
    case VEL_REG_DBG_DMA_ADDR_HI:
        return s->dma_addr_hi;
    case VEL_REG_DBG_DMA_DEV:
        return s->dma_dev;
    case VEL_REG_DBG_DMA_LEN:
        return s->dma_len;
    case VEL_REG_DBG_DMA_STATUS:
        return s->dma_status;

    case VEL_REG_RSC_ADDR_LO:
        return s->rsc_addr_lo;
    case VEL_REG_RSC_ADDR_HI:
        return s->rsc_addr_hi;
    case VEL_REG_RSC_LEN:
        return s->rsc_len;
    case VEL_REG_RSC_VALID:
        /*
         * Reads back what the device accepted, not what was written: a
         * driver that publishes a table the device cannot read sees a zero
         * here and knows immediately.
         */
        return s->rsc_valid;

    case VEL_REG_ERR_CODE:
        return s->err_code;
    case VEL_REG_ERR_INFO_LO:
        return s->err_info_lo;
    case VEL_REG_ERR_INFO_HI:
        return s->err_info_hi;

    case VEL_REG_RESET:
        return s->reset;
    case VEL_REG_FW_STATUS:
        return s->fw_status;
    case VEL_REG_FW_ABI:
        /* Zero until the header has been checked (spec 4.1): the driver
         * cannot mistake "not verified yet" for "ABI 1". */
        return s->fw_abi;
    case VEL_REG_GENERATION:
        return s->generation;
    case VEL_REG_IRQ_STATUS:
        return s->irq_status;
    case VEL_REG_IRQ_MASK:
        return s->irq_mask;
    case VEL_REG_ERR_INJECT:
        return s->err_inject;
    case VEL_REG_ERR_INJECT_ARG:
        return s->err_inject_arg;

    case VEL_REG_DBG_DMA_CTL:
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

    case VEL_REG_RESET:
        velocitor_reset_write(s, (uint32_t)val);
        return;

    case VEL_REG_WIN_BASE:
        velocitor_window_write_base(s, (uint32_t)val);
        return;

    case VEL_REG_DBG_DMA_ADDR_LO:
        s->dma_addr_lo = (uint32_t)val;
        return;
    case VEL_REG_DBG_DMA_ADDR_HI:
        s->dma_addr_hi = (uint32_t)val;
        return;
    case VEL_REG_DBG_DMA_DEV:
        s->dma_dev = (uint32_t)val;
        return;
    case VEL_REG_DBG_DMA_LEN:
        s->dma_len = (uint32_t)val;
        return;
    case VEL_REG_DBG_DMA_CTL:
        velocitor_dma_start(s, (uint32_t)val);
        return;

    case VEL_REG_RSC_ADDR_LO:
        s->rsc_addr_lo = (uint32_t)val;
        return;
    case VEL_REG_RSC_ADDR_HI:
        s->rsc_addr_hi = (uint32_t)val;
        return;
    case VEL_REG_RSC_LEN:
        s->rsc_len = (uint32_t)val;
        return;
    case VEL_REG_RSC_VALID:
        /* Written last (spec 4.2): this is the commit point of the three
         * registers above, and where the model first touches the table. */
        velocitor_rsc_publish(s, (uint32_t)val);
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
            /* Annex D.3 again: a crash cancels what was in flight, including
             * a boot that had not finished. */
            timer_del(s->boot_timer);
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
    case VEL_REG_FW_ABI:
    case VEL_REG_GENERATION:
    case VEL_REG_IRQ_STATUS:
    case VEL_REG_DBG_DMA_STATUS:
    case VEL_REG_ERR_CODE:
    case VEL_REG_ERR_INFO_LO:
    case VEL_REG_ERR_INFO_HI:
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
    memory_region_init_ram(&s->mem, OBJECT(s), "velocitor-mem",
                           VEL_MEM_SIZE, errp);
    if (*errp) {
        return;
    }

    memory_region_init(&s->bar2, OBJECT(s), "velocitor-bar2", VEL_BAR2_SIZE);
    memory_region_init_alias(&s->aperture, OBJECT(s), "velocitor-aperture",
                             &s->mem, 0, VEL_APERTURE_SIZE);
    memory_region_init_alias(&s->window, OBJECT(s), "velocitor-window",
                             &s->mem, 0, VEL_WINDOW_SIZE);
    memory_region_add_subregion(&s->bar2, 0, &s->aperture);
    memory_region_add_subregion(&s->bar2, VEL_BAR2_WINDOW_OFF, &s->window);
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

    s->dma_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, velocitor_dma_run, s);
    s->boot_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, velocitor_boot_done, s);
}

static void velocitor_exit(PCIDevice *pdev)
{
    VelocitorState *s = VELOCITOR(pdev);

    timer_free(s->dma_timer);
    timer_free(s->boot_timer);
    msix_uninit(pdev, &s->bar4, &s->bar4);
}

static void velocitor_reset(DeviceState *dev)
{
    VelocitorState *s = VELOCITOR(dev);

    s->scratch = 0;
    memset(s->cnt, 0, sizeof(s->cnt));
    memset(s->cnt_snap, 0, sizeof(s->cnt_snap));

    /* Annex D.3: cancel deferred work before anything else. */
    if (s->dma_timer) {
        timer_del(s->dma_timer);
    }
    if (s->boot_timer) {
        timer_del(s->boot_timer);
    }
    s->dma_addr_lo = 0;
    s->dma_addr_hi = 0;
    s->dma_dev = 0;
    s->dma_len = 0;
    s->dma_ctl = 0;
    s->dma_status = VEL_DMA_STATUS_IDLE;
    s->err_code = VEL_ERR_NONE;
    s->err_info_lo = 0;
    s->err_info_hi = 0;

    /* The shadow table belongs to a boot that is over (spec 6.5). */
    s->rsc_addr_lo = 0;
    s->rsc_addr_hi = 0;
    s->rsc_len = 0;
    s->rsc_valid = 0;

    s->win_base = 0;
    s->win_pending = 0;
    memory_region_set_alias_offset(&s->window, 0);
    velocitor_mem_fill_pattern(s);

    s->irq_status = 0;
    s->irq_mask = 0;

    /*
     * RESET reads back asserted: nothing has been loaded, so saying anything
     * else would be a lie the driver could act on.  GENERATION starts at
     * zero and the first successful boot makes it 1, so "no firmware has
     * ever run here" and "one has" are distinguishable (spec 6.5).
     */
    s->reset = 1;
    s->fw_status = VEL_FW_STATUS_RESET;
    s->fw_abi = 0;
    s->generation = 0;
    s->trace_da = 0;
    s->trace_len = 0;

    s->err_inject = 0;
    s->err_inject_arg = 0;

    msix_reset(PCI_DEVICE(s));
}

static const VMStateDescription vmstate_velocitor = {
    .name = "velocitor",
    .version_id = 7,
    .minimum_version_id = 7,
    .fields = (VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, VelocitorState),
        VMSTATE_UINT32(scratch, VelocitorState),
        VMSTATE_UINT32_ARRAY(cnt, VelocitorState, VEL_CNT_COUNT),
        VMSTATE_UINT32_ARRAY(cnt_snap, VelocitorState, VEL_CNT_COUNT),
        VMSTATE_UINT32(win_base, VelocitorState),
        VMSTATE_UINT32(win_pending, VelocitorState),
        VMSTATE_UINT32(dma_addr_lo, VelocitorState),
        VMSTATE_UINT32(dma_addr_hi, VelocitorState),
        VMSTATE_UINT32(dma_dev, VelocitorState),
        VMSTATE_UINT32(dma_len, VelocitorState),
        VMSTATE_UINT32(dma_ctl, VelocitorState),
        VMSTATE_UINT32(dma_status, VelocitorState),
        VMSTATE_UINT32(rsc_addr_lo, VelocitorState),
        VMSTATE_UINT32(rsc_addr_hi, VelocitorState),
        VMSTATE_UINT32(rsc_len, VelocitorState),
        VMSTATE_UINT32(rsc_valid, VelocitorState),
        VMSTATE_UINT32(err_code, VelocitorState),
        VMSTATE_UINT32(err_info_lo, VelocitorState),
        VMSTATE_UINT32(err_info_hi, VelocitorState),
        VMSTATE_UINT32(irq_status, VelocitorState),
        VMSTATE_UINT32(irq_mask, VelocitorState),
        VMSTATE_UINT32(reset, VelocitorState),
        VMSTATE_UINT32(fw_status, VelocitorState),
        VMSTATE_UINT32(fw_abi, VelocitorState),
        VMSTATE_UINT32(generation, VelocitorState),
        VMSTATE_UINT32(trace_da, VelocitorState),
        VMSTATE_UINT32(trace_len, VelocitorState),
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
