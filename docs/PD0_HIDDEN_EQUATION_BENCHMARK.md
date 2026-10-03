# PD-0: HIDDEN-EQUATION BENCHMARK

**Status:** DRAFT, docs only. Part of [`PHYSICS0_DISCOVERY_ENGINE.md`](PHYSICS0_DISCOVERY_ENGINE.md). No code exists yet. Every threshold below is a **frozen initial value** that no one has yet run a reference solver against; see section 11 for what must happen before any learner is scored.
**Language:** Rust scaffolding per ADR 0024 (aien-architecture `docs/adr/0024-rust-scaffolding-omega-destination.md:21`), wire formats language-neutral (same file, lines 30 to 35). No Python anywhere.
**Wire conventions:** little-endian, fixed-width integers, no floating point in any canonical byte, fractions as integers. This copies the rule of `docs/FORGE_SUBSTRATE_V2_SPEC.md:42-45`.

---

## 1. What PD-0 tests

A learner is given a black box: a dynamical system whose equations it cannot see. It may reset the box to a chosen starting state, apply bounded pushes, and read back measurements, within a fixed budget. It must produce a **compact written relation** that predicts the box on situations it has not seen, and it must reach that relation through the law ladder (section 5), not by curve fit alone. A relation that fits but is not compact, or that was not tested by deliberate experiments, does not pass.

PD-0 is a plumbing and validity benchmark. It is not a blind scientific test, because the generators are written in this public repository (section 2.4).

## 2. Black-box world interface

### 2.1 Number format

All world values are signed 64-bit integers in **micro-units** (1 unit = 1_000_000). Product rescale: `mul(a, b) = (a * b) / 1_000_000`, computed in 128-bit, truncating toward zero. Division by a constant uses the same truncation. Every generator below is exact integer arithmetic, so a world run is bit-reproducible from its seed on any machine.

### 2.2 What the learner is told (the `describe` record)

| Field | Type | Meaning |
| --- | --- | --- |
| `magic` | 8 bytes | `PD0DESC1` |
| `n_obs` | u8 | number of observed variables (labels `s0`, `s1`, ...) |
| `n_channels` | u8 | number of intervention channels (labels `c0`, ...) |
| `dt_micro` | i64 | time per tick, micro-units |
| `chan_min[n_channels]`, `chan_max[n_channels]` | i64 | inclusive intervention bounds |
| `reset_min`, `reset_max` | i64 | inclusive box for each observed variable at reset |
| `episode_max_steps` | u32 | steps per episode, fixed 100 |
| `budget_steps` | u32 | total steps for this world instance |
| `budget_episodes` | u32 | total resets for this world instance |

The learner is **not** told: the level number, the generator, any constant, the number or existence of hidden variables, the noise level, or the seed. The `describe` record has no free-text field.

### 2.3 Calls

Three calls, as a language-neutral byte protocol over a pipe or socket between two processes (world process and learner process). A Rust trait is an implementation detail, not the contract.

| Call | Request | Effect | Charged |
| --- | --- | --- | --- |
| `describe` | none | returns the describe record | free |
| `reset` | `n_obs` values, each in `[reset_min, reset_max]` | starts a new episode with that observed state, hidden variables set to 0; returns the first observation | 1 episode |
| `step` | `channel` u8 (255 = none, i.e. passive), `value` i64 | applies the intervention for one tick; returns one observation record | 1 step |

Bounded intervention rules:

- A `step` value outside `[chan_min, chan_max]`, an unknown channel, or a `reset` value outside the reset box returns an observation record with `status = REFUSED_RANGE`, the state unchanged, and still costs the step or episode. Probing the bounds is therefore not free.
- A step that would take any observed or hidden variable outside `|v| <= 10_000_000` (10.0 units) ends the episode with `status = OUT_OF_BOUNDS`. No post-step state is revealed.
- After `episode_max_steps` steps the episode ends with `status = EPISODE_END` on the last record.
- When either budget is spent, every call returns `status = BUDGET_EXHAUSTED`.
- Exactly one channel is driven per step. All other channels are 0 for that step.

### 2.4 No access to the generating equations

- The world runs in its **own process**. The learner process cannot read its memory or files.
- The learner crate must have **no dependency path** to the world crate or its generator code. Gate G5 (section 11) checks this on the dependency graph.
- Instance seeds are chosen by the harness. Development instances use public seeds 1 to 20. Scored instances use seeds whose SHA-256 commitment is recorded in the repository before any scored run and revealed afterwards, the same discipline the Brownian program uses for its answer key (note `brownian-discovery-program`: "Public repos get only SHA-256 of the profile until [the] seal"). UNVERIFIED that this exact mechanism fits here (confidence: medium).
- **Limitation, stated plainly:** the generators in section 4 are public text. A learner whose developers read this file is contaminated for these generator families. PD-0 therefore validates the machinery (interface, ladder, scoring, negative controls). A blind variant with sealed generator families (PD-0b) is an open question for the operator and is not specified here.

### 2.5 Observation record (`PD0REC1`)

One record per `reset` or `step`. This is what PHYSICS (the stand-in recorder in PD-0) writes and Cortex retains. The learner can only read it.

| Offset | Field | Type | Meaning |
| --- | --- | --- | --- |
| 0 | `magic` | 8 bytes | `PD0REC1\0` |
| 8 | `version` | u16 | 1; readers refuse other values |
| 10 | `n_obs` | u8 | observed variables |
| 11 | `status` | u8 | 0 OK, 1 REFUSED_RANGE, 2 OUT_OF_BOUNDS, 3 EPISODE_END, 4 BUDGET_EXHAUSTED |
| 12 | `kind` | u8 | 0 reset, 1 step |
| 13 | `channel` | u8 | channel driven, 255 none |
| 14 | `reserved` | 2 bytes | zero |
| 16 | `seq` | u64 | global record number from 0 |
| 24 | `episode` | u32 | episode number from 0 |
| 28 | `step_in_episode` | u32 | 0 at reset |
| 32 | `time_micro` | i64 | `step_in_episode * dt_micro` (the time variable) |
| 40 | `requested` | i64 | intervention value as requested |
| 48 | `applied` | i64 | value actually applied (equals requested unless refused, then 0) |
| 56 | `vars_before[n_obs]` | i64 each | observed variables before the step (the "variables") |
| then | `vars_after[n_obs]` | i64 each | observed variables after the step (the "outcome"); equals `vars_before` if refused |
| then | `prev_hash` | 32 bytes | `record_hash` of record `seq-1`; all zero for `seq` 0 |
| then | `record_hash` | 32 bytes | SHA-256 of every preceding byte of this record |

The primitive the learner trains on is `(vars_before, applied) -> vars_after`. Hidden variables never appear in a record. In a `reset` record, `vars_before` equals `vars_after` equals the reset values.

A reader recomputes the chain on load and refuses a broken chain. An unknown `version` is refused.

## 3. Randomness

- `SplitMix64` (Vigna's published constants: increment `0x9E3779B97F4A7C15`, mixers `0xBF58476D1CE4E5B9` and `0x94D049BB133111EB`) is the only generator. UNVERIFIED that these constants are typed correctly here; the implementer must check them against the published reference and record the check (confidence: high).
- Streams: `stream(seed, tag) = SplitMix64 seeded with seed XOR fnv1a64(tag)`. Tags used: `"const"` (hidden constants), `"noise"` (observation noise), `"score"` (scorer schedules), `"null"` (null world).
- Uniform draw: `u = next() % 1_000_001` (micro-units in `[0, 1.0]`).
- Constant draw in `[lo, hi]` (micro-units): `lo + ((hi - lo) * u) / 1_000_000`, drawn in the order listed in each level.
- Noise draw (Irwin-Hall, four uniforms, standard deviation of the sum 577_350 micro): `e = ((u1 + u2 + u3 + u4) - 2_000_000) * sigma_obs / 577_350`.

## 4. Level ladder and hidden generators

Notation: `s` observed state, `u` the applied intervention on the channel named, `dt` = `dt_micro`. All updates use the **old** state on the right side (explicit Euler, simultaneous update). `*` means `mul`. Variables are `s0, s1, ...`. The equation text below appears only here and in the world crate. The learner never sees it.

| Level | Name | Observed | Hidden | Channels | `dt` | Intervention bounds | Instance budget |
| --- | --- | --- | --- | --- | --- | --- | --- |
| L0 | kinematics | s0, s1 | none | c0 | 1.0 | [-1.0, 1.0] | 3000 steps, 300 episodes |
| L1 | spring | s0, s1 | none | c0 | 0.05 | [-2.0, 2.0] | 3000 steps, 300 episodes |
| L2 | damped spring | s0, s1 | none | c0 | 0.05 | [-2.0, 2.0] | 3000 steps, 300 episodes |
| L3 | coupled oscillators | s0 to s3 | none | c0, c1 | 0.05 | [-2.0, 2.0] each | 3000 steps, 300 episodes |
| L4 | nonlinear spring | s0, s1 | none | c0 | 0.05 | [-2.0, 2.0] | 3000 steps, 300 episodes |
| L5 | noisy observations | s0, s1 | none | c0 | 0.05 | [-2.0, 2.0] | 8000 steps, 300 episodes |
| L6 | hidden variable | s0, s1 | h | c0 | 0.05 | [-2.0, 2.0] | 8000 steps, 300 episodes |

Reset box for all levels: each observed variable in `[-2.0, 2.0]` (the training box).

### 4.1 Generators

**L0 kinematics.** No constants. `s0' = s0 + s1`, `s1' = s1 + u`. (Drake's form: `x(t+1) = x + v`, `v(t+1) = v + a`, with `a` the intervention.)

**L1 spring.** Constant `k` in `[1.0, 9.0]`.
`s0' = s0 + s1 * dt`
`s1' = s1 + (-(k * s0) + u) * dt`

**L2 damped spring.** Constants drawn in order: `k` in `[1.0, 9.0]`, `c` in `[0.2, 2.0]`.
`s0' = s0 + s1 * dt`
`s1' = s1 + (-(k * s0) - (c * s1) + u) * dt`

**L3 coupled oscillators.** Constants in order: `k` in `[1.0, 4.0]`, `g` in `[0.5, 2.0]`. Channel `c0` pushes `s1`, channel `c1` pushes `s3`; the undriven channel is 0.
`s0' = s0 + s1 * dt`
`s1' = s1 + (-(k * s0) + g * (s2 - s0) + u0) * dt`
`s2' = s2 + s3 * dt`
`s3' = s3 + (-(k * s2) - g * (s2 - s0) + u1) * dt`

**L4 nonlinear spring (cubic).** Constants in order: `k` in `[1.0, 4.0]`, `b` in `[0.5, 2.0]`.
`s0' = s0 + s1 * dt`
`s1' = s1 + (-(k * s0) - b * (s0 * s0 * s0) + u) * dt`

**L5 noisy observations.** The L2 world (constants drawn the same way) with measurement noise: the learner sees `s_obs = s + e` for each observed variable, `e` from the `"noise"` stream with `sigma_obs = 0.02` (20_000 micro). The underlying state evolves noise-free. Noise applies to every observation including `reset` records.

**L6 hidden variable.** Constants in order: `k` in `[2.0, 5.0]`, `m` in `[0.5, 1.0]`, `w` in `[1.0, 2.0]`, `q` in `[0.5, 1.0]`. Hidden `h`, 0 at every reset.
`s0' = s0 + s1 * dt`
`s1' = s1 + (-(k * s0) + m * h + u) * dt`
`h'  = h + (-(w * h) + q * s0) * dt`
Only `s0, s1` are observed. The scale of `h` is not identifiable, so no coefficient involving `h` is scored (section 6.3).

### 4.2 Level validity checks (builder must run and record before any learner is scored)

- V1 (positive control): a test-only **oracle solver** that is handed the generator form and fits only constants passes every level (section 6). Failure means the benchmark is broken, not the learner.
- V2: for L6, the best model **without** a latent variable (best of the library at the level's size bound) must fail the L6 prediction bound. Failure means the hidden variable is not necessary and L6 is invalid.
- V3: under a uniform random intervention schedule, no seed in the development set `1..20` produces `OUT_OF_BOUNDS` in more than 5% of episodes at any level. Stability of L3 and L6 over the stated constant ranges is UNVERIFIED (confidence: medium; `m*q/w <= 1` and `k >= 2` were chosen to keep L6 stable by hand argument only).

## 5. The law ladder (state machine)

States, in order:

```text
OBSERVATION -> CORRELATION -> CANDIDATE -> HYPOTHESIS -> PREDICTED
            -> INTERVENED -> REPLICATED -> PROVISIONAL_LAW
side states: REFUTED (kills one hypothesis), REJECTED (nothing found)
```

A transition happens only when every item of its evidence list exists as recorded, hash-referenced data. The learner proposes; the **scorer-side ladder checker** (part of the harness, outside the learner) decides. A transition the checker cannot verify does not happen.

Data splits. Every record the learner has ever seen is tagged by the checker as `FIT` (60% of episodes), `SELECT` (20%, model selection only) or `HOLDOUT-0` (20%, never used for fitting or selection). Records gathered after a prediction is registered are new, tagged `TRIAL`. Records gathered for replication are tagged `REP` and may never have influenced any fit.

| # | Transition | Evidence required | Failure path |
| --- | --- | --- | --- |
| T1 | start -> OBSERVATION | at least 100 `PD0REC1` records with `status = OK` from at least 5 episodes, chain verified | stay; gather more |
| T2 | OBSERVATION -> CORRELATION | at least one pair (a variable or the applied intervention at time t, a one-step change of an observed variable) with absolute Pearson correlation at least 0.3 over at least 100 records, and permutation-test `p <= 0.01` after Bonferroni correction over all pairs tested, using at least 2000 shuffles with a recorded shuffle seed | stay; gather more; if the budget is spent, REJECTED |
| T3 | CORRELATION -> CANDIDATE | one explicit relation (a set of equations, one per observed variable, each a sum of monomials with fixed coefficients, plus any declared latent variables) with size within the level bound (section 6.2), selected by minimum description length on `SELECT`, and for noise-free levels one-step NRMSE on `FIT` at most 0.05; for L5 the residuals are white (lag-1 autocorrelation within +/-0.1) and no library term still correlates with them at `p <= 0.01` | stay; widen library; if the budget is spent, REJECTED |
| T4 | CANDIDATE -> HYPOTHESIS | at least one **rival** relation (a different term set, or the explicit null "no relation") also recorded; a **falsifier** declared and hashed before any `TRIAL` data: declared prediction bound `eps` (rollout NRMSE over 20 steps) with `eps <=` the level bound of section 6.1 (noisy levels: section 6.1 note), and a minimum trial count | stay |
| T5 | HYPOTHESIS -> PREDICTED | at least 5 intervention schedules chosen by the planner (section 8), none equal to any schedule hash in `FIT`, `SELECT` or `HOLDOUT-0`, each with predicted trajectories from the hypothesis **and** from each rival, hashed and recorded before the schedule is run | stay |
| T6 | PREDICTED -> INTERVENED | all registered schedules executed through the bounded API; records tagged `TRIAL`; each trial passes if rollout NRMSE (20 steps) is within the declared `eps`; **all** trials pass | any failing trial: the hypothesis moves to REFUTED, the failing record is appended to its `exceptions` list, and the learner returns to CANDIDATE with those records now in `FIT` |
| T7 | INTERVENED -> REPLICATED | at least 3 independent `REP` batches, each at least 10 episodes of 20 steps, schedules drawn from fresh streams never used before, half planner-chosen and half uniform random; each batch within `eps` and no single episode above `3 * eps` | a failing batch: REFUTED as in T6 |
| T8 | REPLICATED -> PROVISIONAL_LAW | the law record of section 7 written with `confidence_ppm` computed by the section 7 rule, `exceptions` listed, all experiment hashes included, every record reference verifying against the chain | stay |

Rules:

- `PROVISIONAL_LAW` is the top. There is no promotion beyond it. A later refuting record demotes the law to HYPOTHESIS, appends an exception, and recomputes confidence. Only evidence moves confidence (invariant I5).
- A transition is atomic and receipted: `PD0_LADDER_T<n>` with the evidence hashes.
- The learner may not skip a state. The checker refuses a law record whose evidence list skips one.
- "Replication count" has two meanings and both are required: T7 inside one world instance, and the 5-instance count of section 6.1 across fresh instances of one level.

## 6. Per-level pass criteria

A level passes when **all 5** independent world instances (5 fresh seeds, each with its own constants and its own budget, starting from nothing) pass all of 6.1 to 6.3. Zero retries within a level. Instance seeds are fixed (section 2.4).

### 6.1 Held-out prediction error

The scorer holds the generator. After a learner declares its final relation on an instance, the scorer generates held-out scoring episodes: 20 episodes of 20 steps from the `"score"` stream, with initial states and intervention schedules the learner has never seen. Half start inside the training box `[-2, 2]`, half in the extrapolation box `[-3, 3]` (reset restricted to `[-2, 2]` for the learner, so extrapolation episodes are scorer-only). For L6 every episode starts with `h = 0`.

The learner supplies a prediction for each episode from the initial observed state and the intervention sequence. The scorer compares with the **noise-free true trajectory**.

`NRMSE = max over variables j of ( RMSE_j / std_j )`, where `RMSE_j` is the root mean square error of variable `j` over all steps and episodes and `std_j` is the standard deviation of the true values of variable `j` over the same set.

| Level | In-box NRMSE (20-step) bound | Extrapolation-box bound | One-step NRMSE bound |
| --- | --- | --- | --- |
| L0 to L4 | 0.02 | 0.06 | 0.01 |
| L5 | 0.02 (versus noise-free truth) | 0.06 | 0.01 (versus noise-free truth) |
| L6 | 0.03 | 0.08 | not scored (hidden state not given) |

Note for the T4 and T6 trials on L5: the learner can only compare with noisy observations, so the in-ladder declared `eps` for L5 is capped at 0.05. The final 6.1 score still uses noise-free truth.

### 6.2 Compactness

Size of a relation `|R|` = (number of nonzero monomial terms summed over all equations, counting an intervention term as one term) + (number of declared latent variables). The generator's true size `S*` and the bound `|R| <= S* + 2`:

| Level | True terms | `S*` | Bound |
| --- | --- | --- | --- |
| L0 | `Ds0`: s1; `Ds1`: u | 2 | 4 |
| L1 | `Ds0`: s1; `Ds1`: s0, u | 3 | 5 |
| L2 | `Ds0`: s1; `Ds1`: s0, s1, u | 4 | 6 |
| L3 | `Ds0`: s1; `Ds1`: s0, s2, u0; `Ds2`: s3; `Ds3`: s0, s2, u1 | 8 | 10 |
| L4 | `Ds0`: s1; `Ds1`: s0, s0^3, u | 4 | 6 |
| L5 | as L2 | 4 | 6 |
| L6 | `Ds0`: s1; `Ds1`: s0, h, u; `Dh`: h, s0; plus 1 latent | 7 | 9 |

(`Dx` is the one-tick change of `x`; the `dt` factor sits in the coefficient.) Further rules: monomial degree at most 3; no lookup tables, no stored trajectories, no non-parametric component; any such component counts one term per stored number, so a memoriser fails by size. The relation must be expressible in the canonical form of section 7.

### 6.3 Structure and constants

- **Support:** every true term (L0 to L5) must appear. Extra terms are limited only by the size bound of 6.2.
- **Constants:** every true coefficient (in micro-units) within 5% of the true value (L0 to L4), 10% (L5). L6: no coefficient is scored, but the relation must declare at least one latent variable and its observed-only prediction must meet 6.1.
- **L6 additional condition:** the learner's best latent-free model, evaluated by the scorer, must **fail** the 6.1 bound by at least a factor of 3. This proves the learner's improvement came from the latent and not from loose bounds.

### 6.4 Replication

5 of 5 instances pass 6.1 to 6.3, and within each instance the ladder of section 5 reaches `PROVISIONAL_LAW` with T7 satisfied.

### 6.5 Level receipts

Pass writes a new receipt `PD0_L<n>_PASS` containing the seed commitments, the five law records, the scorer output, the harness commit and the label `STAND_IN` for recorder and range check. Fail writes `PD0_L<n>_FAIL` with the reason. Neither overwrites an earlier receipt. Level `n+1` is attempted only after level `n` passes.

## 7. Law record format (`PDLAW1`)

A canonical fixed byte layout (language-neutral, version-refused-if-unknown), with a non-canonical JSON projection for humans. All integers little-endian.

| Field | Type | Meaning |
| --- | --- | --- |
| `magic` | 8 bytes | `PDLAW1\0\0` |
| `version` | u16 | 1 |
| `state` | u8 | 0 REJECTED, 1 REFUTED, 2 HYPOTHESIS, 3 PROVISIONAL_LAW |
| `law_id` | 32 bytes | SHA-256 of the canonical bytes with this field zeroed |
| `relationship` | block | see below |
| `observed_domain` | block | see below |
| `confidence_ppm` | u32 | see rule below, 0 to 1_000_000 |
| `exceptions` | list | each: `record_seq` u64, `record_hash` 32 bytes, `predicted` i64, `observed` i64, `error_micro` i64 |
| `experiments` | list | each: `experiment_id` 32 bytes (hash), `kind` u8 (0 trial, 1 replication batch), `prereg_hash` 32 bytes, `outcome_hash` 32 bytes, `result` u8 (0 pass, 1 fail), `first_seq` u64, `last_seq` u64 |
| `chain_root` | 32 bytes | `record_hash` of the last record the law depends on |
| `claim_text` | length-prefixed bytes | generated only from the template below |

`relationship` block: `n_vars` u8 (observed plus latent), `n_latent` u8, `n_equations` u8, then per equation: `target` u8 (variable index), `n_terms` u16, then per term: `coef` i64 (micro-units), `exponents[n_vars + n_channels]` u8 each (powers of each variable then each channel; an intervention term has exponent 1 on its channel and 0 elsewhere). Variable order: observed, then latent. Equation form: `Dtarget = sum of coef * monomial`, where `Dtarget` is the one-tick change.

`observed_domain` block: per observed variable `min` i64, `max` i64 (the range actually visited in `FIT`, `SELECT` and `TRIAL` records); per channel `min` i64, `max` i64 applied; `dt_micro` i64; `n_observations` u64 (all `status = OK` records used); `n_episodes` u32; `boundary_conditions` as the fixed set: reset box, episode length, latent reset value (0 when declared).

`confidence_ppm` rule (author's choice, UNVERIFIED as the best rule, confidence: low-medium): `floor(1_000_000 * (p + 1) / (p + f + 2))`, where `p` is the number of registered held-out episodes (T6 trials and T7 batch episodes) within `eps` and `f` the number outside. A Laplace rule of succession over recorded held-out trials. No other input changes it.

`claim_text` template (the only allowed phrasing): `Across <n_observations> observations under <domain summary>, this relation predicts within <eps> (rollout NRMSE, 20 steps). Confidence <confidence_ppm> ppm. <n_exceptions> exceptions recorded.` No sentence using "always", "universally" or "true" may be generated.

Illustrative JSON projection for an L1 instance (values illustrative, never run, UNVERIFIED as a real result):

```json
{
  "state": "PROVISIONAL_LAW",
  "relationship": [
    {"target": "s0", "terms": [{"coef": 50000, "monomial": "s1"}]},
    {"target": "s1", "terms": [{"coef": -200000, "monomial": "s0"}, {"coef": 50000, "monomial": "c0"}]}
  ],
  "observed_domain": {"s0": [-2500000, 2500000], "s1": [-3000000, 3000000], "c0": [-2000000, 2000000], "n_observations": 2400},
  "confidence_ppm": 972222,
  "exceptions": [],
  "experiments": ["trial x5", "replication batch x3"]
}
```

(Here `k = 4.0` and `dt = 0.05`, so the `s1 -> s0` coefficient is 0.05 = 50000 and the `s0 -> s1` coefficient is `-k * dt = -0.2`.)

## 8. Experiment planner contract

Language-neutral contract; the algorithm is free.

**Inputs:** the live hypothesis set `H` (at least the best candidate and one rival, possibly the null); the visited domain (from `observed_domain`); the remaining budget; the API bounds from `describe`; a planner seed.

**Output (`PD0EXP1` record):** a schedule (a reset state inside the reset box plus a sequence of at most 20 `(channel, value)` steps inside the bounds); `expected[h]` = the predicted trajectory for every `h` in `H`; the divergence score `D`; the schedule hash.

**Selection rule:** choose the schedule that maximizes `D(s) = max over pairs (i, j) in H of max over steps of |pred_i(s) - pred_j(s)| / r`, where `r` is the pooled residual standard deviation of the hypotheses on `SELECT`. Candidate set: at least 256 seeded random schedules plus local refinement of the top 16. Ties break by lowest total absolute intervention, then by lexicographic schedule hash.

**Required behaviours:**

1. Deterministic given inputs and seed.
2. Never emits a request outside the API bounds (a refused step in a planner schedule is a planner defect and a gate failure).
3. Never repeats a schedule hash already in `FIT`, `SELECT`, `HOLDOUT-0`, `TRIAL` or `REP`.
4. If the best `D < 3` (rivals indistinguishable at three residual standard deviations), the planner returns `NO_DISCRIMINATING_EXPERIMENT`. The learner must then widen its domain (it may not repeat a tested range) or report `REJECTED`. It may not fake a divergence.
5. It must push into regions rarely visited: at least 20% of planner schedules in a run must start or reach outside the middle 50% of the visited range of some variable. UNVERIFIED that 20% is a good number (confidence: low); it is a starting value to prevent a planner that only re-tests the comfortable centre.

## 9. Negative controls and PD-1 pointer

### 9.1 Negative controls (mandatory gates; red before green)

**NC-1: null world.** A world with the same interface and record shape as L2 in which **nothing depends on anything**: each observed variable after each step is a fresh independent draw `e` from the `"null"` stream with standard deviation 0.5 (the same Irwin-Hall form), regardless of state or intervention. 20 instances, seeds `101..120`, full L2 budget. **Pass condition: the engine emits zero `PROVISIONAL_LAW` records in 20 of 20 instances.** The highest ladder state reached is recorded in the receipt and is not itself a failure. A single `PROVISIONAL_LAW` is a failed gate `PD0_NC1_FAIL`.

**NC-2: shuffled real data.** Take a recorded L1 run and permute the outcome blocks (`vars_after`) across records with a recorded seed, rebuilding valid hash chains. The learner is fed the permuted data only. Same pass condition: zero laws in 20 permutations.

**NC-3: mutated pass criteria must fail.** The harness runs the scorer with deliberately wrong inputs; every row must produce the stated verdict, or the scorer is not trustworthy.

| Mutation | Action | Expected verdict |
| --- | --- | --- |
| M1 | oracle solver's recovered `k` multiplied by 1.10 | FAIL (6.3 constants) |
| M2 | oracle relation padded with 5 spurious terms having tiny nonzero coefficients | FAIL (6.2 size) |
| M3 | memoriser: a lookup table of all training transitions, nearest-neighbour prediction | FAIL (6.2 size, and extrapolation bound 6.1) |
| M4 | "always emit a law" stub learner (writes a law record with no evidence) | FAIL (ladder checker refuses the record; NC-1 red) |
| M5 | the 6.1 bound loosened 100 times | the scorer must still FAIL M1 through M4 on structure, constants and size; if any passes only because of the loosened bound, the criteria were redundant and that is reported |
| M6 | trial schedule reused from `FIT` data (leak) | FAIL (T5 refuses a repeated schedule hash) |

**Positive control (V1):** the oracle solver passes every level. Together with NC-1 to NC-3, a result is believed only if both the oracle passes and every null or mutated run fails. Receipt `PD0_CONTROLS_PASS` is required before any learner receipt is accepted.

### 9.2 PD-1 pointer (not specified here)

PD-1 is the second benchmark and the test that matters: can the engine discover a machine behaviour nobody told it? Retrospective target: the GB10 stale-command bug of 2026-10-02, where a host-polled completion word placed in GPU-cacheable memory became visible about 1.06 s late and rarely never, fixed by allocating it GPU-uncached plus an L2 flush (mind map note `gb10-completion-flag-uncached`; omega#230, fb36109 per that note). PD-1 would give the learner only intervention and outcome records with **labels stripped**: no words "cache", "stale", "flush", "uncached"; opaque variable names; interventions (allocation attribute, flush present or absent, wait style) and outcomes (host-visible delay, or never). Success: the learner recovers, as a compact relation with a law record, that delay depends on the allocation attribute and the flush and not on the other recorded knobs. UNVERIFIED: that per-launch intervention and outcome records of the campaign were preserved in a form that can be stripped and replayed (confidence: low-medium). Item for the PD-1 spec: the learner must not have the mind map or Cortex entries about this bug. A separate document will specify it.

## 10. Explicit non-goals

- No English. The learner reads and writes no natural-language text. `claim_text` is a fixed template for humans, not learner output.
- No Llama and no pretrained model of any kind in the discovery lineage. Pretrained-model experiments are a separately named class outside Physics Zero (aien-architecture `doctrine/DISCOVERY.md:516`). Native Llama inference (aienos#34) is separate infrastructure.
- No reward model, no reward shaping, no reinforcement signal.
- No human preference data, no human-written labels, no human physics terms or constants given to the learner.
- No hardware access in PD-0. No driver call, no GPU, no live Spark intervention.
- No change to PHYSICS, FORGE, AEGIS, any receipt, binary or gate in this repository.
- No claim that any learner exists or passes.

## 11. Gates and what a builder must do first

Builder order (each step receipted, no step skipped):

1. G0 harness self-test: wire records round-trip, hash chain verifies, unknown version refused, reproducibility (same seed, same record bytes, run twice).
2. G1 level validity V1 to V3 (section 4.2) and the oracle solver passes L0 to L6 under the section 6 criteria.
3. **Threshold calibration check:** the section 6 numbers are untested initial values. Run the oracle solver and one plain reference solver (a sparse least-squares fit over the monomial library, written in Rust, no Python) and record their scores. If the oracle fails a bound, that is a spec defect: stop and report with the numbers. Do not tune a bound silently. Any bound change needs the operator's approval and a new revision of this file.
4. G2 ladder checker unit tests: each transition accepts valid evidence and refuses every missing-evidence case; skipping a state is refused.
5. G3 planner contract tests (section 8 behaviours 1 to 4).
6. G4 negative controls NC-1 to NC-3 and the positive control (`PD0_CONTROLS_PASS`).
7. G5 isolation check: the learner crate's dependency graph has no path to the world crate; the world runs in a separate process.
8. Only then score a learner, level by level (section 6.5).

Suggested crates (names only, layout open): `pd0-wire` (byte layouts), `pd0-world` (generators, own binary), `pd0-recorder` (stand-in for PHYSICS, labelled `STAND_IN`), `pd0-guard` (stand-in for AEGIS range check, labelled `STAND_IN`), `pd0-ladder` (checker), `pd0-score`, `pd0-planner`, `pd0-learner`. Boundaries between crates follow ADR 0024 lines 30 to 35.

## 12. Open items

- Where the code lives (this repository or a new one): operator decision.
- PD-0b, a blind variant with sealed generator families: operator decision (section 2.4).
- Whether the doctrine owner accepts the P0 tier mapping (overview, section 2).
- All numeric bounds: UNVERIFIED until step 3 of section 11.
