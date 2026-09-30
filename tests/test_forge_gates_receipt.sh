#!/bin/bash
# tests/test_forge_gates_receipt.sh -- host-only tests (no GPU) for the Gate
# 3/4 receipt writer in tests/run_forge_gates.sh.
#
#   tests/test_forge_gates_receipt.sh OMEGA_DIR
#
# OMEGA_DIR is an omega checkout; its tools/json_canon.c is the digest helper.
# The PASS fixture tests/fixtures/forge_gates_pass.log is the unedited Gate
# 3/4 log of the Gate 14 legs run on omega 62f5ba5 + physics f63a6ef.
set -u
HERE=$(cd -P "$(dirname "$0")/.." && pwd)
OMEGA_DIR=${1:-${OMEGA_DIR:-}}
[ -n "$OMEGA_DIR" ] && [ -f "$OMEGA_DIR/tools/json_canon.c" ] ||
    { echo "usage: $0 OMEGA_DIR (an omega checkout with tools/json_canon.c)"; exit 2; }
# shellcheck source=tests/run_forge_gates.sh
. "$HERE/tests/run_forge_gates.sh"
forge_build_canon "$OMEGA_DIR" || { echo "FAIL: $FORGE_ERR"; exit 1; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP" "$FORGE_TMP"' EXIT
fails=0 passes=0
ok() { echo "  [PASS] $1"; passes=$((passes + 1)); }
bad() { echo "  [FAIL] $1"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

FIX=$HERE/tests/fixtures/forge_gates_pass.log
mkrepo() {
    git init -q "$1" && echo "$2" > "$1/f" && git -C "$1" add f &&
        git -C "$1" -c user.name=T -c user.email=t@example.invalid commit -qm c &&
        git -C "$1" rev-parse HEAD
}
O=$TMP/omega; P=$TMP/physics
osha=$(mkrepo "$O" omega); psha=$(mkrepo "$P" physics)
BIN3=$(printf 3 | sha256sum | cut -d' ' -f1); BIN4=$(printf 4 | sha256sum | cut -d' ' -f1)
body_of() { forge_body "$1" "${2:-0}" "${3:-0}" "$BIN3" "$BIN4" "$O" "$osha" "$P" "$psha" 20260930T000000Z-000000000000 2026-09-30T00:00:00.000000Z; }
field() { printf '%s' "$1" | jq -r "$2"; }
variant() { sed -E "$1" "$FIX" > "$TMP/v.log"; echo "$TMP/v.log"; }

echo "gate verdicts on the real PASS log"
check "gate 3 PASS" '[ "$(forge_gate_verdict "$FIX" 3 0 "$FORGE_G3_IDS")" = PASS ]'
check "gate 4 PASS" '[ "$(forge_gate_verdict "$FIX" 4 0 "$FORGE_G4_IDS")" = PASS ]'
check "gate 3 has 11 events" '[ "$(forge_events "$FIX" 3 | wc -l)" = 11 ]'
check "gate 4 has 10 events" '[ "$(forge_events "$FIX" 4 | wc -l)" = 10 ]'
check "nonzero exit status fails" '[ "$(forge_gate_verdict "$FIX" 3 1 "$FORGE_G3_IDS")" = "FAIL: gate 3 binary exit status 1" ]'
check "a FAIL line fails" 'v=$(variant "s/^\[PASS\] NEG_NULL_NVRM_REJECTED$/[FAIL] NEG_NULL_NVRM_REJECTED/"); [ "$(forge_gate_verdict "$v" 3 0 "$FORGE_G3_IDS")" = "FAIL: gate 3 printed a FAIL line" ]'
check "a missing id fails" 'v=$(variant "/^\[PASS\] DESCRIPTOR_DIGEST_BOUND$/d"); forge_gate_verdict "$v" 4 0 "$FORGE_G4_IDS" | grep -q "^FAIL"'
check "an unexpected id fails" 'v=$(variant "s/^\[PASS\] AEGIS_VERIFY_API_NAMED$/[PASS] SOMETHING_ELSE/"); [ "$(forge_gate_verdict "$v" 3 0 "$FORGE_G3_IDS")" = "FAIL: gate 3 observed ids differ from the expected ids" ]'
check "FAILED=1 in results fails" 'v=$(variant "s/^Gate 3 Results: .*/Gate 3 Results: TOTAL=11 PASSED=10 FAILED=1/"); forge_gate_verdict "$v" 3 0 "$FORGE_G3_IDS" | grep -q "^FAIL: gate 3 results"'
check "missing results line fails" 'v=$(variant "/^Gate 4 Results:/d"); forge_gate_verdict "$v" 4 0 "$FORGE_G4_IDS" | grep -q "results line missing"'
check "repeated results line fails" 'v=$(variant "s/^(Gate 4 Results: .*)/\1\n\1/"); forge_gate_verdict "$v" 4 0 "$FORGE_G4_IDS" | grep -q "results line missing or repeated"'
check "a truncated log (no gate 3) fails" 'v=$(variant "/Running Gate 3:/,\$d"); forge_gate_verdict "$v" 3 0 "$FORGE_G3_IDS" | grep -q "^FAIL"'

echo "receipt body and digest"
body=$(body_of "$FIX")
check "body is valid JSON" 'printf "%s" "$body" | "$JSON_CANON" --check'
check "status PASS" '[ "$(field "$body" .status)" = PASS ]'
check "schema" '[ "$(field "$body" .schema)" = AIEN_M19R_FORGE_GATES_V1 ]'
check "candidate_git_commit is the omega commit" '[ "$(field "$body" .candidate_git_commit)" = "$osha" ]'
check "physics_candidate_git_commit is the physics commit" '[ "$(field "$body" .physics_candidate_git_commit)" = "$psha" ]'
check "both trees clean" '[ "$(field "$body" "[.candidate_trees_clean.omega, .candidate_trees_clean.physics] | all")" = true ]'
check "per-gate PASS" '[ "$(field "$body" "[.gates[].status] | join(\",\")")" = PASS,PASS ]'
check "observed 21 pass, 0 fail" '[ "$(field "$body" "[.observed_test_count,.observed_pass_count,.observed_fail_count] | join(\",\")")" = 21,21,0 ]'
check "descriptor digest from the log" '[ "$(field "$body" .hardware_descriptor_digest)" = 1ce23d57112901c414ddc75ba35b65bc8c38fc0bb5e85f3a995634af456bf24b ]'
check "log sha256 bound" '[ "$(field "$body" .gate_log_sha256)" = 2c139d36a055dc1e7bfa1e6074d8b14a23d90f74b13216d8d70b529c0f66e805 ]'
forge_receipt "$body"; receipt=$FORGE_RECEIPT
check "receipt digest recomputes from the body" '[ "$(printf "%s" "$receipt" | jq -c "del(.receipt_digest)" | "$JSON_CANON" --sha256)" = "$FORGE_DIGEST" ]'
check "gate 4 exit 1 gives status FAIL with error" 'b=$(body_of "$FIX" 0 1); [ "$(field "$b" .status)" = FAIL ] && [ "$(field "$b" .gates.GATE_4_FORGE_HWID.status)" = FAIL ] && field "$b" .error | grep -q "gate 4 binary exit status 1"'
echo dirty > "$P/f"
check "dirty physics tree gives FAIL" 'b=$(body_of "$FIX"); [ "$(field "$b" .status)" = FAIL ] && [ "$(field "$b" .candidate_trees_clean.physics)" = false ]'
git -C "$P" checkout -q f
echo new > "$O/untracked"
check "untracked file in omega gives FAIL" 'b=$(body_of "$FIX"); [ "$(field "$b" .status)" = FAIL ] && [ "$(field "$b" .candidate_trees_clean.omega)" = false ]'
rm -f "$O/untracked"
check "wrong omega candidate gives FAIL" 'b=$(forge_body "$FIX" 0 0 "$BIN3" "$BIN4" "$O" "$psha" "$P" "$psha" r t); [ "$(field "$b" .status)" = FAIL ]'
check "candidate check refuses short SHA" '! forge_candidate "$O" "${osha:0:12}"'
check "candidate check refuses other HEAD" '! forge_candidate "$O" "$psha"'
check "candidate check accepts HEAD of clean tree" 'forge_candidate "$O" "$osha"'

echo "permanent receipt"
EV=$TMP/evidence
check "record writes digest-named receipt" 'forge_record "$receipt" "$FIX" "$EV" "$O" "$P" && [ -f "$EV/$FORGE_DIGEST.json" ]'
check "receipt is 0444" '[ "$(stat -c %a "$EV/$FORGE_DIGEST.json")" = 444 ]'
check "log blob kept by sha256" '[ -f "$EV/blobs/2c139d36a055dc1e7bfa1e6074d8b14a23d90f74b13216d8d70b529c0f66e805.log" ]'
check "on-disk receipt digest verifies" '[ "$(jq -c "del(.receipt_digest)" "$EV/$FORGE_DIGEST.json" | "$JSON_CANON" --sha256)" = "$FORGE_DIGEST" ]'
check "second write refused" '! forge_record "$receipt" "$FIX" "$EV" "$O" "$P" 2>/dev/null'
check "evidence dir inside a candidate tree refused" '! forge_record "$receipt" "$FIX" "$P/evidence" "$O" "$P" && [ ! -e "$P/evidence" ]'

echo "Gate 3/4 receipt tests: $passes passed, $fails failed"
[ "$fails" -eq 0 ]
