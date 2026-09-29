# FORGE SUBSTRATE V2

**Status:** DRAFT. Depends on ARCH-0018 "Substrate-Neutral Physical Realization"
(**PROPOSED**, `aien-dev/aien-architecture` `docs/adr/0018-substrate-neutral-physical-realization.md`).
Nothing here is binding until ARCH-0018 is accepted by the operator and this document is merged.
**Workstream gate:** AR1 (FORGE substrate-neutral descriptor contract) of ARCH-0018 §12.
**Reference code:** `forge/v2/forge_substrate_v2.{h,c}`, `forge/v2/forge_sha256.h`.
**Tests:** `tests/test_forge_v2_kat.c`, `tests/run_forge_v2_gates.sh`.
**Golden vectors:** `docs/forge-v2-golden-vectors.md` (produced by the test binary; never hand-edited).

---

## 1. Purpose and non-goals

FORGE realizes semantic operations on physical devices. FORGE v1 ("FORGE MACHINE DESCRIPTOR V1", physics
PR #13) describes exactly one device: the GB10 GPU of the first qualified Machine, as a fixed 208-byte record
whose field vocabulary is vendor-specific (`pci_*`, `rm_compute_class`, `vbios_version`, ...).

FORGE SUBSTRATE V2 defines a **substrate-neutral** description of a Machine and of every physical substrate
attached to it, plus the records that let a non-exact substrate be used without weakening what "correct"
means: a semantic result contract, a numerical realization profile, a calibration artifact, execution
evidence with an explicit provenance class, and a pointer-free cross-machine reference.

Goals:

- One data model for digital CPU, digital GPU, analog in-memory, neuromorphic, FPGA/CGRA and optical
  substrates, with no vendor or product name in any canonical field or enum.
- Deterministic canonical bytes and SHA-256 identities for every object.
- The v1 GB10 identity stays reproducible and is **wrapped** (by digest), never re-serialized.

Non-goals:

- **No physical analog claim.** No analog, neuromorphic, FPGA or optical device exists in this project. The
  `ANALOG_IN_MEMORY` substrate in the tests and golden vectors is a development fixture. Every evidence vector
  here carries `provenance_class = SIMULATED_DEVELOPMENT`.
- FORGE MACHINE DESCRIPTOR V1 remains **the qualified first implementation** on GB10. V2 does not replace,
  modify or re-qualify it. No v1 type, file or receipt changes. Historical receipts stay historical.
- No hardware access, no driver calls, no submission path. V2 here is a data contract plus a host-only
  reference serializer. Realization on a V2-described substrate is later work (AR2 onward).
- No change to the Omega runtime (R16 owns it until it closes) and no ARGUS ABI change.

## 2. Wire format

All integers are **little-endian and fixed-width** (same byte order as v1). There are no floating-point
values anywhere in canonical bytes; fractions are fixed-point integers (ppm, ppb, milli-units).

### 2.1 Object framing

Every top-level object is:

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | magic | ASCII `FGV2` (`46 47 56 32`) |
| 4 | 2 | kind | u16, object kind (below) |
| 6 | 2 | version | u16, `2` for every object in this document |
| 8 | 4 | body_len | u32, bytes after the header |
| 12 | body_len | body | sequence of TLV fields |

Each TLV field is `tag u32 | len u32 | value[len]`. A value is **exactly one** of: `u32` (len 4), `i32`
(len 4, two's complement), `u64` (len 8), `i64` (len 8), `digest` (len 32), or one **nested object** (a
complete framed object, len = its total size). Composite concepts (an envelope, a shape) are groups of
adjacent tags, not packed structs, so every field has one unambiguous width.

The magic prefix guarantees a V2 object can never be confused with the unframed 208-byte v1 stream.

| kind | Object |
|---|---|
| `0x0001` | ForgeMachineDescriptorV2 |
| `0x0002` | ForgeSubstrateDescriptor |
| `0x0003` | SemanticResultContract |
| `0x0004` | NumericalRealizationProfile |
| `0x0005` | CalibrationArtifact |
| `0x0006` | ForgeExecutionEvidenceV2 |
| `0x0007` | Cross-machine reference |

### 2.2 Canonical form

- Fields appear in **strictly ascending tag order**. A tag appears at most once, except a repeatable field
  (only `0x0140` substrate today), whose elements are consecutive and ordered by strictly ascending
  `substrate_identity` (bytewise); equal identities are refused.
- Every required field is present, including zero values. Optional fields (marked *opt*) are either absent or
  carry a non-zero digest; "present but zero" is refused.
- No padding, no trailing bytes; `body_len` equals the byte count after the header; maximum object size 65536.
- **Strict decode** (the default for identity and admission) refuses unknown tags and additionally re-encodes
  the decoded object and requires byte equality with the input. Lenient decode (for reading newer additive
  records) skips unknown tags that are still in ascending order, and enforces every other rule.

### 2.3 Identity digest

`digest(object) = SHA-256(header || body)` of the canonical bytes. A digest is **derived, never stored inside
the bytes it covers** (so CalibrationArtifact's "digest" is computed, not a field). Digests are 32 raw bytes,
shown as lowercase hex. The SHA-256 is the pinned root `sha256_clean.c` (`sha256_compute`), reused through
the declaration-only `forge/v2/forge_sha256.h`; it is not modified and not duplicated.

**Identity never includes** physical addresses, process pointers, PCI BDF or other bus locations, driver /
firmware / VBIOS version strings, measured (boot-volatile) memory sizes, or timestamps. Those are evidence.
There are **no strings** in canonical bytes; a name enters only as a digest. One deliberate exception: the
CalibrationArtifact carries its calibration `epoch_s`, and its digest is bound into realization identity
(§7), so recalibration produces a new realization identity. That is intended.

### 2.4 Tag tables

Values are listed in canonical (tag) order. Type `d32` is a 32-byte digest.

**ForgeMachineDescriptorV2 (kind 1)**

| Tag | Type | Field | Rule |
|---|---|---|---|
| 0x0101 | d32 | machine_identity | non-zero; stable identity of the Machine (e.g. owner-enrolled key digest) |
| 0x0102 | d32 | authority_domain | non-zero; the AIENOS authority domain that governs it |
| 0x0110 | u64 | memory_nominal_bytes | declared nominal capacity, **not** measured MemTotal |
| 0x0111 | u32 | memory_numa_domains | |
| 0x0112 | u32 | memory_coherency | UNIFIED 1, NONCOHERENT 2, DISCRETE 3 |
| 0x0120 | u32 | interconnect_topology | SINGLE_PACKAGE 1, SHARED_BUS 2, SWITCHED 3, MESH 4, REMOTE 5 |
| 0x0121 | u32 | interconnect_links | |
| 0x0130 | u32 | transport_class | ON_PACKAGE 1, PCIE 2, USB 3, ETHERNET 4, RDMA_ETHERNET 5, BOARD_TO_BOARD 6, REMOTE_MACHINE 7 |
| 0x0131 | u32 | transport_bandwidth_mbps | cost metadata |
| 0x0132 | u32 | transport_latency_ns | cost metadata |
| 0x0140 | nested substrate, repeated | substrates | 1..8, ascending unique `substrate_identity`; at least one `DIGITAL_*` |

**ForgeSubstrateDescriptor (kind 2)**

| Tag | Type | Field | Rule |
|---|---|---|---|
| 0x0201 | d32 | substrate_identity | non-zero |
| 0x0202 | u32 | substrate_class | §5.2 |
| 0x0203 | u64 | operation_families | non-zero mask, §5.3 |
| 0x0210–0x0213 | u32 | input_repr, input_bits, output_repr, output_bits | repr 1..8 (§5.3), bits 1..128 |
| 0x0220 | u32 | state_model | STATELESS 1, VOLATILE 2, PERSISTENT_WEIGHTS 3, DYNAMICAL 4 |
| 0x0221 | u32 | sync_model | CLOCKED 1, ASYNC_EVENT 2, BATCH_BARRIER 3, FREE_RUNNING 4 |
| 0x0230 | u32 | memory_model | SHARED_COHERENT 1, DEVICE_LOCAL 2, IN_MEMORY_COMPUTE 3, NONE 4 |
| 0x0231 | u64 | memory_capacity_bytes | declared nominal |
| 0x0240 | nested profile | numerical | NumericalRealizationProfile (§6.3) |
| 0x0250 | u32 | calibration_required | 0 or 1 |
| 0x0251 | u32 | calibration_method_mask | bit `1 << method`; 0 iff not required |
| 0x0252 | u64 | calibration_max_window_s | > 0 iff required |
| 0x0253 | u32 | calibration_min_fixtures | > 0 iff required |
| 0x0260 | u32 | error_kind | best achievable kind (§6.1) |
| 0x0261 | u32 | error_rel_ppb | guaranteed relative bound |
| 0x0262 | u64 | error_abs | guaranteed absolute bound, output LSB units |
| 0x0263 | u32 | fault_detect_mask | device-reported fault classes |
| 0x0270–0x0273 | u64,u64,u64,u32 | timing: min_latency_ns, setup_ns, conversion_ns, deterministic (0/1) | |
| 0x0280–0x0283 | u32,u32,u64,u32 | limits: max_rows, max_cols, max_state_bytes, max_concurrent (≥1) | |
| 0x0290 | u32 | evidence_capabilities | DURATION, ENERGY, TEMPERATURE, MEASURED_ERROR, REPEATS, HW_STATUS, FAULT (bits 0..6) |
| 0x02F0 | d32 *opt* | legacy_v1_descriptor_digest | only on `DIGITAL_CPU`/`DIGITAL_GPU` (§4) |

**SemanticResultContract (kind 3)**

| Tag | Type | Field | Rule |
|---|---|---|---|
| 0x0301 | u32 | operation_family | exactly one known bit |
| 0x0302 | d32 | reference_oracle_digest | identity of the oracle that defines "correct"; non-zero |
| 0x0310 | u32 | shape_rank | 1..4 |
| 0x0311–0x0314 | u32 | shape_dim[0..3] | dims < rank non-zero, dims ≥ rank zero |
| 0x0320 | u32 | error_kind | §6.1 |
| 0x0321 | u32 | error_rel_ppb | |
| 0x0322 | u64 | error_abs | |
| 0x0323 | u32 | error_norm | ELEMENTWISE_MAX 1, L2_RELATIVE 2 |
| 0x0330 | u32 | confidence_ppm | 1..1 000 000; exact and bounded-deterministic require 1 000 000 |
| 0x0331 | u32 | min_samples | ≥ 1; stochastic kinds ≥ 2 |
| 0x0340 | u32 | allowed_nondeterminism | REDUCTION_ORDER 1, ROUNDING 2, STOCHASTIC_NOISE 4, TIMING 8 |

**NumericalRealizationProfile (kind 4)**

| Tag | Type | Field |
|---|---|---|
| 0x0401 | u32 | dynamic_range_mdb (dB × 1000) |
| 0x0402 | u32 | resolution_bits (1..128) |
| 0x0403 | u32 | repeatability_ppb |
| 0x0404 | u32 | drift_ppb_per_hour |
| 0x0405 | u32 | temperature_ppb_per_k |
| 0x0406 | u64 | settling_ns |
| 0x0407 | u64 | sample_rate_hz |
| 0x0408 | u64 | calibration_validity_s (0 = no calibration; must be ≤ substrate max window) |

**CalibrationArtifact (kind 5)**

| Tag | Type | Field |
|---|---|---|
| 0x0501 | d32 | substrate_id |
| 0x0502 | d32 | calibration_id |
| 0x0503 | u32 | method: REFERENCE_FIXTURE_SWEEP 1, SELF_TEST 2, FACTORY_TABLE 3, SIMULATED_MODEL 4 |
| 0x0504 | u64 | epoch_s |
| 0x0505 | u64 | validity_window_s (> 0, no overflow with epoch) |
| 0x0510–0x0513 | i32,i32,u32,u32 | operating envelope: temp_min_mc, temp_max_mc, supply_min_uv, supply_max_uv |
| 0x0520–0x0522 | i64,i64,u32 | transfer: gain_ppb, offset_lsb, linearity_ppb |
| 0x0530–0x0532 | u32 | noise: rms_ppb, peak_ppb, samples |
| 0x0540–0x0542 | u32,u32,u64 | drift: ppb_per_hour, temp_ppb_per_k, observed_span_s |
| 0x0550 | u32 | fixture_count (≥ 1) |
| 0x0551 | d32 | fixtures_digest (reference fixtures) |
| 0x0560 | d32 | evidence_root |

**ForgeExecutionEvidenceV2 (kind 6)** — see §8. **Cross-machine reference (kind 7)** — see §9.

## 3. Versioning

- Every object carries `version u16 = 2`. The version names the **wire generation**, not a per-object
  revision.
- `version 1` is refused with a distinct error (`FORGE_V2_ERR_LEGACY_VERSION`): in V2 framing, version 1 is
  reserved for the legacy / 32-bit-generation era and is never accepted. Any other unknown version is
  refused (`FORGE_V2_ERR_VERSION`).
- Growth is **additive**: a later revision adds new tags only, with numbers above the existing ones in that
  object's range. It never renumbers, re-types or removes a tag. Old strict readers refuse the newer record
  (so nothing is silently accepted with unknown meaning); lenient readers skip the new tags.
- An incompatible change requires a new version number and a new spec section; objects of the old version
  keep their digests forever.

## 4. V1 compatibility

- FORGE MACHINE DESCRIPTOR V1 is untouched: same 208-byte layout, same KAT, same receipts.
- V2 **wraps** v1: the `DIGITAL_GPU` substrate that is the GB10 carries `legacy_v1_descriptor_digest`
  (tag `0x02F0`), the SHA-256 of the v1 208-byte canonical stream. V2 never re-serializes any v1 field, and no
  v1 bus-address (BDF) field ever appears in V2 canonical bytes (checked by `v2_bytes_carry_no_v1_fields`; the
  check searches for that 16-byte field, not for every v1 byte sequence).
- The **only frozen, reproducible v1 identity is the v1 KAT digest**
  `10d63d05f888eaba4fe473c5f17febe22f462f543ae0c2b005a9a8e5417e09fd`. The V2 KAT holds the v1 KAT 208-byte
  stream as a literal, hashes it with `sha256_compute`, asserts that digest, embeds it in the frozen reference
  V2 descriptor, and freezes the V2 digest:

  **Frozen reference ForgeMachineDescriptorV2 digest:**
  `89cb5ba667a75ab2871a6ba0d2f812c84d95c376cbfdf3b9b987ecacda1106c0`

- The **live** GB10 v1 digest is boot-volatile (it includes measured total memory, driver/firmware/VBIOS
  strings and PCI BDF). V2 therefore does not hard-code a live digest; a live V2 descriptor binds whatever v1
  digest FORGE recorded in its own evidence for that boot.
- The v1 vendor gate (`pci_vendor_id` must be `0x10de`) and all v1 vendor-named fields stay inside the v1
  record. They never become V2 fields or enums.
- After physics PR #13 merges, a follow-up adds a cross-check that `forge_descriptor_run_kat() == 0` and that
  the v1 `compute_digest` of the KAT descriptor equals the digest V2 wraps. V2 does not link v1 code today.

## 5. Machine/substrate model

### 5.1 Machine

A Machine (`ForgeMachineDescriptorV2`) is: machine identity, memory topology, interconnect topology,
transport, authority domain, and one or more attached substrates. **Every Machine has at least one digital
substrate** (enforced), so the digital realization is always available as the fallback (§10).

`machine_identity` is the stable name; the descriptor digest describes the Machine's current declared
configuration. Changing transport metadata changes the descriptor digest but not `machine_identity`, and not
any realization identity (§12).

### 5.2 Substrate classes

Frozen numeric ids. No vendor, product, architecture or driver name appears in any class, field or enum.

| Id | Class |
|---|---|
| 0x0000 | invalid |
| 0x0001 | DIGITAL_CPU |
| 0x0002 | DIGITAL_GPU |
| 0x0010 | ANALOG_IN_MEMORY |
| 0x0020 | NEUROMORPHIC_SPIKING |
| 0x0030 | FPGA_CGRA_DATAFLOW |
| 0x0040 | OPTICAL_PHOTONIC |
| 0x8000–0xFFFF | FUTURE_RESERVED (assigned only by a later revision of this spec; refused until then) |

All unlisted ids are unassigned and refused.

### 5.3 Substrate descriptor

A `ForgeSubstrateDescriptor` states: substrate identity; class; supported operation families (MATVEC,
MATMUL, CONV, ELEMENTWISE, REDUCTION, SPIKE_INTEGRATE, TRANSFORM, SAMPLE, GENERAL); input/output
representation (DIGITAL_INT, DIGITAL_FLOAT, FIXED_POINT, ANALOG_VOLTAGE, ANALOG_CURRENT, SPIKE_TRAIN,
OPTICAL_INTENSITY, OPTICAL_PHASE) with bit widths; state model; synchronization model; memory model;
numerical model (a nested NumericalRealizationProfile); calibration requirements; error model; timing
model; resource limits; evidence capabilities. Capacities are declared nominal values; measured values go in
evidence.

### 5.4 Relationship to Omega

Omega's capability query (`CqNeed`, `rx_capq.h`) is already substrate-neutral. The one hardware-naming type
on omega main is `enum { RX_COG_HW_CPU_P, RX_COG_HW_CPU_E, RX_COG_HW_GPU }` (`rx_route.h:61`); it is hashed
into the routing registry digest. `AG_REAL_HARDWARE` (`rx_graph.h`) is the existing placeholder
realization kind. Replacing `RX_COG_HW_*` with substrate-neutral requirements is an Omega change after R16
closes (ARCH-0018 §15); V2 supplies the vocabulary but does not edit Omega.

## 6. Numeric/error model

### 6.1 SemanticResultContract

Owned by Omega semantically; FORGE carries its canonical form and digest. Fields: operation family,
reference oracle digest, expected shape, acceptable error, confidence requirement, minimum samples, allowed
nondeterminism.

`acceptable_error` kinds:

| Id | Kind | Bound rule | Nondeterminism |
|---|---|---|---|
| 1 | EXACT | rel = abs = 0 | no ROUNDING, no STOCHASTIC_NOISE; confidence 1 000 000 ppm |
| 2 | BOUNDED_DETERMINISTIC | at least one of rel/abs non-zero | no STOCHASTIC_NOISE; confidence 1 000 000 ppm |
| 3 | BOUNDED_STOCHASTIC | at least one of rel/abs non-zero | STOCHASTIC_NOISE required; min_samples ≥ 2 |
| 4 | MEASURED_DISTRIBUTION | bounds optional | STOCHASTIC_NOISE required; min_samples ≥ 2 |

Bounds are integers only: `error_rel_ppb` (parts per billion) and `error_abs` (absolute, in output LSB units).
Acceptance per element: `|result − reference| ≤ error_abs + error_rel_ppb × |reference| / 10⁹`, measured in
`error_norm`.

### 6.2 Eligibility against a contract

A substrate is eligible for a contract only if all hold: it is not faulted; it supports the operation family;
its best `error_kind` is **no looser** than the contract's (so an `EXACT` contract can never be realized on a
weaker substrate); if the substrate is non-exact and the contract states bounds, both substrate bounds are
within the contract bounds; and, if it needs calibration, the calibration check passes (§7).

### 6.3 NumericalRealizationProfile

Dynamic range, resolution bits, repeatability, drift rate, temperature sensitivity, settling time (ns),
sample rate, calibration validity window. All fixed-point integers.

### 6.4 Omega anchors (AR2)

- Omega's result contracts today are **exact-only**: `RcKind` / `rc_check` (`src/runtime/rx_contract.{h,c}`)
  check integer fields; there is no tolerance, epsilon or float type. V2's EXACT kind is exactly that
  behavior; kinds 2–4 are new and live in FORGE records until Omega adds them (after R16).
- AR2 parity anchors: the exact integer oracle `omega_matvec_reference` (`src/omega_matvec.h:74`, wrapping
  u64 arithmetic), and the float oracles `omega_matmul_cpu_oracle_{i32,f16,bf16}`
  (`src/omega_blackwell_matmul.h:50-55`), compared in `src/omega_blackwell_submit.c` with a hard-coded
  `1e-4` absolute tolerance. In V2 terms that comparator is a BOUNDED_DETERMINISTIC contract whose bound must
  be restated as an integer (`error_abs` in LSB units of the output type) rather than a float literal.

## 7. Calibration model

- A **CalibrationArtifact** records: substrate_id, calibration_id, method, epoch, validity window, operating
  envelope (temperature, supply), transfer characteristics (gain, offset, linearity), noise statistics, drift
  statistics, reference fixtures (count + digest), evidence_root. Its digest is derived from its canonical
  bytes.
- A realization on a substrate that requires calibration **binds the calibration digest into realization
  identity**:
  `realization_id = SHA-256("FORGE-V2-REALIZE" || contract_digest || machine_identity || substrate_digest ||
  code_digest || has_cal(u8) || calibration_digest_or_zero)` (177 bytes in). Recalibration ⇒ new identity.
- **Staleness:** eligible only if `epoch_s ≤ execution_epoch_s < epoch_s + window`, where
  `window = min(artifact.validity_window_s, substrate.calibration_max_window_s)`. An artifact that
  over-claims its window is capped. A calibration dated after execution is stale.
- **Envelope:** measured conditions (temperature, supply) outside the calibrated envelope ⇒ not eligible
  (`FORGE_V2_ERR_OUT_OF_ENVELOPE`).
- Stale, missing, foreign (other substrate), wrong-method, too-few-fixtures or out-of-envelope calibration ⇒
  the substrate is **not eligible**. AEGIS refuses it. It is never "degraded" and never silently accepted.
- **Calibration is evidence, not authority.** A calibration grants no right, widens no contract and changes no
  semantic meaning.

## 8. Evidence model

`ForgeExecutionEvidenceV2` (kind 6) is new and additive. v1 `ForgeExecutionEvidence` (`forge_types.h`) is
untouched; historical receipts stay historical and are never rewritten.

| Tag | Type | Field |
|---|---|---|
| 0x0601 | d32 | realization_identity |
| 0x0602 | d32 | machine_identity |
| 0x0603 | d32 | substrate_identity |
| 0x0604 | d32 | semantic_contract_digest |
| 0x0605 | d32 *opt* | calibration_digest |
| 0x0606 | d32 | input_identity |
| 0x0607 | d32 | output_identity |
| 0x0610 | u64 | execution_epoch_s |
| 0x0611 | u64 | execution_duration_ns |
| 0x0612 | u64 | conversion_duration_ns |
| 0x0620 | u64 | energy_nj |
| 0x0621 | u32 | energy_source: NOT_MEASURED 0 (then energy_nj must be 0), MEASURED 1, ESTIMATED 2 |
| 0x0630 | i32 | temperature_mc |
| 0x0631 | u32 | supply_uv |
| 0x0640 | u32 | measured_error_rel_ppb |
| 0x0641 | u64 | measured_error_abs |
| 0x0642 | u32 | confidence_ppm |
| 0x0650–0x0654 | u32,u32,u64,u64,u32 | repeated runs: runs, passes (≤ runs), mean_ns, spread_ns, error_spread_ppb |
| 0x0660 | u32 | hardware_status |
| 0x0661 | u32 | fault_state: NONE 0, TRANSIENT 1, PERSISTENT 2, UNAVAILABLE 3 |
| 0x0670 | u32 | **provenance_class**: PHYSICAL 1, SIMULATED_DEVELOPMENT 2 — mandatory |

- `provenance_class` is mandatory and has no default: absent or `0` is refused at **both** encode and decode
  (`FORGE_V2_ERR_PROVENANCE`). A simulated result may qualify a development gate; it may never be presented as
  physical, and a gate that requires PHYSICAL refuses SIMULATED_DEVELOPMENT.
- Timestamps, measured sizes and device status live here, never in identity.
- Receipts built from V2 evidence are expected to be stored digest-named (`evidence/<AREA>/<sha256>.json`),
  following the roadmap requirement; physics has no such receipts yet and this document creates none.

## 9. Cross-machine reference form

Kind 7. Never a pointer, address, file descriptor or handle number.

| Tag | Type | Field | Rule |
|---|---|---|---|
| 0x0701 | d32 | program_digest | |
| 0x0702 | d32 | semantic_object_id | |
| 0x0703 | d32 | ir_reference | content digest of the IR bytes |
| 0x0710 | u64 | ir_size | |
| 0x0711 | u64 | range_offset | `offset + length` must not overflow and must be ≤ ir_size |
| 0x0712 | u64 | range_length | > 0 |
| 0x0720 | u32 | target_kind | MACHINE 1, SUBSTRATE 2 |
| 0x0721 | d32 | target_digest | machine or substrate descriptor digest |
| 0x0730 | u64 | generation | full 64-bit capability generation; non-zero |
| 0x0740 | u64 | rights | READ 1, EXECUTE_PURE 2, CONFIGURE_REVERSIBLE 4, CONFIGURE_IRREVERSIBLE 8 |

`generation` matches Omega `EffectPayload.capability_generation` (`OMEGA_EFFECT_VERSION 0x02`,
`src/omega_types.h`, merged in omega#71). A 32-bit generation (a 4-byte `0x0730` value) is refused with its own
error `FORGE_V2_ERR_GENERATION_32`, mirroring Omega's refusal of the 176-byte v1 effect payload
(`OMEGA_EFFECT_LEGACY_V1_LEN`). Omega's effect wire form is big-endian; FORGE V2 carries the generation as a
u64 **value**, little-endian like the rest of V2, not Omega's wire bytes.

The v1 `ir_payload` pointer is an in-process detail: its value is never hashed and never appears in V2.

## 10. Failure behavior

- A substrate that is faulted or unavailable (`fault_state ≠ NONE`) is **excluded from the eligible set**
  (`FORGE_V2_ERR_FAULTED`). Stale or out-of-envelope calibration excludes it the same way.
- The **digital realization always remains the fallback**: every Machine descriptor must contain a digital
  substrate, and the exact digital path stays eligible for bounded and exact contracts alike.
- A faulted device **cannot corrupt World state**: its only output path is evidence. A result is published only
  if the semantic contract check passes against the oracle; the device returning a value confers nothing.
- Decoder failures are total: on any error the output object is zeroed, so a caller cannot act on a
  half-decoded record.

## 11. Authority boundary

- **AEGIS verifies eligibility; FORGE realizes; ARGUS (later) detects only.** Nothing in V2 grants authority.
  AIEN may propose and select among admissible realizations; it never obtains a capability to a substrate by
  doing so.
- Read-only computation and irreversible physical configuration are distinct, mirroring Omega
  `CQ_FX_PURE` (0x001), `CQ_FX_READ` (0x002), `CQ_FX_EXTERNAL_WRITE` (0x040) and
  `CQ_FX_EXTERNAL_IRREVERSIBLE` (0x080) (`rx_capq.h`). A pure computation needs `EXECUTE_PURE`; a reversible
  hardware-state change needs `CONFIGURE_REVERSIBLE`; an irreversible one needs `CONFIGURE_IRREVERSIBLE`.
- Every use requires the reference's **full 64-bit generation to equal the current generation** (a difference
  only in the high 32 bits is refused). No forged authority (rights come only from the reference, validated),
  no stale handles, no host-pointer authority, no implicit device privilege (being attached to the Machine
  grants nothing).
- ARGUS status as of this draft: omega#72 is MERGED (`f308ac7`, `aienos.lock` pin only). The ARGUS
  performance gate is still treated as **not passed**: aienos#160 records +6.2% overhead against a 5% limit;
  the omega#70 re-measurement (+3.87%) is not on main. V2 makes no ARGUS ABI change; analog telemetry
  requirements stay documentation-only (ARCH-0018 §8).

## 12. Fabric implications

- A substrate reached over a link (another Machine, an external instrument) is still a capability provider
  described by the same records.
- **Transport is cost metadata, never semantic identity.** PCIe, USB, Ethernet, RDMA over Ethernet,
  board-to-board links and remote Machines differ only in `transport_class`, bandwidth and latency. Those
  enter the Machine descriptor digest and the cost model; they never enter program identity, contract
  identity, result identity, or realization identity (which uses `machine_identity`, not the descriptor
  digest). Checked by `transport_changes_descriptor_digest_not_realization_identity`.
- Cross-machine references (§9) are content-addressed, so they mean the same thing on every Machine.
- Fabric state: omega#72 is merged (`f308ac7`); a Fabric-connected analog Machine is AR6 and waits on Fabric
  F5. ARGUS gate status as in §11.

## 13. Acceptance tests (AR1..AR4 mapped)

Run `tests/run_forge_v2_gates.sh` (host-only; no hardware; binaries in `build/`). Check names below are the
exact strings the test prints.

| Gate | ARCH-0018 requirement | Covered here by | Status |
|---|---|---|---|
| AR1 | V2 descriptor and evidence encodings with KATs | `frozen_v2_machine_descriptor_digest`, `frozen_semantic_result_contract_digest`, `frozen_calibration_artifact_digest`, `frozen_execution_evidence_v2_digest`, `frozen_cross_machine_ref_digest`, `golden_vector_doc_matches_binary_output` | covered |
| AR1 | v1 KAT digest wrapped and reproduced | `v1_kat_stream_digest_reproduced`, `v1_digest_wrapped_in_digital_gpu_substrate`, `v2_bytes_carry_no_v1_fields` | covered |
| AR1 | strict decode | `roundtrip_*_strict`, `unknown_tag_rejected_strict`, `unknown_tag_skipped_lenient_same_digest`, `out_of_order_tags_rejected`, `missing_required_field_rejected`, `unknown_enum_bits_rejected`, `truncated_object_rejected`, `duplicate_substrate_identity_rejected`, `substrates_out_of_identity_order_rejected`, `zero_substrates_rejected`, `reserved_substrate_class_rejected`, `legacy_v1_digest_only_on_digital_substrate` | covered |
| AR1 | stale calibration refused | `stale_calibration_rejected`, `missing_calibration_rejected`, `calibration_from_future_rejected`, `calibration_window_capped_by_substrate`, `calibration_for_other_substrate_rejected`, `out_of_envelope_conditions_rejected` | covered |
| AR1 | v1 generation refused | `v1_32bit_generation_refused`, `legacy_version_1_object_refused`, `stale_generation_high_bits_refused` | covered |
| AR1 | `SIMULATED_DEVELOPMENT` provenance explicit | `evidence_encode_without_provenance_refused`, `evidence_decode_missing_provenance_refused`, `evidence_decode_zero_provenance_refused`, `simulated_provenance_explicit_in_kat_evidence` | covered |
| AR2 | analog simulation provider with digital oracle parity | contract/profile/evidence data model only; anchors in §6.4 | **not covered** (needs the simulation provider) |
| AR3 | staleness ⇒ ineligible; uncertainty propagates into the contract check; evidence complete | `fresh_calibration_eligible`, `stale_calibration_rejected`, `substrate_bound_looser_than_contract_rejected`, `stochastic_substrate_not_eligible_for_exact_contract`, `recalibration_changes_realization_identity` | partial (data model + eligibility; no live evidence completeness audit) |
| AR4 | first physical analog operation; clean digital fallback; faulted device cannot corrupt World; no authority bypass; no vendor identity | `faulted_substrate_excluded`, `digital_fallback_eligible_for_bounded_and_exact`, `machine_without_digital_fallback_rejected`, `irreversible_config_without_right_refused`, `irreversible_config_with_right_and_generation_allowed`, `authorize_pure_current_generation`, `reference_range_overflow_refused`, `no_vendor_names_in_v2_identifiers` | preconditions only; **no physical device exists** and none is claimed |

Also enforced by the gate script: build with `gcc -O2 -Wall -Wextra -Werror -std=c11`
(`build_gcc_O2_Wall_Wextra_Werror_std_c11`), no hardware includes / no heap in V2 code
(`no_hardware_access_no_heap_in_v2`), and the R16 loop-inventory heuristics
(`r16_loop_inventory_heuristics_clean`: no endless loops; no identifiers built from sleep, poll, recv,
heartbeat, tick, dispatch, schedule, orchestrate, turn, pulse, yield or epoll).

## 14. Open questions

1. **`physical` effect class.** Omega `RC_EFFECT_CLASSES` (`rx_contract.h:175`) has a `physical` bit with no
   `CQ_FX_*` counterpart. V2 maps hardware configuration onto `CQ_FX_EXTERNAL_WRITE` /
   `CQ_FX_EXTERNAL_IRREVERSIBLE`; should Omega add a distinct `CQ_FX_PHYSICAL`?
2. **Error-model richness.** V2 declares a guaranteed (rel, abs) bound plus a numerical profile. ARCH-0018 §3.3
   also names bias, quantization step and saturation range. Add explicit tags, or keep them inside the
   calibration transfer/noise groups?
3. **ULP bounds.** ARCH-0018 lists ULP as a unit. V2 has only relative (ppb) and absolute (LSB) bounds. A ULP
   bound needs a declared float format; defer until a float contract is needed.
4. **Machine identity source.** `machine_identity` is opaque here. Should it be the owner-enrolled key digest
   (TRUST-1), and how is it re-derived on a Machine rebuild?
5. **Calibration clock.** `epoch_s` and `execution_epoch_s` assume one authority clock per Machine. Cross-machine
   calibration comparison needs a Fabric time source or a monotonic counter instead of seconds.
6. **Omega empirical model.** omega#60's cost model has no substrate axis and its observations have no error
   field; AR5 needs both. The shape of that change belongs to Omega after R16.
7. **Evidence receipts.** Digest-named JSON receipts for V2 evidence do not exist yet; their JSON projection of
   the canonical bytes is unspecified.
8. **Verification secret.** v1's AEGIS verification token is a plain prefix hash with a compiled-in constant,
   not an HMAC. V2 defines no token; AEGIS admission for V2 should not inherit the v1 construction.
