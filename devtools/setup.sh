#!/usr/bin/env bash

# devtools/setup.sh -- one-time setup of the guest side.
# Downloads the kernel source and busybox, builds both, creates an initramfs.
# Each phase is idempotent: skipped when its output exists AND the config it
# depends on has not changed.
#
# Adapted from sysprog21/lkmpg, devtools/setup.sh -- see README.md.
# The prebuilt-tarball shortcut of the original is removed: it downloads a
# kernel built with lkmpg's config fragment, which is not ours.

set -euo pipefail

DEVTOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$DEVTOOLS_DIR/.." && pwd)"
. "$DEVTOOLS_DIR/config.defaults"
[ -f "$DEVTOOLS_DIR/config.local" ] && . "$DEVTOOLS_DIR/config.local"

NPROC=$(nproc 2>/dev/null || echo 4)

die() { echo "ERROR: $*" >&2; exit 1; }

# Short hash of the given files, to detect config changes.
stamp_hash() {
    cat "$@" 2>/dev/null | sha256sum | cut -c1-16
}

stamp_fresh() {
    local stamp_file="$1" expected="$2"
    [ -f "$stamp_file" ] && [ "$(cat "$stamp_file")" = "$expected" ]
}

check_host() {
    [ "$(uname)" = "Linux" ] || die "The kernel and busybox must be built on Linux."
}

# --- Phase 1: kernel source ---
# Always needed: kbuild reaches the source tree through the build directory's
# "source" symlink, even for out-of-tree module builds.
download_kernel() {
    if [ -d "$KERNEL_SRC" ]; then
        echo "[1/3] Kernel source already at $KERNEL_SRC (skipping download)"
        return
    fi

    echo "[1/3] Downloading kernel $KERNEL_VERSION ..."
    mkdir -p "$CACHE_DIR"

    local tarball_xz="$CACHE_DIR/linux-${KERNEL_VERSION}.tar.xz"
    local tarball_gz="$CACHE_DIR/linux-${KERNEL_VERSION}.tar.gz"

    if [ -f "$tarball_xz" ]; then
        echo "     Using cached tarball: $tarball_xz"
        tar -xf "$tarball_xz" -C "$CACHE_DIR"
    elif [ -f "$tarball_gz" ]; then
        echo "     Using cached tarball: $tarball_gz"
        tar -xzf "$tarball_gz" -C "$CACHE_DIR"
    elif curl -fL --progress-bar -o "$tarball_xz" "$KERNEL_URL_PRIMARY"; then
        echo "     Extracting (kernel.org) ..."
        tar -xf "$tarball_xz" -C "$CACHE_DIR"
    elif curl -fL --progress-bar -o "$tarball_gz" "$KERNEL_URL_GITHUB"; then
        echo "     Extracting (GitHub mirror) ..."
        tar -xzf "$tarball_gz" -C "$CACHE_DIR"
        # GitHub archives extract to linux-v$VERSION
        if [ -d "$CACHE_DIR/linux-v${KERNEL_VERSION}" ] && [ ! -d "$KERNEL_SRC" ]; then
            mv "$CACHE_DIR/linux-v${KERNEL_VERSION}" "$KERNEL_SRC"
        fi
    else
        die "Failed to download the kernel source from kernel.org and GitHub."
    fi

    [ -d "$KERNEL_SRC" ] || die "$KERNEL_SRC missing after extraction."
    echo "     Done: $KERNEL_SRC"
}

# --- Phase 2: kernel build ---
build_kernel() {
    local bzimage="$KERNEL_BUILD/arch/x86/boot/bzImage"
    local stamp="$KERNEL_BUILD/.config-stamp"
    local expected
    expected=$(stamp_hash "$DEVTOOLS_DIR/kernel.config" "$DEVTOOLS_DIR/config.defaults")

    if [ -f "$bzimage" ] && stamp_fresh "$stamp" "$expected"; then
        echo "[2/3] Kernel already built (config unchanged, skipping)"
        ln -sfn "$KERNEL_SRC" "$KERNEL_BUILD/source"
        return
    fi

    for cmd in make gcc bc flex bison; do
        command -v "$cmd" >/dev/null 2>&1 || \
            die "$cmd not found. apt install build-essential bc flex bison libelf-dev libssl-dev"
    done

    if [ -f "$bzimage" ]; then
        echo "[2/3] Kernel config changed, rebuilding ..."
    else
        echo "[2/3] Building kernel $KERNEL_VERSION from source (~15 min) ..."
    fi

    mkdir -p "$KERNEL_BUILD"

    # defconfig, then merge our fragment.  merge_config.sh runs a bare `make`
    # internally, so it has to execute from the source directory.
    make -C "$KERNEL_SRC" O="$KERNEL_BUILD" defconfig
    (cd "$KERNEL_SRC" && \
        scripts/kconfig/merge_config.sh \
            -O "$KERNEL_BUILD" \
            "$KERNEL_BUILD/.config" \
            "$DEVTOOLS_DIR/kernel.config")

    # `make bzImage` produces vmlinux.symvers during the vmlinux link;
    # `modules_prepare` sets up the generated headers and host tools that
    # out-of-tree module builds need.  Copying vmlinux.symvers over
    # Module.symvers lets those builds resolve vmlinux symbols without
    # building every in-tree module.
    #
    # -std=gnu11 is pinned because GCC 15+ defaults to C23, where bool/true/
    # false are keywords that clash with the kernel's own typedefs; it goes
    # through three variables because several x86 sub-Makefiles rebuild
    # KBUILD_CFLAGS with := and drop the top-level setting.  -Wno-error
    # demotes CONFIG_WERROR, which newer GCC trips on for deliberate kernel
    # patterns.
    local stdflag="-std=gnu11 -Wno-error"
    make -C "$KERNEL_SRC" O="$KERNEL_BUILD" -j"$NPROC" \
        KCFLAGS="$stdflag" KCPPFLAGS="$stdflag" HOSTCFLAGS="$stdflag" bzImage
    make -C "$KERNEL_SRC" O="$KERNEL_BUILD" -j"$NPROC" \
        KCFLAGS="$stdflag" KCPPFLAGS="$stdflag" HOSTCFLAGS="$stdflag" modules_prepare
    cp "$KERNEL_BUILD/vmlinux.symvers" "$KERNEL_BUILD/Module.symvers"

    echo "$expected" > "$stamp"
    echo "     Done: $bzimage"
}

# --- Phase 3: busybox and initramfs ---
build_busybox() {
    if [ -f "$BUSYBOX_SRC/busybox" ]; then
        echo "     Busybox already built (skipping)"
        return
    fi

    local tarball="$CACHE_DIR/busybox-${BUSYBOX_VERSION}.tar.bz2"
    if [ ! -d "$BUSYBOX_SRC" ]; then
        echo "     Downloading busybox $BUSYBOX_VERSION ..."
        [ -f "$tarball" ] || curl -L --progress-bar -o "$tarball" "$BUSYBOX_URL"
        tar -xf "$tarball" -C "$CACHE_DIR"
    fi

    echo "     Building busybox (static) ..."
    make -C "$BUSYBOX_SRC" defconfig
    # Static for the initramfs; tc is disabled because it fails to build
    # against kernel headers that dropped the CBQ structures.
    sed -i -e 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' \
           -e 's/CONFIG_TC=y/# CONFIG_TC is not set/' \
        "$BUSYBOX_SRC/.config"
    make -C "$BUSYBOX_SRC" -j"$NPROC"
}

build_initramfs() {
    local stamp="$CACHE_DIR/.initramfs-stamp"
    local expected
    expected=$(stamp_hash "$DEVTOOLS_DIR/initramfs/init")

    if [ -f "$INITRAMFS_CPIO" ] && stamp_fresh "$stamp" "$expected"; then
        echo "[3/3] Initramfs already built (init unchanged, skipping)"
        return
    fi

    if [ -f "$INITRAMFS_CPIO" ]; then
        echo "[3/3] init changed, rebuilding initramfs ..."
    else
        echo "[3/3] Creating initramfs ..."
    fi

    build_busybox

    rm -rf "$INITRAMFS_DIR"
    mkdir -p "$INITRAMFS_DIR"
    make -C "$BUSYBOX_SRC" CONFIG_PREFIX="$INITRAMFS_DIR" install

    mkdir -p "$INITRAMFS_DIR"/{proc,sys,dev,tmp,etc}
    mkdir -p "$INITRAMFS_DIR$GUEST_SHARE_DIR"

    cp "$DEVTOOLS_DIR/initramfs/init" "$INITRAMFS_DIR/init"
    chmod +x "$INITRAMFS_DIR/init"

    (cd "$INITRAMFS_DIR" && find . | cpio -o -H newc 2>/dev/null | gzip > "$INITRAMFS_CPIO")

    echo "$expected" > "$stamp"
    echo "     Done: $INITRAMFS_CPIO ($(du -h "$INITRAMFS_CPIO" | cut -f1))"
}

check_host
download_kernel
build_kernel
build_initramfs

echo ""
echo "Guest side ready. Next:"
echo "  devtools/build-qemu.sh       # QEMU $QEMU_VERSION with the velocitor device"
echo "  devtools/build-module.sh     # build module/ against this kernel"
echo "  devtools/boot.sh             # boot (interactive)"
