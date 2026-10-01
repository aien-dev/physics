#!/bin/bash
# tests/run_forge_analog_sim_gates.sh -- AR2 gates (ARCH-0018): simulated analog
# realization provider with digital oracle parity. Host-only: no hardware, no
# driver, no GPU lock. Every result is SIMULATED_DEVELOPMENT.
#
#   tests/run_forge_analog_sim_gates.sh           build, run every check, verify the
#                                                 committed receipt is reproduced byte for byte
#   tests/run_forge_analog_sim_gates.sh --regen   also (re)write evidence/AR2/<sha256>.json
#
# Optional: OMEGA_DIR (default ../omega next to this repo). When it holds
# src/omega_matvec.c and src/omega_blackwell_matmul.c, the two restated
# oracles are compared against the omega originals; otherwise that gate SKIPs.
#
# Exit code 0 only when every gate passes.
set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd -P)
cd "$REPO_ROOT" || exit 1

BUILD_DIR="$REPO_ROOT/build"
BIN="$BUILD_DIR/test_forge_analog_sim"
RECEIPT_DIR="evidence/AR2"
SRC_C="forge/analog-sim/forge_analog_sim.c"
SRC_H="forge/analog-sim/forge_analog_sim.h"
TEST_C="tests/test_forge_analog_sim.c"
OMEGA_DIR="${OMEGA_DIR:-$REPO_ROOT/../omega}"
FAILS=0
PASSES=0
SKIPS=0

gate() { # name, status(0 = pass)
    if [ "$2" -eq 0 ]; then echo "PASS $1"; PASSES=$((PASSES + 1));
    else echo "FAIL $1"; FAILS=$((FAILS + 1)); fi
}

echo "=== AR2 simulated analog provider gates (host-only, SIMULATED_DEVELOPMENT) ==="
mkdir -p "$BUILD_DIR"

gcc -O2 -Wall -Wextra -Werror -std=c11 -I./forge/v2 -I./forge/analog-sim \
    forge/v2/forge_substrate_v2.c "$SRC_C" sha256_clean.c "$TEST_C" -o "$BIN"
gate "build_gcc_O2_Wall_Wextra_Werror_std_c11" $?
if [ ! -x "$BIN" ]; then
    echo "RESULT gates: $PASSES passed, $FAILS failed (build failed)"
    exit 1
fi

"$BIN" | grep -E '^(FAIL|RESULT)'
"$BIN" > /dev/null
gate "ar2_checks_all_pass" $?

# Same checks under the undefined-behaviour sanitizer (any UB aborts the run).
gcc -O1 -g -Wall -Wextra -Werror -std=c11 -fsanitize=undefined -fno-sanitize-recover=all \
    -I./forge/v2 -I./forge/analog-sim \
    forge/v2/forge_substrate_v2.c "$SRC_C" sha256_clean.c "$TEST_C" -o "$BIN-ubsan" \
  && "$BIN-ubsan" > /dev/null
gate "ar2_checks_pass_under_ubsan" $?

# Deterministic receipt, named by the SHA-256 of its bytes.
"$BIN" --receipt > "$BUILD_DIR/ar2-receipt.json"
gate "receipt_emitted_by_code" $?
SUM=$(sha256sum "$BUILD_DIR/ar2-receipt.json" | cut -d' ' -f1)
if [ "${1:-}" = "--regen" ]; then
    mkdir -p "$RECEIPT_DIR"
    rm -f "$RECEIPT_DIR"/*.json
    cp "$BUILD_DIR/ar2-receipt.json" "$RECEIPT_DIR/$SUM.json"
    gate "receipt_regenerated" $?
fi
"$BIN" --receipt | cmp -s - "$BUILD_DIR/ar2-receipt.json"
gate "receipt_deterministic_across_runs" $?
COMMITTED=$(ls "$RECEIPT_DIR"/*.json 2>/dev/null | wc -l)
[ "$COMMITTED" -eq 1 ] && [ -f "$RECEIPT_DIR/$SUM.json" ] && cmp -s "$RECEIPT_DIR/$SUM.json" "$BUILD_DIR/ar2-receipt.json"
gate "committed_receipt_reproduced_digest_named" $?

# Provenance: the receipt says SIMULATED_DEVELOPMENT and never claims a physical result.
grep -q '"provenance_class": "SIMULATED_DEVELOPMENT"' "$BUILD_DIR/ar2-receipt.json" \
  && grep -q '"status": "PASS"' "$BUILD_DIR/ar2-receipt.json" \
  && ! grep -qiE 'physical|measured' "$BUILD_DIR/ar2-receipt.json"
gate "receipt_simulated_development_no_physical_claim" $?
CASES=$(grep -c '"name": ' "$BUILD_DIR/ar2-receipt.json")
SIMCASES=$(grep -c '"provenance_class": "SIMULATED_DEVELOPMENT"}' "$BUILD_DIR/ar2-receipt.json")
[ "$CASES" -gt 0 ] && [ "$CASES" -eq "$SIMCASES" ]
gate "every_receipt_case_simulated_development" $?

# The provider source never names the PHYSICAL provenance.
! grep -n 'PROVENANCE_PHYSICAL' "$SRC_C" "$SRC_H" > /dev/null
gate "provider_source_cannot_emit_physical" $?

# Vendor-neutral: no vendor or product word in any identifier of the new files.
! grep -nE '\b[A-Za-z_]*(NVIDIA|Nvidia|nvidia|GB10|gb10|BLACKWELL|Blackwell|blackwell|CUDA|cuda|NVRM|nvrm)[A-Za-z_0-9]*\b' \
    "$SRC_C" "$SRC_H" "$TEST_C" \
    | grep -vE '^\S+:[0-9]+:\s*(/\*|\*)' | grep -vE 'omega_blackwell_matmul\.[ch]' > /dev/null
gate "no_vendor_names_in_identifiers" $?

# No hardware, driver, heap or I/O in the provider.
! grep -nE '#include\s*[<"](nvrm|nv|cuda|sys/ioctl|sys/mman|fcntl|stdio)' "$SRC_C" "$SRC_H" > /dev/null \
  && ! grep -nE '\b(malloc|calloc|realloc|free|ioctl|mmap|open|printf|fopen)\s*\(' "$SRC_C" > /dev/null
gate "no_hardware_access_no_heap_no_io_in_provider" $?

# R16 loop-inventory heuristics (same patterns as the V2 gates).
! grep -nE 'while\s*\(\s*(1|true)\s*\)|for\s*\(\s*;\s*;\s*\)' "$SRC_C" "$TEST_C" > /dev/null \
  && ! grep -niE '(^|[^a-z])(u?sleep|nanosleep|msleep|poll|recv|heartbeat|tick|dispatch|schedule|scheduler|orchestrate|orchestrator|turns?|pulse|yield|epoll|run_until_complete|max_steps|max_turns)([^a-z]|$)' \
       "$SRC_C" "$SRC_H" "$TEST_C" > /dev/null
gate "r16_loop_inventory_heuristics_clean" $?

# Oracle parity with the omega originals (read-only; omega is never modified).
MV="$OMEGA_DIR/src/omega_matvec.c"
MM="$OMEGA_DIR/src/omega_blackwell_matmul.c"
if [ -f "$MV" ] && [ -f "$MM" ]; then
    X="$BUILD_DIR/ar2-omega-oracle"
    mkdir -p "$X"
    {
        echo '#include <stdint.h>'
        echo '#include <stddef.h>'
        awk '/^void omega_matvec_reference\(/,/^}/' "$MV"
        awk '/^int omega_matmul_cpu_oracle_i32\(/,/^}/' "$MM"
    } > "$X/omega_oracles.c"
    cat > "$X/cross.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include "forge_analog_sim.h"
void omega_matvec_reference(const uint64_t *A, const uint64_t *x, uint64_t *y, uint32_t M, uint32_t N);
int omega_matmul_cpu_oracle_i32(const uint32_t *a, const uint32_t *b, uint32_t *c, uint32_t m, uint32_t k, uint32_t n);
int main(void)
{
    static uint64_t A[64 * 64], x[64], y1[64], y2[64];
    static uint32_t a32[64 * 64], x32[64], z1[64], z2[64];
    uint64_t s = 0x5eed;
    int bad = 0;
    for (int c = 0; c < 64; c++) {
        uint32_t M = 1 + (uint32_t)(c % 64), N = 1 + (uint32_t)((c * 37) % 64);
        for (uint32_t k = 0; k < M * N; k++) {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            A[k] = (c & 1) ? s : (s >> 56);
            a32[k] = (uint32_t)(s >> 7);
        }
        for (uint32_t j = 0; j < N; j++) {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            x[j] = (c & 1) ? s : (s >> 56);
            x32[j] = (uint32_t)(s >> 9);
        }
        omega_matvec_reference(A, x, y1, M, N);
        forge_asim_oracle_exact_u64(A, x, y2, M, N);
        omega_matmul_cpu_oracle_i32(a32, x32, z1, M, N, 1);
        forge_asim_oracle_i32(a32, x32, z2, M, N);
        if (memcmp(y1, y2, M * sizeof y1[0]) || memcmp(z1, z2, M * sizeof z1[0])) bad++;
    }
    printf("omega oracle cross-check: %d mismatching cases of 64\n", bad);
    return bad != 0;
}
EOF
    gcc -O2 -std=c11 -Wall -Werror -I./forge/v2 -I./forge/analog-sim \
        "$X/omega_oracles.c" "$X/cross.c" "$SRC_C" forge/v2/forge_substrate_v2.c sha256_clean.c -o "$X/cross" \
      && "$X/cross"
    gate "restated_oracles_match_omega_originals" $?
else
    echo "SKIP restated_oracles_match_omega_originals (no omega checkout at $OMEGA_DIR)"
    SKIPS=$((SKIPS + 1))
fi

# The AR1 (V2 descriptor contract) gates are untouched and still pass.
bash tests/run_forge_v2_gates.sh > "$BUILD_DIR/ar1-gates.log" 2>&1
gate "ar1_v2_gates_still_pass" $?

echo "RESULT gates: $PASSES passed, $FAILS failed, $SKIPS skipped"
[ "$FAILS" -eq 0 ]
