# PD-1: MACHINE-LAW BENCHMARK (retrospective, label-stripped)

**Status:** DRAFT, docs only. No learner has been run on PD-1. No blind benchmark has been run. Companion to [`PD0_HIDDEN_EQUATION_BENCHMARK.md`](PD0_HIDDEN_EQUATION_BENCHMARK.md) (PD-0, rev 6) and [`PHYSICS0_DISCOVERY_ENGINE.md`](PHYSICS0_DISCOVERY_ENGINE.md) section 5 (PD-1 row).
**Prerequisite:** PD-0 machinery working (world, ladder checker, scorer, controls, learner: omega `src/physics0`, merged 2026-10-04; learner reaches `PROVISIONAL_LAW` on PD-0 L1 to L4).
**Conversion tool:** omega `tools/physics0/pd1-convert` (harness side, never learner side).

---

## 1. What PD-1 tests, and what it does not

PD-1 asks whether the Physics-0 engine, given only unlabeled records of what was done to the machine and what the machine then did, recovers a compact relation that predicts a delayed or failed outcome from the recorded knobs, and rejects knobs that do not matter.

The target is the GB10 behaviour campaign of 2026-10-02/03 (the engine is told none of this): a host-polled completion word behaved differently depending on one structural property of the launcher build, and a family of other changes did not help.

**Limitation, stated plainly.** PD-1 is a retrospective replay of preserved evidence. There is no live intervention, the learner cannot ask for a new experiment, and the planner (PD-0 section 8) is not exercised. The campaign was run by people who knew the hypothesis, so the knob coverage is what those people chose to run, not what a learner would have chosen.

The only claim PD-1 can support on PASS: *"From unlabeled machine observations, the system discovered a compact intervention-dependent relation that generalized to held-out machine outcomes."* No claim about caches, coherence, drivers or understanding.

## 2. Learner-visible dataset (`PD1REC1`)

Fixed-width little-endian record, 112 bytes, SHA-256 chain, version refused if unknown (layout in the tool header). Field names are opaque; the learner never sees this table's right-hand column.

| Field | Type | Learner meaning | Harness meaning (sealed) |
| --- | --- | --- | --- |
| `src` | u8 | which record series | campaign id (1 per-launch probe, 2 gate A/B under load, 3 lifecycle A/B, 4 single-trial variants) |
| `q0` | u8 | knob, 0/1, 255 if absent | allocation attribute of the completion word (derived, see section 4) |
| `q1` | u8 | knob ordinal, 255 if absent | variant index where a variant field exists (src 4 only) |
| `q2` | u8 | knob, always 255 | explicit operation present/absent: **never recorded** |
| `q3` | u8 | knob, always 255 | reuse pattern: **never recorded** |
| `q4` | u32 | ordinal position in the series | launch index or repetition number (a queue-position proxy) |
| `q5` | i64 | time knob, always absent | time since publication: **never recorded** |
| `r0` | i64 | outcome magnitude, micro units, -1 if absent | completion visibility latency |
| `r1` | u8 | outcome class 0/1 | run ok / run failed |
| `cov` | u32 | covariate milli units | host load average at campaign start (src 2 only) |

Rules: the learner reads `PD1REC1` only; it never sees directory names, arm names, commit hashes, build flags, log text or file names. The chain is verified on load.

## 3. Protocol

1. **Conversion** (harness): `make -C tools/physics0/pd1-convert run` extracts, converts, verifies the chain and runs the leak check. Output hash and row counts go into a `PD1_CONVERT` receipt (first run: omega `evidence/physics0/pd1/pd1-convert-receipt-2026-10-04.txt`, 2330 records).
2. **Splits** (harness holds them, PD-0 invariant I10): `FIT` 60% and `SELECT` 20% of records within src 1 and src 3, stratified by `q0`; `HOLDOUT-0` 20% of the same plus the whole of src 2 as a cross-series holdout. src 4 (5 rows, `q0` absent) is a development curiosity only, never scored.
3. **Hypothesis set** (learner, PD-0 vocabulary, degree at most 2 over `q0`, `q1`, `q4`, `cov`): the learner must submit at least the following rivals, found by itself or supplied as the mandatory null set by the harness: the empty relation (`r0` and `r1` independent of all knobs), `q4`-only (position explains outcome), `cov`-only (load explains outcome), and its best candidate.
4. **Preregistered predictions** (learner, hashed before holdout release): for every `HOLDOUT-0` record, the predicted `r0` (or class probability for `r1` where `r0` is absent) under the candidate and under each rival.
5. **Scoring** (harness): on `HOLDOUT-0`, candidate error versus each rival; the PD-0 scorer's NRMSE on `r0` for src 1 and src 3 records that carry it; log-loss on `r1` for src 2 and src 3 records.
6. **Compactness charge:** PD-0 section 6.2 size and `description_bits`; a candidate using `q4` or `cov` must improve holdout error by more than the bits it costs (PD-0 MDL rule), otherwise the smaller relation wins. Rejecting irrelevant knobs is scored this way, not by a separate declaration.
7. **Ladder:** PD-0 section 5 ladder with `TRIAL` replaced by the preregistered holdout prediction (no live trial exists). The top state reachable in PD-1 is therefore `PREDICTED`, not `PROVISIONAL_LAW`; the law record is written with state `HYPOTHESIS` and the holdout result in `experiments`. This is a deliberate downgrade: no intervention, no law.

## 4. Evidence sufficiency audit

Sources are those identified by the dataset scout (`~/handoffs/2026-10-04-pd1-dataset-scout.md`, read-only, 2026-10-04). "Derived" means the value is a deterministic function of a recorded fact that is not the outcome.

| Series | Preserved facts (file, field) | Derived knobs | Missing | What would have been required |
| --- | --- | --- | --- | --- |
| src 1: `E1-STALL-PROBE/20261002T193710Z` | `arm_{A,B}.stderr` lines `GB10_PROBE ... marker2_ms= sem_ms=`, 1119 per arm in launch order; `meta.txt` commits; `summary.txt` | `q0` from arm letter (runner script header: A default build, B alternative define), `q4` = line index, `r0` = `marker2_ms` x 1000, `r1` = 0 (parity PASS both arms) | `q2`, `q3`, `q5`; per-launch timestamps; host load | a per-launch log line carrying the allocation attribute as a runtime value, a submission counter and a monotonic timestamp |
| src 2: `M18-UNCACHED-20261003` | `*/exit-codes.txt` (`rep=`, `rc=`), `campaign.txt` load average, `started.txt`/`finished.txt`, `omega-head.txt` | `q0` from directory prefix (`baseline-` vs `fix-`), `q4` = rep, `r1` = rc != 0, `cov` = first load average x 1000 | `r0` (no latency per run), `q2`, `q3`, `q5` | the gate to print the measured wait per run; the allocation attribute printed by the binary itself |
| src 3: `WORLD-UNCACHED-20261003` | `*/exit-codes.txt`; `wait_ms=` in two run logs (`red-001` 1038.736, `green-001` 0.117) | `q0` from prefix (`red-`/`baseline-` vs `green-`/`fix-`), `q4` = rep, `r1` = rc, `r0` for the two runs that printed it | `r0` for 45 of 47 rows; the M18 half of this series was run before the M18 path was fixed, so its `q0` split is **not** a clean A/B (7/20 vs 6/20 FAIL in the source) | re-running the M18 half after the fix commit; wait printed per run |
| src 4: `CHIPWAIT-ASTRA-20261003/result.json` | `variant`, `result`, `expected`, `observed`, `marker2` | `q1` = variant index, `r1` = 1 for all five | `q0` (the allocation attribute of each variant is not stated as a recorded fact), any replication (1 trial each) | at least 5 trials per variant and a per-variant record of the allocation attribute |
| C3 A/B (`E1N-C3AB-101051.log`, sealed log) | per-arm aggregates (800 runs, hits, device errors) | none per run | everything per run | the sealed log opened and per-run lines confirmed; not done here, to stay lean |

Counts after conversion: src 1 2238 rows with latency; src 2 40 rows (1 fail); src 3 47 rows (14 fail, 2 with latency); src 4 5 rows. Total 2330.

**Verdict: INSUFFICIENT for the full Direction 7 target, SUFFICIENT for a reduced development benchmark.**

- Sufficient: the relation "`r0` is large when `q0` = 1 and small when `q0` = 0" is testable with 2238 per-launch rows, and the cross-series holdout (src 2, run-level class instead of latency) tests transfer to a different outcome type under load. The empty, `q4`-only and `cov`-only rivals are testable.
- Insufficient: three of the eight knob categories named in Direction 7 (reuse pattern, explicit operation, time since publication) were never recorded in any campaign, so "rejects irrelevant knobs" can only be tested on `q4` and `cov`. The src 1 effect is perfectly separable (every `q0` = 1 launch is above 1000 ms, every `q0` = 0 launch below 30 ms), so the numerical part of the task is trivial; the scientific content of PD-1 is the rejection of `q4` and `cov` and the transfer to src 2, which is a thin test. src 4 has no `q0` and no replication.
- Not fabricated: the missing fields stay sentinels. Filling them would need the experiments in the right-hand column, run live, which is PD-2 territory.

### 4.1 Circularity of the derived `q0`

`q0` is derived from the arm letter or directory prefix, which the campaign authors chose because they already believed the allocation attribute was the cause. That join does **not** encode the answer: it records which treatment each run received, not what happened, and it would be the same join if the hypothesis had been wrong (arm B would then show no difference). So the derivation is not circular in the statistical sense.

It is circular in the **coverage** sense: the only knob that varies with enough rows is the one the humans suspected. A learner that finds "`q0` predicts `r0`" has rediscovered the humans' experimental design as much as the machine's behaviour. What would break this: a campaign in which several knobs were varied factorially with the allocation attribute recorded as a runtime value, so that the learner has to pick the right knob among real alternatives. That campaign was not run and is not synthesised here.

### 4.2 Is a sealed retrospective holdout defensible?

Mechanically yes: src 2 (40 run-level outcomes under documented load, a different outcome type from the fit series) can be withheld with a public SHA-256 commitment, and the learner's predictions can be hashed before release.

Scientifically only in the weak sense. The learner's authors and every agent in this hive know the answer, the fit series separates perfectly, and the holdout tests transfer of a one-knob relation. The honest label for a PD-1 pass is therefore **development evidence of the machinery on real machine data**, not a blind discovery. Do not run PD-1 as a blind benchmark and do not report it as one.

## 5. Contamination audit

Stores the learner lane must be cut off from while PD-1 runs (the learner process reads `PD1REC1` only; the learner's *author* must also not consult these while building the PD-1 configuration):

- Cortex: every `aien-lessons` and `aien-sources` entry about the GB10 completion flag, uncached allocation, L2 flush, C3 tail, E1 stall, CHIPWAIT, M18 or WORLD uncached campaigns.
- Mind map: `gb10-completion-flag-uncached` and any note linking to it; the PD-0 and PD-1 notes themselves.
- Memory and handoff files: the memory entry `gb10-completion-flag-uncached`, `chip-repro-method-from-astra`, `~/handoffs/2026-10-04-pd1-dataset-scout.md`, this document's section 4, omega `docs/physics0/CALIBRATION.md` if it ever mentions PD-1 sources.
- GitHub: omega PRs #225 and #230 (bodies and commit messages), the `E1-SIMT-C3`, `E1-CLOSURE`, `M18-UNCACHED`, `WORLD-UNCACHED` evidence READMEs, the CHIPWAIT REPORT.md, and the runner scripts `ab_campaign.sh`, `runjob.sh`, `run_reduce_stall_probe.sh`.
- Source: the launcher sources with the fix (any file calling the uncached allocation or issuing the flush mem-op), `nvos.h` and the driver headers, the physics `nvrm/` layer.
- The conversion tool itself: `extract.sh` holds the join tables and is the answer key for `q0`.

Label-stripping checklist for the conversion tool (enforced by `leakcheck.sh` on every learner-visible output): no token from the set {cache, cached, uncached, coherent, stale, flush, l2, nvidia, nvrm, nvos, gpu, cuda, marker, sem, arm, baseline, fix, variant, oldstream, sleep, m18, world, probe, chipwait, astra, e1, commit, omega, physics, allocation, attr}; no file names, directory names, hashes or dates; column names are `src q0..q5 r0 r1 cov` only; the `src` id is an integer with no legend in the learner lane.

**Residual risk, stated plainly.** This hive shares memory. The learner code was written by sessions that have read every store above, and the PD-0 learner's candidate vocabulary was designed by people who know the PD-0 equations. For PD-1 the learner code is unchanged and generic (it reads opaque knobs), so code-level leakage is limited to the choice of vocabulary; the real leakage is that nobody involved is blind. The audit cannot remove that; it can only make it visible. Any future claim stronger than section 1's sentence needs the PD-0b author-separation rule (`PD0B_SEALED_BENCHMARK.md` section 4) applied to a *new* live campaign, which is PD-2.

## 6. Negative controls (mandatory before any PD-1 result is believed)

- NC-P1 shuffled outcomes: permute `r0` and `r1` across records within each `src` with a recorded seed; the engine must find no relation better than the empty one (zero candidates past PD-0 T3).
- NC-P2 label-permuted knob: swap `q0` values between the two arms of src 1 only; the fitted relation must flip sign, proving the learner reads `q0` and not row order.
- NC-P3 null knob added: append a knob `q6` drawn from a recorded stream independent of everything; the MDL charge must exclude it from the winning candidate.
- NC-P4 order knob: a candidate using `q4` alone must lose to the `q0` candidate on `HOLDOUT-0`; if it does not, the series has a time trend that the audit missed and the result is void.
- Positive control: the harness's own `q0` relation (fitted on `FIT` by the reference sparse solver) must pass the holdout bound; if it does not, the bound is wrong, not the learner.

## 7. Pass / fail

PASS requires all of: the controls of section 6 hold; the learner's winning candidate uses `q0` and no other knob; the preregistered holdout predictions beat every rival on `HOLDOUT-0` (src 1 and src 3 NRMSE within the PD-0 L2 bound, inherited and provisional; src 2 log-loss better than the empty relation by a margin the harness fixes before the run and records in the receipt); and the law record states the observed domain (two values of `q0`, one machine, one driver, one day) and the exceptions (the 14 src 3 failures that occur under both `q0` values, from the pre-fix M18 half). FAIL otherwise. Either outcome is recorded in a new `PD1_RESULT` receipt; nothing is retried after seeing holdout numbers.

## 8. Open items

- Whether to run the reduced PD-1 at all before PD-2, given section 4.2. Recommendation: run it once as development evidence with the controls, record the result, make no discovery claim.
- The C3 A/B sealed log: open it only if someone needs per-run rows; it is not needed for the reduced benchmark.
- src 3's pre-fix M18 half: keep as the recorded exception set, or drop by series rule fixed before the run. Operator choice; default keep.
