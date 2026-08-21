#!/usr/bin/env bash

# devtools/build-module.sh -- build module/ against the guest kernel.
#
# Wraps module/Makefile with the right KDIR; it does not modify anything in
# module/ beyond the object files kbuild produces there.
#
# Adapted from sysprog21/lkmpg, devtools/build-modules.sh -- see README.md.
#
# Any extra argument is forwarded to make, so `build-module.sh clean` works.
# --sparse adds the static checks the spec's annex A.1 relies on.

set -euo pipefail

DEVTOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$DEVTOOLS_DIR/.." && pwd)"
. "$DEVTOOLS_DIR/config.defaults"
[ -f "$DEVTOOLS_DIR/config.local" ] && . "$DEVTOOLS_DIR/config.local"

die() { echo "ERROR: $*" >&2; exit 1; }

# C=2 rather than C=1: kbuild's C=1 only checks the files it recompiles, and
# the module is normally already built, so C=1 would check nothing at all.
SPARSE_ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --sparse) SPARSE_ARGS=(C=2); shift ;;
        -h|--help)
            echo "Usage: $0 [--sparse] [MAKE_ARGS...]"
            exit 0 ;;
        *) break ;;
    esac
done

# Outside sparse, __bitwise expands to nothing and __le32 is plain __u32 --
# gcc cannot see a single one of the conversions annex A.1 asks for. sparse is
# what makes that contract executable instead of declarative.
if [ ${#SPARSE_ARGS[@]} -gt 0 ]; then
    command -v sparse >/dev/null 2>&1 || \
        die "sparse not found. Install it: apt install sparse"
fi

[ -d "$KERNEL_BUILD" ] || die "Kernel not built. Run devtools/setup.sh first."
[ -f "$KERNEL_BUILD/Module.symvers" ] || \
    die "Kernel not prepared for out-of-tree modules. Run devtools/setup.sh first."
[ -f "$MODULE_DIR/Makefile" ] || die "No Makefile in $MODULE_DIR."

echo "Building $MODULE_DIR against kernel $KERNEL_VERSION ..."
make -C "$MODULE_DIR" KDIR="$KERNEL_BUILD" "${SPARSE_ARGS[@]}" "$@"
