# Sovereign Tooling Transition Plan: Milestone 2 (PHYSICS)
Tracking: aien-dev/aien-architecture#12, aien-dev/physics#8

## Context & Sovereignty Rule
Per the Sovereign Machine doctrine, qualification and build tooling must be local, in-house, and offline, with zero Python in the trusted path.
Milestone 2 previously relied on Python for canonical binary generation, variant matrix builds, static audits, QEMU dynamic harnesses, and gate orchestration (`build.py`, `run_milestone2_gates.py`, `seam1_static_audit.py`, `seam2_dynamic_harness.py`, `verify_golden.py`, `verify_unit.py`, `verify_trap.py`, `verify_no_auth.py`, `generate_canary.py`, `generate_snippet.py`, `generate_audit_ledger.py`).

## Implementation Summary
A complete, pure C (C11) and POSIX shell replacement suite has been designed and implemented in `aien-dev/physics`:
1. `tools/m2tool.c`: Pure C host qualification tool compiled via host `gcc -O2` and linked directly with audited `sha256_clean.c`.
   - Subcommands: `sha256`, `hexfield`, `bytes`, `pin-inc`, `gen-canary`, `gen-snippet`, `audit-verify`, `verify-golden`, `verify-unit`, `verify-trap`, `verify-no-auth`, `get-symbol`.
2. `tools/qemu_inspect.sh`: Sovereign POSIX shell QEMU inspection harness using QMP socket `pmemsave` to inspect bare-metal execution state directly from physical memory.
3. `m2_build.sh` (symlinked as `build.sh`): Master build pipeline that:
   - Builds canonical `physics.bin` from source.
   - Extracts symbol addresses, produces `physics_pin.inc`, and builds canonical `atlas_m2.bin`.
   - Assembles and links qualification test variants: `unit`, `trap_brk`, `trap_align`, and the 22 hostile atlas defsym variants.
4. `run_m2_gates.sh` (symlinked as `run_milestone2_gates.sh`): Master POSIX shell gate runner orchestrating Seam 1 static audits and Seam 2 dynamic harnesses.

## Artifact Integrity & Receipt Preservation
- The canonical binary artifacts are verified byte-identical to the qualified binaries:
  - `physics.bin`: 6,144 bytes, SHA-256 `4b22a08c02c6d482d8c39db51d8b6daafdfb5fbe4c47ea41444743e498c8a141`
  - `atlas_m2.bin`: 1,764 bytes, SHA-256 `1d6d75a6f29631627f7f0df88e99990526a6e2e59178652d880d32f504ca638d`
- The existing qualification receipt `qualification_receipt.json` remains **untouched** as historical evidence for the Python-based qualification.
- The new C/shell tooling was verified to produce 100% gate pass rate across all 15 gates:
  1. `PHYSICS_ARTIFACT_IDENTITY_PASS`
  2. `PHYSICS_CANARY_INTEGRITY_PASS`
  3. `PHYSICS_DISJOINT_MEMORY_PASS`
  4. `PHYSICS_HANDOFF_ABI_PASS`
  5. `PHYSICS_AUDIT_PASS`
  6. `PHYSICS_STEP_VERIFICATION_PASS`
  7. `PHYSICS_BRANCH_FREE_PASS`
  8. `PHYSICS_GOLDEN_BOOT_PASS`
  9. `PHYSICS_UNIT_TEST_PASS` (13/13 unit tests pass)
  10. `PHYSICS_TRAP_ALIGNMENT_PASS`
  11. `PHYSICS_TRAP_BREAKPOINT_PASS`
  12. `PHYSICS_AUTH_FAIL_ENTRY_PASS`
  13. `PHYSICS_AUTH_FAIL_STACK_PASS`
  14. `PHYSICS_AUTH_FAIL_DESCRIPTOR_PASS`
  15. `PHYSICS_AUTH_FAIL_MUTATION_PASS` (20/20 hostile corrupt ingress cases refused)

All 15 gates complete execution in ~16.2 seconds with zero Python dependencies.

## Transition Phasing
1. **Phase 1 (Dual-Track Availability - Present)**:
   - Pure C and shell tooling (`tools/m2tool.c`, `tools/qemu_inspect.sh`, `m2_build.sh`, `run_m2_gates.sh`) are active.
   - Developers and automated CI can build and verify all gates using `./run_m2_gates.sh` without Python.
   - Legacy Python scripts remain available as historical reference.
2. **Phase 2 (Formal Requalification Decision)**:
   - When recorded in `aien-architecture#12`, `run_m2_gates.sh --write-receipt` generates a refreshed receipt reflecting the pure C/shell verification toolchain.
3. **Phase 3 (Python Retirement & Archival)**:
   - Legacy Python files are moved to `history/legacy-python/` or deleted, completing total retirement of Python from the sovereign path.
