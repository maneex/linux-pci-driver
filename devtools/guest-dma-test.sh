#!/bin/sh

# devtools/guest-dma-test.sh -- layer 2 of spec section 13.1: the bring-up DMA
# block of section 4.3, driven from the guest through the two debugfs files
# the driver exposes.
#
# This runs INSIDE the guest, under busybox ash -- not bash.  Launch it with:
#
#   devtools/boot.sh --test /mnt/velocitor/devtools/guest-dma-test.sh
#
# Environment:
#   SEED=n    seed of the fuzzing pass; the same seed replays the same cases
#   FUZZ=n    number of fuzzed transfers (0 disables the pass entirely)
#
# The deterministic part comes first and in a fixed order, because the first
# check leans on an oracle that only holds before anything has been written:
# at reset the model fills its 256 MB of local memory so that every 32-bit
# word holds its own offset.  A driver cannot forge that value, which is what
# makes it worth checking -- an address arithmetic error that a plain
# write-then-read-back round trip would cancel out shows up here.

set -u

MODULE=/mnt/velocitor/module/velocitor.ko
TRACING=/sys/kernel/tracing
MEM_SIZE=$((256 * 1024 * 1024))   # VEL_MEM_SIZE
POOL_SIZE=$((64 * 1024 * 1024))   # VEL_HOST_POOL_SIZE
SEED="${SEED:-1}"
FUZZ="${FUZZ:-200}"

PASSED=0
FAILED=0

pass() { PASSED=$((PASSED + 1)); echo "  ok    $*"; }
fail() { FAILED=$((FAILED + 1)); echo "  FAIL  $*"; }
die()  { echo "ERROR: $*" >&2; exit 2; }

# ---------------------------------------------------------------- setup ----

lsmod | grep -q '^velocitor ' || insmod "$MODULE" || die "insmod failed"

# One directory per bound device; there is only ever one here.
DBG=""
for d in /sys/kernel/debug/velocitor/*/; do
    [ -d "$d" ] && DBG="${d%/}"
done
[ -n "$DBG" ] || die "no velocitor debugfs directory -- did probe fail?"
[ -e "$DBG/dma_ctrl" ] || die "$DBG/dma_ctrl missing"

# The tracepoint is the only place the device-side error code surfaces, so a
# failing run leaves something to read.
if [ -w "$TRACING/events/velocitor/velocitor_dma_dbg/enable" ]; then
    echo 1 > "$TRACING/events/velocitor/velocitor_dma_dbg/enable"
    : > "$TRACING/trace"
    HAVE_TRACE=1
else
    echo "note: tracefs not available, running without the trace"
    HAVE_TRACE=0
fi

# ------------------------------------------------------------- helpers ----

# One line, one transfer.  The errno of the write is the errno of the
# transfer, so the exit status is the whole verdict.
dma() { echo "$1 $2 $3 $4" > "$DBG/dma_ctrl" 2>/dev/null; }

# The pool is plain host memory: these two touch no device at all.
pool_load() { dd if="$1" of="$DBG/dma_pool" bs=4096 seek="$2" conv=notrunc 2>/dev/null; }
pool_save() { dd if="$DBG/dma_pool" of="$3" bs=4096 skip="$1" count="$2" 2>/dev/null; }

counter() { awk -v k="$1" '$1 == k { print $2 }' "$DBG/counters"; }

echo "velocitor bring-up DMA test -- $DBG"

# ---------------------------------------------------- 1. the pool alone ----
#
# Before blaming the DMA for anything, prove that the buffer behind dma_pool
# is a plain piece of memory that keeps what it is given.

dd if=/dev/urandom of=/tmp/pattern bs=4096 count=1 2>/dev/null
pool_load /tmp/pattern 0
pool_save 0 1 /tmp/pool-back
if cmp -s /tmp/pattern /tmp/pool-back; then
    pass "dma_pool round trip, no device involved"
else
    fail "dma_pool did not keep what it was given"
fi

# --------------------------------------------------------- 2. D2H first ----
#
# The device memory still holds the reset pattern, so this reads a value the
# driver could not have produced.  Word k of the transfer must equal the
# device offset it came from.

if dma d2h 0x2000 0x8000 64; then
    WORDS=$(dd if="$DBG/dma_pool" bs=1 skip=$((0x8000)) count=16 2>/dev/null | od -An -tx4)
    W0=$(echo "$WORDS" | awk '{ print $1 }')
    W1=$(echo "$WORDS" | awk '{ print $2 }')
    if [ "$W0" = "00002000" ] && [ "$W1" = "00002004" ]; then
        pass "D2H against the reset pattern (words $W0 $W1)"
    else
        fail "D2H returned $W0 $W1, expected 00002000 00002004"
    fi
else
    fail "D2H of 64 bytes at 0x2000 was refused"
fi

# --------------------------------------------------------- 3. H2D, then ----
#
# Push the random pattern out and pull it back to a *different* pool offset,
# so the comparison cannot succeed by reading the bytes we started from.

if dma h2d 0x100000 0 4096 && dma d2h 0x100000 0x10000 4096; then
    pool_save 16 1 /tmp/dma-back
    if cmp -s /tmp/pattern /tmp/dma-back; then
        pass "H2D then D2H round trip, 4096 bytes at 0x100000"
    else
        fail "round trip came back altered"
    fi
else
    fail "round trip transfer was refused"
fi

# ------------------------------------------------- 4. device-side bound ----
#
# The driver passes the device offset through untouched, so this must be
# refused by the device and counted as a range error (section 4.5).

BEFORE=$(counter err_range)
if dma h2d $((MEM_SIZE - 16)) 0 4096; then
    fail "transfer past the end of device memory was accepted"
else
    AFTER=$(counter err_range)
    if [ "$AFTER" -gt "$BEFORE" ]; then
        pass "device refused an out-of-range offset, err_range $BEFORE -> $AFTER"
    else
        fail "transfer refused but err_range did not move ($BEFORE)"
    fi
fi

# --------------------------------------------------- 5. driver-side bound ----
#
# The pool bound is the one check the driver owes: the device can reach
# nothing else.  It must be caught before any register is written, so no
# counter moves at all.

BEFORE=$(counter dma_rd)
if dma h2d 0 $((POOL_SIZE - 16)) 4096; then
    fail "transfer past the end of the pool was accepted"
elif [ "$(counter dma_rd)" = "$BEFORE" ]; then
    pass "driver refused a pool overrun without touching the device"
else
    fail "pool overrun reached the device"
fi

# ---------------------------------------------------------- 6. fuzzing ----
#
# Random parameters, with one in five deliberately out of range.  The rule
# being checked is not that a given transfer succeeds, but that the verdict
# always matches the bounds -- and that the driver is still alive after.

if [ "$FUZZ" -gt 0 ]; then
    echo "  fuzzing $FUZZ transfers, seed $SEED"

    awk -v seed="$SEED" -v n="$FUZZ" -v mem="$MEM_SIZE" -v pool="$POOL_SIZE" '
    BEGIN {
        srand(seed);
        for (i = 0; i < n; i++) {
            dir  = (rand() < 0.5) ? "h2d" : "d2h";
            len  = int(rand() * 8192);
            # One in five walks off the end on purpose, on either side.
            off  = (rand() < 0.2) ? int(rand() * 2 * mem)  : int(rand() * (mem  - 8192));
            poff = (rand() < 0.2) ? int(rand() * 2 * pool) : int(rand() * (pool - 8192));
            ok   = (off + len <= mem && poff + len <= pool) ? 1 : 0;
            print dir, off, poff, len, ok;
        }
    }' > /tmp/fuzz-cases

    FUZZ_BAD=0
    while read -r DIR OFF POFF LEN EXPECT; do
        if dma "$DIR" "$OFF" "$POFF" "$LEN"; then
            GOT=1
        else
            GOT=0
        fi
        if [ "$GOT" != "$EXPECT" ]; then
            FUZZ_BAD=$((FUZZ_BAD + 1))
            echo "        $DIR off=$OFF poff=$POFF len=$LEN: expected $EXPECT, got $GOT"
        fi
    done < /tmp/fuzz-cases

    if [ "$FUZZ_BAD" -eq 0 ]; then
        pass "$FUZZ fuzzed transfers, every verdict matched the bounds"
    else
        fail "$FUZZ_BAD of $FUZZ fuzzed transfers disagreed (replay with SEED=$SEED)"
    fi

    # Still breathing?  A valid transfer after the storm is the cheapest
    # proof that nothing was left half-programmed.
    if dma d2h 0x3000 0 64; then
        pass "device still answers after fuzzing"
    else
        fail "device stopped answering after fuzzing"
    fi
fi

# ------------------------------------------------------- 7. the kernel ----
#
# A test that only reads its own return codes would miss a splat entirely.

SPLATS=$(dmesg | grep -c -E 'BUG:|Oops|WARNING:|call trace' || true)
if [ "$SPLATS" -eq 0 ]; then
    pass "no kernel splat in dmesg"
else
    fail "$SPLATS kernel splat(s) in dmesg"
    dmesg | grep -E -A 20 'BUG:|Oops|WARNING:'
fi

# --------------------------------------------------------------- verdict ----

echo
echo "passed $PASSED, failed $FAILED"

if [ "$FAILED" -ne 0 ] && [ "$HAVE_TRACE" -eq 1 ]; then
    echo
    echo "--- last trace entries ---"
    grep velocitor_dma_dbg "$TRACING/trace" | tail -20
fi

[ "$FAILED" -eq 0 ]
