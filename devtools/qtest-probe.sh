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
    "FW_STATUS at reset        |readl $((BAR0 + 0x20))|0x0000000000000000"
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
