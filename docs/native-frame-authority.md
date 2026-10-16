# Native Physical Frame Authority for DGX Spark (design)

Status: **DESIGN ONLY.** Governing issue: physics#2. Nothing here changes the
Milestone 2 QEMU artifacts (`physics.bin`, `*.s`, `machine_contract.json`,
gate scripts, `qualification_receipt.json`). `PHYSICS_BOOT_NATIVE_PASS`
remains **pending**; nothing in this document is a hardware result.
Revised 2026-10-16: section 11 re-checks every figure against a receipt and
records what changed since 2026-09-26; section 12 freezes the first
implementation cut.

Companion draft contract: [`contracts/native-dgx-spark-draft.json`](../contracts/native-dgx-spark-draft.json)
(DRAFT, not canonical).

## 0. Scope and non-goals

PHYSICS remains an **authority nucleus**. The native frame authority answers
exactly four questions, and nothing more:

1. Which physical frames exist and are grantable (the managed set)?
2. Which physical ranges are reserved, and to which class?
3. Grant one free frame (deterministically), or refuse.
4. Accept a frame back (after its authority is revoked), or refuse.

Explicit non-goals: no virtual-memory policy, no paging/swapping, no NUMA
policy, no buddy/slab/size-class allocator, no per-object heaps, no
reclamation heuristics, no per-frame owner/type map. Ownership and use of a
granted frame are carried by the **capability** that authorizes it, not by the
frame authority. Large contiguous pools (accelerator, OMEGA) are **carved once
at boot from the contract** as reservation classes; they are not served by the
per-frame allocator.

## 1. Evidence base: measured vs assumed

Every fact is labelled. Classes:

| Label | Meaning |
| --- | --- |
| `measured_native_uefi` | Observed by native AIENOS code on Machine 1 (DGX Spark) after firmware, read back from the firmware-variable report. Source: aienos `evidence/m2_first_boot_2026-09-24.md`. |
| `measured_linux` | Observed read-only under Linux 7.0.0-1019-nvidia on Machine 1 (`/proc/iomem`, `dmesg`, `/proc/meminfo`, aienos `evidence/gb10_linux_readonly_2026-09-25.md`), collected 2026-09-26. Linux's view, **not** a native proof. |
| `assumed` | Not measured by this project. Public-spec knowledge or engineering assumption. Must be confirmed before native qualification. |
| `derived` | Arithmetic over the above. |
| `vendor_doc` | Stated in NVIDIA public documentation, quoted with its URL (section 11.2). A vendor statement, not a measurement by this project. |

### 1.1 Facts relevant to frame authority

| Fact | Value | Label |
| --- | --- | --- |
| Exception level at handoff | EL2 | `measured_native_uefi` |
| UEFI memory map descriptors | 184 | `measured_native_uefi` |
| Conventional-memory descriptors | 36 | `measured_native_uefi` |
| Conventional memory total | 130,073,720 KiB (133,195,489,280 B, ~124.05 GiB) | `measured_native_uefi` |
| Largest conventional region | base `0x323800000`, 30,776,318 pages, i.e. `[0x323800000, 0x207D3FE000)` | `measured_native_uefi` (end `derived`) |
| CPU cores in MADT | 20 (10 + 10 efficiency classes), boot core Cortex-A725 | `measured_native_uefi` |
| DRAM envelope (lowest to highest RAM-class address) | `[0x80000000, 0x2080000000)` = exactly 128 GiB = 33,554,432 frames | `measured_linux` (`NUMA: Faking a node at [mem 0x80000000-0x207fffffff]`) |
| NUMA | single node, faked by Linux (no SRAT proximity) | `measured_linux` |
| Linux "System RAM" top-level ranges | 8 discontiguous ranges, 133,297,463,296 B total | `measured_linux` |
| Linux top-level "reserved" in envelope | 3.49 GiB incl. `[0x280000000, 0x323800000)` (2.55 GiB) | `measured_linux` |
| Unlisted holes inside envelope | 7 gaps, e.g. `[0xB0000000, 0xB9600000)`, `[0xB9800000, 0xC2E00000)` | `measured_linux` (absent from `/proc/iomem`) |
| MMIO below DRAM | GICD `0x06800000`, GITS `0x06840000`, GICR `[0x06880000, 0x06D80000)`, SMMUv3 x3 (`0x13000000`, `0x13800000`, `0x14900000`), GB10 BAR0 `0x24000000` 64 MiB, ECAM seg 15 `0x29000000` | `measured_linux` |
| MMIO above DRAM | PCI windows from `0x3D00000000`, ECAM from `0xF300000000` | `measured_linux` |
| SMMUv3 | 3 instances, OAS 40-bit, 2-level stream table, 25 SID bits | `measured_linux` |
| GIC ITS tables | allocated by Linux in DRAM (`0x100520000`..) | `measured_linux` (OS-allocated, not firmware) |
| LPDDR5x is one pool shared by CPU and GPU; no discrete VRAM/HBM; no fixed GPU carve-out | n/a | `vendor_doc` since 2026-10-16 (NVIDIA DGX Spark User Guide and Porting Guide, section 11.2); coherence still `assumed` |
| Physical address size (ID_AA64MMFR0_EL1.PARange) | at least 40 bits | `assumed` (only SMMU OAS measured) |
| Secure-world / TF-A carve-outs | exist, not described as RAM | `assumed` |
| Per-descriptor UEFI types (runtime code/data, ACPI reclaim/NVS, reserved) | not captured in any receipt yet | **unknown** |

Consequences already visible from measurement:

- **DRAM is discontiguous** (8 Linux ranges, 36 UEFI conventional regions,
  holes and firmware carve-outs). A single `[base, base+size)` descriptor, as
  in M2, cannot describe it.
- **DRAM does not begin at `0x40000000`.** The M2 fixed layout
  (`0x40200000`..`0x40208000`) is not DRAM on the Spark at all. Every native
  PHYSICS address must come from the trusted profile, not from constants.
- The envelope is exactly 128 GiB, so the issue's worst case (2^25 frames) is
  the real upper bound of the managed set on this machine.

## 2. Native memory topology in the contract (Required work 1)

The native machine contract (MachineGraph memory node) supplies a **memory
profile**. PHYSICS never reads the UEFI map itself; ATLAS translates the UEFI
map before `ExitBootServices` into the profile, binds it by digest into the
boot descriptor, and PHYSICS validates it at ingress.

### 2.1 Profile structure (binary, little-endian, all u64 unless noted)

```text
MemoryProfileHeader (64 B)
  +0x00 magic            "PHYSMEM1"
  +0x08 version          u32 = 1 ; flags u32 = 0
  +0x10 frame_shift      u32 = 12 ; pa_bits u32 (from contract)
  +0x18 dram_count       u32 ; rsv_count u32
  +0x20 dram_offset      (byte offset of DramRange[0] from profile base)
  +0x28 rsv_offset       (byte offset of Reservation[0])
  +0x30 profile_bytes    (total, bounded by contract max_profile_bytes)
  +0x38 cpu_count        u32 ; reserved u32 = 0

DramRange (16 B)      : base, size           -- RAM-class physical memory
Reservation (32 B)    : base, size, class u32, flags u32, source_tag u64
```

- `DramRange` lists every physical range that is DRAM (any UEFI RAM-class
  type: conventional, loader, boot-services, runtime, ACPI reclaim/NVS,
  persistent, and RAM the firmware withholds). It is the **universe** the
  authority may ever describe.
- `Reservation` lists every sub-range that must never be granted, with its
  class (section 4). MMIO apertures appear as reservations whose class is
  `MMIO_APERTURE` and which must lie **outside** every `DramRange`.
- Everything not in any `DramRange` is a hole and is unrepresentable.

### 2.2 Contract limits (bounded by construction)

| Limit | Draft value | Why |
| --- | --- | --- |
| `max_dram_ranges` (D_MAX) | 64 | 36 conventional regions measured; RAM-class descriptors are at most 184, and ATLAS merges adjacent RAM-class descriptors |
| `max_reservations` (RSV_MAX) | 192 | at least 184 descriptors measured, plus Atlas/Physics/derived carves |
| `max_candidate_ranges` (C_MAX) | 256 | = D_MAX + RSV_MAX: subtracting k intervals from n disjoint intervals yields at most n + k intervals |
| `max_managed_frames` (F_MAX) | 2^25 | 128 GiB / 4 KiB (envelope, `measured_linux`) |
| `max_metadata_bytes` (META_MAX) | 8 MiB | about 2x the worst case of section 3.3 |
| `pa_bits` | 40 (placeholder) | `assumed`; every address must be below 2^pa_bits |

Exceeding any limit is a **fail-closed refusal at ingress**, never a
truncation.

### 2.3 Ingress validation (all checks before any write outside PHYSICS_STATE)

For each `DramRange` i (sorted ascending by base, strictly):

1. `base % 4096 == 0`, `size % 4096 == 0`, `size != 0`.
2. `end = base + size` with carry check (`adds; b.cs refuse`).
3. `end <= 2^pa_bits`.
4. `end_{i-1} < base_i` (strictly disjoint **and non-adjacent**; ATLAS must
   merge adjacent RAM ranges so the representation is canonical).

For each `Reservation` j:

1. `size != 0`; `end = base + size` with carry check; `end <= 2^pa_bits`.
2. Rounded **outward** to frames: `rbase = base & ~0xFFF`,
   `rend = (end + 0xFFF) & ~0xFFF` with carry check on the add. A partially
   reserved frame is wholly reserved.
3. Class is a known class id (unknown class: refuse).
4. If class is `MMIO_APERTURE`: `[rbase, rend)` intersects no `DramRange`.
   Otherwise: `[rbase, rend)` is contained in the union of `DramRange`s
   (a DRAM-class reservation outside DRAM is a contract error: refuse).
5. Reservations may overlap each other (firmware maps can nest); the union is
   taken. Overlap is never an error; precedence is irrelevant because every
   class means "not grantable".

The profile digest (SHA-256 over `profile_bytes`) is recorded in
`PHYSICS_STATE` and in the native receipt.

## 3. Representation choice (Required work 2)

### 3.1 Candidates

Let F = managed frames (at most 2^25), R = candidate ranges (at most C_MAX = 256).

| Option | Metadata worst case (128 GiB, 4 KiB) | Grant time worst case | Determinism | Audit cost | Verdict |
| --- | --- | --- | --- | --- | --- |
| A. Flat bitmap over the whole envelope | 2^25 / 8 = **4,194,304 B (4 MiB)**, fixed | linear scan of 524,288 u64 words | lowest-free, deterministic | lowest; but holes and reservations are merely "bits set to 1" (excluded by initialization, not by construction) | rejected as the sole form |
| B. Hierarchical bitmap (leaf + 2 summary levels) over a range-segmented index | **4,196,352 B leaf + 65,568 B L1 + 1,032 B L2**; region 4,280,320 B total (section 3.3) | at most 129 summary words + 3 loads | lowest-free, deterministic | moderate: one extra invariant (summary consistency) | **recommended** |
| C. Extent/range free list with subdivision | fragmentation-dependent: alternating grants give F/2 extents, 2^24 x 16 B = **256 MiB**; or a bounded table that must refuse legal grants | O(extents), or tree operations; coalescing on release | deterministic only with a fully specified split/coalesce rule | highest; split/merge proofs in assembly | rejected for per-frame authority |
| D. Per-frame state nibble (type map) | 2^25 x 4 bits = 16 MiB | as A | as A | duplicates capability ownership | rejected (PHYSICS is not a general memory manager) |

Extent authority (C) **is** used, but only where extents are the natural unit
and their count is bounded by the contract: the static reservation table and
the candidate range table. The dynamic per-frame state is the bitmap.

### 3.2 Recommendation: range-segmented hierarchical bitmap

**Structure.**

- **Range table**: the R candidate ranges C' (DRAM minus all reservations,
  section 5), sorted ascending. Entry (32 B): `base, frames, word_offset,
  end`.
- **Leaf bitmap**: for range r, `words_r = ceil(frames_r / 64)` u64 words,
  laid out consecutively in range order starting at `word_offset_r`. Bit
  value `0 = free`, `1 = granted` (same polarity as M2). Bits
  `j >= frames_r` in the last word of range r are **padding, pinned to 1**.
- **L1 summary**: one bit per leaf word, `1 = leaf word has at least one free
  bit`.
- **L2 summary**: one bit per L1 word, `1 = L1 word is non-zero`.
- **Reservation table**: the validated, outward-rounded reservations plus the
  derived carves (section 4), sorted, 32 B each. Used only for
  `is_frame_reserved` classification queries.

**Why reservations are excluded by construction.** The leaf index space is
defined only over C'. No bit anywhere denotes a reserved frame, a hole, or an
MMIO page. Define

```text
phi(f) = (r, (f - base_r) >> 12)   for the unique r with base_r <= f < end_r
```

Because C' ranges are pairwise disjoint and sorted, `phi` is a bijection from
frames of C' onto the non-padding bits. No sequence of bitmap operations can
produce an address outside C', because the inverse
`addr = base_r + (j << 12)` is only evaluated for `j < frames_r`.

**Formal accounting argument.** Header counters `total_frames`,
`free_frames`, `granted_frames` (u64). Invariants, re-checkable by an
independent audit at any quiescent point:

```text
A1  total_frames = sum_r frames_r                      (fixed after init)
A2  free_frames + granted_frames = total_frames
A3  free_frames = number of non-padding bits equal to 0
A4  every padding bit == 1
A5  L1[w] == (leaf[w] != ~0)             for every leaf word w
A6  L2[v] == (L1word[v] != 0)            for every L1 word v
A7  L1/L2 bits beyond the last valid word are 0 (never selected)
```

A grant flips exactly one non-padding bit 0 to 1 and does `free--,
granted++`; a release flips exactly one non-padding bit 1 to 0 and does the
reverse; summaries are updated in the same critical section. A1..A7 are
therefore preserved by induction from the initial state (all non-padding bits
0, A5/A6 computed once, `free = total`, `granted = 0`).

### 3.3 Worst-case metadata bound (128 GiB at 4 KiB)

With F at most 2^25 and R at most 256, each section 64-byte aligned:

```text
W        = sum_r ceil(frames_r/64) <= 2^25/64 + R = 524,288 + 256 = 524,544 words
leaf     = 8 * W                                = 4,196,352 B
L1words  = ceil(524,544/64) = 8,196   ->  65,568 B  (aligned 65,600)
L2words  = ceil(8,196/64)   = 129     ->   1,032 B  (aligned  1,088)
range table   = 256 * 32 = 8,192 B
reservations  = 192 * 32 = 6,144 B
header        = 64 B
total         = 64 + 8,192 + 6,144 + 1,088 + 65,600 + 4,196,352 = 4,277,440 B
metadata region = align_up(4,277,440, 4096) = 4,280,320 B = 1,045 frames (about 4.08 MiB)
```

Every figure in this block is re-derived with a receipt in section 11.1.

That is 0.0031% of 128 GiB (calc:ad669fc92acfd2d5d95b7c0999dd52fe63546c17efcbfba73b2220854be91904), under META_MAX (8 MiB) with 1.96x margin (calc:93389c0b542543ee8bc95579531c509cbacffbbe5eadf39618f2e7fee5a9114d).
The flat bitmap alone is 4 MiB; the hierarchy and tables cost 86,016 B more (calc:f64b164ed9ff40a8d039084029d3369475484e937c431a43a822bb447497e644)
(about 2%) and buy a bounded, fragmentation-independent grant path.

Illustrative only (not a qualification value): over the 8 Linux-observed
System RAM ranges (32,543,326 frames), W = 508,492 and the region is about
1,010 frames (`derived` from `measured_linux`).

## 4. Reservation classes (Required work 4)

Every class is **non-grantable**. Classes marked *derived* are carved by
PHYSICS from the candidate set in the fixed order of section 5.3; all others
are supplied by the profile.

| Id | Class | Source | Rule |
| --- | --- | --- | --- |
| 0x01 | `ATLAS_IMAGE` | profile (Atlas load address + size) | Reserved for the life of PHYSICS v1 (no reclaim). |
| 0x02 | `ATLAS_HANDOFF` | profile | Boot descriptor, profile blob, Atlas stack/scratch. Must contain the profile itself. No reclaim in v1. |
| 0x10 | `PHYSICS_IMAGE` | profile (Atlas-chosen, not a constant) | Covers `.text/.rodata` + vector table. Vector base must be 2 KiB aligned inside it. |
| 0x11 | `PHYSICS_STACKS` | profile | `cpu_count x stack_size` + one exception stack per CPU; `cpu_count` = 20 (`measured_native_uefi`). Guard frames belong to the class. |
| 0x12 | `PHYSICS_STATE` | profile | Boot state + capability table + static data. Holds ingress scratch before metadata exists. |
| 0x13 | `PHYSICS_FRAME_METADATA` | *derived* | Sections 3.3 and 5.3. Carved last. |
| 0x14 | `PHYSICS_TRANSLATION_TABLES` | *derived*, size from contract | PHYSICS's own translation tables if the native build enables the MMU (needed for cacheable metadata access; see section 9). |
| 0x20 | `FW_RUNTIME` | profile, from UEFI `EfiRuntimeServicesCode/Data` | Permanently reserved. |
| 0x21 | `FW_ACPI` | profile, from `EfiACPIReclaimMemory`, `EfiACPIMemoryNVS` | Reserved in v1 (no reclaim of ACPI tables). |
| 0x22 | `FW_RESERVED` | profile, from `EfiReservedMemoryType`, `EfiUnusableMemory`, `EfiPalCode`, and RAM the firmware withholds (e.g. `[0x280000000, 0x323800000)`, `measured_linux`) | Permanent. |
| 0x23 | `FW_BOOT_TABLES` | profile | TPM event log, ESRT, MOK/RNG config tables, SMBIOS, `MEMRESERVE` entries that must survive `ExitBootServices` (Linux reports these near `0x84AF....`, `0x86DC....`, `0x8706....`, `measured_linux`). |
| 0x24 | `SECURE_WORLD` | profile | TF-A / secure carve-outs if described (`assumed` to exist). |
| 0x30 | `MMIO_APERTURE` | profile | GIC, SMMU, ECAM, PCI windows, GB10 BAR0. **Outside every DramRange** (validated). Never in the frame index; governed by a separate MMIO capability, not by this authority. |
| 0x40 | `DMA_SMMU_TABLES` | *derived*, size from contract | SMMUv3 stream tables, CD tables, command/event/PRI queues for all SMMU instances (3 measured). Size = contract formula over SID bits (2-level, 25 bits measured), level-2 tables only for populated SIDs. |
| 0x41 | `INTERRUPT_TABLES` | *derived*, size from contract | GIC ITS device/collection/vPE tables and LPI property/pending tables (Linux places these in DRAM, `measured_linux`). |
| 0x50 | `ACCEL_COHERENT_POOL` | *derived*, size from contract (owner decision; may be 0) | Ordinary coherent LPDDR frames that PHYSICS pre-commits for accelerator DMA through the SMMU. Contiguous extent. **Semantics are "DRAM frames with accelerator-visible authority"; there is no VRAM/HBM class and none may be added.** |
| 0x60 | `OMEGA_SHARED_OBJECT_SPACE` | *derived*, size from contract (default 0 in v1) | Future OMEGA object space. The class id and carve rule exist now so enabling it is a contract change, not a code change. |

Accelerator visibility of **dynamically granted** frames is not a frame
state: it is an attribute of the capability that maps the frame into an SMMU
context. The frame authority only guarantees the release rule of 6.4.

## 5. Derived, overflow-checked sizing and placement (Required work 3, 5, 6)

### 5.1 Checked primitives (AArch64)

```text
checked_add(a, b):  adds x, a, b ; b.cs REFUSE
checked_shl(a, k):  (a >> (64-k)) != 0 -> REFUSE ; x = a << k
align_up(a, 2^k):   x = checked_add(a, 2^k - 1) ; and x, x, ~(2^k - 1)
ceil_div64(a):      x = checked_add(a, 63) ; lsr x, x, #6
```

`REFUSE` is a fail-closed panic trap with a distinct telemetry string
(`PHYSICS: PANIC_PROFILE_*`), exactly like M2's ingress-corruption trap.

### 5.2 Candidate set computation (no storage required)

C0 = (union of DramRanges) minus (union of rounded non-MMIO reservations) is
computed by a **streaming merge** of two sorted lists: a cursor over
DramRanges and a cursor over reservations sorted by `rbase`. It emits C0
ranges in ascending order and can be re-run; it needs only O(1) scratch in
`PHYSICS_STATE`. Pass 1 counts `R0` and `F0`; `R0 <= D_MAX + RSV_MAX` holds
by the interval-subtraction bound and is re-checked.

### 5.3 Carve order (deterministic)

1. Profile-supplied reservations (every class not marked *derived*).
2. `PHYSICS_TRANSLATION_TABLES`, `DMA_SMMU_TABLES`, `INTERRUPT_TABLES`:
   each carved from the **base of the lowest-address C range that fits**.
3. `ACCEL_COHERENT_POOL`, then `OMEGA_SHARED_OBJECT_SPACE`: each carved from
   the **top of the highest-address C range that fits** (keeps low memory for
   small tables, keeps pools contiguous). If none fits: refuse (the contract
   asked for more than the machine has).
4. `PHYSICS_FRAME_METADATA`: sized on the current C (call it C_pre), carved
   from the base of the lowest-address C_pre range with
   `frames >= meta_frames`. Result: C'.

Every carve takes a **prefix or suffix** of one range, never a middle, so the
range count never increases (a range consumed exactly is removed).

### 5.4 Metadata sizing arithmetic (exact)

Input: the C_pre ranges `(base_r, size_r)`, count R.

```text
REFUSE unless 1 <= R <= C_MAX
W = 0 ; F = 0
for r in C_pre (ascending):
    frames_r = size_r >> 12                        ; size_r page-aligned, end < 2^pa_bits
    F        = checked_add(F, frames_r)
    words_r  = ceil_div64(frames_r)
    W        = checked_add(W, words_r)
REFUSE unless F <= F_MAX                           ; 2^25
REFUSE unless W <= F_MAX/64 + C_MAX                ; 524,544
L1 = ceil_div64(W) ; L2 = ceil_div64(L1)
REFUSE unless L2 <= L2_MAX                         ; 129, bounds the top-level scan
off_hdr  = 0
off_rt   = align_up(checked_add(off_hdr, 64), 64)
off_rsv  = align_up(checked_add(off_rt,  checked_shl(C_MAX, 5)), 64)
off_l2   = align_up(checked_add(off_rsv, checked_shl(RSV_MAX, 5)), 64)
off_l1   = align_up(checked_add(off_l2,  checked_shl(L2, 3)), 64)
off_leaf = align_up(checked_add(off_l1,  checked_shl(L1, 3)), 64)
end      =          checked_add(off_leaf, checked_shl(W, 3))
M0       = align_up(end, 4096)
REFUSE unless M0 <= META_MAX                       ; 8 MiB
meta_frames = M0 >> 12
```

The table sections are sized to the contract maxima (not to R), so the layout
depends only on W; this removes one term from the monotonicity proof.

### 5.5 Proof obligation: metadata never overwrites the frames it describes

**Lemma 1 (monotone sizing).** Let C' be C_pre with a prefix of one range
removed. Then R' <= R and each `frames'_r <= frames_r`, so `W' <= W`,
`L1' <= L1`, `L2' <= L2`, and `M0(C') <= M0(C_pre)` (`ceil_div64` and
`align_up` are monotone non-decreasing; the table sections are fixed). Hence
a region of `M0(C_pre)` bytes holds the metadata of C'.

**Lemma 2 (disjointness by subtraction).** `Meta = [m, m + M0)` is a prefix
of a C_pre range, and `C' = C_pre \ Meta` by definition of the carve.
Therefore `Meta ∩ C' = ∅`, `Meta ∩ Rsv = ∅` (C_pre already excludes every
reservation), and `Meta ⊆ DRAM`.

**Lemma 3 (bounded writes).** Every metadata store address is `m + off` with
`off` built from the checked quantities of 5.4, and every section write is
bounded by its section length; therefore every store lies in `[m, m + M0)`.
Before `Meta` exists, the authority writes only into `PHYSICS_STATE` (a
profile reservation, hence disjoint from C0).

**Theorem.** No metadata store performed by the frame authority targets a
frame of C'. By Lemma 3 all such stores lie in `Meta ∪ PHYSICS_STATE`; by
Lemma 2 and the reservation rule both are disjoint from C'. Lemma 1
guarantees the layout fits, so no section is truncated or spills. ∎

The only stores that touch C' are **zero-on-grant** stores into the frame
being granted (6.2), which by then has left the free set.

`PHYSICS_METADATA_DISJOINTNESS_PASS` (section 8) checks this both statically
(store-base audit, as M2's `PHYSICS_STATIC_MEMORY_BOUNDS_PASS` does) and
dynamically (emulation with write watchpoints over C').

## 6. Deterministic allocation semantics

Determinism statement: **given the same profile bytes (same digest), the same
contract, and the same sequence of grant/release calls, PHYSICS returns the
same sequence of addresses and ends in byte-identical metadata.** No
randomness, and no timing-, CPU-, or cache-dependent choice.

### 6.1 Init
All non-padding leaf bits 0, padding bits 1, L1/L2 computed, counters set,
header written last with a `state = READY` word. Before READY every call
refuses.

### 6.2 `grant_frame() -> addr | 0`
1. Scan L2 words `0..L2-1` for the first non-zero word (at most 129 loads).
   None: return 0 (exhausted; not a panic).
2. `v = 64*i + ctz(L2[i])`; `w = 64*v + ctz(L1[v])`; `b = ctz(~leaf[w])`
   (`ctz` = `rbit` + `clz`).
3. Map global word `w` to range r (binary search on `word_offset`, at most 8
   steps for 256 ranges); `j = 64*(w - word_offset_r) + b`;
   require `j < frames_r` (else integrity panic: a padding bit was 0).
4. Set the bit; if `leaf[w] == ~0` clear the L1 bit; if `L1[v] == 0` clear
   the L2 bit; `free--`, `granted++`.
5. `addr = base_r + (j << 12)`; zero the 4 KiB frame; return `addr`.

Result: always the **lowest-address free frame** in C', because ranges are
laid out in ascending address order. Worst-case cost is a constant (at most
129 + about 12 word operations + 4 KiB zeroing), independent of
fragmentation.

M2 note: M2 has no release, so its first-fit scan (and the next-free cursor
of the physics#1 requalification) produce the same lowest-free sequence. The
native semantics are a strict generalisation; M2 is untouched.

### 6.3 `is_frame_reserved(addr) -> class | FREE_MANAGED | GRANTED | HOLE`
Binary search the range table; if not in C', binary search the reservation
table for a class; otherwise `HOLE`. Read-only.

### 6.4 `release_frame(addr, cap)`
Refuses (no state change) unless: `addr % 4096 == 0`; `addr` is in C'; its
bit is 1; `cap` is the revoked capability that authorized the frame, and its
revocation receipt shows **all SMMU mappings removed and SMMU TLB
invalidation completed** (`CMD_TLBI_*` + `CMD_SYNC`) and all CPU mappings
removed. This DMA-quiescence rule prevents a released frame from remaining
reachable by a device. Then clear the bit, set the summaries, `free++`,
`granted--`. Double release, unaligned, reserved, hole, and out-of-range
addresses are refused.

### 6.5 Concurrency
v1: the boot CPU alone owns the authority. When secondaries come up, one lock
protects it; there is no per-CPU cache (it would make grant order depend on
scheduling).

## 7. QEMU vs native receipts (Required work 7)

- `qualification_receipt.json` remains the **QEMU M2** receipt for
  `CONTRACT-QEMU-VIRT-AARCH64-M2`. This design does not touch it.
- Native work produces `receipts/native/<contract_id>/<gate>.json` with
  mandatory fields `contract_id`, `contract_digest`, `profile_digest`,
  `platform`, `evidence_class` in {`emulation_native_profile`,
  `native_hardware`}, and `physics_bin_sha256`.
- Gates exercised in QEMU with a native-shaped synthetic profile may pass with
  `evidence_class = emulation_native_profile`. They **never** satisfy a
  hardware gate. `PHYSICS_BOOT_NATIVE_PASS` requires
  `evidence_class = native_hardware` on a physical DGX Spark and stays
  **pending** until then.
- The receipt checker refuses any receipt whose `platform` or `contract_id`
  does not match the gate's namespace (a QEMU receipt presented for a native
  gate is a hard failure).
- If QEMU ever adopts this representation, it does so under a **new** QEMU
  contract version (e.g. `CONTRACT-QEMU-VIRT-AARCH64-M3`) with its own
  receipts; M2 stays reproducible byte-for-byte.

## 8. Qualification gates for the native implementation

| Gate | Must prove |
| --- | --- |
| `PHYSICS_NATIVE_CONTRACT_PASS` | Native contract parses, is not marked DRAFT, has a distinct `contract_id`, all limits present, digest pinned in the manifest. |
| `PHYSICS_NATIVE_PROFILE_INGRESS_PASS` | A valid profile (including one shaped like the measured Spark map: 8+ discontiguous ranges, 180+ descriptors) is accepted; digest recorded in `PHYSICS_STATE`; no store outside `PHYSICS_STATE` before metadata exists. |
| `PHYSICS_NATIVE_PROFILE_CORRUPTION_REFUSAL_PASS` | Fail-closed refusal for: unsorted, overlapping or adjacent DRAM ranges; unaligned base/size; zero size; base+size wrap; end beyond 2^pa_bits; count above limit; unknown class; MMIO reservation inside DRAM; DRAM-class reservation outside DRAM; bad magic/version; `profile_bytes` beyond limit; empty managed set; a carve that does not fit. |
| `PHYSICS_METADATA_SIZING_PASS` | PHYSICS's computed W, L1, L2, M0 equal an independent recomputation; the synthetic 2^25-frame / 256-range profile yields exactly 4,280,320 B; overflow-crafted inputs are refused at the exact checked step. |
| `PHYSICS_METADATA_DISJOINTNESS_PASS` | Meta ∩ C' = ∅, Meta ⊆ DRAM, Meta disjoint from every reservation; static store-base audit; emulation watchpoints over C' never fire during init. |
| `PHYSICS_RESERVATION_CARVE_PASS` | Independent recomputation of C' (and of each derived carve) matches PHYSICS's range and reservation tables byte-for-byte; carves are prefix/suffix only; the order of 5.3 is respected. |
| `PHYSICS_NATIVE_FRAME_BOUNDS_PASS` | Every grant lies in C'; exhaustion returns 0 after exactly `total_frames` grants on a native-shaped profile. |
| `PHYSICS_NATIVE_RESERVED_FRAME_REFUSAL_PASS` | For every reservation class and every hole: first frame, last frame and one interior frame are never granted and are refused by release; `is_frame_reserved` returns the right class. |
| `PHYSICS_FRAME_ACCOUNTING_PASS` | Invariants A1..A7 hold after init, after N grant/release sequences from a recorded seed, and at exhaustion. |
| `PHYSICS_DETERMINISTIC_ALLOCATION_PASS` | Two independent runs with the same profile and call sequence produce identical address sequences and identical metadata SHA-256; grants are ascending lowest-free. |
| `PHYSICS_RELEASE_INTEGRITY_PASS` | Double, unaligned, reserved, hole and out-of-range releases refused with no state change; release without an SMMU-unmap + TLBI-sync receipt refused. |
| `PHYSICS_NO_DISCRETE_VRAM_ASSUMPTION_PASS` | Static audit: no VRAM/HBM class or constant; the accelerator pool is carved from DRAM; a contract with `ACCEL_COHERENT_POOL` size 0 boots and qualifies. |
| `PHYSICS_QEMU_M2_REGRESSION_PASS` | `physics.bin` SHA-256 unchanged and the M2 suite (`run_m2_gates.sh`) passes unchanged. |
| `PHYSICS_RECEIPT_SEPARATION_PASS` | Native receipts live in their own namespace; the checker rejects a QEMU receipt for a native gate and vice versa. |
| `PHYSICS_BOOT_NATIVE_PASS` | **Pending.** Physical DGX Spark boot, ATLAS to PHYSICS with the real UEFI-derived profile, every native gate above re-run with `evidence_class = native_hardware`, profile and metadata digests recorded. |

## 9. Implementation notes (non-normative)

- Native PHYSICS cannot reuse M2's fixed addresses; its image, stacks and
  state come from the profile. The entry ABI must carry the profile pointer,
  length and digest in the boot descriptor.
- With the MMU off, AArch64 data accesses are Device-nGnRnE: zeroing 4 MiB of
  metadata and 4 KiB per grant would be slow and alignment-strict. The native
  build will likely need an identity stage-1 map with metadata and grantable
  frames as Normal write-back inner-shareable (hence class 0x14).
- Accelerator coherence: if memory is unified and coherent (`assumed`), no
  cache maintenance is needed for accelerator visibility of granted frames,
  but the release rule (6.4) is still mandatory.

## 10. Open questions for the owner

1. Size of `ACCEL_COHERENT_POOL` (0, a fixed size, or a fraction of managed
   memory)? Or should all accelerator memory be dynamic grants?
2. Size and v1 enablement of `OMEGA_SHARED_OBJECT_SPACE` (default 0)?
3. Reclaim policy: may `ATLAS_HANDOFF`, boot-services and `FW_ACPI` ranges
   ever return to the managed set? (v1 says never.)
4. Stacks for all 20 CPUs now, or boot CPU only until SMP bring-up?
5. Zero-on-grant (design default) or zero-on-release?
6. Who owns the UEFI-type to class translation? (Design: ATLAS translates,
   PHYSICS validates; the translation table must be in the contract.)
7. Confirm `pa_bits` (read `ID_AA64MMFR0_EL1` natively) and capture the full
   per-descriptor UEFI map with types in a native receipt; that turns most
   `assumed` rows of section 1 into `measured_native_uefi`.
8. Does the per-frame authority need a contiguous multi-frame grant
   (`grant_extent(n, align)`), or do all large contiguous needs go through the
   boot-time pools?

## 11. Revision 2026-10-16: receipts, public figures, and what changed

This section re-checks the document against today's ground truth. It changes
no design decision. Two small rounding statements in section 3.3 were
corrected (11.4).

### 11.1 Receipts for every figure

Each figure below was computed with the `aien-calc` exact calculator. The id
is the receipt; `aien-calc verify <id>` recomputes it from its own inputs.

| Figure | Expression | Result | Receipt |
| --- | --- | --- | --- |
| DRAM envelope size, bytes | `0x2080000000 - 0x80000000` | 137438953472 (exact) | `calc:42b1e2179d0163e2e6c3c11196d9d48750c8a1d341b3ffd09621677092dcf7ce` |
| Envelope frames at 4 KiB | `(0x2080000000 - 0x80000000) / 4096` | 33554432 (exact) | `calc:a1a0fdf69d12928baecb0cef6e7d0e2cc761ce5cdf8412b531ea45c108c15671` |
| Envelope as GiB | `137438953472 B -> GiB` | 128 GiB (exact) | `calc:2d1c4d89a34ea08a172fdae68c8ce50457b518521f811c3bd077c3c3c56a0496` |
| Linux memory blocks spanning the envelope (blocks 16..1039, 128 MiB each), bytes | `(1040 - 16) * 0x8000000` | 137438953472 (exact) | `calc:ec9d481e8d067d6ab0dd451eb17d5e0c7cab541a1c5554290e98f5dd2292e692` |
| Linux memory blocks present today (1023 of 1024) | `1023 * 128 MiB -> GiB` | 127.875 GiB (exact) | `calc:23f1f90a2f0e851f2bb0b44d0e6ba1cd33b24896548cd87bd338e139d8662cbb` |
| Missing Linux memory block 22 starts at | `22 * 0x8000000` | 2952790016 (exact) | `calc:d30dfb0e7b1165777e5b721df784fae4070ff7be03d7bcde5a6ee5036412e1a4` |
| NVIDIA "128 GB" read as decimal | `128 GB -> GiB` | 119.20928955078125 GiB (exact) | `calc:a74722f586f990919c4aac835887e696c08dca3efe6cb377ceebbe2e5f113ee4` |
| Linux MemTotal today | `127598752 KiB -> GiB` | 121.687652587890625 GiB (exact) | `calc:484726a4a8f3e8d697dca54cd2ed177c0c8c00ac74eaad0cc957a96b76625acc` |
| MemTotal minus decimal 128 GB, bytes | `127598752 * 1024 - 128 * 10^9` | 2661122048 (exact) | `calc:bf0483f7bd71d71aee9501eb9c9bac99692eea137a0bd660a09bc72b93187983` |
| Flat 1-bit bitmap over the envelope, bytes | `2^25 / 8` | 4194304 (exact) | `calc:6343705f58b13da1cc490263dccb42d1698c897c559cb9582c659b8d25dc3010` |
| Leaf words W (worst case) | `2^25 / 64 + 256` | 524544 (exact) | `calc:820717e2fd5d59547005049c34980c208c35ee1f926f9365ed4b66b242c2c6ff` |
| Leaf bitmap bytes | `8 * 524544` | 4196352 (exact) | `calc:aad0265f0b4e07c88b50503b30e26a80adb0be8bc48a7467e6a4c2b73ff05f3b` |
| L1 summary words | `ceil(524544 / 64)` | 8196 (exact) | `calc:102d47ac04596209dca011d1f3ac40795659db9a36edebb3c22cdcf9c41b533e` |
| L1 bytes, 64-byte aligned | `ceil(8196 * 8 / 64) * 64` | 65600 (exact) | `calc:c112af981aa75defc5b571e2c6f31f613a50a4cc64e582567e1fce4dbb9dad85` |
| L2 summary words | `ceil(8196 / 64)` | 129 (exact) | `calc:8a1b9c6d1f70641f9c09ec74f4a5fc3f6b552617c8019f8b7b4cf8f957c8d46f` |
| L2 bytes, 64-byte aligned | `ceil(129 * 8 / 64) * 64` | 1088 (exact) | `calc:b520660fc00e303457ededdfce31dbbc7889e11193c049d5288f7cabc78f9f01` |
| Metadata total before page rounding, bytes | `64 + 256*32 + 192*32 + 1088 + 65600 + 4196352` | 4277440 (exact) | `calc:5a9d9da5a5e94c14d86337760cde4ee31a434be672de5fa1388731905d7f0e56` |
| Metadata frames | `ceil(4277440 / 4096)` | 1045 (exact) | `calc:cd0be622ed3418c777724f9a4984e29c7d50ca52ef0b6be7cacbba3cca95257c` |
| Metadata region, bytes | `1045 * 4096` | 4280320 (exact) | `calc:b44459565740784c3162ec8d2b2286e9caa2adb84b4b4ca64264da092185db6f` |
| Metadata region as MiB | `4280320 B -> MiB` | 4.082 MiB (rounded to 4 significant digits) | `calc:338da895a4b4f92c7db40b4498b403ec6fdc917d3c7f71cec2bff395fbd925bc` |
| Metadata as percent of 128 GiB | `4280320 / 137438953472 * 100` | 0.003114 (rounded to 4 significant digits) | `calc:ad669fc92acfd2d5d95b7c0999dd52fe63546c17efcbfba73b2220854be91904` |
| Hierarchy and tables cost over the flat bitmap, bytes | `4280320 - 4194304` | 86016 (exact) | `calc:f64b164ed9ff40a8d039084029d3369475484e937c431a43a822bb447497e644` |
| Overhead over the flat bitmap, percent | `86016 / 4194304 * 100` | 2.051 (rounded to 4 significant digits) | `calc:d09a298aaffab97ec55f448fa47db192a90e16abd8b5ff4e6a55c34d0b326b7b` |
| META_MAX margin (8 MiB / region) | `8388608 / 4280320` | 1.96 (rounded to 4 significant digits) | `calc:93389c0b542543ee8bc95579531c509cbacffbbe5eadf39618f2e7fee5a9114d` |
| Metadata region versus the one 4 KiB M2 PHYSICS_STATIC_DATA page | `4280320 / 4096` | 1045 (exact) | `calc:e3e37a04cb5da3658a21bd4d9c37e6f9f55bde734db9fee6dbd26081cc113a85` |
| Largest conventional UEFI region end check (0 = matches 0x207D3FE000) | `0x323800000 + 30776318 * 4096 - 0x207D3FE000` | 0 (exact) | `calc:602b7252771f058617a631df9854c977aca1ea9b59289e716e19ee380a2a2dd1` |
| Linux System RAM frames (133,297,463,296 B) | `133297463296 / 4096` | 32543326 (exact) | `calc:168264390113509a53977d295b253f6dea42c8af3e29cf53872302cfe8dceaa7` |
| M2 QEMU managed frames | `(0x48000000 - 0x40208000) / 4096` | 32248 (exact) | `calc:1fb454ab44218d97e0e2d1519d12660348dcd76d83c61eec4d1e1bb4624efafb` |
| M2 header plus bitmap bytes (limit 4,096) | `ceil(32248 / 8) + 32` | 4063 (exact) | `calc:740dc8fb1588bd0ff039efecf556874b3b180c66e3f8e1b0bc1aa7e5cc24a87c` |
| M3 QEMU managed frames | `(0x48000000 - 0x40210000) / 4096` | 32240 (exact) | `calc:aa89e2bf7a732af602871d651258eeed0dc64bcdc1d3993055c27393b80e2207` |
| M3 header plus bitmap bytes (limit 12,288) | `ceil(32240 / 8) + 32` | 4062 (exact) | `calc:9d98bcde0682354cefc90d2664df48e6fd46ef12bcef380769beb7d5b84b099b` |
| QEMU M2 contract RAM, MiB | `0x08000000 B -> MiB` | 128 MiB (exact) | `calc:dd9d2a63498337ac4c72543fb7ace946e249423c49c4a31255085d3088fd7150` |

### 11.2 Public NVIDIA figures

- DGX Spark User Guide, Hardware Overview
  (<https://docs.nvidia.com/dgx/dgx-spark/hardware.html>, read 2026-10-16),
  component table, memory row: "128 GB LPDDR5x unified system memory, 256-bit
  interface, 4266 MHz, 273 GB/s bandwidth".
- DGX Spark Porting Guide, System Overview
  (<https://docs.nvidia.com/dgx/dgx-spark-porting-guide/overview.html>, read
  2026-10-16): "The CPU is a 20-core ARM64-based chip, sharing 128GB of
  LPDDR5x memory with the integrated GPU (iGPU)" and "In UMA, both CPU and GPU
  share the same physical memory space without a fixed carve-out."

What this settles:

- **"128 GB" means 128 GiB here.** Read as decimal, 128 GB is 119.21 GiB
  (calc:a74722f586f990919c4aac835887e696c08dca3efe6cb377ceebbe2e5f113ee4).
  Linux already reports 121.69 GiB usable
  (calc:484726a4a8f3e8d697dca54cd2ed177c0c8c00ac74eaad0cc957a96b76625acc),
  2,661,122,048 B more than decimal 128 GB
  (calc:bf0483f7bd71d71aee9501eb9c9bac99692eea137a0bd660a09bc72b93187983).
  The installed memory is therefore the binary 128 GiB that the measured
  envelope shows (`derived`).
- **No discrete VRAM or HBM, and no fixed GPU carve-out.** NVIDIA's own pages
  now back the section 1.1 row that was labelled `assumed`. The row moves from
  `assumed` to `vendor_doc`. The class table of section 4 already forbids any
  VRAM/HBM class; the vendor text confirms that rule.
- **Cache coherence of that shared memory is not stated on the two pages
  read.** NVIDIA product marketing describes the memory as "coherent unified
  system memory" according to a web-search summary; the page was not read
  verbatim. UNVERIFIED, confidence medium-high. Section 9's coherence note
  stays labelled `assumed`.

### 11.3 Re-measurement on Machine 1 (`measured_linux`, 2026-10-16)

Read-only, without root, under Linux 7.0.0-1019-nvidia:

- `/sys/devices/system/memory/block_size_bytes` = `0x8000000` (128 MiB).
- Memory blocks 16 through 1039 exist, except block 22. Block 16 starts at
  `0x80000000` and block 1039 ends at `0x2080000000`, so the envelope of
  section 1.1 is confirmed again: 128 GiB
  (calc:2d1c4d89a34ea08a172fdae68c8ce50457b518521f811c3bd077c3c3c56a0496),
  2^25 = 33,554,432 frames
  (calc:a1a0fdf69d12928baecb0cef6e7d0e2cc761ce5cdf8412b531ea45c108c15671).
- The missing block 22 starts at `0xB0000000`
  (calc:d30dfb0e7b1165777e5b721df784fae4070ff7be03d7bcde5a6ee5036412e1a4),
  inside the hole `[0xB0000000, 0xB9600000)` already listed in section 1.1.
- `MemTotal` = 127,598,752 kB.

### 11.4 Corrections to this document

- Section 3.3 said "0.0032% of 128 GiB". The exact value is 0.003114%
  (calc:ad669fc92acfd2d5d95b7c0999dd52fe63546c17efcbfba73b2220854be91904),
  now written as 0.0031%.
- Section 3.3 said "about 2x margin" under META_MAX. The exact margin is 1.96x
  (calc:93389c0b542543ee8bc95579531c509cbacffbbe5eadf39618f2e7fee5a9114d).
- Every other figure of sections 1.1 and 3.3 reproduces exactly (11.1).
- The illustrative Linux-range figures of section 3.3 (W = 508,492, about
  1,010 frames) could not be re-derived: they need the eight per-range sizes,
  which are not in this repository. Only their total, 32,543,326 frames,
  reproduces (calc:168264390113509a53977d295b253f6dea42c8af3e29cf53872302cfe8dceaa7).
  UNVERIFIED, confidence medium. They were never qualification values.

### 11.5 What changed since the issue was filed (2026-09-26)

- **The design is merged.** This document and
  `contracts/native-dgx-spark-draft.json` landed in physics#4 (commit
  `ff13208`, 2026-09-26). On 2026-10-04 the issue was narrowed to
  implementation (issue comment). Its four remaining items are: capture the
  UEFI memory map with per-descriptor types on the Spark; promote the native
  contract out of DRAFT; metadata sizing and reservation code under a QEMU
  native-profile contract; `PHYSICS_BOOT_NATIVE_PASS` on silicon. This
  revision closes none of them.
- **The issue's QEMU facts still hold for M2.** `memory_alloc.s` keeps one
  4 KiB `PHYSICS_STATIC_DATA` page at `0x40207000`: 32,248 frames
  (calc:1fb454ab44218d97e0e2d1519d12660348dcd76d83c61eec4d1e1bb4624efafb),
  4,063 header and bitmap bytes against a 4,096-byte limit
  (calc:740dc8fb1588bd0ff039efecf556874b3b180c66e3f8e1b0bc1aa7e5cc24a87c).
- **M3 now exists beside M2.** `m3/` carries `CONTRACT-QEMU-VIRT-AARCH64-M3`:
  a 12 KiB `PHYSICS_STATIC_DATA` at `0x4020D000`, free frames from
  `0x40210000`, 32,240 frames
  (calc:aa89e2bf7a732af602871d651258eeed0dc64bcdc1d3993055c27393b80e2207),
  4,062 bitmap bytes against 12,288
  (calc:9d98bcde0682354cefc90d2664df48e6fd46ef12bcef380769beb7d5b84b099b),
  and new `release_frame` and `allocate_frame_in_range` routines. M3 is still a
  flat bitmap pinned to the QEMU constant `CONTRACT_DRAM_END = 0x48000000`; it
  does not scale to the Spark either. The native design keeps both M2 and M3
  untouched.
- **M2 cannot boot natively as built.** The issue comment of the M3 audit
  (aien-architecture#11) shows the M2 image takes an alignment fault at
  CAP_ROOT genesis on real hardware. Native frame authority therefore lands in
  a new native image, never as a patch to M2. Section 7 already requires this.
- **No native frame-authority code exists in this repository.** On
  2026-10-16, `PHYSMEM1` appears only in this document and the draft contract.
- **AIENOS has its own native allocators (cross-repo discrepancy).** On
  aienos-mv `main` (`4bd49ca`), `native/kernel/mm/frames.h` (added in
  `89cccf4`, 2026-10-01) is a list of at most 128 free ranges built from
  `EfiConventionalMemory`, with a counter of "ranges lost because the table
  was full". `crates/aienos-kernel/src/mem/frame_allocator.rs` is a
  fixed-size bitmap allocator. These serve the AIENOS kernel, not PHYSICS. The
  lost-ranges counter is a silent truncation, which section 2.2 forbids for
  PHYSICS (refuse, never truncate). Recorded here only; this revision changes
  nothing in aienos.
- **The typed UEFI map is still not captured.** A search of aienos-mv file
  names on 2026-10-16 found no evidence file for a typed memory map. File
  contents were not searched. UNVERIFIED, confidence medium.

### 11.6 Where the metadata region lives in the native layout

By the carve order of section 5.3, the metadata region (1,045 frames,
calc:cd0be622ed3418c777724f9a4984e29c7d50ca52ef0b6be7cacbba3cca95257c) is the
first 1,045 frames of the lowest-address managed range that is large enough,
after every profile reservation and the low table carves are removed. Its
base address is an output of the profile, never a constant. Today it cannot
be named: DRAM starts at `0x80000000`, firmware boot tables sit near
`0x84AF....`, `0x86DC....` and `0x8706....` (`measured_linux`), and holes
begin at `0xB0000000`. Which low range survives the carves depends on the
typed UEFI map that 11.5 says is still missing. The M2 address `0x40207000`
is below the start of DRAM on the Spark (section 1.1), so the M2 location
cannot be reused.

Scale against M2: the native worst-case region is 4,280,320 B
(calc:b44459565740784c3162ec8d2b2286e9caa2adb84b4b4ca64264da092185db6f),
1,045 times the single 4 KiB M2 page
(calc:e3e37a04cb5da3658a21bd4d9c37e6f9f55bde734db9fee6dbd26081cc113a85).

### 11.7 How the QEMU contract keeps working unchanged

- Native work never edits `physics.bin`, the M2 `*.s` files,
  `machine_contract.json`, `qualification_receipt.json`, or anything in `m3/`.
- On 2026-10-16 `sha256sum physics.bin` gives
  `4b22a08cd408b5cf90258af13b833b97a5211b4d2480fa74be357228f4aed3bd`, identical to
  `physics.sha256` on `main` (`6207d51`). This revision changes only this
  document.
- Native code gets its own contract id and its own receipt namespace
  (section 7). A QEMU receipt can never satisfy a native gate, and the reverse.
- `PHYSICS_QEMU_M2_REGRESSION_PASS` (section 8) checks the M2 hash and suite on
  every native change.
- The native grant order (lowest free frame first) is a strict
  generalisation of M2's next-free scan (section 6.2), so the semantics M2
  qualified stay true.

## 12. First cut (frozen 2026-10-16)

The first implementation cut is **profile ingress validation (section 2.3)
and metadata sizing (section 5.4), and nothing else.** It is written as
AArch64 assembly routines in a new native directory of this repository and
runs in QEMU against synthetic `PHYSMEM1` profiles under a new contract id,
`CONTRACT-QEMU-VIRT-AARCH64-NATIVE-PROFILE-1`, with
`evidence_class = emulation_native_profile`. It writes only into
`PHYSICS_STATE` scratch; it carves nothing, builds no bitmap, and grants or
releases no frame. An independent recomputation written in C under `tools/`
(no Python) checks every output. Its gates, added under `tests/gates/` so the
Spark pipeline judges them, are `PHYSICS_METADATA_SIZING_PASS` (the synthetic
2^25-frame, 256-range profile yields exactly 4,280,320 B, and each
overflow-crafted input is refused at its named checked step),
`PHYSICS_NATIVE_PROFILE_CORRUPTION_REFUSAL_PASS` (every refusal case listed
in section 8), and `PHYSICS_QEMU_M2_REGRESSION_PASS` (`physics.bin` hash
unchanged, M2 and M3 files untouched). Out of scope: carving, bitmap
initialisation, grant and release, MMU setup, and any silicon run. The cut is
done when the three gates pass on the Spark pipeline and a native-profile
receipt is committed under `receipts/native/CONTRACT-QEMU-VIRT-AARCH64-NATIVE-PROFILE-1/`.
Separately, and outside this repository, the typed UEFI memory map must be
captured on Machine 1; that capture is the input for promoting the draft
contract out of DRAFT.
