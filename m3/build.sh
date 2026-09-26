#!/bin/sh
# m3/build.sh -- Python-free build system for PHYSICS M3 image and qualification tools.
#
# Outputs under m3/build/ (gitignored). Never touches repo-root M2 files.
# sha256_clean.c is compiled from repo root (audited source, not forked).
# Deterministic: no timestamps, --build-id=none, prefix mapped to "."
set -eu

M3=$(cd "$(dirname "$0")" && pwd -P)
ROOT=$(cd "$M3/.." && pwd -P)
OUT=$M3/build

CROSS=${CROSS:-aarch64-linux-gnu-}
CC=${CC:-${CROSS}gcc}
AS=${AS:-${CROSS}as}
LD=${LD:-${CROSS}ld}
OBJCOPY=${OBJCOPY:-${CROSS}objcopy}
NM=${NM:-${CROSS}nm}
SIZE=${SIZE:-${CROSS}size}
HOSTCC=${HOSTCC:-gcc}
QEMU=${QEMU:-qemu-system-aarch64}

PREFIX_MAP="-ffile-prefix-map=$ROOT=."
CFLAGS_TARGET="-O2 -mstrict-align -mgeneral-regs-only -ffreestanding -nostdlib \
-fno-asynchronous-unwind-tables -fno-unwind-tables -fno-stack-protector $PREFIX_MAP"
ASFLAGS=""
LDFLAGS="-nostdlib --build-id=none -z noexecstack"
HOSTCFLAGS="-O2 -std=c11 -Wall -Wextra -Werror $PREFIX_MAP"

M3_KERNEL_SRCS="sha256_be.s capability.s receipt_ledger.s replay_cache.s memory_alloc.s vector_table.s effect_broker.s physics.s"
M3_TEST_SRCS="tests/test_m3_authority.s tests/sha256_kat.s tests/kat_stub.s"

mkdir -p "$OUT/obj"
cd "$ROOT"

toolver() { "$@" 2>&1 | head -n 1; }
{
  echo "cc:      $(toolver "$CC" --version)"
  echo "as:      $(toolver "$AS" --version)"
  echo "ld:      $(toolver "$LD" --version)"
  echo "objcopy: $(toolver "$OBJCOPY" --version)"
  echo "hostcc:  $(toolver "$HOSTCC" --version)"
  echo "qemu:    $(toolver "$QEMU" --version)"
} > "$OUT/toolchain.txt"

# 1. Audited SHA-256, compiled from repo root
"$CC" $CFLAGS_TARGET -c sha256_clean.c -o "$OUT/obj/sha256_clean.o"
undef=$("$NM" -u "$OUT/obj/sha256_clean.o")
if [ -n "$undef" ]; then
  echo "build.sh: sha256_clean.o has undefined symbols:" >&2
  echo "$undef" >&2; exit 1
fi

# 2. Build host tool m3tool
"$HOSTCC" $HOSTCFLAGS -o "$OUT/m3tool" m3/tools/m3tool.c sha256_clean.c

# 3. Assemble all M3 kernel and test sources
for s in $M3_KERNEL_SRCS $M3_TEST_SRCS; do
  o=$OUT/obj/$(basename "$s" .s).o
  "$AS" $ASFLAGS "m3/$s" -o "$o"
done
"$AS" $ASFLAGS --defsym KAT_FAULT_PROBE=1 m3/tests/sha256_kat.s -o "$OUT/obj/sha256_kat_fault.o"

# 4. Link KAT tests (standalone SHA-256 verification)
link_kat() {
  "$LD" $LDFLAGS -T m3/tests/kat.ld -o "$OUT/$2.elf" \
    "$1" "$OUT/obj/sha256_be.o" "$OUT/obj/sha256_clean.o"
  "$OBJCOPY" -O binary "$OUT/$2.elf" "$OUT/$2.bin"
}
link_kat "$OUT/obj/sha256_kat.o" kat
link_kat "$OUT/obj/sha256_kat_fault.o" kat_fault
"$LD" $LDFLAGS -Ttext=0 -e _start -o "$OUT/kat_stub.elf" "$OUT/obj/kat_stub.o"
"$OBJCOPY" -O binary "$OUT/kat_stub.elf" "$OUT/kat_stub.bin"

# 5. Link Canonical M3 Physics Kernel: physics.elf and physics.bin
KERNEL_OBJS="$OUT/obj/physics.o \
$OUT/obj/vector_table.o \
$OUT/obj/capability.o \
$OUT/obj/receipt_ledger.o \
$OUT/obj/replay_cache.o \
$OUT/obj/memory_alloc.o \
$OUT/obj/effect_broker.o \
$OUT/obj/test_m3_authority.o \
$OUT/obj/sha256_be.o \
$OUT/obj/sha256_clean.o"

"$LD" $LDFLAGS -T m3/physics.ld $KERNEL_OBJS -o "$OUT/physics.elf"
"$OBJCOPY" -O binary "$OUT/physics.elf" "$OUT/physics.bin"

# Check binary size
PHYS_SIZE=$(wc -c < "$OUT/physics.bin")
if [ "$PHYS_SIZE" -ne 18432 ]; then
  echo "build.sh: physics.bin size is $PHYS_SIZE (expected 18432 bytes)" >&2
  exit 1
fi

# 6. Generate pinned digest include for Atlas
"$OUT/m3tool" pin-inc "$OUT/physics.bin" "$OUT/m3_physics_pin.inc"

# 7. Assemble and link Atlas M3 bootstrap seed: atlas_m3.bin
"$AS" $ASFLAGS -I"$OUT" m3/atlas_m3.s -o "$OUT/obj/atlas_m3.o"
"$LD" $LDFLAGS -Ttext=0x00000000 "$OUT/obj/atlas_m3.o" "$OUT/obj/sha256_clean.o" -o "$OUT/atlas_m3.elf"
"$OBJCOPY" -O binary "$OUT/atlas_m3.elf" "$OUT/atlas_m3.bin"

# 8. Produce size and accounting report
{
  echo "=== M3 OBJECT SIZES ==="
  ( cd "$OUT/obj" && "$SIZE" -A *.o )
  echo ""
  echo "=== M3 ELF IMAGES ==="
  ( cd "$OUT" && "$SIZE" physics.elf atlas_m3.elf kat.elf )
  echo ""
  echo "=== M3 BINARY SIZES ==="
  ls -l "$OUT/physics.bin" "$OUT/atlas_m3.bin" "$OUT/kat.bin"
} > "$OUT/sizes.txt"

echo "build.sh: OK -> $OUT (physics.bin: $PHYS_SIZE bytes, atlas_m3.bin: $(wc -c < "$OUT/atlas_m3.bin") bytes)"
