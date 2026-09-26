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
- [`physics.sha256`](file:///home/drakestapleton/workspace/physics/physics.sha256): Cryptographic hash (`4ab2f5abc083b1c38aa15c136a94837f0de706eb86a53915b32a27b7a516422e`).
- [`physics.manifest`](file:///home/drakestapleton/workspace/physics/physics.manifest): Canonical manifest and provenance record anchored to Atlas.
- [`physics.memory-map`](file:///home/drakestapleton/workspace/physics/physics.memory-map): Normative physical memory layout with formal pairwise disjointness proof.
- [`physics.control-flow`](file:///home/drakestapleton/workspace/physics/physics.control-flow): Textual/graphical control flow graph and fail-closed panic traps.
- [`physics.audit`](file:///home/drakestapleton/workspace/physics/physics.audit): 100% byte-reconciled machine operation ledger (370 instructions, 96 rodata words, 558 canonical padding words, 512 vector table words, 0 discrepancy).
- [`physics.decode`](file:///home/drakestapleton/workspace/physics/physics.decode): Disassembly dump with static target proof.
- [`machine_contract.json`](file:///home/drakestapleton/workspace/physics/machine_contract.json): Formal machine contract (`CONTRACT-QEMU-VIRT-AARCH64-M2`).
- [`qualification_receipt.json`](file:///home/drakestapleton/workspace/physics/qualification_receipt.json): Cryptographic qualification receipt for Milestone 2 (`PHYSICS_BOOT`).

---

## Qualification Architecture (Dual Verification Seams)

### Seam 1: Independent Artifact-Audit Seam (`seam1_physics_audit.py`)
Validates static artifact properties without requiring execution:
1. `PHYSICS_ARTIFACT_IDENTITY_PASS`: `physics.bin` matches `physics.sha256`.
2. `PHYSICS_MACHINE_CONTRACT_PASS`: Code size $\le 32\text{ KiB}$ (actual: 1,480 B), total image $\le 64\text{ KiB}$ (actual: 6,144 B), anti-bloat law satisfied.
3. `PHYSICS_AUDIT_PASS`: Generalized byte accounting formula reconciled to exact 0 bytes discrepancy:
   $$\text{CODE} (1480) + \text{RODATA} (384) + \text{PAD} (2232) + \text{VBAR} (2048) = 6144\text{ bytes}$$
4. `PHYSICS_VECTOR_LAYOUT_PASS`: Exception vector table base satisfies strict 2 KiB alignment, spans exactly 2,048 bytes (`0x000`..`0x7FF`), with all 16 architectural slots verified at 128-byte stride.
5. `PHYSICS_VECTOR_ALIGNMENT_PASS`: Vector base `0x40201000 % 2048 == 0`.
6. `PHYSICS_MEMORY_DISJOINTNESS_PASS`: Static mathematical proof that all 7 physical memory regions (`IMAGE`, `VBAR`, `STACK`, `STATE`, `CAP_TABLE`, `STATIC_DATA`, `FREE_FRAME_REGION`) are pairwise disjoint.
7. `PHYSICS_STATIC_MEMORY_BOUNDS_PASS`: Memory operations confined strictly to declared regions; initial kernel SP is `0x40205800`.

### Seam 2: External Execution Seam (`seam2_physics_harness.py`)
Validates runtime execution in bare-metal QEMU AArch64:
1. `PHYSICS_ENTRY_EL_PASS`: Current EL verified EL1/EL2, contract-driven installation of `VBAR_ELx`.
2. `PHYSICS_DESCRIPTOR_INGRESS_PASS`: Validates ingress pointer and fields, copies descriptor to `PHYSICS_BOOT_STATE`, zeroes `x0` to vacate Atlas scratchpad.
3. `PHYSICS_BOOT_QEMU_PASS`: Clean end-to-end boot progression from Atlas reset to `PHYSICS: QUIESCENT_READY`.
4. `PHYSICS_FRAME_BOUNDS_PASS`: Physical frame allocation verified strictly within `[FREE_FRAME_BASE (0x40208000), DRAM_END (0x48000000))`.
5. `PHYSICS_RESERVED_FRAME_REFUSAL_PASS`: Assert allocator permanently refuses any allocation in `[0x00000000, 0x40208000)`.
6. `PHYSICS_CAP_ROOT_PASS`: Validates genesis of kernel-private root capability `CapabilityId { slot: 0, generation: 1 }` anchored to `atlas.sha256`.
7. `PHYSICS_EXCEPTION_STATE_CAPTURE_PASS`: Synchronous exception traps to vector table, captures raw architectural state (`VECTOR_SLOT`, `CURRENT_EL`, `ESR`, `ELR`, `SPSR`, `FAR` with validity check), emits diagnostic telemetry, and halts in fail-closed quiescence.
8. `PHYSICS_CORRUPTION_REFUSAL_PASS`: 6-scenario hostile ingress corruption matrix (bad cookie, bad descriptor pointer, zero DRAM size, DRAM base+size overflow, image outside DRAM, invalid UART MMIO) verified to halt immediately in fail-closed quiescence before granting authority.

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
