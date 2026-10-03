# PHYSICS-0 DISCOVERY ENGINE (program overview)

**Status:** DRAFT, docs only. No code in this repository implements anything described here. Nothing here is a hardware result.
**Source of truth:** Drake's approved program text of 2026-10-03 (mind map note `2026-10-03-physics-0-discovery-engine`, "Core claim", "Epistemic rule", "Architecture invariant", "Milestones") and the PRIORITY CHANGE section of the hive phase map (`~/workspace/hive-phases.md`, line 288). Both are local, not in any GitHub repo. UNVERIFIED that either is mirrored on GitHub (confidence: high that they are not).
**First milestone spec:** [`PD0_HIDDEN_EQUATION_BENCHMARK.md`](PD0_HIDDEN_EQUATION_BENCHMARK.md).

---

## 1. Purpose

Today this repository is a deterministic authority nucleus plus the FORGE realization layer. It answers what hardware exists, what memory exists, who may touch it, and what actually happened (`docs/native-frame-authority.md:13` states that PHYSICS "remains an authority nucleus"; `README.md:3` and `README.md:15` describe FORGE as realization, not a gatekeeper). It contains no learning code and must not gain any.

The Physics-0 Discovery Engine (PD) is a learner placed **above** that authority. Its loop:

```text
observe -> predict -> hypothesize -> experiment -> falsify -> retain
```

It starts with no supplied physical law and tries to find a compact relation that predicts a world it can observe and perturb. Its first target is a controlled black-box dynamical system (PD-0). Its test that matters is a machine behaviour nobody told it about (PD-1). Its home is the Spark itself (PD-2); outside senses (camera, microphone, inertial sensor, temperature, light, actuators) come later under the same architecture.

## 2. Names, and what this is not

- **PHYSICS** (this repository): machine authority and realization. Never a learner.
- **Physics Zero** (aien-architecture `doctrine/DISCOVERY.md:81-95`): the clean-room scientific discovery program, with its nine-tier benchmark pyramid P0-0 to P0-8 (`doctrine/DISCOVERY.md:354-364`). `README.md:17` states that no code in this repository implements it.
- **Physics-0 Discovery Engine (PD)**: the learner and experimenter machinery defined here. PD-0 is the first benchmark. It reuses the Physics Zero vocabulary and rules; it does not replace the doctrine. Mapping, as a proposal: PD-0 levels L0 to L4 sit in tier P0-1 (simple continuous dynamics), L5 and L6 in tier P0-3 (noisy and partially observed systems). UNVERIFIED that the doctrine owner agrees (confidence: medium).
- The doctrine's contamination rules apply to the learner: no human physical laws, no named constants, no human physics terms as inputs (`doctrine/DISCOVERY.md:95-107`). Variable names the learner sees are therefore opaque (`s0`, `s1`, `c0`).

## 3. Invariants

I1. **Authority never learns.** No neural network, regression fit, heuristic or any other learned component runs inside PHYSICS, the frame authority or AEGIS. The learner may be wrong. Physics cannot.
I2. **Reality stays outside the learner.** The learner has no handle on hardware, no driver call, no memory it could use to touch a device. It only emits hypotheses and requests. Only the Experimenter, through AEGIS and PHYSICS, touches reality.
I3. **Bounded intent only.** Every intervention is a bounded, typed request with explicit ranges. AEGIS refuses anything outside the bounds. A refusal is itself a recorded outcome.
I4. **Observations are immutable and recorded by the authority.** The learner reads records. It cannot write, edit or delete one. Records form a hash chain (SHA-256, the same primitive family as `forge/v2/forge_sha256.h`, cited at `docs/FORGE_SUBSTRATE_V2_SPEC.md:7`).
I5. **Only evidence changes confidence.** Confidence is a function of recorded held-out trials (PD-0 spec section 7). No other input moves it.
I6. **No law by fit alone.** Nothing is called a law because it fits. The ladder in the PD-0 spec section 5 is the only path, and its top state is `PROVISIONAL_LAW`. There is no state called "law" or "true". Statements take the form: across N observations under these boundary conditions this relation predicts within this error.
I7. **No reward, no human preference.** Training signal is prediction minus observation. See the non-goals in the PD-0 spec section 10.
I8. **Everything is a receipt.** Gate results are written as new receipts. Old receipts are never overwritten (`README.md:25`).
I9. **No Python anywhere.** Not in code, build, CI, tests or helper tooling (`README.md:25`).
I10. **The harness holds the split.** The learner receives only `FIT` and `SELECT` raw records. `HOLDOUT-0`, `TRIAL` and `REP` records reach it only as scorer verdicts until the instance is closed (PD-0 spec section 5, gate G5).

## 4. Architecture

```text
 Learner            numeric model + symbolic hypothesis engine + planner (may be wrong)
    |  hypotheses, predictions (preregistered, hashed)
    v
 Experimenter       turns a chosen experiment into bounded intent
    |  bounded intent (typed, ranged)
    v
 AEGIS              verifies the intent is inside bounds; refuses otherwise
    |  approved intervention
    v
 PHYSICS            performs the actual intervention; timestamps; measures
    |
    v
 REALITY            the machine (PD-2) or a simulated world (PD-0)
    |  measurement
    v
 PHYSICS            writes the immutable observation record
    |
    v
 Cortex             retains records; the Learner reads them back
```

Reading rules:

- The arrow from Learner to PHYSICS passes through the Experimenter and AEGIS only. There is no direct arrow.
- The Learner sits above the line. The authority sits below it. A compromised or wrong Learner can at worst request refused interventions or write wrong hypotheses.
- Cortex is retention, not authority. A Cortex entry is never evidence of a law, only of a record or a stated hypothesis. Writes follow the Cortex rules in the AIEN project instructions (`AGENTS.md` in the operator home, not in this repository): concise, source-linked, no unverified claims promoted.

### 4.1 Three representations inside the Learner

1. **Numerical world model:** predicts `(state_t, intervention_t) -> state_{t+1}`. Free to be approximate.
2. **Symbolic hypothesis engine:** the simplest relation that explains the records (candidates include sparse system identification, symbolic regression, dimensional analysis, invariance discovery, causal experimentation). Output is a compact relation with a size count.
3. **Experiment planner:** picks the intervention where competing hypotheses diverge most (PD-0 spec section 8).

### 4.2 How PD-0, PD-1, PD-2 use the chain

| Milestone | REALITY | PHYSICS | AEGIS | Cortex |
| --- | --- | --- | --- | --- |
| PD-0 | simulated black-box world, separate process | stand-in recorder (hash chain) | stand-in range check | file-backed stand-in |
| PD-1 | frozen retrospective dataset, no live intervention | replay only | not exercised | file-backed stand-in |
| PD-2 | the Spark (memory, caches, latency, thermal, power, CPU/GPU interaction, queues, clocks, scheduling) | real authority | real bounded intent | real Cortex |

PD-2 depends on AEGIS bounded intent and Cortex being available (phase map line 288 block, PD-2 row). Neither is assumed here. Stand-ins used in PD-0 and PD-1 must carry the label `STAND_IN` in every receipt so they are never mistaken for the real components.

## 5. Milestones

| Id | Content | Prerequisite | Spec |
| --- | --- | --- | --- |
| PD-0 | Hidden-equation black-box benchmark, seven levels, law ladder, law record, planner, negative controls | none (spec first) | `PD0_HIDDEN_EQUATION_BENCHMARK.md` |
| PD-1 | Rediscover the GB10 stale-command behaviour from unlabeled intervention and outcome records | PD-0 | pointer in PD-0 spec section 9; own spec not yet written |
| PD-2 | Live loop on the Spark | PD-1, AEGIS bounded intent, Cortex | not yet written |

## 6. Implementation language and placement

ADR 0024 (aien-architecture `docs/adr/0024-rust-scaffolding-omega-destination.md`, ACCEPTED) makes Rust the primary scaffolding language for new, non-hardware-bound systems work (lines 15 and 21), keeps C where hardware justifies it (line 22), and requires boring, versioned, language-neutral boundary contracts: `#[repr(C)]`-style fixed-width layouts, opaque handles, numeric error codes, explicit refusal of unknown versions, no Rust serialization as a canonical format (lines 30 to 35). PD follows that: the PD-0 wire records are fixed-width little-endian byte layouts, so an Omega implementation can later replace the Rust one against the same contract.

Discrepancy to record, not hide: `README.md:25` of this repository still says "C is the target language, with assembly only where measured". ADR 0024 Decision 1 and 2 supersede C-as-default for new non-hardware work. The README has not been updated for it. This spec follows ADR 0024 and does not edit the README.

Placement of the code (new top-level slice in this repository, or a new repository) is an open decision for Drake. See the PR description. This repository's `docs/.crumb` says only "Purpose not yet described by a human" and gives no rule either way.

## 7. Status of claims in this document

- Repository facts above carry file and line citations. They were read from `origin/main` on 2026-10-03.
- Architecture, invariants and the chain are Drake's approved program restated. They are design, not measured results.
- Nothing here claims PD works. No learner exists.
