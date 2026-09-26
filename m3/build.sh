#!/bin/sh
# m3/build.sh -- Python-free build skeleton for the PHYSICS M3 image and tools.
#
# Outputs only under m3/build/ (gitignored). Never touches repo-root M2 files;
# sha256_clean.c is compiled from the repo root (same audited source, not forked).
# Deterministic: no timestamps, --build-id=none, source paths mapped to "."
# (-ffile-prefix-map), fixed flags. Rebuilding gives byte-identical outputs.
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
# Freestanding target C: general registers only, no unaligned accesses.
CFLAGS_TARGET="-O2 -mstrict-align -mgeneral-regs-only -ffreestanding -nostdlib \
-fno-asynchronous-unwind-tables -fno-unwind-tables -fno-stack-protector $PREFIX_MAP"
ASFLAGS=""
LDFLAGS="-nostdlib --build-id=none -z noexecstack"
HOSTCFLAGS="-O2 -std=c11 -Wall -Wextra -Werror $PREFIX_MAP"

# M3 kernel-side sources (m3/*.s). The M3 image proper is added here later.
M3_SRCS="sha256_be.s"
# Test harness sources (m3/tests/*.s).
TEST_SRCS="tests/sha256_kat.s tests/kat_stub.s"

mkdir -p "$OUT/obj"
cd "$ROOT"   # relative source paths: nothing host-specific lands in objects

toolver() { "$@" 2>&1 | head -n 1; }
{
  echo "cc:      $(toolver "$CC" --version)"
  echo "as:      $(toolver "$AS" --version)"
  echo "ld:      $(toolver "$LD" --version)"
  echo "objcopy: $(toolver "$OBJCOPY" --version)"
  echo "hostcc:  $(toolver "$HOSTCC" --version)"
  echo "qemu:    $(toolver "$QEMU" --version)"
} > "$OUT/toolchain.txt"

# 1. Audited SHA-256, compiled from the repo root.
"$CC" $CFLAGS_TARGET -c sha256_clean.c -o "$OUT/obj/sha256_clean.o"
undef=$("$NM" -u "$OUT/obj/sha256_clean.o")
if [ -n "$undef" ]; then
  echo "build.sh: sha256_clean.o has undefined symbols (freestanding violation):" >&2
  echo "$undef" >&2; exit 1
fi

# 2. Assemble m3/*.s and m3/tests/*.s.
for s in $M3_SRCS $TEST_SRCS; do
  o=$OUT/obj/$(basename "$s" .s).o
  "$AS" $ASFLAGS "m3/$s" -o "$o"
done
"$AS" $ASFLAGS --defsym KAT_FAULT_PROBE=1 m3/tests/sha256_kat.s -o "$OUT/obj/sha256_kat_fault.o"

# 3. SHA-256 KAT image (raw at 0x40000000) + fault-probe variant + -bios stub.
link_kat() {  # $1 = kat object, $2 = output stem
  "$LD" $LDFLAGS -T m3/tests/kat.ld -o "$OUT/$2.elf" \
    "$1" "$OUT/obj/sha256_be.o" "$OUT/obj/sha256_clean.o"
  "$OBJCOPY" -O binary "$OUT/$2.elf" "$OUT/$2.bin"
}
link_kat "$OUT/obj/sha256_kat.o" kat
link_kat "$OUT/obj/sha256_kat_fault.o" kat_fault
"$LD" $LDFLAGS -Ttext=0 -e _start -o "$OUT/kat_stub.elf" "$OUT/obj/kat_stub.o"
"$OBJCOPY" -O binary "$OUT/kat_stub.elf" "$OUT/kat_stub.bin"

# 4. Host tool, linking the same repo-root sha256_clean.c.
"$HOSTCC" $HOSTCFLAGS -o "$OUT/m3tool" m3/tools/m3tool.c sha256_clean.c

# 5. Size report.
( cd "$OUT/obj" && "$SIZE" -A sha256_clean.o sha256_be.o ) > "$OUT/sizes.txt"
( cd "$OUT" && "$SIZE" kat.elf ) >> "$OUT/sizes.txt"

echo "build.sh: OK -> $OUT"
