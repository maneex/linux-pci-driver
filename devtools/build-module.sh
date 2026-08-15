#!/usr/bin/env bash

# devtools/build-module.sh -- build module/ against the guest kernel.
#
# Wraps module/Makefile with the right KDIR; it does not modify anything in
# module/ beyond the object files kbuild produces there.
#
# Adapted from sysprog21/lkmpg, devtools/build-modules.sh -- see README.md.
#
# Any extra argument is forwarded to make, so `build-module.sh clean` works.

set -euo pipefail

DEVTOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$DEVTOOLS_DIR/.." && pwd)"
. "$DEVTOOLS_DIR/config.defaults"
[ -f "$DEVTOOLS_DIR/config.local" ] && . "$DEVTOOLS_DIR/config.local"

die() { echo "ERROR: $*" >&2; exit 1; }

[ -d "$KERNEL_BUILD" ] || die "Kernel not built. Run devtools/setup.sh first."
[ -f "$KERNEL_BUILD/Module.symvers" ] || \
    die "Kernel not prepared for out-of-tree modules. Run devtools/setup.sh first."
[ -f "$MODULE_DIR/Makefile" ] || die "No Makefile in $MODULE_DIR."

echo "Building $MODULE_DIR against kernel $KERNEL_VERSION ..."
make -C "$MODULE_DIR" KDIR="$KERNEL_BUILD" "$@"
