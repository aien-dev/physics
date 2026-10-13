#!/bin/bash
# tests/run_forge_aegis_host_gates.sh -- host-only checks of the FORGE AEGIS
# verifier and the submit-side refusals (physics #53). No GPU, no NVRM calls,
# no GPU lock: every submit in the test is refused before hardware access.
# Exit code 0 only when every check passes.
set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd -P)
cd "$REPO_ROOT" || exit 1

BUILD_DIR="$REPO_ROOT/build"
BIN="$BUILD_DIR/test_forge_aegis_host"
mkdir -p "$BUILD_DIR"

# Same flags and sources as the GB10 seam build in tests/run_forge_gates.sh.
CFLAGS="-O2 -Wall -Wextra -Werror \
  -I./forge -I./nvrm -I./m16 \
  -I./third_party/nvidia-open-580.173.02/src/common/sdk/nvidia/inc \
  -I./third_party/nvidia-open-580.173.02/kernel-open/common/inc \
  -I./third_party/nvidia-open-580.173.02/kernel-open/nvidia-uvm \
  -I./third_party/nvidia-open-580.173.02/src/nvidia/arch/nvalloc/unix/include"
SRCS="forge/forge_descriptor.c forge/forge_realize.c sha256_clean.c nvrm/nvrm.c m16/m16_native.c"

echo "=== FORGE AEGIS host gates ==="
# shellcheck disable=SC2086
if ! gcc $CFLAGS $SRCS tests/test_forge_aegis_host.c -o "$BIN"; then
    echo "FAIL build test_forge_aegis_host"
    exit 1
fi
echo "PASS build test_forge_aegis_host"
"$BIN"
