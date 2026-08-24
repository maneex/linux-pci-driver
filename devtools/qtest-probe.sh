#!/usr/bin/env bash

# devtools/qtest-probe.sh -- layer 1 of spec section 13.1: the QEMU model
# alone, no Linux.
#
# QEMU's qtest accelerator lets a test program drive MMIO and port I/O
# directly, with no CPU and no guest.  So the BARs are programmed by hand
# through the legacy 0xcf8/0xcfc config ports and the registers are read
# back, which checks the whole of what the model answers today in about two
# seconds -- no kernel, no initramfs, no boot.
#
# No equivalent upstream: lkmpg has no device of its own to test.

set -euo pipefail

DEVTOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$DEVTOOLS_DIR/.." && pwd)"
. "$DEVTOOLS_DIR/config.defaults"
[ -f "$DEVTOOLS_DIR/config.local" ] && . "$DEVTOOLS_DIR/config.local"

die() { echo "ERROR: $*" >&2; exit 1; }

[ -x "$QEMU_BIN" ] || die "$QEMU_BIN not found. Run devtools/build-qemu.sh first."

# The device is pinned to slot 3 so the config address below is fixed.
SLOT=03
CFG_BASE=0x80001800          # 0x80000000 | slot 3 << 11
# Inside the q35 32-bit PCI hole, above MMCONFIG.  BAR2 is 32 MiB and has to
# sit on a 32 MiB boundary.
BAR0=0xc0000000
BAR4=0xc4000000
BAR2=0xc8000000

# Bring the BARs up: write the base addresses, then enable memory decoding and
# bus mastering in the command register.  No firmware has run, so nothing is
# assigned yet.  Bus mastering (bit 2) is what pci_set_master() turns on in the
# driver; without it the device DMA address space is disabled and every
# pci_dma_read() of section 4.3 fails.
# BAR2 is the 64-bit one, so it takes config slots 0x18 and 0x1c and BAR4
# lands at 0x20 -- the layout spec section 3 calls out.
SETUP=(
    "outl 0xcf8 $((CFG_BASE + 0x10))"  "outl 0xcfc $BAR0"
    "outl 0xcf8 $((CFG_BASE + 0x18))"  "outl 0xcfc $BAR2"
    "outl 0xcf8 $((CFG_BASE + 0x1c))"  "outl 0xcfc 0x00000000"
    "outl 0xcf8 $((CFG_BASE + 0x20))"  "outl 0xcfc $BAR4"
    "outl 0xcf8 $((CFG_BASE + 0x04))"  "outl 0xcfc 0x00000006"
)

# label | qtest command | expected response, as qtest prints it
CHECKS=(
    "MAGIC                     |readl $((BAR0 + 0x00))|0x000000004f4c4556"
    "VERSION                   |readl $((BAR0 + 0x04))|0x0000000000000006"
    "CAPS                      |readl $((BAR0 + 0x08))|0x0000000000000007"
    "SCRATCH inverted at reset |readl $((BAR0 + 0x0c))|0x00000000ffffffff"
    "SCRATCH write             |writel $((BAR0 + 0x0c)) 0x12345678|"
    "SCRATCH inverted read-back|readl $((BAR0 + 0x0c))|0x00000000edcba987"
    "MEM_SIZE                  |readl $((BAR0 + 0x10))|0x0000000010000000"
    "TOPOLOGY                  |readl $((BAR0 + 0x14))|0x0000000000020002"
    "DMA_BITS                  |readl $((BAR0 + 0x18))|0x000000000000002a"
    "1-byte read is all ones   |readb $((BAR0 + 0x00))|0x00000000000000ff"
    "8-byte read is all ones   |readq $((BAR0 + 0x00))|0xffffffffffffffff"
    "unaligned read is all ones|readl $((BAR0 + 0x02))|0x00000000ffffffff"
    "reserved offset reads 0   |readl $((BAR0 + 0xf00))|0x0000000000000000"

    # Counters, spec 4.5.  Nothing increments them yet, so the values are all
    # zero; what is checked here is the mechanism -- the block is readable,
    # the snapshot write is accepted, the counters refuse writes, and the two
    # write-only registers answer all ones when read (annex A.3).
    "CNT_DB_RX reads 0         |readl $((BAR0 + 0x98))|0x0000000000000000"
    "CNT_CYCLES_E1 reads 0     |readl $((BAR0 + 0xe4))|0x0000000000000000"
    "CNT_SNAP write            |writel $((BAR0 + 0x94)) 0x00000001|"
    "CNT_DB_RX after snapshot  |readl $((BAR0 + 0x98))|0x0000000000000000"
    "write to counter          |writel $((BAR0 + 0xac)) 0xdeadbeef|"
    "CNT_GEMM survived that    |readl $((BAR0 + 0xac))|0x0000000000000000"
    "read of CNT_SNAP is WO    |readl $((BAR0 + 0x94))|0x00000000ffffffff"
    "read of CNT_RESET is WO   |readl $((BAR0 + 0x90))|0x00000000ffffffff"
    "CNT_RESET write           |writel $((BAR0 + 0x90)) 0x00000001|"
    "CNT_DB_RX after reset     |readl $((BAR0 + 0x98))|0x0000000000000000"
    "write to read-only MAGIC  |writel $((BAR0 + 0x00)) 0xdeadbeef|"
    "MAGIC survived that write |readl $((BAR0 + 0x00))|0x000000004f4c4556"
    # BAR2, spec 3.1.  Device-local memory is filled at reset so that every
    # 32-bit word holds its own offset; that is the oracle the step 4 sweep
    # compares against, and the driver cannot forge it.
    "aperture word 0           |readl $BAR2|0x0000000000000000"
    "aperture word at 0x1000   |readl $((BAR2 + 0x1000))|0x0000000000001000"
    "aperture last word        |readl $((BAR2 + 0xfffffc))|0x0000000000fffffc"
    "window at reset shows 0   |readl $((BAR2 + 0x1000000))|0x0000000000000000"

    # The read-back of spec section 9.  Writing WIN_BASE arms the move; only
    # reading the register commits it.  These two checks are the whole point
    # of step 4: a driver that skips the read-back reads the same window over
    # and over, with no error anywhere.
    "arm WIN_BASE = 16 MiB     |writel $((BAR0 + 0x24)) 0x01000000|"
    "window has NOT moved yet  |readl $((BAR2 + 0x1000000))|0x0000000000000000"
    "read-back commits it      |readl $((BAR0 + 0x24))|0x0000000001000000"
    "window now shows 16 MiB   |readl $((BAR2 + 0x1000000))|0x0000000001000000"
    "and one word further in   |readl $((BAR2 + 0x1000004))|0x0000000001000004"

    # Last legal base, then the two rejections of spec 3.1.
    "arm WIN_BASE = 240 MiB    |writel $((BAR0 + 0x24)) 0x0f000000|"
    "commit                    |readl $((BAR0 + 0x24))|0x000000000f000000"
    "window shows 240 MiB      |readl $((BAR2 + 0x1000000))|0x000000000f000000"
    "unaligned base refused    |writel $((BAR0 + 0x24)) 0x0f000004|"
    "out of range base refused |writel $((BAR0 + 0x24)) 0x10000000|"
    "WIN_BASE unchanged        |readl $((BAR0 + 0x24))|0x000000000f000000"

    # ERR_INJECT bit 6 (spec 9): the next WIN_BASE write is swallowed with no
    # error at all.  Only a driver that compares its read-back notices.
    "arm the win-ignore bit    |writel $((BAR0 + 0x40)) 0x00000040|"
    "write that gets swallowed |writel $((BAR0 + 0x24)) 0x00000000|"
    "read-back still 240 MiB   |readl $((BAR0 + 0x24))|0x000000000f000000"
    "bit was consumed, retry   |writel $((BAR0 + 0x24)) 0x00000000|"
    "this one took effect      |readl $((BAR0 + 0x24))|0x0000000000000000"

    # CNT_WIN_MOVE counts commits, not writes: three so far.
    "snapshot                  |writel $((BAR0 + 0x94)) 0x00000001|"
    "CNT_WIN_MOVE = 3          |readl $((BAR0 + 0xc8))|0x0000000000000003"
    "CNT_ERR_RANGE = 2         |readl $((BAR0 + 0xd4))|0x0000000000000002"

    # MSI-X, spec 3.3.  The capability sits where the shared header reserves
    # it, 0x40, and its two BIR fields pin the contractual placement: table
    # at BAR4 offset 0, PBA at BAR4 offset 0x1000.  Both read back as the
    # offset OR'd with the BAR number, so 0x...4.
    "select cap 0x40          |outl 0xcf8 $((CFG_BASE + 0x40))|"
    "MSI-X cap id/next/control|inl 0xcfc|0x56011"
    "select MSIX_TABLE        |outl 0xcf8 $((CFG_BASE + 0x44))|"
    "MSI-X table is BAR4 + 0  |inl 0xcfc|0x0004"
    "select MSIX_PBA          |outl 0xcf8 $((CFG_BASE + 0x48))|"
    "MSI-X PBA is BAR4 + 4K   |inl 0xcfc|0x1004"
    "vector 0 starts masked   |readl $((BAR4 + 0x0c))|0x0000000000000001"

    # Interrupts, spec 4.1.  Nothing has fired, so the latch is clear and
    # FW_STATUS is 0 (reset).
    "RESET at reset is asserted|readl $((BAR0 + 0x1c))|0x0000000000000001"
    "FW_STATUS at reset        |readl $((BAR0 + 0x20))|0x0000000000000000"
    "FW_ABI is 0 until checked |readl $((BAR0 + 0x38))|0x0000000000000000"
    "GENERATION at reset       |readl $((BAR0 + 0x3c))|0x0000000000000000"
    "IRQ_STATUS at reset       |readl $((BAR0 + 0x28))|0x0000000000000000"
    "IRQ_MASK at reset         |readl $((BAR0 + 0x2c))|0x0000000000000000"
    "read of IRQ_ACK is WO     |readl $((BAR0 + 0x30))|0x00000000ffffffff"

    # ERR_INJECT bit 2 (spec 9): the firmware crashes and vector 5 goes up.
    # This is what makes step 3 falsifiable -- without a trigger, IRQ_ACK
    # could never be exercised because no bit could ever be set.
    "inject firmware crash     |writel $((BAR0 + 0x40)) 0x00000004|"
    "FW_STATUS is CRASHED      |readl $((BAR0 + 0x20))|0x0000000000000003"
    "IRQ_STATUS latched vec 5  |readl $((BAR0 + 0x28))|0x0000000000000020"
    "IRQ_ACK clears vec 5      |writel $((BAR0 + 0x30)) 0x00000020|"
    "IRQ_STATUS back to 0      |readl $((BAR0 + 0x28))|0x0000000000000000"
    "write to read-only STATUS |writel $((BAR0 + 0x28)) 0xffffffff|"
    "IRQ_STATUS still 0        |readl $((BAR0 + 0x28))|0x0000000000000000"

    # The raise above is the first event any counter has ever seen, so it
    # is also the first real test of the snapshot: the live counter is 1,
    # but a read before CNT_SNAP still answers the stale snapshot.
    "CNT_NOTIFY_TX before snap |readl $((BAR0 + 0x9c))|0x0000000000000000"
    "CNT_SNAP write            |writel $((BAR0 + 0x94)) 0x00000001|"
    "CNT_NOTIFY_TX after snap  |readl $((BAR0 + 0x9c))|0x0000000000000001"

    # Bring-up DMA, spec 4.3.  qtest can seed guest RAM directly, so a full
    # host <-> device round trip is checkable with no kernel in sight.  RAM at
    # 16 MiB is well inside the q35 default.
    "seed guest RAM            |write 0x1000000 4 0x11223344|"
    "DBG_DMA_ADDR_LO          |writel $((BAR0 + 0x70)) 0x01000000|"
    "DBG_DMA_ADDR_HI          |writel $((BAR0 + 0x74)) 0x00000000|"
    "DBG_DMA_DEV = 0x1000     |writel $((BAR0 + 0x78)) 0x00001000|"
    "DBG_DMA_LEN = 4          |writel $((BAR0 + 0x7c)) 0x00000004|"
    "read of DBG_DMA_CTL is WO |readl $((BAR0 + 0x80))|0x00000000ffffffff"
    "trigger H2D              |writel $((BAR0 + 0x80)) 0x00000001|"
    "DBG_DMA_STATUS = busy     |readl $((BAR0 + 0x84))|0x0000000000000001"
    "let virtual time pass    |clock_step 2000|"
    "DBG_DMA_STATUS = done     |readl $((BAR0 + 0x84))|0x0000000000000002"
    "device memory got it      |readl $((BAR2 + 0x1000))|0x0000000044332211"

    # D2H: send the pattern word at device offset 0x2000 back out to RAM.
    "DBG_DMA_DEV = 0x2000     |writel $((BAR0 + 0x78)) 0x00002000|"
    "DBG_DMA_ADDR_LO          |writel $((BAR0 + 0x70)) 0x01000100|"
    "trigger D2H              |writel $((BAR0 + 0x80)) 0x00000002|"
    "let virtual time pass    |clock_step 2000|"
    "DBG_DMA_STATUS = done     |readl $((BAR0 + 0x84))|0x0000000000000002"
    "host RAM got the pattern  |read 0x1000100 4|0x00200000"

    # The 42-bit trap of spec 9.1 and annex D.2, checked before any access.
    "IOVA at exactly 2^42     |writel $((BAR0 + 0x74)) 0x00000400|"
    "trigger H2D              |writel $((BAR0 + 0x80)) 0x00000001|"
    "let virtual time pass    |clock_step 2000|"
    "DBG_DMA_STATUS = error    |readl $((BAR0 + 0x84))|0x0000000000000003"
    "ERR_CODE = 4 (DMA width)  |readl $((BAR0 + 0x50))|0x0000000000000004"
    "ERR_INFO_HI holds the top |readl $((BAR0 + 0x58))|0x0000000000000400"

    # Out of bounds on the device side.
    "back to a legal IOVA     |writel $((BAR0 + 0x74)) 0x00000000|"
    "DBG_DMA_DEV near the end |writel $((BAR0 + 0x78)) 0x0ffffffc|"
    "DBG_DMA_LEN = 16         |writel $((BAR0 + 0x7c)) 0x00000010|"
    "trigger H2D              |writel $((BAR0 + 0x80)) 0x00000001|"
    "let virtual time pass    |clock_step 2000|"
    "DBG_DMA_STATUS = error    |readl $((BAR0 + 0x84))|0x0000000000000003"
    "ERR_CODE = 2 (out of bnds)|readl $((BAR0 + 0x50))|0x0000000000000002"

    # One read and one write actually happened; the two failures counted none.
    "snapshot                  |writel $((BAR0 + 0x94)) 0x00000001|"
    "CNT_DMA_RD = 1            |readl $((BAR0 + 0xb0))|0x0000000000000001"
    "CNT_DMA_WR = 1            |readl $((BAR0 + 0xb4))|0x0000000000000001"
    "CNT_BYTES_RD = 4          |readl $((BAR0 + 0xb8))|0x0000000000000004"

    # Shadow resource table, spec 4.2 and 6.4.  The driver says where it put
    # its copy of the table; the device believes it only after reading the
    # two words that make a resource table one.  A driver that publishes the
    # wrong buffer is caught here rather than three steps later, when a
    # notifyid comes back as garbage.
    "RSC_VALID at reset        |readl $((BAR0 + 0xfc))|0x0000000000000000"
    "seed a table, ver = 1     |write 0x1100000 4 0x01000000|"
    "and              num = 1  |write 0x1100004 4 0x01000000|"
    "RSC_ADDR_LO               |writel $((BAR0 + 0xf0)) 0x01100000|"
    "RSC_ADDR_HI               |writel $((BAR0 + 0xf4)) 0x00000000|"
    "RSC_LEN = 16              |writel $((BAR0 + 0xf8)) 0x00000010|"
    "RSC_ADDR_LO reads back    |readl $((BAR0 + 0xf0))|0x0000000001100000"
    "RSC_LEN reads back        |readl $((BAR0 + 0xf8))|0x0000000000000010"
    "publish it                |writel $((BAR0 + 0xfc)) 0x00000001|"
    "device accepted the table |readl $((BAR0 + 0xfc))|0x0000000000000001"
    "withdraw it               |writel $((BAR0 + 0xfc)) 0x00000000|"
    "RSC_VALID back to 0       |readl $((BAR0 + 0xfc))|0x0000000000000000"

    # Too short to hold even the table header: refused without a single DMA
    # read, since there is nothing there worth reading.
    "RSC_LEN = 8               |writel $((BAR0 + 0xf8)) 0x00000008|"
    "publish it                |writel $((BAR0 + 0xfc)) 0x00000001|"
    "device refused it         |readl $((BAR0 + 0xfc))|0x0000000000000000"
    "ERR_CODE = 1 (bad desc)   |readl $((BAR0 + 0x50))|0x0000000000000001"
    "ERR_NOTIFYID = none       |readl $((BAR0 + 0x5c))|0x00000000ffffffff"
    "ERR_GENERATION = 0        |readl $((BAR0 + 0x64))|0x0000000000000000"
    "nothing dropped yet       |readl $((BAR0 + 0x68))|0x0000000000000000"

    # Version 2 does not exist -- the remoteproc core would not accept it
    # either, so the two sides agree on what a table is.  Vector 5 from the
    # refusal above has not been acknowledged, so this second error does not
    # overwrite the first: the block keeps the root cause and ERR_DROPPED
    # counts what followed (spec 4.4).
    "RSC_LEN = 16              |writel $((BAR0 + 0xf8)) 0x00000010|"
    "make the table version 2  |write 0x1100000 4 0x02000000|"
    "publish it                |writel $((BAR0 + 0xfc)) 0x00000001|"
    "device refused it         |readl $((BAR0 + 0xfc))|0x0000000000000000"
    "first ERR_CODE survived   |readl $((BAR0 + 0x50))|0x0000000000000001"
    "ERR_DROPPED = 1           |readl $((BAR0 + 0x68))|0x0000000000000001"

    # The 42-bit trap guards the control path too, not only the data path.
    # Acknowledge first, the way a driver does before retrying, or the block
    # would still be holding the error from two cases ago.
    "ack vector 5              |writel $((BAR0 + 0x30)) 0x00000020|"
    "restore version 1         |write 0x1100000 4 0x01000000|"
    "RSC_ADDR_HI = 2^42 >> 32  |writel $((BAR0 + 0xf4)) 0x00000400|"
    "publish it                |writel $((BAR0 + 0xfc)) 0x00000001|"
    "device refused it         |readl $((BAR0 + 0xfc))|0x0000000000000000"
    "ERR_CODE = 4 (DMA width)  |readl $((BAR0 + 0x50))|0x0000000000000004"
    "ERR_DROPPED stayed at 1   |readl $((BAR0 + 0x68))|0x0000000000000001"

    # Firmware life cycle, spec 4.1, 6.1, 6.5 and 6.6.  Put the table back
    # where the device can read it first, so that releasing RESET is the
    # sequence section 6.1 prescribes and not a second thing under test.
    "ack vector 5              |writel $((BAR0 + 0x30)) 0x00000020|"
    "clear the injection bits  |writel $((BAR0 + 0x40)) 0x00000000|"
    "RSC_ADDR_HI back in range |writel $((BAR0 + 0xf4)) 0x00000000|"
    "publish the table         |writel $((BAR0 + 0xfc)) 0x00000001|"
    "table is valid again      |readl $((BAR0 + 0xfc))|0x0000000000000001"

    # Nothing has been loaded, so device memory still holds the reset
    # pattern and word 0 is not the magic.  This is the check that makes the
    # load falsifiable: a broken `load` cannot boot.
    "release RESET, no firmware|writel $((BAR0 + 0x1c)) 0x00000000|"
    "RESET reads back released |readl $((BAR0 + 0x1c))|0x0000000000000000"
    "FW_STATUS stayed at reset |readl $((BAR0 + 0x20))|0x0000000000000000"
    "ERR_CODE = 10 (fw header) |readl $((BAR0 + 0x50))|0x000000000000000a"
    "vector 5 was raised       |readl $((BAR0 + 0x28))|0x0000000000000020"
    "ack it                    |writel $((BAR0 + 0x30)) 0x00000020|"

    # Now write a header the model will accept, through the fixed aperture --
    # which is how ops->load() reaches device memory too (spec 6.2).
    "header magic              |writel $BAR2 0x4f465456|"
    "header ABI = 1            |writel $((BAR2 + 0x04)) 0x00000001|"
    "header trace_da = 64 KiB  |writel $((BAR2 + 0x08)) 0x00010000|"
    "header trace_len = 64 KiB |writel $((BAR2 + 0x0c)) 0x00010000|"
    "assert RESET              |writel $((BAR0 + 0x1c)) 0x00000001|"
    "RESET reads back asserted |readl $((BAR0 + 0x1c))|0x0000000000000001"
    "release it                |writel $((BAR0 + 0x1c)) 0x00000000|"
    "FW_STATUS = verified      |readl $((BAR0 + 0x20))|0x0000000000000001"
    "FW_ABI now reads 1        |readl $((BAR0 + 0x38))|0x0000000000000001"
    "GENERATION not yet bumped |readl $((BAR0 + 0x3c))|0x0000000000000000"
    "let virtual time pass    |clock_step 200000|"
    "FW_STATUS = running       |readl $((BAR0 + 0x20))|0x0000000000000002"
    "GENERATION = 1            |readl $((BAR0 + 0x3c))|0x0000000000000001"
    # Only the entry size is the model's to write.  head, tail and dropped
    # are zeroed by the loader, which memset_io()s everything past the end of
    # the ELF's file data -- and there is no loader here, so device memory
    # still holds the reset pattern at those three words.
    "trace ring entry size     |readl $((BAR2 + 0x1000c))|0x0000000000000080"

    # A second boot: the generation moves, which is what keeps a handle from
    # the previous one from naming someone else's allocation (spec 6.5).
    "assert RESET              |writel $((BAR0 + 0x1c)) 0x00000001|"
    "FW_STATUS back to reset   |readl $((BAR0 + 0x20))|0x0000000000000000"
    "FW_ABI back to 0          |readl $((BAR0 + 0x38))|0x0000000000000000"
    "release it                |writel $((BAR0 + 0x1c)) 0x00000000|"
    "let virtual time pass    |clock_step 200000|"
    "FW_STATUS = running       |readl $((BAR0 + 0x20))|0x0000000000000002"
    "GENERATION = 2            |readl $((BAR0 + 0x3c))|0x0000000000000002"

    # A trace ring the host could not reach through the fixed aperture is a
    # header that has to be refused (spec 6.6).
    "assert RESET              |writel $((BAR0 + 0x1c)) 0x00000001|"
    "trace_da past the aperture|writel $((BAR2 + 0x08)) 0x00ff8000|"
    "release it                |writel $((BAR0 + 0x1c)) 0x00000000|"
    "FW_STATUS stayed at reset |readl $((BAR0 + 0x20))|0x0000000000000000"
    "ERR_CODE = 10 (fw header) |readl $((BAR0 + 0x50))|0x000000000000000a"
    "GENERATION did not move   |readl $((BAR0 + 0x3c))|0x0000000000000002"

    # ---- Transport: notifyid walk, queue window, doorbell (spec 4.1, 4.2,
    # annex D.2 and D.4) --------------------------------------------------
    #
    # Build a resource table the shape section 6.3 prescribes -- a carveout
    # then two vdevs of two vrings -- and let the device learn which notifyid
    # names which queue.  Everything below then hangs off that: the doorbell
    # carries a notifyid and nothing else, so if the walk is wrong, no ring
    # is ever swept and the counters stay at zero.
    #
    # qtest 'write' takes the bytes in the order given, so a little-endian
    # word reads back reversed: 0x03000000 here is the value 3.
    "ack the header error      |writel $((BAR0 + 0x30)) 0x00000020|"
    "table ver = 1             |write 0x1100000 4 0x01000000|"
    "num = 3 entries           |write 0x1100004 4 0x03000000|"
    "offset[0] = 0x1c carveout |write 0x1100010 4 0x1c000000|"
    "offset[1] = 0x54 vdev0    |write 0x1100014 4 0x54000000|"
    "offset[2] = 0x98 vdev1    |write 0x1100018 4 0x98000000|"
    "entry 0 type = CARVEOUT   |write 0x110001c 4 0x00000000|"
    "entry 1 type = VDEV       |write 0x1100054 4 0x03000000|"
    "vdev0 id = VIRTIO_ID_RPMSG|write 0x1100058 4 0x07000000|"
    "vdev0 gfeatures = F_NS    |write 0x1100064 4 0x01000000|"
    "vdev0 status/num_of_vrings|write 0x110006c 4 0x04020000|"
    "vdev0 vring0 notifyid = 0 |write 0x110007c 4 0x00000000|"
    "vdev0 vring1 notifyid = 1 |write 0x1100090 4 0x01000000|"
    "entry 2 type = VDEV       |write 0x1100098 4 0x03000000|"
    "vdev1 id = VEL_VIRTIO_ID  |write 0x110009c 4 0x00400000|"
    "vdev1 status/num_of_vrings|write 0x11000b0 4 0x04020000|"
    "vdev1 vring0 notifyid = 2 |write 0x11000c0 4 0x02000000|"
    "vdev1 vring1 notifyid = 3 |write 0x11000d4 4 0x03000000|"
    "RSC_LEN = 0xdc            |writel $((BAR0 + 0xf8)) 0x000000dc|"
    "publish the table         |writel $((BAR0 + 0xfc)) 0x00000001|"
    "device accepted it        |readl $((BAR0 + 0xfc))|0x0000000000000001"

    # The window, spec 4.2.  VQ_NUM_MAX is the device's own limit; VQ_SELECT
    # is global and contractual, so a fifth queue does not exist.
    "VQ_NUM_MAX = VEL_VRING_NUM|readl $((BAR0 + 0x104))|0x0000000000000100"
    "VQ_SELECT = 0             |writel $((BAR0 + 0x100)) 0x00000000|"
    "VQ_SELECT reads back 0    |readl $((BAR0 + 0x100))|0x0000000000000000"
    "VQ_SELECT = 4 refused     |writel $((BAR0 + 0x100)) 0x00000004|"
    "queue 0 still selected    |readl $((BAR0 + 0x100))|0x0000000000000000"
    "write to RO VQ_NUM_MAX    |writel $((BAR0 + 0x104)) 0xdeadbeef|"
    "VQ_NUM_MAX survived that  |readl $((BAR0 + 0x104))|0x0000000000000100"

    # Program queue 0 in the order section 4.2 fixes: addresses, VQ_NUM,
    # VQ_MSIX_VECTOR, then VQ_ENABLE last.
    "VQ_DESC_LO                |writel $((BAR0 + 0x110)) 0x01210000|"
    "VQ_DESC_HI                |writel $((BAR0 + 0x114)) 0x00000000|"
    "VQ_AVAIL_LO               |writel $((BAR0 + 0x118)) 0x01200000|"
    "VQ_AVAIL_HI               |writel $((BAR0 + 0x11c)) 0x00000000|"
    "VQ_USED_LO                |writel $((BAR0 + 0x120)) 0x01220000|"
    "VQ_USED_HI                |writel $((BAR0 + 0x124)) 0x00000000|"
    "VQ_NUM = 256              |writel $((BAR0 + 0x108)) 0x00000100|"
    "VQ_MSIX_VECTOR = 1        |writel $((BAR0 + 0x128)) 0x00000001|"
    "VQ_AVAIL_LO reads back    |readl $((BAR0 + 0x118))|0x0000000001200000"
    "VQ_NUM reads back         |readl $((BAR0 + 0x108))|0x0000000000000100"
    "not enabled yet           |readl $((BAR0 + 0x10c))|0x0000000000000000"

    # Enabling sweeps the ring at once (spec 4.2).  It is empty, so nothing
    # is counted -- but the read went out over the PCI DMA space, which is
    # what proves the address the driver published is reachable.
    "VQ_ENABLE = 1             |writel $((BAR0 + 0x10c)) 0x00000001|"
    "queue 0 is enabled        |readl $((BAR0 + 0x10c))|0x0000000000000001"
    "let virtual time pass     |clock_step 2000|"
    "snapshot                  |writel $((BAR0 + 0x94)) 0x00000001|"
    "no heads swept yet        |readl $((BAR0 + 0xa8))|0x0000000000000000"

    # A ring under description is frozen once enabled: section 4.2 puts
    # VQ_ENABLE last precisely so the description is complete and then still.
    "VQ_NUM write while enabled|writel $((BAR0 + 0x108)) 0x00000008|"
    "VQ_NUM unchanged          |readl $((BAR0 + 0x108))|0x0000000000000100"

    # A real receive buffer, and the announcement that consumes it.
    #
    # Queue 0 is what section 3.3 fixes as "what the device sends to Linux",
    # so its available ring carries buffers the host lends the device rather
    # than requests: one descriptor at 0x1210000 pointing at 0x1230000, and
    # a single head published.  The doorbell then arms the sweep -- annex D.1
    # forbids doing the work in the MMIO callback, so nothing has happened
    # until virtual time moves.
    "desc[0].addr = 0x1230000  |write 0x1210000 8 0x0000230100000000|"
    "desc[0].len = 512         |write 0x1210008 4 0x00020000|"
    "desc[0].flags = WRITE     |write 0x121000c 2 0x0200|"
    "avail.ring[0] = 0         |write 0x1200004 2 0x0000|"
    "avail.idx = 1             |write 0x1200002 2 0x0100|"
    "read of DOORBELL is WO    |readl $((BAR0 + 0x34))|0x00000000ffffffff"
    "ring the doorbell, id 0   |writel $((BAR0 + 0x34)) 0x00000000|"
    "nothing yet, D.1          |read 0x1230000 4|0x00000000"
    "let virtual time pass     |clock_step 2000|"

    # The announcement of spec 7.1, byte for byte: an rpmsg header addressed
    # from VEL_RPMSG_CTRL_ADDR to VEL_RPMSG_NS_ADDR, carrying a 40-byte
    # rpmsg_ns_msg.  If either side gets this one wrong, no channel appears
    # and nothing upstream can say why.
    "hdr.src = 1024            |read 0x1230000 4|0x00040000"
    "hdr.dst = 53 (NS)         |read 0x1230004 4|0x35000000"
    "hdr.len = 40              |read 0x123000c 2|0x2800"
    "ns.name = velocitor-ctrl  |read 0x1230010 8|0x76656c6f6369746f"
    "ns.addr = 1024            |read 0x1230030 4|0x00040000"
    "ns.flags = NS_CREATE      |read 0x1230034 4|0x00000000"

    # And it was handed back through the used ring, with the byte count and
    # the head the host had lent -- 16 of header plus 40 of payload.
    "used.idx = 1              |read 0x1220002 2|0x0100"
    "used.ring[0].id = 0       |read 0x1220004 4|0x00000000"
    "used.ring[0].len = 56     |read 0x1220008 4|0x38000000"
    "snapshot                  |writel $((BAR0 + 0x94)) 0x00000001|"
    "CNT_DB_RX = 1             |readl $((BAR0 + 0x98))|0x0000000000000001"
    "CNT_DESC = 1              |readl $((BAR0 + 0xa8))|0x0000000000000001"

    # Announced once per generation: a second doorbell finds nothing to do.
    "doorbell again            |writel $((BAR0 + 0x34)) 0x00000000|"
    "let virtual time pass     |clock_step 2000|"
    "used.idx still 1          |read 0x1220002 2|0x0100"

    # A doorbell that names no queue, and one that names a queue nobody
    # enabled: both counted, neither swept.  CNT_DB_RX is the truth about
    # what the driver did, not about what the device made of it (annex D.6).
    "doorbell for notifyid 99  |writel $((BAR0 + 0x34)) 0x00000063|"
    "doorbell for queue 3      |writel $((BAR0 + 0x34)) 0x00000003|"
    "let virtual time pass     |clock_step 2000|"
    "snapshot                  |writel $((BAR0 + 0x94)) 0x00000001|"
    "CNT_DB_RX = 4             |readl $((BAR0 + 0x98))|0x0000000000000004"
    "CNT_DESC still 1          |readl $((BAR0 + 0xa8))|0x0000000000000001"

    # ---- Control plane: ALLOC, FREE, STAT (spec 7.2, annex D.5) ---------
    #
    # Queue 1 is the other half of vdev0: what the host sends to the device.
    # Everything below is one exchange -- a request published on queue 1, a
    # doorbell, and a reply written into a buffer lent on queue 0 -- so the
    # test exercises the transport and the operation at the same time. That
    # is deliberate: a reply that never leaves is indistinguishable from an
    # operation that answered wrongly if only the operation is checked.
    #
    # GENERATION is 2 here (the boot above is the second that succeeded), and
    # the allocator restarted with it, so the first handle of the session is
    # 1 at the base of node 0 -- VEL_NODE0_BASE, which is 16 MiB because the
    # fixed aperture comes off that node (spec 3.2).

    # Seven more receive buffers on queue 0, one per reply below.
    "rx desc[1] -> 0x1231000   |write 0x1210010 8 0x0010230100000000|"
    "rx desc[1].len = 512      |write 0x1210018 4 0x00020000|"
    "rx desc[1].flags = WRITE  |write 0x121001c 2 0x0200|"
    "rx desc[2] -> 0x1232000   |write 0x1210020 8 0x0020230100000000|"
    "rx desc[2].len = 512      |write 0x1210028 4 0x00020000|"
    "rx desc[2].flags = WRITE  |write 0x121002c 2 0x0200|"
    "rx desc[3] -> 0x1233000   |write 0x1210030 8 0x0030230100000000|"
    "rx desc[3].len = 512      |write 0x1210038 4 0x00020000|"
    "rx desc[3].flags = WRITE  |write 0x121003c 2 0x0200|"
    "rx desc[4] -> 0x1234000   |write 0x1210040 8 0x0040230100000000|"
    "rx desc[4].len = 512      |write 0x1210048 4 0x00020000|"
    "rx desc[4].flags = WRITE  |write 0x121004c 2 0x0200|"
    "rx desc[5] -> 0x1235000   |write 0x1210050 8 0x0050230100000000|"
    "rx desc[5].len = 512      |write 0x1210058 4 0x00020000|"
    "rx desc[5].flags = WRITE  |write 0x121005c 2 0x0200|"
    "rx desc[6] -> 0x1236000   |write 0x1210060 8 0x0060230100000000|"
    "rx desc[6].len = 512      |write 0x1210068 4 0x00020000|"
    "rx desc[6].flags = WRITE  |write 0x121006c 2 0x0200|"
    "rx desc[7] -> 0x1237000   |write 0x1210070 8 0x0070230100000000|"
    "rx desc[7].len = 512      |write 0x1210078 4 0x00020000|"
    "rx desc[7].flags = WRITE  |write 0x121007c 2 0x0200|"
    "rx avail.ring[1..7]       |write 0x1200006 14 0x0100020003000400050006000700|"
    "rx avail.idx = 8          |write 0x1200002 2 0x0800|"

    # Queue 1, described then enabled, exactly like queue 0 above.
    "VQ_SELECT = 1             |writel $((BAR0 + 0x100)) 0x00000001|"
    "VQ_DESC_LO = 0x1250000    |writel $((BAR0 + 0x110)) 0x01250000|"
    "VQ_AVAIL_LO = 0x1240000   |writel $((BAR0 + 0x118)) 0x01240000|"
    "VQ_USED_LO = 0x1260000    |writel $((BAR0 + 0x120)) 0x01260000|"
    "VQ_NUM = 256              |writel $((BAR0 + 0x108)) 0x00000100|"
    "VQ_MSIX_VECTOR = 2        |writel $((BAR0 + 0x128)) 0x00000002|"
    "VQ_ENABLE = 1             |writel $((BAR0 + 0x10c)) 0x00000001|"
    "queue 1 is enabled        |readl $((BAR0 + 0x10c))|0x0000000000000001"

    # The seven requests, laid out once.  Each is an rpmsg header addressed
    # to VEL_RPMSG_CTRL_ADDR, then a vel_msg, then the operation's payload.
    "tx desc[0] -> 0x1270000   |write 0x1250000 8 0x0000270100000000|"
    "tx desc[0].len = 48       |write 0x1250008 4 0x30000000|"
    "tx desc[1] -> 0x1271000   |write 0x1250010 8 0x0010270100000000|"
    "tx desc[1].len = 32       |write 0x1250018 4 0x20000000|"
    "tx desc[2] -> 0x1272000   |write 0x1250020 8 0x0020270100000000|"
    "tx desc[2].len = 40       |write 0x1250028 4 0x28000000|"
    "tx desc[3] -> 0x1273000   |write 0x1250030 8 0x0030270100000000|"
    "tx desc[3].len = 40       |write 0x1250038 4 0x28000000|"
    "tx desc[4] -> 0x1274000   |write 0x1250040 8 0x0040270100000000|"
    "tx desc[4].len = 48       |write 0x1250048 4 0x30000000|"
    "tx desc[5] -> 0x1275000   |write 0x1250050 8 0x0050270100000000|"
    "tx desc[5].len = 48       |write 0x1250058 4 0x30000000|"
    "tx desc[6] -> 0x1276000   |write 0x1250060 8 0x0060270100000000|"
    "tx desc[6].len = 48       |write 0x1250068 4 0x30000000|"
    "tx avail.ring[0..6]       |write 0x1240004 14 0x0000010002000300040005000600|"

    # ALLOC of 4096 bytes on node 0, seq 1.
    "req0 hdr.src = 1024       |write 0x1270000 4 0x00040000|"
    "req0 hdr.dst = ctrl addr  |write 0x1270004 4 0x00040000|"
    "req0 hdr.len = 32         |write 0x127000c 2 0x2000|"
    "req0 msg.seq = 1          |write 0x1270010 4 0x01000000|"
    "req0 msg.op = ALLOC       |write 0x1270014 2 0x0200|"
    "req0 req.size = 4096      |write 0x1270020 8 0x0010000000000000|"
    "req0 req.node = 0         |write 0x127002c 4 0x00000000|"
    "publish it                |write 0x1240002 2 0x0100|"
    "ring the doorbell, id 1   |writel $((BAR0 + 0x34)) 0x00000001|"
    "let virtual time pass     |clock_step 2000|"

    # The reply, byte for byte.  dev_offset is what makes this test worth
    # writing: 16 MiB says the aperture was taken off node 0 (spec 3.2), and
    # nothing else in the exchange would have caught the other convention.
    "rsp0 hdr.src = ctrl addr  |read 0x1231000 4|0x00040000"
    "rsp0 hdr.dst = 1024       |read 0x1231004 4|0x00040000"
    "rsp0 hdr.len = 40         |read 0x123100c 2|0x2800"
    "rsp0 msg.seq = 1          |read 0x1231010 4|0x01000000"
    "rsp0 msg.op = ALLOC       |read 0x1231014 2|0x0200"
    "rsp0 msg.status = 0       |read 0x1231018 4|0x00000000"
    "rsp0 handle = 1, never 0  |read 0x1231020 4|0x01000000"
    "rsp0 node = 0             |read 0x1231024 4|0x00000000"
    "rsp0 generation = 2       |read 0x1231028 4|0x02000000"
    "rsp0 dev_offset = 16 MiB  |read 0x1231030 8|0x0000000100000000"

    # STAT, seq 2.  Node 0 is 112 MiB allocatable and node 1 is 128: the
    # asymmetry section 3.2 exposes rather than corrects, and the reason
    # section 7.2 reports capacity and free per node instead of a total.
    "req1 hdr.src = 1024       |write 0x1271000 4 0x00040000|"
    "req1 hdr.dst = ctrl addr  |write 0x1271004 4 0x00040000|"
    "req1 hdr.len = 16         |write 0x127100c 2 0x1000|"
    "req1 msg.seq = 2          |write 0x1271010 4 0x02000000|"
    "req1 msg.op = STAT        |write 0x1271014 2 0x0400|"
    "publish it                |write 0x1240002 2 0x0200|"
    "ring the doorbell, id 1   |writel $((BAR0 + 0x34)) 0x00000001|"
    "let virtual time pass     |clock_step 2000|"
    "rsp1 hdr.len = 56         |read 0x123200c 2|0x3800"
    "rsp1 node0 cap = 112 MiB  |read 0x1232020 8|0x0000000700000000"
    "rsp1 node0 free = cap-4K  |read 0x1232028 8|0x00f0ff0600000000"
    "rsp1 node1 cap = 128 MiB  |read 0x1232030 8|0x0000000800000000"
    "rsp1 node1 free = whole   |read 0x1232038 8|0x0000000800000000"
    "rsp1 live_handles = 1     |read 0x1232040 4|0x01000000"

    # FREE of handle 1, seq 3.  No payload comes back: status alone.
    "req2 hdr.src = 1024       |write 0x1272000 4 0x00040000|"
    "req2 hdr.dst = ctrl addr  |write 0x1272004 4 0x00040000|"
    "req2 hdr.len = 24         |write 0x127200c 2 0x1800|"
    "req2 msg.seq = 3          |write 0x1272010 4 0x03000000|"
    "req2 msg.op = FREE        |write 0x1272014 2 0x0300|"
    "req2 free.handle = 1      |write 0x1272020 4 0x01000000|"
    "publish it                |write 0x1240002 2 0x0300|"
    "ring the doorbell, id 1   |writel $((BAR0 + 0x34)) 0x00000001|"
    "let virtual time pass     |clock_step 2000|"
    "rsp2 hdr.len = 16 only    |read 0x123300c 2|0x1000"
    "rsp2 msg.status = 0       |read 0x1233018 4|0x00000000"

    # The same FREE again.  Section 7.2 says a handle is never reused within
    # a session, so the second one has to be refused -- without that,
    # ERR_CODE = 3 would never fire and a use-after-free would read someone
    # else's block in silence.
    "req3 hdr.src = 1024       |write 0x1273000 4 0x00040000|"
    "req3 hdr.dst = ctrl addr  |write 0x1273004 4 0x00040000|"
    "req3 hdr.len = 24         |write 0x127300c 2 0x1800|"
    "req3 msg.seq = 4          |write 0x1273010 4 0x04000000|"
    "req3 msg.op = FREE        |write 0x1273014 2 0x0300|"
    "req3 free.handle = 1      |write 0x1273020 4 0x01000000|"
    "publish it                |write 0x1240002 2 0x0400|"
    "ring the doorbell, id 1   |writel $((BAR0 + 0x34)) 0x00000001|"
    "let virtual time pass     |clock_step 2000|"
    "rsp3 status = -EINVAL     |read 0x1234018 4|0xeaffffff"
    "ERR_CODE = 3 (bad handle) |readl $((BAR0 + 0x50))|0x0000000000000003"
    "ERR_HANDLE names it       |readl $((BAR0 + 0x60))|0x0000000000000001"

    # ALLOC on a node that does not exist.  Refused without qualifying the
    # error block: section 4.4 has no code for a malformed argument, and
    # inventing one would be changing the contract rather than the model.
    "req4 hdr.src = 1024       |write 0x1274000 4 0x00040000|"
    "req4 hdr.dst = ctrl addr  |write 0x1274004 4 0x00040000|"
    "req4 hdr.len = 32         |write 0x127400c 2 0x2000|"
    "req4 msg.seq = 5          |write 0x1274010 4 0x05000000|"
    "req4 msg.op = ALLOC       |write 0x1274014 2 0x0200|"
    "req4 req.size = 4096      |write 0x1274020 8 0x0010000000000000|"
    "req4 req.node = 99        |write 0x127402c 4 0x63000000|"
    "publish it                |write 0x1240002 2 0x0500|"
    "ring the doorbell, id 1   |writel $((BAR0 + 0x34)) 0x00000001|"
    "let virtual time pass     |clock_step 2000|"
    "rsp4 status = -EINVAL     |read 0x1235018 4|0xeaffffff"
    "ERR_CODE still 3, not 7   |readl $((BAR0 + 0x50))|0x0000000000000003"

    # Bit 5 of ERR_INJECT, section 9: the next ALLOC fails whatever the free
    # memory is, and the bit is consumed when it acts.  Two allocations in a
    # row prove both halves -- the first refused, the second served, which is
    # what "consumed" means and what makes the injection reproducible.
    "ack the handle error      |writel $((BAR0 + 0x30)) 0x00000020|"
    "arm ERR_INJECT bit 5      |writel $((BAR0 + 0x40)) 0x00000020|"
    "req5 hdr.src = 1024       |write 0x1275000 4 0x00040000|"
    "req5 hdr.dst = ctrl addr  |write 0x1275004 4 0x00040000|"
    "req5 hdr.len = 32         |write 0x127500c 2 0x2000|"
    "req5 msg.seq = 6          |write 0x1275010 4 0x06000000|"
    "req5 msg.op = ALLOC       |write 0x1275014 2 0x0200|"
    "req5 req.size = 4096      |write 0x1275020 8 0x0010000000000000|"
    "req5 req.node = ANY       |write 0x127502c 4 0xffffffff|"
    "publish it                |write 0x1240002 2 0x0600|"
    "ring the doorbell, id 1   |writel $((BAR0 + 0x34)) 0x00000001|"
    "let virtual time pass     |clock_step 2000|"
    "rsp5 status = -ENOMEM     |read 0x1236018 4|0xf4ffffff"
    "ERR_CODE = 7 (nomem)      |readl $((BAR0 + 0x50))|0x0000000000000007"
    "bit 5 disarmed itself     |readl $((BAR0 + 0x40))|0x0000000000000000"

    # And the one after it goes through, on node 0 -- VEL_NODE_ANY takes the
    # first node that can hold the block, never the emptiest: section 12 asks
    # for the imbalance to be measured, so the model must not even it out.
    "ack the nomem error       |writel $((BAR0 + 0x30)) 0x00000020|"
    "req6 hdr.src = 1024       |write 0x1276000 4 0x00040000|"
    "req6 hdr.dst = ctrl addr  |write 0x1276004 4 0x00040000|"
    "req6 hdr.len = 32         |write 0x127600c 2 0x2000|"
    "req6 msg.seq = 7          |write 0x1276010 4 0x07000000|"
    "req6 msg.op = ALLOC       |write 0x1276014 2 0x0200|"
    "req6 req.size = 4096      |write 0x1276020 8 0x0010000000000000|"
    "req6 req.node = ANY       |write 0x127602c 4 0xffffffff|"
    "publish it                |write 0x1240002 2 0x0700|"
    "ring the doorbell, id 1   |writel $((BAR0 + 0x34)) 0x00000001|"
    "let virtual time pass     |clock_step 2000|"
    "rsp6 msg.status = 0       |read 0x1237018 4|0x00000000"
    "rsp6 handle = 2, not 1    |read 0x1237020 4|0x02000000"
    "rsp6 node = 0 (ANY)       |read 0x1237024 4|0x00000000"
    "rsp6 offset = 16 MiB + 4K |read 0x1237030 8|0x0010000100000000"

    # Back to queue 0, which is what the checks below assume is selected.
    "VQ_SELECT = 0             |writel $((BAR0 + 0x100)) 0x00000000|"

    # ---- Data plane: COPY_H2D and COPY_D2H (spec 8.2, 8.3) --------------
    #
    # Handle 2 is still live from the block above, 4096 bytes at 16 MiB + 4K
    # on node 0.  Queue 2 is engineq0, so the block is local to the engine
    # serving it and no far penalty applies -- which is what makes the cycle
    # count below a fixed number and not a guess.
    #
    # The check that matters is the cross one: the bytes go in through the
    # copy engine and come back out through the sliding window, two disjoint
    # paths to the same memory. An arithmetic error in one cannot cancel
    # itself out in the other, which a plain write-then-read-back would let it
    # do. Same discipline as guest-dma-test.sh.

    "engineq0 desc @0x1290000  |writel $((BAR0 + 0x100)) 0x00000002|"
    "VQ_DESC_LO                |writel $((BAR0 + 0x110)) 0x01290000|"
    "VQ_AVAIL_LO               |writel $((BAR0 + 0x118)) 0x01280000|"
    "VQ_USED_LO                |writel $((BAR0 + 0x120)) 0x012a0000|"
    "VQ_NUM = 256              |writel $((BAR0 + 0x108)) 0x00000100|"
    "VQ_MSIX_VECTOR = 3        |writel $((BAR0 + 0x128)) 0x00000003|"
    "VQ_ENABLE = 1             |writel $((BAR0 + 0x10c)) 0x00000001|"

    # Eight bytes of host data, and a chain of two: one the device reads,
    # one it writes (spec 8.3).
    "host payload              |write 0x12d0000 8 0xfeedface5a5aa5a5|"
    "desc[0] -> request        |write 0x1290000 8 0x00002b0100000000|"
    "desc[0].len = 48          |write 0x1290008 4 0x30000000|"
    "desc[0].flags = NEXT      |write 0x129000c 2 0x0100|"
    "desc[0].next = 1          |write 0x129000e 2 0x0100|"
    "desc[1] -> response       |write 0x1290010 8 0x00002c0100000000|"
    "desc[1].len = 24          |write 0x1290018 4 0x18000000|"
    "desc[1].flags = WRITE     |write 0x129001c 2 0x0200|"

    # vel_req_hdr then vel_copy_hdr.  GENERATION is 2 here, and a request
    # carrying anything else is refused by spec 6.5 -- checked further down.
    "req.seq = 1               |write 0x12b0000 4 0x01000000|"
    "req.generation = 2        |write 0x12b0004 4 0x02000000|"
    "req.op = COPY_H2D         |write 0x12b0008 2 0x0100|"
    "copy.handle = 2           |write 0x12b0010 4 0x02000000|"
    "copy.dev_offset = 0       |write 0x12b0018 8 0x0000000000000000|"
    "copy.host.dma_addr        |write 0x12b0020 8 0x00002d0100000000|"
    "copy.host.len = 8         |write 0x12b0028 8 0x0800000000000000|"

    "publish it                |write 0x1280004 2 0x0000|"
    "avail.idx = 1             |write 0x1280002 2 0x0100|"
    "ring the doorbell, id 2   |writel $((BAR0 + 0x34)) 0x00000002|"
    "let virtual time pass     |clock_step 2000|"

    "resp.seq = 1              |read 0x12c0000 4|0x01000000"
    "resp.status = 0           |read 0x12c0004 4|0x00000000"
    "resp.cycles = 100, local  |read 0x12c0008 8|0x6400000000000000"
    "resp.far_accesses = 0     |read 0x12c0010 4|0x00000000"
    "resp.engine = 0           |read 0x12c0014 4|0x00000000"
    "used.idx = 1              |read 0x12a0002 2|0x0100"
    "used.ring[0].len = 24     |read 0x12a0008 4|0x18000000"

    # The cross-check.  Handle 2 lives at 16 MiB + 4K; the window is already
    # parked at 16 MiB from the section above, so the block shows up at
    # window offset 0x1000 -- and the eight bytes are the ones the engine
    # wrote, seen through an entirely different path.
    "GENERATION here           |readl $((BAR0 + 0x3c))|0x0000000000000002"
    "arm WIN_BASE = 16 MiB     |writel $((BAR0 + 0x24)) 0x01000000|"
    "read it back to move it   |readl $((BAR0 + 0x24))|0x0000000001000000"
    "the copied bytes, word 0  |readl $((BAR2 + 0x1001000))|0x00000000cefaedfe"
    "the copied bytes, word 1  |readl $((BAR2 + 0x1001004))|0x00000000a5a55a5a"

    # And back out, into a host address the H2D never touched.
    "req2.seq = 2              |write 0x12b1000 4 0x02000000|"
    "req2.generation = 2       |write 0x12b1004 4 0x02000000|"
    "req2.op = COPY_D2H        |write 0x12b1008 2 0x0200|"
    "copy2.handle = 2          |write 0x12b1010 4 0x02000000|"
    "copy2.host.dma_addr       |write 0x12b1020 8 0x00002e0100000000|"
    "copy2.host.len = 8        |write 0x12b1028 8 0x0800000000000000|"
    "desc[2] -> request 2      |write 0x1290020 8 0x00102b0100000000|"
    "desc[2].len = 48          |write 0x1290028 4 0x30000000|"
    "desc[2].flags = NEXT      |write 0x129002c 2 0x0100|"
    "desc[2].next = 3          |write 0x129002e 2 0x0300|"
    "desc[3] -> response 2     |write 0x1290030 8 0x00102c0100000000|"
    "desc[3].len = 24          |write 0x1290038 4 0x18000000|"
    "desc[3].flags = WRITE     |write 0x129003c 2 0x0200|"
    "publish it                |write 0x1280006 2 0x0200|"
    "avail.idx = 2             |write 0x1280002 2 0x0200|"
    "ring the doorbell, id 2   |writel $((BAR0 + 0x34)) 0x00000002|"
    "let virtual time pass     |clock_step 2000|"

    "resp2.seq = 2, answered   |read 0x12c1000 4|0x02000000"
    "resp2.status = 0          |read 0x12c1004 4|0x00000000"
    "the round trip, word 0    |read 0x12e0000 4|0xfeedface"
    "the round trip, word 1    |read 0x12e0004 4|0x5a5aa5a5"

    # A stale generation is refused, spec 6.5.  Same chain, one field wrong.
    "req3.seq = 3              |write 0x12b2000 4 0x03000000|"
    "req3.generation = 1       |write 0x12b2004 4 0x01000000|"
    "req3.op = COPY_H2D        |write 0x12b2008 2 0x0100|"
    "copy3.handle = 2          |write 0x12b2010 4 0x02000000|"
    "copy3.host.len = 8        |write 0x12b2028 8 0x0800000000000000|"
    "desc[4] -> request 3      |write 0x1290040 8 0x00202b0100000000|"
    "desc[4].len = 48          |write 0x1290048 4 0x30000000|"
    "desc[4].flags = NEXT      |write 0x129004c 2 0x0100|"
    "desc[4].next = 5          |write 0x129004e 2 0x0500|"
    "desc[5] -> response 3     |write 0x1290050 8 0x00202c0100000000|"
    "desc[5].len = 24          |write 0x1290058 4 0x18000000|"
    "desc[5].flags = WRITE     |write 0x129005c 2 0x0200|"
    "publish it                |write 0x1280008 2 0x0400|"
    "avail.idx = 3             |write 0x1280002 2 0x0300|"
    "ring the doorbell, id 2   |writel $((BAR0 + 0x34)) 0x00000002|"
    "let virtual time pass     |clock_step 2000|"
    "resp3.seq = 3, answered   |read 0x12c2000 4|0x03000000"
    "resp3.status = -ESTALE    |read 0x12c2004 4|0x8cffffff"
    "ERR_CODE = 9 (stale)      |readl $((BAR0 + 0x50))|0x0000000000000009"

    "back to queue 0           |writel $((BAR0 + 0x100)) 0x00000000|"

    # Annex D.3: RESET purges the window and invalidates the table.  This is
    # what makes the driver reprogram both on every start -- and what makes a
    # recovery that forgets to do so fail loudly instead of going quiet.
    #
    # CNT_DESC is 18 by now: the announcement's buffer, seven control requests
    # and their seven replies, then three data-plane chains.  What the last
    # check asserts is that the doorbell after the reset adds none of its own.
    "assert RESET              |writel $((BAR0 + 0x1c)) 0x00000001|"
    "queue 0 is disabled       |readl $((BAR0 + 0x10c))|0x0000000000000000"
    "RSC_VALID cleared with it |readl $((BAR0 + 0xfc))|0x0000000000000000"
    "the ring addresses stay   |readl $((BAR0 + 0x118))|0x0000000001200000"
    "doorbell after reset      |writel $((BAR0 + 0x34)) 0x00000000|"
    "snapshot                  |writel $((BAR0 + 0x94)) 0x00000001|"
    "counted but not swept     |readl $((BAR0 + 0xa8))|0x0000000000000012"
)

commands=("${SETUP[@]}")
labels=()
expected=()
for check in "${CHECKS[@]}"; do
    IFS='|' read -r label cmd want <<< "$check"
    commands+=("$cmd")
    # Only reads produce a value line; writes answer a bare OK.
    if [ -n "$want" ]; then
        labels+=("$label")
        expected+=("$want")
    fi
done

out=$(printf '%s\n' "${commands[@]}" \
      | timeout 30 "$QEMU_BIN" -M "${QEMU_MACHINE%%,*}" -display none \
            -accel qtest -qtest stdio -nodefaults \
            -device "velocitor,addr=${SLOT}.0" 2>/dev/null || true)

mapfile -t got < <(printf '%s\n' "$out" | sed -n 's/^OK \(0x[0-9a-f]*\)$/\1/p')

if [ "${#got[@]}" -ne "${#expected[@]}" ]; then
    die "expected ${#expected[@]} values, got ${#got[@]}.
The device did not answer; run the same command by hand to see why."
fi

failures=0
for i in "${!expected[@]}"; do
    if [ "${got[$i]}" = "${expected[$i]}" ]; then
        printf '  ok    %s  %s\n' "${labels[$i]}" "${got[$i]}"
    else
        printf '  FAIL  %s  got %s, want %s\n' \
               "${labels[$i]}" "${got[$i]}" "${expected[$i]}"
        failures=$((failures + 1))
    fi
done

echo ""
if [ "$failures" -ne 0 ]; then
    die "$failures check(s) failed."
fi
echo "${#expected[@]} checks passed."
