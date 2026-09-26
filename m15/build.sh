#!/bin/sh
# m15/build.sh -- Build script for Milestone 15 PHYSICS_ACCELERATOR_LINK host tools
set -eu

M15_DIR=$(cd "$(dirname "$0")" && pwd -P)
ROOT_DIR=$(cd "$M15_DIR/.." && pwd -P)
OUT_DIR="$M15_DIR/build"

CC=${CC:-gcc}
CFLAGS="-O2 -std=c11 -Wall -Wextra -Werror -I$M15_DIR -I$ROOT_DIR"

mkdir -p "$OUT_DIR"

echo "[*] Building m15tool..."
"$CC" $CFLAGS -o "$OUT_DIR/m15tool" \
    "$M15_DIR/tools/m15tool.c" \
    "$M15_DIR/physics_accel.c" \
    "$M15_DIR/physics_accel_native.c" \
    "$ROOT_DIR/sha256_clean.c"

echo "[*] Build successful: $OUT_DIR/m15tool"
