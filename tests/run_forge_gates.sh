#!/bin/bash
# tests/run_forge_gates.sh -- Gate 3 (FORGE-0) and Gate 4 (FORGE-HWID) runner
# with a machine-made receipt.
#
#   tests/run_forge_gates.sh
#       log only, no receipt (the old behaviour)
#   tests/run_forge_gates.sh --omega-dir DIR --omega-candidate SHA
#                            --physics-candidate SHA [--evidence-dir DIR --record]
#       also writes build/forge-runs/<run-id>/receipt-preview.json, and with
#       --record the permanent receipt <evidence-dir>/<receipt_digest>.json
#       (created exclusively, mode 0444) plus blobs/<sha256>.log
#
# Needs the GB10. The whole build + test section runs under one exclusive
# lock on /tmp/aien-gb10.lock (fd 9); nothing inside takes the lock again.
#
# Receipt mode checks: both candidates are explicit full 40-hex SHAs equal to
# HEAD of a clean tree (this physics tree and --omega-dir), before the run
# and again at receipt time. A gate is PASS only when its binary exited 0,
# its "Gate N Results:" line appears once with FAILED=0 and TOTAL=PASSED, and
# the "[PASS]/[FAIL] ID" lines are exactly the expected ids, each PASS once.
# Field names follow the omega Gate 1/2 and Gate 5 receipts, so the Gate 14
# combiner reads one set of names: candidate_git_commit is the OMEGA commit,
# physics_candidate_git_commit the physics commit.
#
# The receipt digest is SHA-256 over the canonical JSON of the receipt body
# (sorted keys, compact), computed by omega's tools/json_canon.c built from
# --omega-dir, the same helper the other legs use. It shows the receipt was
# not altered; it is not a signature. Shell + coreutils + git + gcc + awk.
# No Python.
#
# Exit 0 when both gates pass (and, in receipt mode, the receipt is PASS),
# 1 on any failure, 2 on bad arguments.
#
# The functions can be sourced (tests/test_forge_gates_receipt.sh does);
# main only runs when the file is executed.

FORGE_ROOT=$(cd -P "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
FORGE_LOCK=/tmp/aien-gb10.lock
FORGE_ERR=
FORGE_SCHEMA=AIEN_M19R_FORGE_GATES_V1
FORGE_GATE=M19R_GATES_3_4_FORGE
FORGE_G3=GATE_3_FORGE_0
FORGE_G4=GATE_4_FORGE_HWID
FORGE_G3_IDS="FORGE_REALIZE_API_NAMED AEGIS_VERIFY_API_NAMED FORGE_TYPED_BOUNDARY_ENFORCED
HISTORICAL_NAMES_PRESERVED NO_EXISTING_FILE_RENAMED NEG_NULL_NVRM_REJECTED
NEG_UNVERIFIED_REALIZATION_REJECTED NEG_DESCRIPTOR_MISMATCH_REJECTED
NEG_STALE_REALIZATION_IDENTITY_REJECTED NEG_VERIFICATION_FAILURE_PREVENTS_SUBMISSION
NEG_POST_VERIFICATION_MUTATION_REFUSED"
FORGE_G4_IDS="RAW_HARDWARE_PROBE_CAPTURED FORGE_DESCRIPTOR_NORMALIZED DESCRIPTOR_DIGEST_BOUND
RECEIPT_HARDWARE_DERIVED SERIALIZATION_KAT_VERIFIED NEG_MISSING_REQUIRED_FIELD_REJECTED
NEG_MALFORMED_PCI_REJECTED NEG_UNSUPPORTED_COMPUTE_CLASS_REJECTED
NEG_DESCRIPTOR_MUTATION_CAUSES_DIGEST_MISMATCH NEG_UNBOUND_ALIAS_REJECTED"

forge_fail() { FORGE_ERR=$1; return 1; }

forge_sha_file() { sha256sum "$1" | cut -d' ' -f1; }

# forge_candidate REPO SHA -- SHA is a full 40-hex id equal to HEAD of a clean tree.
forge_candidate() {
    local repo=$1 sha=$2 name head st
    name=$(basename "$repo")
    [[ $sha =~ ^[0-9a-fA-F]{40}$ ]] || forge_fail "candidate must be an explicit full 40-hex SHA" || return 1
    head=$(git -C "$repo" rev-parse HEAD 2>/dev/null) || forge_fail "cannot read $name HEAD" || return 1
    [ "${head,,}" = "${sha,,}" ] || forge_fail "$name HEAD differs from supplied candidate" || return 1
    st=$(git -C "$repo" status --porcelain --untracked-files=normal 2>/dev/null) ||
        forge_fail "cannot read $name status" || return 1
    [ -z "$st" ] || forge_fail "$name candidate tree is dirty"
}

# forge_tree_clean REPO -- "true" or "false" from git status right now.
forge_tree_clean() {
    local st
    if st=$(git -C "$1" status --porcelain --untracked-files=normal 2>/dev/null) && [ -z "$st" ]; then
        echo true
    else
        echo false
    fi
}

# forge_build_canon OMEGA_DIR -- build omega's tools/json_canon.c into a
# private temp directory. Sets JSON_CANON and FORGE_TMP.
forge_build_canon() {
    local o=$1
    FORGE_TMP=$(mktemp -d "${TMPDIR:-/tmp}/forge-receipt.XXXXXX") || return 1
    JSON_CANON=$FORGE_TMP/json_canon
    gcc -std=gnu11 -O2 -Wall -Wextra -Werror -I"$o/src" -o "$JSON_CANON" \
        "$o/tools/json_canon.c" "$o/src/sha256.c" -lm ||
        forge_fail "cannot build $o/tools/json_canon.c"
}

# forge_events LOG SECTION -- "id<TAB>status" for each [PASS]/[FAIL] line of
# one gate section: from "Running Gate N:" up to "Gate N Results:".
forge_events() {
    awk -v n="$2" '
        index($0, "Running Gate " n ":") { on = 1; next }
        on && index($0, "Gate " n " Results:") == 1 { on = 0; next }
        on && match($0, /^\[(PASS|FAIL)\] [A-Z0-9_]+$/) {
            print substr($0, 8) "\t" substr($0, 2, 4)
        }' "$1"
}

# forge_results LOG N -- the "TOTAL PASSED FAILED" numbers of the single
# "Gate N Results:" line; fails when there is not exactly one.
forge_results() {
    local lines
    lines=$(grep -E "^Gate $2 Results: TOTAL=[0-9]+ PASSED=[0-9]+ FAILED=[0-9]+$" "$1")
    [ -n "$lines" ] && [ "$(printf '%s\n' "$lines" | wc -l)" -eq 1 ] || return 1
    printf '%s\n' "$lines" | sed -E 's/.*TOTAL=([0-9]+) PASSED=([0-9]+) FAILED=([0-9]+)$/\1 \2 \3/'
}

# forge_gate_verdict LOG N EXIT_STATUS EXPECTED_IDS -- PASS or FAIL: <reason>.
forge_gate_verdict() {
    local log=$1 n=$2 rc=$3 expected=$4 ev nums t p f want got
    [ "$rc" = 0 ] || { echo "FAIL: gate $n binary exit status $rc"; return; }
    nums=$(forge_results "$log" "$n") || { echo "FAIL: gate $n results line missing or repeated"; return; }
    read -r t p f <<< "$nums"
    [ "$f" = 0 ] && [ "$t" = "$p" ] || { echo "FAIL: gate $n results TOTAL=$t PASSED=$p FAILED=$f"; return; }
    ev=$(forge_events "$log" "$n")
    if printf '%s\n' "$ev" | awk -F'\t' '$2 != "PASS" { f = 1 } END { exit !f }'; then
        echo "FAIL: gate $n printed a FAIL line"; return
    fi
    want=$(printf '%s\n' $expected | sort)
    got=$(printf '%s\n' "$ev" | cut -f1 | sort)
    [ "$got" = "$want" ] || { echo "FAIL: gate $n observed ids differ from the expected ids"; return; }
    [ "$(printf '%s\n' "$ev" | grep -c .)" = "$t" ] || { echo "FAIL: gate $n TOTAL=$t differs from observed lines"; return; }
    echo PASS
}

# forge_body LOG G3_RC G4_RC G3_BIN_SHA G4_BIN_SHA OMEGA_DIR OMEGA_SHA PHYSICS_DIR PHYSICS_SHA RUN_ID TS
# Prints the receipt body (JSON object, no receipt_digest). Sets FORGE_STATUS.
forge_body() {
    local log=$1 g3rc=$2 g4rc=$3 g3sha=$4 g4sha=$5 odir=$6 osha=$7 pdir=$8 psha=$9 run_id=${10} ts=${11}
    local v3 v4 oc pc err= desc alias results np nf nt s3 s4
    v3=$(forge_gate_verdict "$log" 3 "$g3rc" "$FORGE_G3_IDS")
    v4=$(forge_gate_verdict "$log" 4 "$g4rc" "$FORGE_G4_IDS")
    desc=$(sed -n -E 's/^\[\*\] Observed Descriptor Digest: ([0-9a-f]{64})$/\1/p' "$log" | tail -n 1)
    alias=$(sed -n -E 's/^\[\*\] Derived Display Alias: ([A-Za-z0-9_]+)$/\1/p' "$log" | tail -n 1)
    oc=$(forge_tree_clean "$odir")
    pc=$(forge_tree_clean "$pdir")
    s3=${v3%%:*}; s4=${v4%%:*}
    [ "$s3" = PASS ] || err=${v3#FAIL: }
    [ "$s4" = PASS ] || err=${err:+$err; }${v4#FAIL: }
    [ "$s4" != PASS ] || [ -n "$desc" ] || { s4=FAIL; err=${err:+$err; }"gate 4 descriptor digest not printed"; }
    [ "$oc" = true ] && [ "$pc" = true ] || err=${err:+$err; }"candidate trees not clean at receipt time"
    [ "$(git -C "$odir" rev-parse HEAD 2>/dev/null)" = "${osha,,}" ] || err=${err:+$err; }"omega HEAD moved during the run"
    [ "$(git -C "$pdir" rev-parse HEAD 2>/dev/null)" = "${psha,,}" ] || err=${err:+$err; }"physics HEAD moved during the run"
    if [ -z "$err" ]; then FORGE_STATUS=PASS; else FORGE_STATUS=FAIL; fi
    results=$( { forge_events "$log" 3 | sed "s/^/$FORGE_G3\t/"; forge_events "$log" 4 | sed "s/^/$FORGE_G4\t/"; } |
        awk -F'\t' 'BEGIN { printf "[" } { printf "%s{\"suite\":\"%s\",\"id\":\"%s\",\"status\":\"%s\"}", (NR > 1 ? "," : ""), $1, $2, $3 } END { printf "]" }')
    np=$( { forge_events "$log" 3; forge_events "$log" 4; } | grep -c $'\tPASS$')
    nf=$( { forge_events "$log" 3; forge_events "$log" 4; } | grep -c $'\tFAIL$')
    nt=$((np + nf))
    printf '{"schema":"%s","gate":"%s","program":"AIEN_M19R_FOUNDATION"' "$FORGE_SCHEMA" "$FORGE_GATE"
    printf ',"run_id":"%s","timestamp_utc":"%s","status":"%s"' "$run_id" "$ts" "$FORGE_STATUS"
    printf ',"candidate_git_commit":"%s","physics_candidate_git_commit":"%s"' "${osha,,}" "${psha,,}"
    printf ',"candidate_trees_clean":{"omega":%s,"physics":%s}' "$oc" "$pc"
    printf ',"gates":{"%s":{"status":"%s","gate_binary_exit_status":%d,"candidate_binary_sha256":"%s"}' \
        "$FORGE_G3" "$s3" "$g3rc" "$g3sha"
    printf ',"%s":{"status":"%s","gate_binary_exit_status":%d,"candidate_binary_sha256":"%s"' \
        "$FORGE_G4" "$s4" "$g4rc" "$g4sha"
    [ -z "$desc" ] || printf ',"hardware_descriptor_digest":"%s"' "$desc"
    [ -z "$alias" ] || printf ',"derived_alias":"%s"' "$alias"
    printf '}}'
    [ -z "$desc" ] || printf ',"hardware_descriptor_digest":"%s"' "$desc"
    printf ',"test_results":%s,"observed_test_count":%d,"observed_pass_count":%d,"observed_fail_count":%d' \
        "$results" "$nt" "$np" "$nf"
    printf ',"gate_log_sha256":"%s"' "$(forge_sha_file "$log")"
    [ -z "$err" ] || printf ',"error":"%s"' "$(printf '%s' "$err" | tr -d '"\\')"
    printf ',"digest_meaning":"integrity only, not authenticity: re-hash blobs/<sha256>.log named here"}'
}

# forge_receipt BODY -- digest BODY with json_canon. Sets FORGE_DIGEST and
# FORGE_RECEIPT (the full receipt, receipt_digest first).
forge_receipt() {
    local body=$1
    FORGE_DIGEST=$(printf '%s' "$body" | "$JSON_CANON" --sha256) || forge_fail "receipt body is not valid JSON" || return 1
    FORGE_RECEIPT="{\"receipt_digest\":\"$FORGE_DIGEST\",${body#\{}"
}

# forge_under DIR TREE -- DIR is TREE or inside it (after resolving links).
forge_under() {
    local d t
    d=$(realpath -m "$1"); t=$(realpath -m "$2")
    [ "$d" = "$t" ] || [ "${d#"$t"/}" != "$d" ]
}

# forge_record RECEIPT LOG EVIDENCE_DIR OMEGA_DIR PHYSICS_DIR -- keep the log as
# blobs/<sha256>.log (0444) and write <evidence-dir>/<digest>.json exclusively.
forge_record() {
    local receipt=$1 log=$2 ev=$3 t sha dst tmp pretty
    for t in "$4" "$5"; do
        ! forge_under "$ev" "$t" || forge_fail "evidence dir $ev is inside $t; receipts must live outside the candidate trees" || return 1
    done
    mkdir -p "$ev/blobs" || forge_fail "cannot create $ev/blobs" || return 1
    sha=$(forge_sha_file "$log"); dst=$ev/blobs/$sha.log
    if [ -e "$dst" ]; then
        [ "$(forge_sha_file "$dst")" = "$sha" ] || forge_fail "blob $dst does not hash to its name" || return 1
    else
        tmp=$(mktemp "$ev/blobs/.tmp.XXXXXX") || return 1
        cp "$log" "$tmp" && chmod 0444 "$tmp" && mv -n "$tmp" "$dst" && [ ! -e "$tmp" ] ||
            { rm -f "$tmp"; forge_fail "cannot keep blob $dst"; return 1; }
    fi
    pretty=$FORGE_TMP/receipt.pretty
    printf '%s' "$receipt" | "$JSON_CANON" --pretty > "$pretty" || forge_fail "receipt is not valid JSON" || return 1
    "$JSON_CANON" --write-exclusive "$ev/$FORGE_DIGEST.json" < "$pretty" ||
        forge_fail "cannot write $ev/$FORGE_DIGEST.json"
}

# forge_run_section RUN_DIR -- compile and run both gates; exit statuses go to
# RUN_DIR/gate3.rc and gate4.rc. Called with the GB10 lock held.
forge_run_section() {
    local rd=$1 rc
    local cflags="-O2 -Wall -Wextra -Werror \
  -I./forge -I./nvrm -I./m16 \
  -I./third_party/nvidia-open-580.173.02/src/common/sdk/nvidia/inc \
  -I./third_party/nvidia-open-580.173.02/kernel-open/common/inc \
  -I./third_party/nvidia-open-580.173.02/kernel-open/nvidia-uvm \
  -I./third_party/nvidia-open-580.173.02/src/nvidia/arch/nvalloc/unix/include"
    local srcs="forge/forge_descriptor.c forge/forge_realize.c sha256_clean.c nvrm/nvrm.c m16/m16_native.c"
    echo 127 > "$rd/gate3.rc"; echo 127 > "$rd/gate4.rc"

    echo -e "\n[*] Compiling test_forge_hwid..."
    # shellcheck disable=SC2086
    gcc $cflags $srcs tests/test_forge_hwid.c -o tests/test_forge_hwid || return 1
    echo -e "[*] Compiling test_forge_seam..."
    # shellcheck disable=SC2086
    gcc $cflags $srcs tests/test_forge_seam.c -o tests/test_forge_seam || return 1

    echo -e "\n[*] Running Gate 4: FORGE-HWID under GPU lock..."
    rc=0; ./tests/test_forge_hwid || rc=$?; echo "$rc" > "$rd/gate4.rc"
    echo -e "\n[*] Running Gate 3: FORGE-0 Seam under GPU lock..."
    rc=0; ./tests/test_forge_seam || rc=$?; echo "$rc" > "$rd/gate3.rc"
}

forge_usage() {
    echo "usage: $0 [--omega-dir DIR --omega-candidate SHA --physics-candidate SHA [--evidence-dir DIR --record]]" >&2
    exit 2
}

forge_main() {
    local odir= osha= psha= ev= record=0 receipt_mode=0 run_id rd log g3 g4 rc body receipt ts
    while [ $# -gt 0 ]; do
        case $1 in
            --omega-dir) [ $# -ge 2 ] || forge_usage; odir=$2; shift 2;;
            --omega-candidate) [ $# -ge 2 ] || forge_usage; osha=$2; shift 2;;
            --physics-candidate) [ $# -ge 2 ] || forge_usage; psha=$2; shift 2;;
            --evidence-dir) [ $# -ge 2 ] || forge_usage; ev=$2; shift 2;;
            --record) record=1; shift;;
            *) forge_usage;;
        esac
    done
    if [ -n "$odir$osha$psha$ev" ] || [ "$record" = 1 ]; then
        [ -n "$odir" ] && [ -n "$osha" ] && [ -n "$psha" ] || forge_usage
        [ "$record" = 0 ] || [ -n "$ev" ] || forge_usage
        receipt_mode=1
        odir=$(realpath -e "$odir") || { echo "no such omega dir" >&2; exit 2; }
    fi
    cd "$FORGE_ROOT" || exit 1

    if [ "$receipt_mode" = 1 ]; then
        forge_candidate "$FORGE_ROOT" "$psha" && forge_candidate "$odir" "$osha" ||
            { echo "Refused before the run: $FORGE_ERR" >&2; exit 1; }
        forge_build_canon "$odir" || { echo "Refused before the run: $FORGE_ERR" >&2; exit 1; }
        trap 'rm -rf "$FORGE_TMP"' EXIT
    fi
    run_id=$(date -u +%Y%m%dT%H%M%SZ)-$(head -c 6 /dev/urandom | od -An -tx1 | tr -d ' \n')
    rd=$FORGE_ROOT/build/forge-runs/$run_id
    mkdir -p "$rd" || exit 1
    log=$rd/gate3_4.log

    echo "=== M19R FORGE Gates (Gate 3 & Gate 4) ==="
    echo "Timestamp: $(date -u +"%Y-%m-%dT%H:%M:%SZ")"
    echo "Host: $(uname -n) ($(uname -m))"
    echo "Run: $run_id"

    exec 9> "$FORGE_LOCK" || { echo "cannot open $FORGE_LOCK" >&2; exit 1; }
    flock -x 9 || { echo "cannot take $FORGE_LOCK" >&2; exit 1; }
    ts=$(date -u +%Y-%m-%dT%H:%M:%S.%6NZ)
    forge_run_section "$rd" 2>&1 | tee "$log"
    exec 9>&-
    g3=$(cat "$rd/gate3.rc"); g4=$(cat "$rd/gate4.rc")
    echo -e "\n=== M19R FORGE Gates Complete (gate3 exit $g3, gate4 exit $g4) ==="
    rc=0
    [ "$g3" = 0 ] && [ "$g4" = 0 ] || rc=1
    [ "$receipt_mode" = 1 ] || { echo "No receipt written (no candidates given)."; exit "$rc"; }

    body=$(forge_body "$log" "$g3" "$g4" \
        "$( [ -f tests/test_forge_seam ] && forge_sha_file tests/test_forge_seam)" \
        "$( [ -f tests/test_forge_hwid ] && forge_sha_file tests/test_forge_hwid)" \
        "$odir" "$osha" "$FORGE_ROOT" "$psha" "$run_id" "$ts")
    forge_receipt "$body" || { echo "Receipt failed: $FORGE_ERR" >&2; exit 1; }
    receipt=$FORGE_RECEIPT
    printf '%s' "$receipt" | "$JSON_CANON" --pretty > "$rd/receipt-preview.json" || exit 1
    status=$(printf '%s' "$receipt" | grep -o '"status":"[A-Z]*"' | head -n 1 | cut -d'"' -f4)
    echo "Receipt status $status, digest $FORGE_DIGEST (preview $rd/receipt-preview.json)"
    if [ "$record" = 1 ]; then
        forge_record "$receipt" "$log" "$ev" "$odir" "$FORGE_ROOT" || { echo "Record failed: $FORGE_ERR" >&2; exit 1; }
        echo "Permanent receipt: $ev/$FORGE_DIGEST.json"
    else
        echo "No permanent receipt (no --record)."
    fi
    [ "$status" = PASS ] || rc=1
    exit "$rc"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    forge_main "$@"
fi
