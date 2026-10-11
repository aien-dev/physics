#!/usr/bin/env bash
# tests/gates/run.sh: the one entry point for the local verification gates of physics.
# Every step runs the SAME command as the hosted job it mirrors (.github/workflows/host-gates.yml
# matrix, with working-directory physics and OMEGA_DIR pointing at a sibling omega checkout, and
# the SKIP jobs that only ever name their GB10/NVRM script). Manifests under tests/gates/ name
# this script plus one step word, so no manifest needs shell expansion (ADR 0028 P6).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
step="${1:?usage: tests/gates/run.sh <step>}"
obs() { printf 'AIEN-OBS %s %s\n' "$1" "$2"; }

# Hosted layout: physics/ with sibling checkouts omega/ (main) and aien-architecture/ (main).
# Several gate scripts look for ../omega (or $OMEGA_DIR) and ../aien-architecture.
need_sibling() {
  local name="$1"
  if [ ! -d "../$name" ]; then
    echo "run.sh: ../$name is missing; the hosted job checks out aien-dev/$name (main) as a sibling" >&2
    exit 3
  fi
  obs "sibling_${name//-/_}_commit" "\"$(git -C "../$name" rev-parse HEAD 2>/dev/null || echo unknown)\""
}
export OMEGA_DIR="${OMEGA_DIR:-$ROOT/../omega}"

case "$step" in
  crumb-verify)
    # Hosted crumb.yml: crumb tool built from the aien-architecture commit in crumb.lock.
    "${CRUMB:-crumb}" verify .
    ;;
  forge-v2)        tests/run_forge_v2_gates.sh ;;
  analog-sim)      need_sibling omega; tests/run_forge_analog_sim_gates.sh ;;
  form0)           need_sibling omega; tests/run_forge_form_gates.sh ;;
  gate34-writer)   need_sibling omega; tests/test_forge_gates_receipt.sh "$OMEGA_DIR" ;;
  m2)              ./run_m2_gates.sh ;;
  m3)              need_sibling aien-architecture; m3/tests/run_m3_gates.sh ;;
  # The five jobs below are SKIP on GitHub (if: false, never green). Their manifests declare
  # the real requirement (gb10 or nvrm) and the commands are the scripts those jobs name.
  forge-gate34)    tests/run_forge_gates.sh ;;
  m15)             tests/run_m15_gates.sh ;;
  m16-requal)      tests/run_m16_requalification.sh ;;
  nvrm-lifecycle)  tests/run_nvrm_lifecycle_gates.sh ;;
  submission-visibility) tests/run_submission_visibility_gates.sh ;;
  zero-disk-secrets)
    # Not a hosted step in this repository; the same invariant the Rust repos enforce.
    echo "Auditing repository for prohibited plaintext secret files..."
    if find . -maxdepth 4 -name ".env*" -not -path "*/.git/*" -not -path "*/build/*" | grep -q .; then
      echo "::error::Prohibited plaintext .env file found. Use an approved AIEN SecretProvider."
      obs env_files_found true
      exit 1
    fi
    obs env_files_found false
    echo "Zero plaintext .env files verified."
    ;;
  *)
    echo "run.sh: unknown step '$step'" >&2
    exit 2
    ;;
esac
