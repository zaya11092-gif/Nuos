#!/usr/bin/env bash
# Builds a patched Monado with the nuos_posed null-HMD driver.
# Best-effort: Monado master moves, so this may need patch adjustments
# (see patches/README.md). Requires: git, cmake, ninja, meson deps of Monado.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../.."           # repo root (Nuos)
MONADO_DIR="${MONADO_DIR:-$ROOT/build/monado-src}"
BUILD_DIR="${MONADO_BUILD_DIR:-$MONADO_DIR/build}"
DRIVER_SRC="$HERE/nuos_posed.c"

mkdir -p "$(dirname "$MONADO_DIR")"
if [ ! -d "$MONADO_DIR/.git" ]; then
    git clone --depth 1 https://gitlab.freedesktop.org/monado/monado.git "$MONADO_DIR"
fi

# 1) copy driver into Monado source tree
DEST="$MONADO_DIR/src/xrt/drivers/nuos_posed"
mkdir -p "$DEST"
cp "$DRIVER_SRC" "$DEST/"
# spine headers the driver needs
cp "$ROOT/proto/nuos_proto.h" "$DEST/" 2>/dev/null || true
cp "$ROOT/platform/include/nuos_shm.h" "$DEST/"

# 2) apply the small drivers/CMakeLists.txt patch (idempotent)
cd "$MONADO_DIR"
if ! grep -q "drv_nuos_posed" src/xrt/drivers/CMakeLists.txt; then
    git apply --whitespace=nowarn "$HERE/patches/nuos_posed-drivers-cmake.patch" \
        || { echo "PATCH FAILED: adjust patches/ for current Monado master" >&2; exit 1; }
fi

# 3) configure + build
cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DXRT_BUILD_DRIVER_NUOS_POSED=ON \
    -DXRT_FEATURE_SERVICE=ON
cmake --build "$BUILD_DIR"

echo "OK: Monado built with nuos_posed driver in $BUILD_DIR"
