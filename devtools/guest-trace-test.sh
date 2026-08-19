#!/bin/sh

# devtools/guest-trace-test.sh -- layer 2 of spec section 13.1: the firmware
# trace ring of section 6.6, driven from the guest through the debugfs file
# the driver exposes.
#
# This runs INSIDE the guest, under busybox ash -- not bash.  Launch it with:
#
#   devtools/boot.sh --test /mnt/velocitor/devtools/guest-trace-test.sh
#
# What is worth testing here is not that entries come out -- that is visible
# by eye -- but the index arithmetic of section 6.6, which both sides
# implement separately and neither can check alone.  The model owns `head`
# and `dropped`, the driver owns `tail` and derives `skipped`.  Overrun the
# ring and the two numbers have to agree, having been computed by code that
# shares nothing but the spec.

set -u

MODULE=/mnt/velocitor/module/velocitor.ko
ENTRIES=511                       # VEL_TRACE_ENTRIES
OVERRUN=520                       # comfortably more than the ring holds

PASSED=0
FAILED=0

pass() { PASSED=$((PASSED + 1)); echo "  ok    $*"; }
fail() { FAILED=$((FAILED + 1)); echo "  FAIL  $*"; }
die()  { echo "ERROR: $*" >&2; exit 2; }

# ---------------------------------------------------------------- setup ----

lsmod | grep -q '^velocitor ' || insmod "$MODULE" || die "insmod failed"

DBG=""
for d in /sys/kernel/debug/velocitor/*/; do
    [ -d "$d" ] && DBG="${d%/}"
done
[ -n "$DBG" ] || die "no velocitor debugfs directory -- did probe fail?"
[ -e "$DBG/trace" ] || die "$DBG/trace missing"

# ------------------------------------------------------------- helpers ----

# Reading the file consumes what it returns (spec 6.6), so a check takes one
# snapshot and pulls every field it needs out of that -- never two reads.
snap() { SNAP=$(cat "$DBG/trace"); }
field() { echo "$SNAP" | awk -v k="$1" '$1 == k { print $2; exit }'; }

# One qualified error is one trace entry, and an out-of-range device offset
# is the cheapest way to produce one from the guest.  It never raises the
# error vector, so the block stays unlatched and every one of them records.
bad_dma() { echo "d2h 0x0ffffffc 0x0 16" > "$DBG/dma_ctrl" 2>/dev/null; }

# ------------------------------------------------- 1. the boot left a log ---

snap
HEAD=$(field head)
COUNT=$(field entries)

if [ "$HEAD" -gt 0 ] && [ "$COUNT" = "$HEAD" ]; then
    pass "boot left $COUNT entries, none missed"
else
    fail "expected a non-empty ring with entries == head, got head=$HEAD entries=$COUNT"
fi

if [ "$(field skipped)" = "0" ] && [ "$(field dropped)" = "0" ]; then
    pass "nothing skipped or dropped on a fresh ring"
else
    fail "fresh ring already reports losses"
fi

# --------------------------------------------- 2. reading consumes it ------

snap
if [ "$(field entries)" = "0" ] && [ "$(field cursor)" = "$HEAD" ]; then
    pass "the read advanced tail to $HEAD and left nothing behind"
else
    fail "second read returned $(field entries) entries, cursor $(field cursor)"
fi

# ------------------------------------------------------- 3. overrun --------
#
# The whole point.  The driver has read everything, so `tail` is level with
# `head`; push more than the ring holds without reading, and the oldest
# entries are overwritten.  The model counts them in `dropped`, the driver
# works out the same number as `skipped` from head - tail - VEL_TRACE_ENTRIES.

i=0
while [ "$i" -lt "$OVERRUN" ]; do
    bad_dma
    i=$((i + 1))
done

snap
SKIPPED=$(field skipped)
DROPPED=$(field dropped)
COUNT=$(field entries)

if [ "$COUNT" = "$ENTRIES" ]; then
    pass "the ring gave back its full $ENTRIES entries"
else
    fail "expected $ENTRIES entries after the overrun, got $COUNT"
fi

EXPECT=$((OVERRUN - ENTRIES))
if [ "$SKIPPED" = "$EXPECT" ]; then
    pass "driver worked out $SKIPPED lost entries"
else
    fail "driver says $SKIPPED lost, arithmetic says $EXPECT"
fi

if [ "$SKIPPED" = "$DROPPED" ]; then
    pass "model and driver agree on the loss ($DROPPED)"
else
    fail "model counted $DROPPED overwritten, driver skipped $SKIPPED"
fi

# ------------------------------------------ 4. and it resynchronised -------

snap
if [ "$(field entries)" = "0" ] && [ "$(field cursor)" = "$(field head)" ]; then
    pass "tail caught up with head after the overrun"
else
    fail "tail did not resynchronise: cursor $(field cursor), head $(field head)"
fi

# `dropped` is the model's own free-running count and no read resets it
# (spec 6.6), so it must still be there.
if [ "$(field dropped)" = "$DROPPED" ]; then
    pass "dropped survived the read, as a free-running counter must"
else
    fail "dropped went from $DROPPED to $(field dropped) across a read"
fi

echo ""
echo "passed $PASSED, failed $FAILED"
[ "$FAILED" -eq 0 ] || exit 1
