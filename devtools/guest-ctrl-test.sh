#!/bin/sh

# devtools/guest-ctrl-test.sh -- layer 2 of spec section 13.1 for the control
# plane of section 7: ALLOC, FREE, STAT and the generation, driven from an
# application through /dev/velocitor.
#
# This runs INSIDE the guest, under busybox ash -- not bash.  Launch it with:
#
#   devtools/boot.sh --test /mnt/velocitor/devtools/guest-ctrl-test.sh
#
# The checks that need an ioctl live in runtime/ctrl-test.c, in one process:
# section 10.2 allows a single open at a time, and the crash test needs a
# handle to outlive a firmware generation.  What is left here is the
# environment -- loading the module, and reading the kernel's own opinion of
# what just happened, which a test that only read its own return codes would
# miss entirely.

set -u

MODULE=/mnt/velocitor/module/velocitor.ko
CTRL_TEST=/mnt/velocitor/runtime/ctrl-test

die() { echo "ERROR: $*" >&2; exit 2; }

lsmod | grep -q '^velocitor ' || insmod "$MODULE" || die "insmod failed"

[ -x "$CTRL_TEST" ] || die "$CTRL_TEST missing -- run devtools/build-runtime.sh"

"$CTRL_TEST"
FAILED=$?

# The recovery of section 6.5 is meant to be orderly: remoteproc says so out
# loud, and a crash that left a splat behind would still let every check above
# pass.
echo
SPLATS=$(dmesg | grep -c -E 'BUG:|Oops|WARNING:|call trace' || true)
if [ "$SPLATS" -eq 0 ]; then
    echo "  ok    no kernel splat in dmesg"
else
    echo "  FAIL  $SPLATS kernel splat(s) in dmesg"
    dmesg | grep -E -A 20 'BUG:|Oops|WARNING:'
    FAILED=$((FAILED + 1))
fi

RECOVERED=$(dmesg | grep -c 'remote processor velocitor is now up' || true)
if [ "$RECOVERED" -ge 2 ]; then
    echo "  ok    remoteproc brought the firmware back up"
else
    echo "  FAIL  firmware came up $RECOVERED time(s), expected at least 2"
    FAILED=$((FAILED + 1))
fi

echo
[ "$FAILED" -eq 0 ] && echo "guest-ctrl-test: PASS" || echo "guest-ctrl-test: FAIL"
[ "$FAILED" -eq 0 ]
