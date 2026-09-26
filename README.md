# PHYSICS: Trusted Machine Authority

```text
SILICON / UNAVOIDABLE FIRMWARE
              ↓
            ATLAS
       BOOTSTRAP SEED
              ↓
           PHYSICS
   TRUSTED MACHINE AUTHORITY
              ↓
            OMEGA
 SEMANTICS + SYNTHESIS + REALIZATION
              ↓
             AIEN
 SOVEREIGN DISCOVERY INTELLIGENCE
```

## The Canonical Statements
```text
ATLAS AWAKENS.
PHYSICS AUTHORIZES.
OMEGA DEFINES, SYNTHESIZES, VERIFIES, AND REALIZES.
AIEN OBSERVES, THINKS, HYPOTHESIZES, SEARCHES, DISCOVERS, AND INVENTS.
```

## The Sovereign Procedural Creed
> **`WEIGHTS SUGGEST.`**  
> **`PROGRAMS EXPLAIN.`**  
> **`OMEGA VERIFIES.`**  
> **`PHYSICS AUTHORIZES.`**  
> **`EXPERIMENTS FALSIFY.`**  
> **`EVIDENCE TEACHES.`**

---

## Canonical Distinction: PHYSICS vs. PHYSICS ZERO
- **`PHYSICS`**: **The Trusted Machine Authority.** The constitutional governor of silicon, CPU exception levels, memory isolation, SMMUv3 DMA protection, device registers, and the Effect Cycle.
- **`PHYSICS ZERO`**: **The Clean-Room Scientific Discovery Program.** An autonomous epistemological methodology wherein AIEN discovers the underlying structure of reality from raw observation without ever receiving human physical theories, constants, or terminology.

---

## Canonical Artifacts & Evidence Files
- [`physics.bin`](file:///home/drakestapleton/workspace/physics/physics.bin): Trusted Machine Authority Nucleus binary (6,144 bytes).
- [`physics.sha256`](file:///home/drakestapleton/workspace/physics/physics.sha256): Cryptographic hash (see `physics.sha256`; pinned into Atlas via `physics_pin.inc`).
- [`physics.manifest`](file:///home/drakestapleton/workspace/physics/physics.manifest): Canonical manifest and provenance record anchored to Atlas.
- [`physics.memory-map`](file:///home/drakestapleton/workspace/physics/physics.memory-map): Normative physical memory layout with formal pairwise disjointness proof.
- [`physics.control-flow`](file:///home/drakestapleton/workspace/physics/physics.control-flow): Textual/graphical control flow graph and fail-closed panic traps.
- [`physics.audit`](file:///home/drakestapleton/workspace/physics/physics.audit): 100% byte-reconciled machine operation ledger (464 instructions, 170 rodata words, 390 canonical padding words, 512 vector table words, 0 discrepancy).
- [`physics.decode`](file:///home/drakestapleton/workspace/physics/physics.decode): Disassembly dump with static target proof.
- [`machine_contract.json`](file:///home/drakestapleton/workspace/physics/machine_contract.json): Formal machine contract (`CONTRACT-QEMU-VIRT-AARCH64-M2`).
- [`qualification_receipt.json`](file:///home/drakestapleton/workspace/physics/qualification_receipt.json): Cryptographic qualification receipt for Milestone 2 (`PHYSICS_BOOT`).

---

## Qualification Architecture (Dual Verification Seams)

### Seam 1: Independent Artifact-Audit Seam (`seam1_physics_audit.py`)
Validates static artifact properties without requiring execution:
1. `PHYSICS_ARTIFACT_IDENTITY_PASS`: `physics.bin` matches `physics.sha256`.
2. `PHYSICS_MACHINE_CONTRACT_PASS`: Code size $\le 32\text{ KiB}$ (actual: 1,856 B), total image $\le 64\text{ KiB}$ (actual: 6,144 B), anti-bloat law satisfied.
3. `PHYSICS_AUDIT_PASS`: Generalized byte accounting formula reconciled to exact 0 bytes discrepancy:
   $$\text{CODE} (1856) + \text{RODATA} (680) + \text{PAD} (1560) + \text{VBAR} (2048) = 6144\text{ bytes}$$
4. `PHYSICS_VECTOR_LAYOUT_PASS`: Exception vector table base satisfies strict 2 KiB alignment, spans exactly 2,048 bytes (`0x000`..`0x7FF`), with all 16 architectural slots verified at 128-byte stride.
5. `PHYSICS_VECTOR_ALIGNMENT_PASS`: Vector base `0x40201000 % 2048 == 0`.
6. `PHYSICS_MEMORY_DISJOINTNESS_PASS`: Static mathematical proof that all 7 physical memory regions (`IMAGE`, `VBAR`, `STACK`, `STATE`, `CAP_TABLE`, `STATIC_DATA`, `FREE_FRAME_REGION`) are pairwise disjoint.
7. `PHYSICS_STATIC_MEMORY_BOUNDS_PASS`: Memory operations confined strictly to declared regions; initial kernel SP is `0x40205800`.

### Seam 2: External Execution Seam (`seam2_physics_harness.py`)
Boots PHYSICS in QEMU virt (AArch64, cortex-a57, 128 MiB) and qualifies it by reading guest memory over QMP (`xp`), not by trusting UART strings. Every image is rebuilt from source on every run (`m2_build.py`); nothing is cached.
1. `PHYSICS_ENTRY_EL_PASS`: recorded `CurrentEL` (boot state +0x40) equals the contract's `expected_entry_el_value` (EL1); a real EL2 entry (`virt,virtualization=on`) is refused with `PANIC_UNCONTRACTED_EL` before any authority exists.
2. `PHYSICS_DESCRIPTOR_INGRESS_PASS`: the PHYSICS-owned 64-byte descriptor copy equals the contract descriptor word for word; every header (magic, version, length, reserved) and machine-profile violation is refused before bitmap initialization.
3. `PHYSICS_BOOT_QEMU_PASS`: `physics.bin` and `atlas_m2.bin` rebuild byte-identically; telemetry is exactly the golden sequence; allocator header is exactly `DRAM_END=0x48000000`, 32,248 frames; no exception taken.
4. `PHYSICS_FRAME_BOUNDS_PASS`: in-kernel exhaustion allocates exactly 32,248 frames, each previous + 4 KiB from `0x40208000` (deterministic, no duplicates), last `0x47FFF000`, then `FRAME_EXHAUSTED`; all 4,031 bitmap bytes set; a canary byte after the bitmap and the whole first free frame stay untouched.
5. `PHYSICS_RESERVED_FRAME_REFUSAL_PASS`: 13 `is_frame_reserved` probes across Atlas, descriptor, image, vectors, boot state, capability table, static data, `DRAM_END` and wraparound.
6. `PHYSICS_CAP_ROOT_PASS`: all 10 CAP_ROOT fields (slot, generation, principal, `resource_type=1`, ops, bounds, revocation, attenuation, 32-byte Atlas digest) equal the contract; 10 `validate_capability` probes show the root grants nothing outside `[FREE_FRAME_BASE, DRAM_END)`.
7. `PHYSICS_EXCEPTION_STATE_CAPTURE_PASS`: two faults (BRK and an alignment Data Abort) with sentinel values in x0..x30; the trap frame at `0x40205880` holds every sentinel, the interrupted SP, vector slot 4, `CurrentEL`, ESR, ELR (== fault site symbol), SPSR `0x3C5`, FAR and FAR_VALID (0 for BRK, 1 with the faulting address for the abort).
8. `PHYSICS_CORRUPTION_REFUSAL_PASS`: 23 hostile cases (cookie, payload size, descriptor pointer, magic, version, length, reserved bits, zero/overflowing/oversized/undersized/shifted/out-of-profile RAM, UART, PHYSICS base/size, EL2 entry) each end in the expected panic with the entry record, allocator header and CAP_ROOT slot still zero in memory.

`run_milestone2_gates.py` deletes `build/`, regenerates every artifact from source, runs both seams, and writes `qualification_receipt.json` bound to the SHA-256 of the contract, audit, gate scripts, sources and both artifacts, plus the QEMU version and base commit.

---

## Native Hardware Qualification Status
> [!IMPORTANT]
> **`PHYSICS_BOOT_NATIVE_PASS` remains strictly decoupled from QEMU qualification.**  
> Native silicon verification on NVIDIA DGX Spark hardware will be evaluated independently. Virtual emulation passes must never be conflated with physical silicon qualification.

---

## Reproduction & Verification
To execute the complete Milestone 2 qualification suite:
```bash
python3 run_milestone2_gates.py
```
Outputs formal qualification receipt to `qualification_receipt.json`.
