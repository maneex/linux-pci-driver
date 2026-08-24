#!/usr/bin/env bash

# devtools/build-runtime.sh -- build libvelocitor and the tools above it.
#
# Wraps runtime/Makefile.  Host compiler, like the firmware generator and
# unlike the module: this is userland, and the guest runs the very same x86-64
# binaries through the 9p share.
#
# Two artefacts, for two reasons.  libvelocitor.so is the deliverable of spec
# section 10.3 -- a C core, so that the measurements of section 12 stay
# scriptable from anything able to bind a C ABI.  libvelocitor.a is what the
# guest links, because its initramfs has no dynamic loader at all: busybox
# itself is static, and a dynamically linked binary simply will not start
# there.
#
# Any extra argument is forwarded to make, so `build-runtime.sh clean` works.

set -euo pipefail

DEVTOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$DEVTOOLS_DIR/.." && pwd)"
. "$DEVTOOLS_DIR/config.defaults"
[ -f "$DEVTOOLS_DIR/config.local" ] && . "$DEVTOOLS_DIR/config.local"

die() { echo "ERROR: $*" >&2; exit 1; }

[ -f "$RUNTIME_DIR/Makefile" ] || die "No Makefile in $RUNTIME_DIR."

# The library is built against the frozen contract of spec 10.2 and the device
# constants of section 2.  Neither is optional, and a missing one fails here
# with a name rather than a hundred lines of preprocessor errors.
[ -f "$UAPI_DIR/velocitor.h" ] || die "No UAPI header in $UAPI_DIR."
[ -f "$DEVICE_DIR/velocitor_hw.h" ] || die "No shared header in $DEVICE_DIR."

echo "Building $RUNTIME_DIR with the host compiler ..."
make -C "$RUNTIME_DIR" "$@"
