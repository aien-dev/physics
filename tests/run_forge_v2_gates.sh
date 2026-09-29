#!/bin/bash
# tests/run_forge_v2_gates.sh -- FORGE SUBSTRATE V2 host-only gates.
#
# Builds the V2 reference serializer + KAT with the pinned root SHA-256 and
# runs it. No hardware, no driver, no GPU lock.
#
#   tests/run_forge_v2_gates.sh            build, run every check, verify the
#                                          committed golden-vector doc matches
#   tests/run_forge_v2_gates.sh --regen    also rewrite docs/forge-v2-golden-vectors.md
#
# Exit code 0 only when every check passes.
set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd -P)
cd "$REPO_ROOT" || exit 1

BUILD_DIR="$REPO_ROOT/build"
BIN="$BUILD_DIR/test_forge_v2_kat"
DOC="docs/forge-v2-golden-vectors.md"
FAILS=0
PASSES=0

gate() { # name, status(0 = pass)
    if [ "$2" -eq 0 ]; then echo "PASS $1"; PASSES=$((PASSES + 1));
    else echo "FAIL $1"; FAILS=$((FAILS + 1)); fi
}

echo "=== FORGE SUBSTRATE V2 gates (host-only) ==="
mkdir -p "$BUILD_DIR"

gcc -O2 -Wall -Wextra -Werror -std=c11 -I./forge/v2 \
    forge/v2/forge_substrate_v2.c sha256_clean.c tests/test_forge_v2_kat.c \
    -o "$BIN"
gate "build_gcc_O2_Wall_Wextra_Werror_std_c11" $?
if [ ! -x "$BIN" ]; then
    echo "RESULT gates: $PASSES passed, $((FAILS)) failed (build failed)"
    exit 1
fi

"$BIN"
gate "kat_binary_all_checks" $?

if [ "${1:-}" = "--regen" ]; then
    "$BIN" --dump-vectors > "$DOC"
    gate "golden_vectors_regenerated" $?
fi
"$BIN" --dump-vectors > "$BUILD_DIR/forge-v2-golden-vectors.md"
diff -u "$DOC" "$BUILD_DIR/forge-v2-golden-vectors.md" > /dev/null
gate "golden_vector_doc_matches_binary_output" $?

# Vendor-neutral canonical names: no vendor or product word in any V2 identifier.
! grep -nE '\b[A-Za-z_]*(NVIDIA|Nvidia|nvidia|GB10|gb10|BLACKWELL|Blackwell|blackwell|CUDA|cuda|NVRM|nvrm)[A-Za-z_0-9]*\b' \
    forge/v2/forge_substrate_v2.h forge/v2/forge_substrate_v2.c \
    | grep -vE '^\S+:[0-9]+:\s*(/\*|\*)' > /dev/null
gate "no_vendor_names_in_v2_identifiers" $?

# No hardware / driver / allocation in the reference code.
! grep -nE '#include\s*[<"](nvrm|nv|cuda|sys/ioctl|sys/mman|fcntl)' forge/v2/*.c forge/v2/*.h tests/test_forge_v2_kat.c > /dev/null \
  && ! grep -nE '\b(malloc|calloc|realloc|free|ioctl|mmap|open)\s*\(' forge/v2/*.c > /dev/null
gate "no_hardware_access_no_heap_in_v2" $?

# R16 loop-inventory heuristics: no endless loops and no loop naming wait/event words.
! grep -nE 'while\s*\(\s*(1|true)\s*\)|for\s*\(\s*;\s*;\s*\)' forge/v2/*.c tests/test_forge_v2_kat.c > /dev/null \
  && ! grep -niE '(^|[^a-z])(u?sleep|nanosleep|msleep|poll|recv|heartbeat|tick|dispatch|schedule|scheduler|orchestrate|orchestrator|turns?|pulse|yield|epoll|run_until_complete|max_steps|max_turns)([^a-z]|$)' \
       forge/v2/*.c forge/v2/*.h tests/test_forge_v2_kat.c > /dev/null
gate "r16_loop_inventory_heuristics_clean" $?

echo "RESULT gates: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
