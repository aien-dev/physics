# PHYSICS

Two things live here: the **FORGE** machine realization layer (C), and the historical **Atlas / PHYSICS boot artifacts** (AArch64 assembly) with their evidence. FORGE turns a verified Omega meaning into a concrete realization on the machine at hand (today CPU and the NVIDIA GB10 GPU; a simulated analog provider exists for testing). It is realization and lowering, not a security gatekeeper: AEGIS verifies, and AIENOS owns the trusted substrate.

```text
ATLAS AWAKENS. AIEN PROPOSES. OMEGA DEFINES. FORGE REALIZES. AEGIS VERIFIES. HARDWARE ACTS. EVIDENCE TEACHES.
```

Historical `PHYSICS_*` milestone names (for example `PHYSICS_BOOT`) are unchanged in evidence; the subsystem role was renamed FORGE by ADR 0014 in aien-architecture.

## Current state

Research-grade and pre-alpha.

- **FORGE:** the realize/verify seam (Gates 3 and 4) and the substrate-neutral descriptor contract (V1 and V2) pass their gates. The analog provider is a simulation checked against a digital oracle; no analog hardware is involved. Status and receipts: [CURRENT_EXECUTION_PLAN.md](https://github.com/aien-dev/aien-architecture/blob/main/CURRENT_EXECUTION_PLAN.md) and [doctrine/ROADMAP.md](https://github.com/aien-dev/aien-architecture/blob/main/doctrine/ROADMAP.md) in aien-architecture.
- **Atlas boot, PHYSICS boot and PHYSICS effects (Milestones 1 to 3):** qualified in QEMU only (AArch64 virt). Native DGX Spark qualification of PHYSICS (`PHYSICS_BOOT_NATIVE_PASS`) has not been done and is never inferred from an emulator pass. The committed `qualification_receipt.json` is historical evidence from a retired Python harness and is kept unchanged ([TRANSITION_PLAN_M2.md](TRANSITION_PLAN_M2.md)).
- **Physics Zero** (a clean-room scientific discovery program, distinct from this repository's PHYSICS) and **DIRAC-0** are documents in aien-architecture only. No code here implements them.

## How this fits with the other repositories

[omega](https://github.com/aien-dev/omega) pins this repository by commit (`physics.lock`) and uses `nvrm/` and `m16/` for the native GB10 path. [aienos](https://github.com/aien-dev/aienos) is the kernel. [aien-architecture](https://github.com/aien-dev/aien-architecture) owns status.

## Standing rules

C is the target language, with assembly only where measured. No Python (the old Python harness was removed; tools are C or shell). No CUDA toolkit or CUDA library dependence: the GB10 is driven natively through its open kernel interface. No systemd. Offline builds. Never overwrite old receipts.

## Build and verify

Needs gcc, QEMU for AArch64 and `socat` (the QEMU inspector uses QMP). Host and emulator gates:

```bash
tests/run_forge_v2_gates.sh          # FORGE V2 known-answer tests
tests/run_forge_analog_sim_gates.sh  # simulated analog provider vs digital oracle
./run_m2_gates.sh                    # M2 PHYSICS_BOOT, 15 gates in QEMU, rebuilt from source
m3/tests/run_m3_gates.sh             # M3 PHYSICS_EFFECTS, 19 gates in QEMU
```

`./run_m2_gates.sh` leaves the committed receipt untouched unless `--write-receipt` is passed, which is only for a recorded requalification decision. Gates that need the real GB10 (`tests/run_forge_gates.sh`, `tests/run_m15_gates.sh`, `tests/run_m16_requalification.sh`, `tests/run_nvrm_lifecycle_gates.sh`) run on the DGX Spark one at a time and are never killed mid-run; CI declares them SKIP, never PASS.

## Contributing

Open a pull request with the command you ran and its output. Do not edit committed receipts or binaries without the matching rebuilt evidence. Contact: aien@aienos.com.

## Crumbs

This repo uses the Crumb Protocol (RFC-0001). Each directory carries a `.crumb` file (what the directory is for, its rules, and a backfilled history). Live coordination between agents goes in `.crumb.local`, which is never committed.
Before editing a directory, read its `.crumb`. Rules and the `crumb` tool: https://github.com/aien-dev/aien-architecture/blob/main/docs/CRUMB_PROTOCOL.md
Backfilled history (marked as such, not live whispers) is in `docs/crumbs/BACKFILL.md`.
