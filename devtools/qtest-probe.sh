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

# Bring the BARs up: write the base addresses, then enable memory decoding in
# the command register.  No firmware has run, so nothing is assigned yet.
# BAR2 is the 64-bit one, so it takes config slots 0x18 and 0x1c and BAR4
# lands at 0x20 -- the layout spec section 3 calls out.
SETUP=(
    "outl 0xcf8 $((CFG_BASE + 0x10))"  "outl 0xcfc $BAR0"
    "outl 0xcf8 $((CFG_BASE + 0x18))"  "outl 0xcfc $BAR2"
    "outl 0xcf8 $((CFG_BASE + 0x1c))"  "outl 0xcfc 0x00000000"
    "outl 0xcf8 $((CFG_BASE + 0x20))"  "outl 0xcfc $BAR4"
    "outl 0xcf8 $((CFG_BASE + 0x04))"  "outl 0xcfc 0x00000002"
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
    "BAR2 stub reads 0         |readl $BAR2|0x0000000000000000"
    "BAR2 window offset reads 0|readl $((BAR2 + 0x1000000))|0x0000000000000000"
    "BAR4 stub reads 0         |readl $BAR4|0x0000000000000000"
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
