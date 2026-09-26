#!/bin/sh
# m2_build.sh -- Python-free build system for Milestone 2 (PHYSICS_BOOT)
#
# Builds canonical artifacts and Seam 2 variants from source.
# Everything is built deterministically.
set -eu

REPO_DIR=$(cd "$(dirname "$0")" && pwd -P)
cd "$REPO_DIR"

CC=${CC:-aarch64-linux-gnu-gcc}
LD=${LD:-aarch64-linux-gnu-ld}
OBJCOPY=${OBJCOPY:-aarch64-linux-gnu-objcopy}
HOSTCC=${HOSTCC:-gcc}

TOOLS_DIR="$REPO_DIR/tools"
BUILD_DIR="$REPO_DIR/build"
M2TOOL="$TOOLS_DIR/m2tool"

# 1. Compile host m2tool if not present or newer
if [ ! -f "$M2TOOL" ] || [ "$TOOLS_DIR/m2tool.c" -nt "$M2TOOL" ] || [ "$REPO_DIR/sha256_clean.c" -nt "$M2TOOL" ]; then
  echo "[*] Compiling m2tool host utility..."
  "$HOSTCC" -O2 -std=c11 -Wall -Wextra -Werror -o "$M2TOOL" "$TOOLS_DIR/m2tool.c" "$REPO_DIR/sha256_clean.c"
fi

# 2. Build canonical artifacts into build/canonical
mkdir -p "$BUILD_DIR/canonical"
CANON="$BUILD_DIR/canonical"

# Compile sha256_clean.o for target
"$CC" -nostdlib -ffreestanding -mgeneral-regs-only -O2 -c "$REPO_DIR/sha256_clean.c" -o "$CANON/sha256_clean.o"

# Assemble canonical physics modules
for s in physics.s vector_table.s memory_alloc.s capability.s; do
  o="$CANON/$(basename "$s" .s).o"
  "$CC" -c -nostdlib -ffreestanding "$REPO_DIR/$s" -o "$o"
done

# Link canonical physics.elf and extract binary
"$LD" -T "$REPO_DIR/physics.ld" \
  "$CANON/physics.o" "$CANON/vector_table.o" "$CANON/memory_alloc.o" "$CANON/capability.o" \
  -o "$CANON/physics.elf"
"$OBJCOPY" -O binary "$CANON/physics.elf" "$CANON/physics.bin"

# Generate pin
"$M2TOOL" pin-inc "$CANON/physics.bin" "$CANON/physics_pin.inc"

# Assemble and link atlas_m2
"$CC" -c -nostdlib -ffreestanding -I"$CANON" "$REPO_DIR/atlas_m2.s" -o "$CANON/atlas_m2.o"
"$LD" -Ttext=0x00000000 "$CANON/atlas_m2.o" "$CANON/sha256_clean.o" -o "$CANON/atlas_m2.elf"
"$OBJCOPY" -O binary "$CANON/atlas_m2.elf" "$CANON/atlas_m2.bin"

# 3. Helper functions for Seam 2 variants
build_variant() {
  name="$1"
  v_dir="$BUILD_DIR/seam2/$name"
  mkdir -p "$v_dir"
  "$M2TOOL" gen-snippet "$name" "$REPO_DIR/physics.s" "$v_dir/physics_$name.s"
  "$CC" -c -nostdlib -ffreestanding "$v_dir/physics_$name.s" -o "$v_dir/physics.o"
  "$CC" -c -nostdlib -ffreestanding "$REPO_DIR/vector_table.s" -o "$v_dir/vector_table.o"
  "$CC" -c -nostdlib -ffreestanding "$REPO_DIR/memory_alloc.s" -o "$v_dir/memory_alloc.o"
  "$CC" -c -nostdlib -ffreestanding "$REPO_DIR/capability.s" -o "$v_dir/capability.o"

  "$LD" -T "$REPO_DIR/physics.ld" \
    "$v_dir/physics.o" "$v_dir/vector_table.o" "$v_dir/memory_alloc.o" "$v_dir/capability.o" \
    -o "$v_dir/physics.elf"
  "$OBJCOPY" -O binary "$v_dir/physics.elf" "$v_dir/physics.bin"

  "$M2TOOL" pin-inc "$v_dir/physics.bin" "$v_dir/physics_pin.inc"
  (cd "$v_dir" && "$CC" -c -nostdlib -ffreestanding "$REPO_DIR/atlas_m2.s" -o atlas_m2.o)
  "$LD" -Ttext=0x00000000 "$v_dir/atlas_m2.o" "$CANON/sha256_clean.o" -o "$v_dir/atlas_m2.elf"
  "$OBJCOPY" -O binary "$v_dir/atlas_m2.elf" "$v_dir/atlas_m2.bin"
}

build_hostile() {
  tag="$1"
  defsym="$2"
  h_dir="$BUILD_DIR/seam2/hostile_$tag"
  mkdir -p "$h_dir"
  cp "$CANON/physics_pin.inc" "$h_dir/"
  (cd "$h_dir" && "$CC" -c -nostdlib -ffreestanding -Wa,--defsym,"$defsym" "$REPO_DIR/atlas_m2.s" -o atlas_m2.o)
  "$LD" -Ttext=0x00000000 "$h_dir/atlas_m2.o" "$CANON/sha256_clean.o" -o "$h_dir/atlas_m2.elf"
  "$OBJCOPY" -O binary "$h_dir/atlas_m2.elf" "$h_dir/atlas_m2.bin"
}

echo "m2_build: built canonical artifacts -> $CANON"
