#!/usr/bin/env bash
# Build the MHGU Vita Port loader VPK on Linux/macOS with VitaSDK + Ninja.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build/vita}"
EXEFS="${MHGU_EXEFS:-}"
STATIC_SHADER_DIR="${MHGU_STATIC_SHADER_DIR:-ux0:data/mhgu-vita-port/static-shaders-current}"
PYTHON="${MHGU_PYTHON:-python3}"
KUBRIDGE="${MHGU_KUBRIDGE_LIB:-}"

if [[ -z "${VITASDK:-}" ]]; then
    echo "error: VITASDK is not set (see docs/02-dependencies.md)" >&2
    exit 1
fi
if [[ -z "$EXEFS" ]]; then
    echo "error: set MHGU_EXEFS to the locked 1.4.0 ExeFS directory" >&2
    exit 1
fi
for name in rtld main subsdk0 sdk main.npdm; do
    if [[ ! -f "$EXEFS/$name" ]]; then
        echo "error: game ExeFS is missing $name: $EXEFS" >&2
        exit 1
    fi
done
if ! "$PYTHON" -c "import sys; assert sys.version_info >= (3, 10)" 2>/dev/null; then
    echo "error: Python 3.10+ not found at $PYTHON" >&2
    exit 1
fi
if ! "$PYTHON" -c "import lz4.block" 2>/dev/null; then
    echo "error: Python package lz4 missing; run $PYTHON -m pip install -r tools/requirements.txt" >&2
    exit 1
fi

mkdir -p "$BUILD_DIR/tmp"
export TMP="$BUILD_DIR/tmp" TEMP="$BUILD_DIR/tmp"

cmake_args=(
    -S "$ROOT" -B "$BUILD_DIR" -G Ninja
    -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake"
    -DPython3_EXECUTABLE="$PYTHON"
    -DVITA_PORT_EXEFS="$EXEFS"
    -DVITA_PORT_STATIC_SHADER_PACKAGE_DIR="$STATIC_SHADER_DIR"
    -DVITA_PORT_BUILD_NATIVE_LOADER=ON
    -DVITA_PORT_NATIVE_SDK_DSO=ON
)
if [[ -n "$KUBRIDGE" ]]; then
    cmake_args+=("-DVITA_PORT_KUBRIDGE_LIBRARY=$KUBRIDGE")
fi

cmake "${cmake_args[@]}"
cmake --build "$BUILD_DIR" --target vita_port_native_loader.vpk

VPK="$BUILD_DIR/vita_port_native_loader.vpk"
echo
echo "VPK:    $VPK"
if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$VPK"
else
    shasum -a 256 "$VPK"
fi
echo "Package runtime dir: $STATIC_SHADER_DIR"
