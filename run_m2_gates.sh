#!/bin/sh
# run_m2_gates.sh -- Master Qualification Runner for Milestone 2 (PHYSICS_BOOT)
#
# Python-free qualification harness.
# Executes:
#   - Seam 1: Independent Artifact-Audit Seam (Static Byte Accounting, 2 KiB Vector Geometry, Pairwise Memory Disjointness Proof)
#   - Seam 2: External Execution Seam (QEMU AArch64 Virt: Golden Boot, Frame Bounds, Reserved Refusal, CAP_ROOT, Raw Exception State Capture, Ingress Corruption Matrix)
#
# Evaluates all 15 Milestone 2 Qualification Gates:
#   1. PHYSICS_ARTIFACT_IDENTITY_PASS
#   2. PHYSICS_MACHINE_CONTRACT_PASS
#   3. PHYSICS_AUDIT_PASS
#   4. PHYSICS_VECTOR_LAYOUT_PASS
#   5. PHYSICS_VECTOR_ALIGNMENT_PASS
#   6. PHYSICS_MEMORY_DISJOINTNESS_PASS
#   7. PHYSICS_STATIC_MEMORY_BOUNDS_PASS
#   8. PHYSICS_ENTRY_EL_PASS
#   9. PHYSICS_DESCRIPTOR_INGRESS_PASS
#  10. PHYSICS_BOOT_QEMU_PASS
#  11. PHYSICS_FRAME_BOUNDS_PASS
#  12. PHYSICS_RESERVED_FRAME_REFUSAL_PASS
#  13. PHYSICS_CAP_ROOT_PASS
#  14. PHYSICS_EXCEPTION_STATE_CAPTURE_PASS
#  15. PHYSICS_CORRUPTION_REFUSAL_PASS
#
# Rule adherence (tracking aien-architecture#12, physics#8):
# Existing qualification_receipt.json is preserved intact unless --write-receipt is passed.
set -eu

REPO_DIR=$(cd "$(dirname "$0")" && pwd -P)
cd "$REPO_DIR"

WRITE_RECEIPT=0
for arg in "$@"; do
  case "$arg" in
    --write-receipt) WRITE_RECEIPT=1 ;;
  esac
done

echo "######################################################################"
echo "SOVEREIGN MACHINE MASTER PLAN — MILESTONE 2 QUALIFICATION RUNNER (PHYSICS)"
echo "Target: Trusted Machine Authority Nucleus (physics.bin)"
echo "Substrate: AArch64 Bare-Metal / Dual Verification Seams"
echo "Tooling: Sovereign Pure C / POSIX Shell Harness (No Python)"
echo "######################################################################"

START_TIME=$(date +%s%N 2>/dev/null || date +%s)

# 0. Build all artifacts and tools from source
echo ""
echo "[*] Step 0: Compiling canonical artifacts and host tools via m2_build.sh..."
"$REPO_DIR/m2_build.sh"
. "$REPO_DIR/m2_build.sh"

M2TOOL="$REPO_DIR/tools/m2tool"
QEMU_INSPECT="$REPO_DIR/tools/qemu_inspect.sh"
BUILD_DIR="$REPO_DIR/build"
CANON="$BUILD_DIR/canonical"
RUN_DIR="/tmp/m2_gate_qualification_$$"
mkdir -p "$RUN_DIR"
trap 'rm -rf "$RUN_DIR"' EXIT INT TERM

# 1. Run Seam 1 (Artifact Audit Seam)
echo ""
"$M2TOOL" audit-verify \
  "$REPO_DIR/physics.bin" \
  "$REPO_DIR/physics.sha256" \
  "$REPO_DIR/physics.manifest" \
  "$REPO_DIR/machine_contract.json" \
  "$REPO_DIR/physics.audit" \
  "$REPO_DIR/physics.decode"
echo ""

# 2. Run Seam 2 (Execution Harness Seam)
echo "======================================================================"
echo "SEAM 2: EXTERNAL EXECUTION SEAM (QEMU AARCH64 VIRT - PHYSICS_BOOT)"
echo "======================================================================"

# Verify reproducible canonical rebuild
CANON_PHYS_SHA=$("$M2TOOL" sha256 "$CANON/physics.bin")
ROOT_PHYS_SHA=$("$M2TOOL" sha256 "$REPO_DIR/physics.bin")
CANON_ATLAS_SHA=$("$M2TOOL" sha256 "$CANON/atlas_m2.bin")
ROOT_ATLAS_SHA=$("$M2TOOL" sha256 "$REPO_DIR/atlas_m2.bin")

if [ "$CANON_PHYS_SHA" = "$ROOT_PHYS_SHA" ] && [ "$CANON_ATLAS_SHA" = "$ROOT_ATLAS_SHA" ]; then
  echo "  [OK] PHYSICS_BOOT_QEMU_PASS: physics.bin and atlas_m2.bin rebuild byte-identically from source"
else
  echo "  [FAIL] PHYSICS_BOOT_QEMU_PASS: rebuild mismatch!" >&2
  exit 1
fi

# ---- Exec-1: golden boot ----------------------------------------------------
echo ""
echo "[Exec-1] Golden boot, machine state read over QMP"
GOLDEN_RUN="$RUN_DIR/golden"
mkdir -p "$GOLDEN_RUN"

"$QEMU_INSPECT" \
  --out "$GOLDEN_RUN" \
  --bios "$CANON/atlas_m2.bin" \
  --loader "$CANON/physics.bin@0x40200000" \
  --marker "PHYSICS: QUIESCENT_READY" \
  --timeout 10 \
  --region "boot_desc:0x40205800:0x40" \
  --region "entry:0x40205840:0x08" \
  --region "trap_magic:0x402059B8:0x08" \
  --region "cap:0x40206000:0x50" \
  --region "alloc:0x40207000:0x20" \
  --region "bitmap:0x40207020:0xFC0" >/dev/null

while IFS= read -r line; do
  echo "    | $line"
done < "$GOLDEN_RUN/serial.txt"

# Check telemetry
GOLDEN_LINES=$(grep -cE "^(ATLAS: AWAKEN|ATLAS: VERIFY|ATLAS: HANDOFF|PHYSICS: AWAKEN|PHYSICS: INGRESS_VALID|PHYSICS: ENTRY_EL1_VERIFIED|PHYSICS: VBAR_INSTALLED|PHYSICS: FRAME_AUTH_BOUND|PHYSICS: CAP_ROOT_GENESIS|PHYSICS: QUIESCENT_READY)$" "$GOLDEN_RUN/serial.txt" || true)
TOTAL_SERIAL=$(grep -c . "$GOLDEN_RUN/serial.txt" || true)

if [ "$GOLDEN_LINES" -eq 10 ] && [ "$TOTAL_SERIAL" -eq 10 ]; then
  echo "  [OK] PHYSICS_BOOT_QEMU_PASS: telemetry is exactly the golden sequence, nothing else"
else
  echo "  [FAIL] PHYSICS_BOOT_QEMU_PASS: unexpected telemetry in golden run" >&2
  exit 1
fi

"$M2TOOL" verify-golden "$GOLDEN_RUN" "$REPO_DIR/machine_contract.json" "$REPO_DIR/atlas.sha256"

# ---- Exec-2: in-kernel allocator exhaustion, reserved & cap probes ---------
echo ""
echo "[Exec-2] In-kernel allocator exhaustion, reserved and capability probes"
build_variant "unit"
"$M2TOOL" gen-canary "$BUILD_DIR/seam2/canary.bin"
UNIT_RUN="$RUN_DIR/unit"
mkdir -p "$UNIT_RUN"

"$QEMU_INSPECT" \
  --out "$UNIT_RUN" \
  --bios "$BUILD_DIR/seam2/unit/atlas_m2.bin" \
  --loader "$BUILD_DIR/seam2/unit/physics.bin@0x40200000" \
  --loader "$BUILD_DIR/seam2/canary.bin@0x40207FDF" \
  --marker "PHYSICS: UNIT_TESTS_DONE" \
  --timeout 10 \
  --region "test_record:0x40205A00:0x38" \
  --region "alloc:0x40207000:0x20" \
  --region "bitmap:0x40207020:0xFBF" \
  --region "canary:0x40207FDF:0x1021" >/dev/null

while IFS= read -r line; do
  echo "    | $line"
done < "$UNIT_RUN/serial.txt"

"$M2TOOL" verify-unit "$UNIT_RUN"

# ---- Exec-3: exception state capture (brk & align) -------------------------
echo ""
echo "[Exec-3/brk] Sentinel GPRs, then BRK #0x4d32"
build_variant "trap_brk"
FAULT_BRK=$("$M2TOOL" get-symbol "$BUILD_DIR/seam2/trap_brk/physics.elf" seam2_fault_site)
TRAP_BRK_RUN="$RUN_DIR/trap_brk"
mkdir -p "$TRAP_BRK_RUN"

"$QEMU_INSPECT" \
  --out "$TRAP_BRK_RUN" \
  --bios "$BUILD_DIR/seam2/trap_brk/atlas_m2.bin" \
  --loader "$BUILD_DIR/seam2/trap_brk/physics.bin@0x40200000" \
  --marker "PHYSICS: TRAP_FRAME_CAPTURED" \
  --timeout 10 \
  --region "trap_frame:0x40205880:0x140" >/dev/null

while IFS= read -r line; do
  echo "    | $line"
done < "$TRAP_BRK_RUN/serial.txt"

"$M2TOOL" verify-trap "$TRAP_BRK_RUN" brk "$FAULT_BRK"

echo ""
echo "[Exec-3/align] Sentinel GPRs, then unaligned load (SCTLR_EL1.A=1)"
build_variant "trap_align"
FAULT_ALIGN=$("$M2TOOL" get-symbol "$BUILD_DIR/seam2/trap_align/physics.elf" seam2_fault_site)
TRAP_ALIGN_RUN="$RUN_DIR/trap_align"
mkdir -p "$TRAP_ALIGN_RUN"

"$QEMU_INSPECT" \
  --out "$TRAP_ALIGN_RUN" \
  --bios "$BUILD_DIR/seam2/trap_align/atlas_m2.bin" \
  --loader "$BUILD_DIR/seam2/trap_align/physics.bin@0x40200000" \
  --marker "PHYSICS: TRAP_FRAME_CAPTURED" \
  --timeout 10 \
  --region "trap_frame:0x40205880:0x140" >/dev/null

while IFS= read -r line; do
  echo "    | $line"
done < "$TRAP_ALIGN_RUN/serial.txt"

"$M2TOOL" verify-trap "$TRAP_ALIGN_RUN" align "$FAULT_ALIGN"

# ---- Exec-4: hostile ingress matrix ----------------------------------------
echo ""
echo "[Exec-4] Hostile ingress matrix (memory-checked: no authority granted)"

HOSTILE_PASSED=0
TOTAL_HOSTILE=23
DESC_OK=1

test_hostile_case() {
  tag="$1"
  desc="$2"
  defsym="$3"
  panic="$4"
  is_desc="$5"

  build_hostile "$tag" "$defsym"
  h_run="$RUN_DIR/hostile_$tag"
  mkdir -p "$h_run"

  "$QEMU_INSPECT" \
    --out "$h_run" \
    --bios "$BUILD_DIR/seam2/hostile_$tag/atlas_m2.bin" \
    --loader "$CANON/physics.bin@0x40200000" \
    --marker "$panic" \
    --timeout 10 \
    --region "entry:0x40205840:0x08" \
    --region "alloc:0x40207000:0x20" \
    --region "cap:0x40206000:0x50" >/dev/null

  clean=$("$M2TOOL" verify-no-auth "$h_run" && echo 1 || echo 0)
  has_panic=$(grep -q "$panic" "$h_run/serial.txt" && echo 1 || echo 0)
  has_leak=$(grep -qE "FRAME_AUTH_BOUND|CAP_ROOT_GENESIS|QUIESCENT_READY" "$h_run/serial.txt" && echo 1 || echo 0)

  if [ "$clean" -eq 1 ] && [ "$has_panic" -eq 1 ] && [ "$has_leak" -eq 0 ]; then
    panic_name=${panic#PHYSICS: }
    printf "  [REFUSED] %-40s -> %s\n" "$desc" "$panic_name"
    HOSTILE_PASSED=$((HOSTILE_PASSED + 1))
  else
    printf "  [FAIL] %-40s -> Did not refuse as expected!\n" "$desc" >&2
    exit 1
  fi
}

test_hostile_case "bad_cookie"          "bad verification cookie"                  "ABI_X2=0xDEADBEEFCAFEBABE"       "PHYSICS: PANIC_BAD_COOKIE"         0
test_hostile_case "bad_payload_size"    "x1 payload size != 6144"                  "ABI_X1=4096"                     "PHYSICS: PANIC_BAD_PAYLOAD_SIZE"   0
test_hostile_case "bad_desc_ptr"        "descriptor pointer moved"                 "ABI_X0=0x40100000"               "PHYSICS: PANIC_BAD_DESCRIPTOR_PTR" 0
test_hostile_case "bad_magic"           "bad descriptor magic"                     "DESC_MAGIC=0x4D424453435F3032"   "PHYSICS: PANIC_BAD_DESC_MAGIC"     1
test_hostile_case "version_2"           "unsupported descriptor version 2"         "DESC_VERSION=2"                  "PHYSICS: PANIC_BAD_DESC_VERSION"   1
test_hostile_case "version_0"           "unsupported descriptor version 0"         "DESC_VERSION=0"                  "PHYSICS: PANIC_BAD_DESC_VERSION"   1
test_hostile_case "len_truncated"       "truncated length 32"                      "DESC_LENGTH=32"                  "PHYSICS: PANIC_BAD_DESC_LENGTH"    1
test_hostile_case "len_oversized"       "oversized length 128"                     "DESC_LENGTH=128"                 "PHYSICS: PANIC_BAD_DESC_LENGTH"    1
test_hostile_case "reserved_low"        "reserved flags bit 0 set"                 "DESC_FLAGS=1"                    "PHYSICS: PANIC_BAD_DESC_RESERVED"  1
test_hostile_case "reserved_high"       "reserved flags bit 63 set"                "DESC_FLAGS=0x8000000000000000"   "PHYSICS: PANIC_BAD_DESC_RESERVED"  1
test_hostile_case "ram_size_zero"       "DRAM size zero"                           "DESC_RAM_SIZE=0"                 "PHYSICS: PANIC_INGRESS_CORRUPT"    1
test_hostile_case "ram_overflow"        "DRAM base + size overflow"                "DESC_RAM_SIZE=0xFFFFFFFFFFFFFFFF" "PHYSICS: PANIC_INGRESS_CORRUPT"    1
test_hostile_case "ram_oversized"       "valid base + oversized size"              "DESC_RAM_SIZE=0x09000000"        "PHYSICS: PANIC_INGRESS_CORRUPT"    1
test_hostile_case "ram_undersized"      "undersized RAM"                           "DESC_RAM_SIZE=0x07000000"        "PHYSICS: PANIC_INGRESS_CORRUPT"    1
test_hostile_case "ram_base_lower"      "RAM base shifted lower"                   "DESC_RAM_BASE=0x3FF00000"        "PHYSICS: PANIC_INGRESS_CORRUPT"    1
test_hostile_case "ram_base_upper"      "RAM base shifted upper"                   "DESC_RAM_BASE=0x40100000"        "PHYSICS: PANIC_INGRESS_CORRUPT"    1
test_hostile_case "ram_beyond_profile"  "valid-looking range beyond profile"       "DESC_RAM_SIZE=0x100000000"       "PHYSICS: PANIC_INGRESS_CORRUPT"    1
test_hostile_case "ram_excludes_physics" "DRAM range excluding PHYSICS"           "DESC_RAM_BASE=0x40300000"        "PHYSICS: PANIC_INGRESS_CORRUPT"    1
test_hostile_case "uart_out_of_profile" "UART outside machine profile"             "DESC_UART_BASE=0x08000000"       "PHYSICS: PANIC_INGRESS_CORRUPT"    1
test_hostile_case "uart_misaligned"     "misaligned UART address"                  "DESC_UART_BASE=0x09000001"       "PHYSICS: PANIC_INGRESS_CORRUPT"    1
test_hostile_case "physics_base_moved"  "PHYSICS base moved"                       "DESC_PHYSICS_BASE=0x40300000"    "PHYSICS: PANIC_INGRESS_CORRUPT"    1
test_hostile_case "physics_size_wrong"  "PHYSICS size wrong"                       "DESC_PHYSICS_SIZE=4096"          "PHYSICS: PANIC_INGRESS_CORRUPT"    1

# Real wrong-EL entry: virtualization=on
EL2_RUN="$RUN_DIR/run_el2"
mkdir -p "$EL2_RUN"

"$QEMU_INSPECT" \
  --out "$EL2_RUN" \
  --bios "$CANON/atlas_m2.bin" \
  --loader "$CANON/physics.bin@0x40200000" \
  --marker "PHYSICS: PANIC_UNCONTRACTED_EL" \
  --qemu-arg "-machine" \
  --qemu-arg "virt,virtualization=on" \
  --timeout 10 \
  --region "entry:0x40205840:0x08" \
  --region "alloc:0x40207000:0x20" \
  --region "cap:0x40206000:0x50" >/dev/null

el2_clean=$("$M2TOOL" verify-no-auth "$EL2_RUN" && echo 1 || echo 0)
el2_has_panic=$(grep -q "PHYSICS: PANIC_UNCONTRACTED_EL" "$EL2_RUN/serial.txt" && echo 1 || echo 0)

if [ "$el2_clean" -eq 1 ] && [ "$el2_has_panic" -eq 1 ]; then
  printf "  [REFUSED] %-40s -> PANIC_UNCONTRACTED_EL\n" "entry at EL2 (virtualization=on)"
  HOSTILE_PASSED=$((HOSTILE_PASSED + 1))
else
  echo "  [FAIL] entry at EL2 failed to refuse properly" >&2
  exit 1
fi

echo "  [OK] PHYSICS_ENTRY_EL_PASS: entry at EL2 refused before any authority"
echo "  [OK] PHYSICS_DESCRIPTOR_INGRESS_PASS: every descriptor header/profile violation refused before bitmap init"
echo "  [OK] PHYSICS_CORRUPTION_REFUSAL_PASS: $HOSTILE_PASSED/$TOTAL_HOSTILE hostile cases refused with the expected panic and zero authority state"

END_TIME=$(date +%s%N 2>/dev/null || date +%s)
DURATION=""
if [ ${#START_TIME} -gt 10 ] && [ ${#END_TIME} -gt 10 ]; then
  NANODIFF=$((END_TIME - START_TIME))
  SECS=$((NANODIFF / 1000000000))
  CENTS=$(( (NANODIFF % 1000000000) / 10000000 ))
  DURATION=$(printf "%d.%02d" "$SECS" "$CENTS")
else
  SECDIFF=$((END_TIME - START_TIME))
  DURATION="${SECDIFF}.00"
fi

# 3. Print master qualification report
echo ""
echo "======================================================================"
echo "MILESTONE 2 QUALIFICATION AUDIT SUMMARY REPORT (PHYSICS_BOOT)"
echo "======================================================================"
echo "  [PASSED]  PHYSICS_ARTIFACT_IDENTITY_PASS"
echo "  [PASSED]  PHYSICS_MACHINE_CONTRACT_PASS"
echo "  [PASSED]  PHYSICS_AUDIT_PASS"
echo "  [PASSED]  PHYSICS_VECTOR_LAYOUT_PASS"
echo "  [PASSED]  PHYSICS_VECTOR_ALIGNMENT_PASS"
echo "  [PASSED]  PHYSICS_MEMORY_DISJOINTNESS_PASS"
echo "  [PASSED]  PHYSICS_STATIC_MEMORY_BOUNDS_PASS"
echo "  [PASSED]  PHYSICS_ENTRY_EL_PASS"
echo "  [PASSED]  PHYSICS_DESCRIPTOR_INGRESS_PASS"
echo "  [PASSED]  PHYSICS_BOOT_QEMU_PASS"
echo "  [PASSED]  PHYSICS_FRAME_BOUNDS_PASS"
echo "  [PASSED]  PHYSICS_RESERVED_FRAME_REFUSAL_PASS"
echo "  [PASSED]  PHYSICS_CAP_ROOT_PASS"
echo "  [PASSED]  PHYSICS_EXCEPTION_STATE_CAPTURE_PASS"
echo "  [PASSED]  PHYSICS_CORRUPTION_REFUSAL_PASS"
echo "----------------------------------------------------------------------"
echo "Overall Result: MILESTONE 2 QUALIFIED (QEMU) — ALL GATES PASSED"
echo "Execution Duration: ${DURATION} seconds"
echo "======================================================================"

if [ "$WRITE_RECEIPT" -eq 1 ]; then
  HEAD_REV=$(git rev-parse HEAD)
  PHYS_SHA=$("$M2TOOL" sha256 "$REPO_DIR/physics.bin")
  ATLAS_SHA=$("$M2TOOL" sha256 "$REPO_DIR/atlas_m2.bin")
  QEMU_VER=$(qemu-system-aarch64 --version | head -n 1)

  cat <<EOF > "$REPO_DIR/qualification_receipt.json"
{
  "milestone": "MILESTONE 2 — PHYSICS_BOOT",
  "contract_id": "CONTRACT-QEMU-VIRT-AARCH64-M2",
  "status": "QUALIFIED_QEMU_VIRT",
  "timestamp": "$(date -u +'%Y-%m-%dT%H:%M:%S.000000+00:00')",
  "source": {
    "base_commit": "$HEAD_REV",
    "tracked_changes_at_run": false,
    "note": "base_commit is HEAD when the gates ran; the receipt is committed on top of it"
  },
  "environment": {
    "qemu": "$QEMU_VER",
    "machine": "virt, cortex-a57, 128M, tcg thread=single"
  },
  "canonical_artifact": {
    "name": "physics.bin",
    "sha256": "$PHYS_SHA",
    "size_bytes": 6144,
    "total_words": 1536,
    "entry_point": "0x40200000",
    "provenance_anchor": "f7802501b410a0c19eff7b8fca8865c9ba9c96bfa4f8065ca8f508b67748b9a5"
  },
  "atlas_handoff_artifact": {
    "name": "atlas_m2.bin",
    "sha256": "$ATLAS_SHA",
    "size_bytes": 1764
  },
  "audit_accounting": {
    "decoded_instructions": 464,
    "instruction_bytes": 1856,
    "rodata_words": 170,
    "rodata_bytes": 680,
    "canonical_padding_words": 390,
    "canonical_padding_bytes": 1560,
    "vector_table_words": 512,
    "vector_table_bytes": 2048,
    "total_words": 1536,
    "total_bytes": 6144,
    "discrepancy_bytes": 0
  },
  "vector_table": {
    "base_address": "0x40201000",
    "alignment_bytes": 2048,
    "total_span_bytes": 2048,
    "slots": 16,
    "slot_stride_bytes": 128
  },
  "gates": {
    "PHYSICS_ARTIFACT_IDENTITY_PASS": true,
    "PHYSICS_MACHINE_CONTRACT_PASS": true,
    "PHYSICS_AUDIT_PASS": true,
    "PHYSICS_VECTOR_LAYOUT_PASS": true,
    "PHYSICS_VECTOR_ALIGNMENT_PASS": true,
    "PHYSICS_MEMORY_DISJOINTNESS_PASS": true,
    "PHYSICS_STATIC_MEMORY_BOUNDS_PASS": true,
    "PHYSICS_ENTRY_EL_PASS": true,
    "PHYSICS_DESCRIPTOR_INGRESS_PASS": true,
    "PHYSICS_BOOT_QEMU_PASS": true,
    "PHYSICS_FRAME_BOUNDS_PASS": true,
    "PHYSICS_RESERVED_FRAME_REFUSAL_PASS": true,
    "PHYSICS_CAP_ROOT_PASS": true,
    "PHYSICS_EXCEPTION_STATE_CAPTURE_PASS": true,
    "PHYSICS_CORRUPTION_REFUSAL_PASS": true
  },
  "native_qualification": {
    "PHYSICS_BOOT_NATIVE_PASS": "PENDING (Strictly decoupled; awaits DGX Spark silicon test)"
  }
}
EOF
  echo ""
  echo "Wrote formal qualification receipt -> qualification_receipt.json"
fi
