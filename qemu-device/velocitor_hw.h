/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VELOCITOR -- shared hardware constants.
 *
 * This file is the seed of the shared header called for by spec sections 2
 * and C.2.  It is meant to end up consumed unchanged by the three sides --
 * QEMU model, kernel driver, user runtime -- so it depends on nothing: no
 * QEMU header, no kernel header, no libc header, no typedef.
 *
 * It carries the full contract (all of spec section 2, and the complete BAR0
 * register map of section 4) even where the model does not implement the
 * corresponding behaviour yet.  The map is the contract; velocitor.c states
 * separately what it answers today.
 *
 * Reference: velocitor-device-spec.md v0.6.3.
 */
#ifndef VELOCITOR_HW_H
#define VELOCITOR_HW_H

/* ------------------------------------------------------------------ */
/* PCI identity -- spec section 2.1                                    */
/* ------------------------------------------------------------------ */

#define VEL_PCI_VENDOR_ID   0x1B36u  /* QEMU experimental device vendor   */
#define VEL_PCI_DEVICE_ID   0x0100u
#define VEL_PCI_REVISION    0x01u
#define VEL_PCI_CLASS       0x1200u  /* processing accelerator, prog-if 0 */

/*
 * Config space capability layout.  MSI-X spans 12 bytes, PCI Express v2
 * spans 0x3C, and the two must not collide.
 */
#define VEL_PCI_CAP_MSIX    0x40u    /* 0x40 .. 0x4B                      */
#define VEL_PCI_CAP_EXPRESS 0x60u    /* 0x60 .. 0x9B                      */

/* ------------------------------------------------------------------ */
/* Device geometry -- spec section 2                                   */
/* ------------------------------------------------------------------ */

#define VEL_MEM_SIZE        (256u << 20)  /* total device-local memory     */
#define VEL_NODES           2u            /* memory nodes                  */
#define VEL_ENGINES         2u            /* GEMM engines                  */
#define VEL_STREAMS         2u            /* = VEL_ENGINES in v1, cf. 8.4  */
#define VEL_APERTURE_SIZE   (16u << 20)   /* fixed aperture, low BAR2      */
#define VEL_WINDOW_SIZE     (16u << 20)   /* sliding window                */
#define VEL_DMA_BITS        42u           /* supported DMA address width   */
#define VEL_FAR_PENALTY     4u            /* remote access cost factor     */

/* Host memory */
#define VEL_HOST_POOL_SIZE  (64u << 20)   /* coherent pool; requires CMA   */

/* Device allocation */
#define VEL_ALLOC_ALIGN     256u
#define VEL_NODE_ANY        0xFFFFFFFFu
#define VEL_ENGINE_ANY      0xFFFFFFFFu

/* virtio transport */
#define VEL_VIRTIO_ID       0x4000u       /* data vdev, spec 2.3           */
#define VEL_VRING_NUM       256u          /* descriptors per vring         */
#define VEL_VRING_ALIGN     4096u
#define VEL_MSIX_VECTORS    6u
#define VEL_VQ_CTRL_RX      0u            /* VQ_SELECT index, cf. 4.2      */
#define VEL_VQ_CTRL_TX      1u
#define VEL_VQ_ENGINE0      2u
#define VEL_VQ_ENGINE1      3u

/* rpmsg */
#define VEL_RPMSG_NS_ADDR   53u
#define VEL_RPMSG_CTRL_ADDR 1024u
#define VEL_CTRL_NAME       "velocitor-ctrl"

/* firmware */
#define VEL_FW_MAGIC        0x4F465456u
#define VEL_FW_ABI          1u
#define VEL_FW_HDR_DA       0u
#define VEL_FW_NAME         "velocitor-fw.elf"

/*
 * Firmware header, spec section 6.6:
 *
 *     struct vel_fw_hdr {
 *         __le32 magic;      VEL_FW_MAGIC
 *         __le32 abi;        VEL_FW_ABI
 *         __le32 trace_da;   where the firmware put its trace ring
 *         __le32 trace_len;  VEL_TRACE_SIZE
 *     };
 *
 * Given as offsets rather than as a struct so that this header goes on
 * depending on nothing: it has no fixed-width types of its own to declare
 * the fields with, and the three sides that read it -- the image generator,
 * the model and the driver -- each already have theirs.
 */
#define VEL_FW_HDR_OFF_MAGIC     0x00u
#define VEL_FW_HDR_OFF_ABI       0x04u
#define VEL_FW_HDR_OFF_TRACE_DA  0x08u
#define VEL_FW_HDR_OFF_TRACE_LEN 0x0Cu
#define VEL_FW_HDR_SIZE          0x10u

/* trace */
#define VEL_TRACE_SIZE      (64u << 10)
#define VEL_TRACE_ENTRY     128u
#define VEL_TRACE_ENTRIES   511u

/*
 * Trace ring header, spec section 6.6:
 *
 *     struct vel_trace_hdr { __le32 head, tail, dropped, entry_size; };
 *
 * Offsets rather than a struct, for the reason given for the firmware
 * header below.  head belongs to the model and tail to the driver; neither
 * writes the other's index, which is what keeps two concurrent readers from
 * stealing each other's entries.
 */
#define VEL_TRACE_OFF_HEAD       0x00u
#define VEL_TRACE_OFF_TAIL       0x04u
#define VEL_TRACE_OFF_DROPPED    0x08u
#define VEL_TRACE_OFF_ENTRY_SIZE 0x0Cu
#define VEL_TRACE_HDR_SIZE       0x10u

/* ------------------------------------------------------------------ */
/* BAR layout -- spec section 3                                        */
/* ------------------------------------------------------------------ */

#define VEL_BAR0_INDEX      0             /* MEM32, non-prefetchable       */
#define VEL_BAR0_SIZE       (4u << 10)
#define VEL_BAR2_INDEX      2             /* MEM64, prefetchable           */
#define VEL_BAR2_SIZE       (VEL_APERTURE_SIZE + VEL_WINDOW_SIZE)
#define VEL_BAR4_INDEX      4             /* MEM32, MSI-X tables           */
#define VEL_BAR4_SIZE       (8u << 10)

/*
 * Where the MSI-X table and PBA sit inside BAR4.  The spec fixes the BAR
 * and its size but not this split; table at the base and PBA at half the
 * BAR follows the QEMU convention and leaves both 4 KiB-aligned.  A driver
 * never needs these -- the PCI core reads them out of the capability -- but
 * the qtest layer does, and so would a second implementation.
 */
#define VEL_MSIX_TABLE_OFF  0x0000u
#define VEL_MSIX_PBA_OFF    0x1000u

/*
 * MSI-X vector assignment, section 3.3.  Only CONFIG and ERROR latch a bit
 * in IRQ_STATUS and require an IRQ_ACK; the four queue vectors acknowledge
 * nothing and their handlers touch no register at all.
 */
#define VEL_IRQ_VEC_CONFIG  0u
#define VEL_IRQ_VEC_VRING0  1u            /* device -> host                */
#define VEL_IRQ_VEC_VRING1  2u            /* host -> device                */
#define VEL_IRQ_VEC_ENGQ0   3u
#define VEL_IRQ_VEC_ENGQ1   4u
#define VEL_IRQ_VEC_ERROR   5u

#define VEL_IRQ_LATCHED     ((1u << VEL_IRQ_VEC_CONFIG) | \
                             (1u << VEL_IRQ_VEC_ERROR))

/* Offset of the sliding window inside BAR2 (section 3.1) */
#define VEL_BAR2_WINDOW_OFF VEL_APERTURE_SIZE

/* ------------------------------------------------------------------ */
/* BAR0 block map -- spec section 4                                    */
/* ------------------------------------------------------------------ */

#define VEL_BLK_CTRL_BASE    0x000u  /* control, 4.1                       */
#define VEL_BLK_CTRL_END     0x04Fu
#define VEL_BLK_ERR_BASE     0x050u  /* qualified error, 4.4               */
#define VEL_BLK_ERR_END      0x06Fu
#define VEL_BLK_DBGDMA_BASE  0x070u  /* bring-up DMA, 4.3                  */
#define VEL_BLK_DBGDMA_END   0x08Fu
#define VEL_BLK_CNT_BASE     0x090u  /* counters, 4.5                      */
#define VEL_BLK_CNT_END      0x0EFu
#define VEL_BLK_RSC_BASE     0x0F0u  /* shadow resource table, 4.2         */
#define VEL_BLK_RSC_END      0x0FFu
#define VEL_BLK_VQ_BASE      0x100u  /* queue config window, 4.2           */
#define VEL_BLK_VQ_END       0x12Fu

/* ---- control, section 4.1 ---- */
#define VEL_REG_MAGIC           0x000u  /* RO                              */
#define VEL_REG_VERSION         0x004u  /* RO                              */
#define VEL_REG_CAPS            0x008u  /* RO                              */
#define VEL_REG_SCRATCH         0x00Cu  /* RW, reads back bitwise inverted */
#define VEL_REG_MEM_SIZE        0x010u  /* RO                              */
#define VEL_REG_TOPOLOGY        0x014u  /* RO                              */
#define VEL_REG_DMA_BITS        0x018u  /* RO                              */
#define VEL_REG_RESET           0x01Cu  /* RW                              */
#define VEL_REG_FW_STATUS       0x020u  /* RO                              */
#define VEL_REG_WIN_BASE        0x024u  /* RW                              */
#define VEL_REG_IRQ_STATUS      0x028u  /* RO, bits 0 and 5 only           */
#define VEL_REG_IRQ_MASK        0x02Cu  /* RW                              */
#define VEL_REG_IRQ_ACK         0x030u  /* WO                              */
#define VEL_REG_DOORBELL        0x034u  /* WO                              */
#define VEL_REG_FW_ABI          0x038u  /* RO                              */
#define VEL_REG_GENERATION      0x03Cu  /* RO                              */
#define VEL_REG_ERR_INJECT      0x040u  /* RW                              */
#define VEL_REG_ERR_INJECT_ARG  0x044u  /* RW                              */

/* ---- qualified error, section 4.4 ---- */
#define VEL_REG_ERR_CODE        0x050u
#define VEL_REG_ERR_INFO_LO     0x054u
#define VEL_REG_ERR_INFO_HI     0x058u
#define VEL_REG_ERR_NOTIFYID    0x05Cu
#define VEL_REG_ERR_HANDLE      0x060u
#define VEL_REG_ERR_GENERATION  0x064u
#define VEL_REG_ERR_DROPPED     0x068u

/* ---- bring-up DMA, section 4.3 ---- */
#define VEL_REG_DBG_DMA_ADDR_LO 0x070u
#define VEL_REG_DBG_DMA_ADDR_HI 0x074u
#define VEL_REG_DBG_DMA_DEV     0x078u
#define VEL_REG_DBG_DMA_LEN     0x07Cu
#define VEL_REG_DBG_DMA_CTL     0x080u  /* WO, 1 = H2D, 2 = D2H            */
#define VEL_REG_DBG_DMA_STATUS  0x084u  /* RO                              */

/* DBG_DMA_CTL commands and DBG_DMA_STATUS values, section 4.3 */
#define VEL_DBG_DMA_H2D         1u
#define VEL_DBG_DMA_D2H         2u
#define VEL_DMA_STATUS_IDLE     0u
#define VEL_DMA_STATUS_BUSY     1u
#define VEL_DMA_STATUS_DONE     2u
#define VEL_DMA_STATUS_ERROR    3u

/* ---- counters, section 4.5 ---- */
#define VEL_REG_CNT_RESET            0x090u  /* WO                         */
#define VEL_REG_CNT_SNAP             0x094u  /* WO                         */
#define VEL_REG_CNT_DB_RX            0x098u
#define VEL_REG_CNT_NOTIFY_TX        0x09Cu
#define VEL_REG_CNT_NOTIFY_COALESCED 0x0A0u
#define VEL_REG_CNT_NOTIFY_DROPPED   0x0A4u
#define VEL_REG_CNT_DESC             0x0A8u
#define VEL_REG_CNT_GEMM             0x0ACu
#define VEL_REG_CNT_DMA_RD           0x0B0u
#define VEL_REG_CNT_DMA_WR           0x0B4u
#define VEL_REG_CNT_BYTES_RD_LO      0x0B8u
#define VEL_REG_CNT_BYTES_RD_HI      0x0BCu
#define VEL_REG_CNT_BYTES_WR_LO      0x0C0u
#define VEL_REG_CNT_BYTES_WR_HI      0x0C4u
#define VEL_REG_CNT_WIN_MOVE         0x0C8u
#define VEL_REG_CNT_FAR_ACCESS       0x0CCu
#define VEL_REG_CNT_ERR_DESC         0x0D0u
#define VEL_REG_CNT_ERR_RANGE        0x0D4u
#define VEL_REG_CNT_STALL_E0         0x0D8u
#define VEL_REG_CNT_STALL_E1         0x0DCu
#define VEL_REG_CNT_CYCLES_E0        0x0E0u
#define VEL_REG_CNT_CYCLES_E1        0x0E4u

/*
 * The readable counters are contiguous and 32-bit, so both sides index the
 * block rather than enumerate it -- same three values, same arithmetic, no
 * chance of the two implementations disagreeing on how many there are.
 * CNT_RESET and CNT_SNAP are deliberately outside the range: they are
 * write-only with a side effect (annex A.3).
 */
#define VEL_CNT_FIRST       VEL_REG_CNT_DB_RX
#define VEL_CNT_LAST        VEL_REG_CNT_CYCLES_E1
#define VEL_CNT_COUNT       (((VEL_CNT_LAST - VEL_CNT_FIRST) / 4u) + 1u)
#define VEL_CNT_INDEX(off)  (((off) - VEL_CNT_FIRST) / 4u)

/* ---- shadow resource table, section 4.2 ---- */
#define VEL_REG_RSC_ADDR_LO     0x0F0u
#define VEL_REG_RSC_ADDR_HI     0x0F4u
#define VEL_REG_RSC_LEN         0x0F8u
#define VEL_REG_RSC_VALID       0x0FCu

/* ---- queue configuration window, section 4.2 ---- */
#define VEL_REG_VQ_SELECT       0x100u
#define VEL_REG_VQ_NUM_MAX      0x104u  /* RO                              */
#define VEL_REG_VQ_NUM          0x108u
#define VEL_REG_VQ_ENABLE       0x10Cu
#define VEL_REG_VQ_DESC_LO      0x110u
#define VEL_REG_VQ_DESC_HI      0x114u
#define VEL_REG_VQ_AVAIL_LO     0x118u
#define VEL_REG_VQ_AVAIL_HI     0x11Cu
#define VEL_REG_VQ_USED_LO      0x120u
#define VEL_REG_VQ_USED_HI      0x124u
#define VEL_REG_VQ_MSIX_VECTOR  0x128u

/* ------------------------------------------------------------------ */
/* Register values                                                     */
/* ------------------------------------------------------------------ */

#define VEL_MAGIC           0x4F4C4556u   /* "VELO" little-endian          */

/*
 * VERSION is not fixed by the spec.  It tracks the revision of the contract
 * the model implements, so the driver can refuse a model it predates.
 * Bumping it is a section 16 decision, not an implementation detail.
 */
#define VEL_VERSION_MAJOR   0u
#define VEL_VERSION_MINOR   6u
#define VEL_VERSION         ((VEL_VERSION_MAJOR << 16) | VEL_VERSION_MINOR)

/* CAPS bits, section 4.1 -- what the hardware has */
#define VEL_CAP_FP32        (1u << 0)
#define VEL_CAP_BF16        (1u << 1)
#define VEL_CAP_TRANSPOSE   (1u << 2)

#define VEL_TOPOLOGY        ((VEL_NODES << 16) | VEL_ENGINES)

/* ERR_INJECT bits, section 9 */
#define VEL_ERR_INJECT_GEMM_EIO   (1u << 0)
#define VEL_ERR_INJECT_CORRUPT    (1u << 1)
#define VEL_ERR_INJECT_FW_CRASH   (1u << 2)
#define VEL_ERR_INJECT_DELAY      (1u << 3)
#define VEL_ERR_INJECT_DROP_NOTIF (1u << 4)
#define VEL_ERR_INJECT_ALLOC_FAIL (1u << 5)
#define VEL_ERR_INJECT_WIN_IGNORE (1u << 6)
#define VEL_ERR_INJECT_ENG1_STALL (1u << 7)
#define VEL_ERR_INJECT_STALE_GEN  (1u << 8)

/* FW_STATUS values, section 4.1 */
#define VEL_FW_STATUS_RESET     0u
#define VEL_FW_STATUS_VERIFIED  1u
#define VEL_FW_STATUS_RUNNING   2u
#define VEL_FW_STATUS_CRASHED   3u

/* ERR_CODE values, section 4.4 */
#define VEL_ERR_NONE            0u
#define VEL_ERR_BAD_DESC        1u   /* fatal    */
#define VEL_ERR_OUT_OF_BOUNDS   2u   /* sync     */
#define VEL_ERR_BAD_HANDLE      3u   /* sync     */
#define VEL_ERR_DMA_WIDTH       4u   /* fatal    */
#define VEL_ERR_GEMM_DIMS       5u   /* sync     */
#define VEL_ERR_DTYPE           6u   /* sync     */
#define VEL_ERR_NOMEM           7u   /* sync     */
#define VEL_ERR_WINDOW_MOVED    8u   /* fatal    */
#define VEL_ERR_STALE           9u   /* sync     */
#define VEL_ERR_FW_HEADER       10u  /* fatal    */

#endif /* VELOCITOR_HW_H */
