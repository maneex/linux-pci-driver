#!/usr/bin/env bash

# devtools/build-qemu.sh -- build QEMU with the velocitor device model.
#
# Spec section C.4 pins QEMU to 7.2.22 and requires the model to be built
# *in the tree*, from the upstream tag, never against the distribution
# package: an in-tree device is compiled with QEMU, not loaded into it.
#
# So this script fetches the pinned tag, drops qemu-device/*.{c,h} into
# hw/misc/, patches the two build-glue files, and builds.  Every phase is
# idempotent; editing qemu-device/velocitor.c and re-running only re-runs
# ninja.

set -euo pipefail

DEVTOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$DEVTOOLS_DIR/.." && pwd)"
. "$DEVTOOLS_DIR/config.defaults"
[ -f "$DEVTOOLS_DIR/config.local" ] && . "$DEVTOOLS_DIR/config.local"

NPROC=$(nproc 2>/dev/null || echo 4)

die() { echo "ERROR: $*" >&2; exit 1; }

MARKER="# --- velocitor (out-of-tree project, see devtools/build-qemu.sh) ---"

check_deps() {
    local missing=()
    for cmd in curl python3 ninja pkg-config make gcc; do
        command -v "$cmd" >/dev/null 2>&1 || missing+=("$cmd")
    done
    pkg-config --exists glib-2.0 2>/dev/null || missing+=("libglib2.0-dev")
    pkg-config --exists pixman-1 2>/dev/null || missing+=("libpixman-1-dev")
    # virtfs carries the 9p share boot.sh relies on
    pkg-config --exists libcap-ng 2>/dev/null || missing+=("libcap-ng-dev")
    [ -f /usr/include/attr/xattr.h ] || missing+=("libattr1-dev")

    if [ "${#missing[@]}" -gt 0 ]; then
        die "missing build dependencies: ${missing[*]}
  apt install build-essential ninja-build pkg-config python3 python3-venv \\
              libglib2.0-dev libpixman-1-dev libcap-ng-dev libattr1-dev \\
              flex bison"
    fi
}

# --- Phase 1: QEMU source at the pinned tag ---
download_qemu() {
    if [ -d "$QEMU_SRC" ]; then
        echo "[1/4] QEMU source already at $QEMU_SRC (skipping download)"
        return
    fi

    echo "[1/4] Fetching QEMU $QEMU_VERSION ..."
    mkdir -p "$CACHE_DIR"

    local tarball="$CACHE_DIR/qemu-${QEMU_VERSION}.tar.xz"

    # The release tarball is preferred over git: it is self-contained, while
    # a git checkout pulls submodules at build time.
    if [ -f "$tarball" ]; then
        echo "     Using cached tarball: $tarball"
        tar -xf "$tarball" -C "$CACHE_DIR"
    elif curl -fL --progress-bar -o "$tarball" "$QEMU_URL_PRIMARY"; then
        echo "     Extracting ..."
        tar -xf "$tarball" -C "$CACHE_DIR"
    else
        rm -f "$tarball"
        echo "     Tarball unavailable, cloning tag v${QEMU_VERSION} ..."
        git clone --depth 1 --branch "v${QEMU_VERSION}" \
            "$QEMU_GIT_URL" "$QEMU_SRC" \
            || die "Failed to obtain QEMU $QEMU_VERSION."
    fi

    [ -f "$QEMU_SRC/configure" ] || die "$QEMU_SRC does not look like a QEMU tree."
    echo "     Done: $QEMU_SRC"
}

# --- Phase 2: install the device into the tree ---
# Copies only when the content differs, so an unchanged source does not
# retrigger a ninja rebuild.
install_source() {
    local src="$1" dst="$2"
    if cmp -s "$src" "$dst" 2>/dev/null; then
        return 1
    fi
    cp "$src" "$dst"
    echo "     Installed $(basename "$dst")"
    return 0
}

install_device() {
    echo "[2/4] Installing the device model into hw/misc/ ..."

    local misc="$QEMU_SRC/hw/misc"
    [ -d "$misc" ] || die "$misc not found."

    install_source "$DEVICE_DIR/velocitor.c"      "$misc/velocitor.c"      || true
    install_source "$DEVICE_DIR/velocitor_hw.h"   "$misc/velocitor_hw.h"   || true
    install_source "$DEVICE_DIR/velocitor_wire.h" "$misc/velocitor_wire.h" || true

    # meson glue.  The source-set variable was renamed between QEMU
    # releases (softmmu_ss -> system_ss in 8.x), so detect it rather than
    # assume: this script should survive an unpin of QEMU_VERSION.
    local meson="$misc/meson.build"
    if ! grep -q "velocitor.c" "$meson"; then
        local ss
        if grep -q '^softmmu_ss' "$meson"; then
            ss=softmmu_ss
        elif grep -q '^system_ss' "$meson"; then
            ss=system_ss
        else
            die "cannot tell which source set $meson uses."
        fi
        {
            echo ""
            echo "$MARKER"
            echo "${ss}.add(when: 'CONFIG_VELOCITOR', if_true: files('velocitor.c'))"
        } >> "$meson"
        echo "     Patched hw/misc/meson.build (${ss})"
    fi

    # Kconfig glue.  PCI_DEVICES is enabled by the pc/q35 machine configs,
    # so the device is built for x86_64-softmmu without a configure flag.
    local kconfig="$misc/Kconfig"
    if ! grep -q "config VELOCITOR" "$kconfig"; then
        {
            echo ""
            echo "$MARKER"
            echo "config VELOCITOR"
            echo "    bool"
            echo "    default y if PCI_DEVICES"
            echo "    depends on PCI && MSI_NONBROKEN"
        } >> "$kconfig"
        echo "     Patched hw/misc/Kconfig"
    fi
}

# --- Phase 3: configure ---
configure_qemu() {
    local stamp="$QEMU_BUILD/.velocitor-configure-stamp"
    local expected
    expected=$(printf '%s\n' "$QEMU_VERSION" \
                             "${QEMU_CONFIGURE_EXTRA[@]+"${QEMU_CONFIGURE_EXTRA[@]}"}" \
               | sha256sum | cut -c1-16)

    if [ -f "$QEMU_BUILD/build.ninja" ] && \
       [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$expected" ]; then
        echo "[3/4] QEMU already configured (flags unchanged, skipping)"
        return
    fi

    echo "[3/4] Configuring QEMU (x86_64-softmmu only) ..."
    mkdir -p "$QEMU_BUILD"
    (cd "$QEMU_BUILD" && "$QEMU_SRC/configure" \
        --target-list=x86_64-softmmu \
        ${QEMU_CONFIGURE_EXTRA[@]+"${QEMU_CONFIGURE_EXTRA[@]}"})

    echo "$expected" > "$stamp"
}

# --- Phase 4: build and check ---
build_qemu() {
    echo "[4/4] Building QEMU (first build ~10 min) ..."
    ninja -C "$QEMU_BUILD" -j"$NPROC"

    [ -x "$QEMU_BIN" ] || die "$QEMU_BIN was not produced."

    # The device has to be reachable by name, or boot.sh fails later with a
    # much less obvious message.
    if ! "$QEMU_BIN" -device help 2>&1 | grep -q '"velocitor"'; then
        die "velocitor is not in '$QEMU_BIN -device help'.
The Kconfig or meson patch did not take effect; inspect
  $QEMU_SRC/hw/misc/Kconfig
  $QEMU_SRC/hw/misc/meson.build"
    fi
}

check_deps
download_qemu
install_device
configure_qemu
build_qemu

echo ""
echo "QEMU ready: $QEMU_BIN"
"$QEMU_BIN" --version | head -1
echo ""
echo "Next:"
echo "  devtools/build-module.sh     # build module/ against the guest kernel"
echo "  devtools/boot.sh             # boot with -device velocitor"
