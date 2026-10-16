#!/bin/bash
# tests/run_submission_visibility_gates.sh -- GB10 CPU-published queue entries and rewritten command buffers.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd -P)
LOCK_FILE=/tmp/aien-gb10.lock

cd "$REPO_ROOT"

echo "=== GB10 Submission Visibility Gates ==="
echo "Timestamp: $(date -u +"%Y-%m-%dT%H:%M:%SZ")"
echo "Host: $(uname -n) ($(uname -m))"

echo -e "\n[*] Building submission_visibility_gates..."
gcc -O2 -Wall -Wextra -Werror \
  -I./nvrm -I./m16 \
  -I./third_party/nvidia-open-580.173.02/src/common/sdk/nvidia/inc \
  -I./third_party/nvidia-open-580.173.02/kernel-open/common/inc \
  -I./third_party/nvidia-open-580.173.02/kernel-open/nvidia-uvm \
  -I./third_party/nvidia-open-580.173.02/src/nvidia/arch/nvalloc/unix/include \
  nvrm/nvrm.c m16/m16_native.c nvrm/submission_visibility_gates.c -o nvrm/submission_visibility_gates

echo "    submission_visibility_gates built cleanly."

if [ ! -e /dev/nvidia0 ]; then
  echo "SKIP: /dev/nvidia0 not present; submission visibility gates need the GB10 (build above succeeded)."
  exit 0
fi

# Under aien-test the runner already holds $LOCK_FILE for this gate (ADR 0033:
# gb10 is serialized by the trusted side) and says so with AIEN_GB10_LOCK_HELD=1.
# Taking it again would fail as aien-run (cannot open the worker's file) or
# block forever as the same user. By hand, without the marker, take it here.
if [ "${AIEN_GB10_LOCK_HELD:-0}" = 1 ]; then
  echo -e "\n[*] Running submission_visibility_gates (GPU lock $LOCK_FILE held by the caller)..."
  ./nvrm/submission_visibility_gates
else
  echo -e "\n[*] Running submission_visibility_gates under GPU lock ($LOCK_FILE)..."
  flock "$LOCK_FILE" ./nvrm/submission_visibility_gates
fi

echo -e "\n=== GB10 Submission Visibility Gates: done ==="
