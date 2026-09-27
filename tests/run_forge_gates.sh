#!/bin/bash
# tests/run_forge_gates.sh -- Gate 3 (FORGE-0) and Gate 4 (FORGE-HWID) runner
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd -P)
LOCK_FILE=/tmp/aien-gb10.lock

cd "$REPO_ROOT"

echo "=== M19R FORGE Gates (Gate 3 & Gate 4) ==="
echo "Timestamp: $(date -u +"%Y-%m-%dT%H:%M:%SZ")"
echo "Host: $(uname -n) ($(uname -m))"

CFLAGS="-O2 -Wall -Wextra -Werror \
  -I./forge -I./nvrm -I./m16 \
  -I./third_party/nvidia-open-580.173.02/src/common/sdk/nvidia/inc \
  -I./third_party/nvidia-open-580.173.02/kernel-open/common/inc \
  -I./third_party/nvidia-open-580.173.02/kernel-open/nvidia-uvm \
  -I./third_party/nvidia-open-580.173.02/src/nvidia/arch/nvalloc/unix/include"

COMMON_SRCS="forge/forge_descriptor.c forge/forge_realize.c sha256_clean.c nvrm/nvrm.c m16/m16_native.c"

echo -e "\n[*] Compiling test_forge_hwid..."
gcc $CFLAGS $COMMON_SRCS tests/test_forge_hwid.c -o tests/test_forge_hwid

echo -e "[*] Compiling test_forge_seam..."
gcc $CFLAGS $COMMON_SRCS tests/test_forge_seam.c -o tests/test_forge_seam

echo -e "\n[*] Running Gate 4: FORGE-HWID under GPU lock..."
flock "$LOCK_FILE" ./tests/test_forge_hwid

echo -e "\n[*] Running Gate 3: FORGE-0 Seam under GPU lock..."
flock "$LOCK_FILE" ./tests/test_forge_seam

echo -e "\n=== M19R FORGE Gates Complete ==="
