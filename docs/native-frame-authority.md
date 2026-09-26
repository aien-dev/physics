# Native Physical Frame Authority for DGX Spark (design)

Status: **DESIGN ONLY.** Governing issue: physics#2. Nothing here changes the
Milestone 2 QEMU artifacts (`physics.bin`, `*.s`, `machine_contract.json`,
gate scripts, `qualification_receipt.json`). `PHYSICS_BOOT_NATIVE_PASS`
remains **pending**; nothing in this document is a hardware result.

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
| LPDDR5x is one coherent pool shared by CPU and GPU; no discrete VRAM/HBM | n/a | `assumed` (public product description; not measured) |
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

That is 0.0032% of 128 GiB, under META_MAX (8 MiB) with about 2x margin.
The flat bitmap alone is 4 MiB; the hierarchy and tables cost 86,016 B more
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
| `PHYSICS_QEMU_M2_REGRESSION_PASS` | `physics.bin` SHA-256 unchanged and the M2 suite (`run_milestone2_gates.py`) passes unchanged. |
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
