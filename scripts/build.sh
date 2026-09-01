#!/usr/bin/env bash
# Build Stems module for Schwung
#
# The separation engine ships pre-built, but the wavchunk helper (which caps the
# engine's peak RSS by splitting long inputs) is compiled here for ARM64. Use
# the host cross-compiler when one is on PATH, otherwise Docker.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
MODULE_ID="stems"

cd "$REPO_ROOT"

echo "=== Building Stems Module ==="

# --- wavchunk (ARM64) ---
: "${CROSS_PREFIX:=aarch64-linux-gnu-}"
CFLAGS="-O2 -Wall -Wextra -Werror -static-libgcc"
mkdir -p build

if command -v "${CROSS_PREFIX}gcc" >/dev/null 2>&1; then
    echo "Compiling wavchunk with ${CROSS_PREFIX}gcc..."
    "${CROSS_PREFIX}gcc" $CFLAGS -o build/wavchunk src/tools/wavchunk.c -lm
elif command -v docker >/dev/null 2>&1; then
    echo "Compiling wavchunk via Docker..."
    docker run --rm -v "$REPO_ROOT:/build" -w /build debian:bookworm sh -c "
        set -e
        apt-get update -qq && apt-get install -y -qq gcc-aarch64-linux-gnu >/dev/null
        aarch64-linux-gnu-gcc $CFLAGS -o build/wavchunk src/tools/wavchunk.c -lm
    "
else
    echo "Error: need ${CROSS_PREFIX}gcc or docker to build wavchunk for ARM64." >&2
    exit 1
fi

# A helper that silently failed to build would leave the module falling back to
# whatever stale copy is already on the device, which defeats any later bisect.
if [ ! -x build/wavchunk ]; then
    echo "Error: build/wavchunk was not produced." >&2
    exit 1
fi
if ! file build/wavchunk | grep -q 'ARM aarch64'; then
    echo "Error: build/wavchunk is not an ARM64 binary:" >&2
    file build/wavchunk >&2
    exit 1
fi
echo "wavchunk: $(file -b build/wavchunk | cut -d, -f1-2)"

# --- package ---
rm -rf "dist/$MODULE_ID"
mkdir -p "dist/$MODULE_ID/engine"

echo "Packaging..."
cp src/module.json "dist/$MODULE_ID/"
[ -f src/help.json ] && cp src/help.json "dist/$MODULE_ID/"
cp src/separate "dist/$MODULE_ID/"
cp build/wavchunk "dist/$MODULE_ID/"
chmod +x "dist/$MODULE_ID/separate" "dist/$MODULE_ID/wavchunk"

cp src/engine/spleeter "dist/$MODULE_ID/engine/"
cp src/engine/libgfortran.so.5 "dist/$MODULE_ID/engine/"
cp src/engine/libopenblas.so.0 "dist/$MODULE_ID/engine/"
chmod +x "dist/$MODULE_ID/engine/spleeter"

cd dist
tar -czf "$MODULE_ID-module.tar.gz" "$MODULE_ID/"
cd ..

# The tarball is what actually reaches devices, so assert the helper is in it.
if ! tar -tzf "dist/$MODULE_ID-module.tar.gz" | grep -q "^$MODULE_ID/wavchunk$"; then
    echo "Error: wavchunk missing from the release tarball." >&2
    exit 1
fi

echo ""
echo "=== Build Complete ==="
echo "Output: dist/$MODULE_ID/"
echo "Tarball: dist/$MODULE_ID-module.tar.gz"
