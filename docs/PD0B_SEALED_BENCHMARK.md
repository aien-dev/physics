# PD-0b: SEALED HIDDEN-EQUATION BENCHMARK

**Status:** DRAFT, docs only. Nothing is implemented. Companion to [`PD0_HIDDEN_EQUATION_BENCHMARK.md`](PD0_HIDDEN_EQUATION_BENCHMARK.md) (PD-0, rev 2). This document records the contract for the sealed variant and nothing else: no new numbers, no new levels, no new record formats.

## 1. Why PD-0b exists

PD-0 is the transparent engineering qualification. It validates the apparatus, the ladder, the scorer and the controls. It makes no scientific claim, because its generator families are public text (PD-0 section 2.4, "Limitation, stated plainly"): anyone who built the learner may have read them. PD-0b is the only variant that may carry the claim of blind discovery, and it may carry that claim only if every rule in this document is met and receipted.

## 2. What is identical to PD-0

- The black-box world interface: the `describe` record, the calls, the bounds, the budget rules (PD-0 section 2). A learner cannot tell from the interface whether it is running PD-0 or PD-0b.
- The record formats: `PD0REC1` observation records and `PDLAW1` law records, unchanged (PD-0 sections 2.5 and 7).
- The randomness discipline (PD-0 section 3), the law ladder and its scorer-side checker (section 5), the data splits, the experiment planner contract (section 8), the negative and positive controls (section 9), and the non-goals (section 10).
- Every pass criterion and threshold of PD-0 section 6 and every transition rule of section 5 are **inherited unchanged**. Each inherited threshold keeps its PD-0 status: a frozen initial value, **provisional** until PD-0's section 11 steps have run against the reference solver. PD-0b adds no threshold of its own and tightens or loosens none.

## 3. What is sealed

In PD-0 the generator **families** (the equation forms of each ladder level) are public and only seeds are committed. In PD-0b the families themselves are hidden.

- The sealed material lives in the private `aien-sealed` repository: for each level, the generator family (its equation form and the ranges its constants are drawn from) and the scored instance seeds.
- Before any PD-0b run, the public `physics` repository records a SHA-256 commitment for **each family** and for **each scored seed**, one line per item, in a receipt named `PD0B_COMMIT`. The commitment covers the exact bytes that will later be revealed. A commitment that is recorded after a run started, or that does not match the revealed bytes, voids every run under it.
- The level ladder keeps its shape (the same number of levels, the same budget per level, the same five instances per level). Only the equation families behind the levels are unknown to the learner lane.
- Validity of the sealed families is the sealing author's job. Before `PD0B_COMMIT`, the sealing author runs PD-0 section 4.2 (V1 oracle positive control, V2 latent necessity, V3 breach rate) and the section 11 step 3 threshold calibration against each sealed family, inside the sealed environment, and publishes only a receipt `PD0B_VALIDITY` with per-level pass/fail and breach rates, no equations and no constants. A family that fails is replaced before commitment; a run over an unvalidated family is void.
- Known leak, declared: the `describe` record (variable count, channel count, `dt`, bounds, budget) and the level order still tell the learner the level class (for example a larger budget marks the noisy or hidden-variable levels, as in PD-0). PD-0b therefore permutes the order in which levels are presented and may equalise budgets across levels; either choice is recorded in `PD0B_COMMIT`. The describe record is the only level information the learner receives, and this leak is accepted and stated rather than hidden.

## 4. Author separation (the hard requirement)

Two roles, which must never be the same session or person:

- **The sealing author** writes the families and seeds, computes the commitments, and records them. From that moment on, the sealing author never briefs, reviews, advises or messages the learner author, directly or through an intermediary.
- **The learner author** builds the learner that runs PD-0b. The learner author reads nothing produced by the sealing author's session.

Concrete exclusions. Everything the sealing session produced is excluded from what the learner lane can read: its chat transcript, its handoff files, its Cortex entries (both `aien-lessons` and `aien-sources`), its mind map notes and whispers, its crumb messages, and any ledger or inbox lines it wrote. The sealing session must not write to any of those shared stores while sealing; its only public output is the commitment receipt.

**The known risk, stated plainly.** This hive shares memory. Every agent reads the same handoffs, the same mind map, the same Cortex and the same crumb whispers, and the queen's chat spans sessions. A sealing author who works in that environment the ordinary way leaks the families into exactly the stores the learner author reads. The separation rule is therefore the hardest part of PD-0b, not a formality. Before the first seal, the operator must name the sealing author, confirm it ran outside the shared stores, and record that confirmation in the receipt. If that confirmation cannot be given, PD-0b has not been run, whatever the scorer says.

## 5. Learner frozen before unseal

The learner is frozen by commit hash before the seal is opened. The receipt `PD0B_FREEZE` records the learner's repository, its commit hash, and the harness commit hash. Any change to the learner after that receipt invalidates the run. The scored run uses exactly the frozen commit, built from a clean tree, with the build digest recorded.

Order of operations is fixed: commit (section 3), then freeze (this section), then run, then reveal (section 6). A freeze recorded after the reveal is void.

## 6. Reveal procedure and receipts

1. `PD0B_COMMIT`: one SHA-256 line per family and per scored seed, with the sealing-author confirmation of section 4.
2. `PD0B_FREEZE`: learner commit hash, harness commit hash, build digest.
3. `PD0B_RUN`: for each level, the five instance runs under the PD-0 pass criteria, with the scorer output, the five law records and the control receipts (`PD0_CONTROLS_PASS` must already exist for this harness commit).
4. `PD0B_REVEAL`: after every level has been scored, the sealed families and seeds are published in `physics`, and their SHA-256 values are checked against `PD0B_COMMIT` line by line. Any mismatch voids the run.
5. `PD0B_VERDICT`: PASS only if the commitments match, the freeze preceded the run, the separation confirmation is present, the controls passed, and every level passed under the inherited criteria. Otherwise FAIL with the first failing condition named.

Receipts are append-only and never overwrite earlier ones, following PD-0 section 6.5.

## 7. The only claim allowed on PASS

On `PD0B_VERDICT` PASS, the one sentence that may be claimed is: the frozen learner recovered compact predictive relations for equation families it had no access to, through the recorded ladder of deliberate experiments, under the inherited and still provisional PD-0 thresholds.

Nothing stronger. No claim about physics beyond the sealed families, no claim about generality, and no claim at all while any inherited threshold remains provisional without saying so.

## 8. Not decided here

- Who the sealing author is, and where the sealing session runs so that it stays outside the shared stores.
- Whether the learner lane needs its own repository for the access boundary (raised in the PD-0 review; left open).
- Any number: every threshold, budget and count is PD-0's, inherited as provisional.
