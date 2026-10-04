#!/bin/bash
# tests/run_forge_form_gates.sh -- FORM0 gates: FORGE REALIZATION FORM V1
# (substrate-neutral physical-form realization IR, ADR 0034 PROPOSED; extends
# ARCH-0018 / ARCH-0019). Host-only: no hardware, no driver, no GPU lock.
# Every result is SIMULATED_DEVELOPMENT.
#
#   tests/run_forge_form_gates.sh           build, run every check, verify the
#                                           committed receipt is reproduced byte for byte
#   tests/run_forge_form_gates.sh --regen   also (re)write evidence/FORM0/<sha256>.json
#
# Exit code 0 only when every gate passes.
set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd -P)
cd "$REPO_ROOT" || exit 1

BUILD_DIR="$REPO_ROOT/build"
BIN="$BUILD_DIR/test_forge_form"
RECEIPT_DIR="evidence/FORM0"
SRC_C="forge/form/forge_form.c"
SRC_H="forge/form/forge_form.h"
TEST_C="tests/test_forge_form.c"
SRCS="forge/v2/forge_substrate_v2.c forge/analog-sim/forge_analog_sim.c $SRC_C sha256_clean.c"
INC="-I./forge/v2 -I./forge/analog-sim -I./forge/form"
FAILS=0
PASSES=0

gate() { # name, status(0 = pass)
    if [ "$2" -eq 0 ]; then echo "PASS $1"; PASSES=$((PASSES + 1));
    else echo "FAIL $1"; FAILS=$((FAILS + 1)); fi
}

echo "=== FORM0 realization-form gates (host-only, SIMULATED_DEVELOPMENT) ==="
mkdir -p "$BUILD_DIR"

# shellcheck disable=SC2086
gcc -O2 -Wall -Wextra -Werror -std=c11 $INC $SRCS "$TEST_C" -o "$BIN"
gate "build_gcc_O2_Wall_Wextra_Werror_std_c11" $?
if [ ! -x "$BIN" ]; then
    echo "RESULT gates: $PASSES passed, $FAILS failed (build failed)"
    exit 1
fi

"$BIN" | grep -E '^(FAIL|INFO|RESULT)'
"$BIN" > /dev/null
gate "form0_checks_all_pass" $?

# Same checks under the undefined-behaviour sanitizer (any UB aborts the run).
# shellcheck disable=SC2086
gcc -O1 -g -Wall -Wextra -Werror -std=c11 -fsanitize=undefined -fno-sanitize-recover=all \
    $INC $SRCS "$TEST_C" -o "$BIN-ubsan" \
  && "$BIN-ubsan" > /dev/null
gate "form0_checks_pass_under_ubsan" $?

# Deterministic receipt, named by the SHA-256 of its bytes.
"$BIN" --receipt > "$BUILD_DIR/form0-receipt.json"
gate "receipt_emitted_by_code" $?
SUM=$(sha256sum "$BUILD_DIR/form0-receipt.json" | cut -d' ' -f1)
if [ "${1:-}" = "--regen" ]; then
    mkdir -p "$RECEIPT_DIR"
    rm -f "$RECEIPT_DIR"/*.json
    cp "$BUILD_DIR/form0-receipt.json" "$RECEIPT_DIR/$SUM.json"
    gate "receipt_regenerated" $?
fi
"$BIN" --receipt | cmp -s - "$BUILD_DIR/form0-receipt.json"
gate "receipt_deterministic_across_runs" $?
COMMITTED=$(ls "$RECEIPT_DIR"/*.json 2>/dev/null | wc -l)
[ "$COMMITTED" -eq 1 ] && [ -f "$RECEIPT_DIR/$SUM.json" ] && cmp -s "$RECEIPT_DIR/$SUM.json" "$BUILD_DIR/form0-receipt.json"
gate "committed_receipt_reproduced_digest_named" $?

# Provenance: the receipt says SIMULATED_DEVELOPMENT and never claims a physical result.
grep -q '"provenance_class": "SIMULATED_DEVELOPMENT"' "$BUILD_DIR/form0-receipt.json" \
  && grep -q '"status": "PASS"' "$BUILD_DIR/form0-receipt.json" \
  && ! grep -qiE 'physical_result|measured_on_device|PROVENANCE_PHYSICAL' "$BUILD_DIR/form0-receipt.json"
gate "receipt_simulated_development_no_physical_claim" $?
CASES=$(grep -c '"name": ' "$BUILD_DIR/form0-receipt.json")
SIMCASES=$(grep -c '"provenance_class": "SIMULATED_DEVELOPMENT"}' "$BUILD_DIR/form0-receipt.json")
[ "$CASES" -gt 0 ] && [ "$CASES" -eq "$SIMCASES" ]
gate "every_receipt_case_simulated_development" $?

# The success criterion is in the receipt: one contract, two realizations,
# same contract digest, different realization identities.
grep -q '"proof_two_realizations_same_contract": "PASS"' "$BUILD_DIR/form0-receipt.json"
gate "receipt_proof_two_realizations_same_contract" $?

# The provider source never names the PHYSICAL provenance (except to refuse it).
[ "$(grep -c 'PROVENANCE_PHYSICAL' "$SRC_C" "$SRC_H" | awk -F: '{s+=$2} END {print s}')" -eq 0 ]
gate "provider_source_cannot_emit_physical" $?

# The form object itself carries no substrate, vendor, device or calibration coordinate.
! awk '/^typedef struct \{/{b=1; buf=""} b{buf=buf $0 "\n"} /\} ForgeRealizationForm;/{print buf; b=0}' "$SRC_H" \
    | grep -qiE 'substrate|vendor|device|calibrat|clock|temperature|address|bus|machine'
gate "form_struct_has_no_realization_coordinates" $?

# Vendor-neutral: no vendor or product word in any identifier of the new files.
! grep -nE '\b[A-Za-z_]*(NVIDIA|Nvidia|nvidia|GB10|gb10|BLACKWELL|Blackwell|blackwell|CUDA|cuda|NVRM|nvrm)[A-Za-z_0-9]*\b' \
    "$SRC_C" "$SRC_H" "$TEST_C" | grep -vE '^\S+:[0-9]+:\s*(/\*|\*)' > /dev/null
gate "no_vendor_names_in_identifiers" $?

# No quantum vocabulary in classical semantics.
! grep -niE 'quantum|superposition|wavefunction|collapse|entangle' "$SRC_C" "$SRC_H" > /dev/null
gate "no_quantum_metaphors_in_provider" $?

# No hardware, driver, heap or I/O in the provider.
! grep -nE '#include\s*[<"](nvrm|nv|cuda|sys/ioctl|sys/mman|fcntl|stdio)' "$SRC_C" "$SRC_H" > /dev/null \
  && ! grep -nE '\b(malloc|calloc|realloc|free|ioctl|mmap|open|printf|fopen)\s*\(' "$SRC_C" > /dev/null
gate "no_hardware_access_no_heap_no_io_in_provider" $?

# R16 loop-inventory heuristics (same patterns as the V2 / AR2 gates).
! grep -nE 'while\s*\(\s*(1|true)\s*\)|for\s*\(\s*;\s*;\s*\)' "$SRC_C" "$TEST_C" > /dev/null \
  && ! grep -niE '(^|[^a-z])(u?sleep|nanosleep|msleep|poll|recv|heartbeat|tick|dispatch|schedule|scheduler|orchestrate|orchestrator|turns?|pulse|yield|epoll|run_until_complete|max_steps|max_turns)([^a-z]|$)' \
       "$SRC_C" "$SRC_H" "$TEST_C" > /dev/null
gate "r16_loop_inventory_heuristics_clean" $?

# FORGE V2 and AR2 files are untouched by this gate: their committed receipts
# and golden vectors must still reproduce (AR2 script also runs the AR1 gates).
bash tests/run_forge_analog_sim_gates.sh > "$BUILD_DIR/ar2-gates-from-form0.log" 2>&1
gate "ar1_ar2_gates_still_pass" $?

echo "RESULT gates: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
