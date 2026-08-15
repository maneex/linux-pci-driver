#!/usr/bin/env bash

# devtools/boot.sh -- boot the guest with the velocitor device attached.
# The project root is shared into the guest over 9p at $GUEST_SHARE_DIR.
#
# Adapted from sysprog21/lkmpg, devtools/boot.sh -- see README.md.
#
# Usage:
#   devtools/boot.sh              # interactive shell
#   devtools/boot.sh --gdb        # wait for GDB on localhost:1234
#   devtools/boot.sh --test CMD   # run CMD in the guest, exit with its status
#   devtools/boot.sh -- EXTRA     # pass extra arguments to QEMU
#
# Getting out: Ctrl+D or `exit` in the guest shell shuts the VM down.  QEMU
# runs -nographic, which multiplexes its monitor onto the same terminal, so
# Ctrl-A X quits QEMU from any state -- including one where the guest is no
# longer listening.  Ctrl-A C switches to the monitor, Ctrl-A H lists the rest.
#
# Example, the vIOMMU configuration of spec section 14:
#   QEMU_MACHINE=q35,kernel-irqchip=split KERNEL_CMDLINE_EXTRA=intel_iommu=on \
#     devtools/boot.sh -- -device intel-iommu,intremap=on,aw-bits=48

set -euo pipefail

DEVTOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$DEVTOOLS_DIR/.." && pwd)"
. "$DEVTOOLS_DIR/config.defaults"
[ -f "$DEVTOOLS_DIR/config.local" ] && . "$DEVTOOLS_DIR/config.local"

die() { echo "ERROR: $*" >&2; exit 1; }

# Parse arguments before the expensive checks, so --help works unset up.
GDB_MODE=0
TEST_CMD=""
EXTRA_QEMU_ARGS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --gdb)  GDB_MODE=1; shift ;;
        --test) shift; TEST_CMD="${1:?--test requires a command}"; shift ;;
        --)     shift; EXTRA_QEMU_ARGS=("$@"); break ;;
        -h|--help)
            echo "Usage: $0 [--gdb] [--test CMD] [-- QEMU_ARGS...]"
            exit 0 ;;
        *)      die "Unknown option: $1" ;;
    esac
done

BZIMAGE="$KERNEL_BUILD/arch/x86/boot/bzImage"
[ -f "$BZIMAGE" ] || die "Kernel not built. Run devtools/setup.sh first."
[ -f "$INITRAMFS_CPIO" ] || die "Initramfs not built. Run devtools/setup.sh first."
[ -x "$QEMU_BIN" ] || die "$QEMU_BIN not found. Run devtools/build-qemu.sh first."

# Guest kernel command line.  cma= is a spec section 2 prerequisite; panic=
# turns a panic into "QEMU exits" rather than "the terminal is stuck".
KCMD="console=ttyS0 loglevel=7 nokaslr cma=$GUEST_CMA panic=$GUEST_PANIC_TIMEOUT"
[ -n "$KERNEL_CMDLINE_EXTRA" ] && KCMD="$KCMD $KERNEL_CMDLINE_EXTRA"
if [ -n "$TEST_CMD" ]; then
    # Base64 so the command survives kernel cmdline word splitting; init
    # decodes it.  tr strips newlines portably (GNU and BSD base64 differ).
    KCMD="$KCMD velocitor.cmd64=$(printf '%s' "$TEST_CMD" | base64 | tr -d '\n')"
fi

QEMU_ARGS=(
    -M "$QEMU_MACHINE"
    -kernel "$BZIMAGE"
    -initrd "$INITRAMFS_CPIO"
    -nographic
    -m "$QEMU_MEM"
    -smp "$QEMU_SMP"
    -no-reboot
    -device velocitor
    -virtfs "local,id=$GUEST_SHARE_TAG,path=$PROJECT_ROOT,security_model=none,mount_tag=$GUEST_SHARE_TAG"
    -append "$KCMD"
)

# KVM when available.  Harmless to skip: the model does nothing timing
# sensitive, and spec annex A.6 already says emulation proves nothing about
# real timing.
if [ -w /dev/kvm ] 2>/dev/null; then
    QEMU_ARGS+=(-enable-kvm -cpu host)
fi

if [ "$GDB_MODE" -eq 1 ]; then
    QEMU_ARGS+=(-gdb "tcp::$QEMU_GDB_PORT" -S)
    echo "QEMU waiting for GDB on localhost:$QEMU_GDB_PORT"
    if [ -f "$KERNEL_BUILD/vmlinux" ]; then
        echo "Connect with: gdb $KERNEL_BUILD/vmlinux -ex 'target remote :$QEMU_GDB_PORT'"
    else
        echo "Connect with: gdb -ex 'target remote :$QEMU_GDB_PORT'"
    fi
fi

QEMU_ARGS+=("${EXTRA_QEMU_ARGS[@]+"${EXTRA_QEMU_ARGS[@]}"}")

# In test mode the guest never reads stdin.  Redirecting from /dev/null keeps
# QEMU's -nographic serial setup from calling tcsetattr() on a real terminal,
# which would take SIGTTOU when the caller pipes stdout or wraps this script
# in timeout(1).
if [ -n "$TEST_CMD" ]; then
    exec "$QEMU_BIN" "${QEMU_ARGS[@]}" < /dev/null
else
    exec "$QEMU_BIN" "${QEMU_ARGS[@]}"
fi
