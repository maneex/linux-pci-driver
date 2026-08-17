#!/usr/bin/env bash

# devtools/build-firmware.sh -- build the remoteproc firmware image.
#
# Wraps firmware/Makefile.  Unlike the module and the QEMU model, this builds
# with the *host* compiler and against nothing at all: the image is generated
# byte by byte, so there is no toolchain for a processor that does not exist
# to install.  See firmware/mkfw.c.
#
# The guest finds the result through the 9p share; devtools/initramfs/init
# links it into /lib/firmware so that request_firmware() can see it.
#
# Any extra argument is forwarded to make, so `build-firmware.sh clean` and
# `build-firmware.sh check` both work.

set -euo pipefail

DEVTOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$DEVTOOLS_DIR/.." && pwd)"
. "$DEVTOOLS_DIR/config.defaults"
[ -f "$DEVTOOLS_DIR/config.local" ] && . "$DEVTOOLS_DIR/config.local"

die() { echo "ERROR: $*" >&2; exit 1; }

[ -f "$FIRMWARE_DIR/Makefile" ] || die "No Makefile in $FIRMWARE_DIR."

make -C "$FIRMWARE_DIR" "$@"
