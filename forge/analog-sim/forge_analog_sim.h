/*
 * forge/analog-sim/forge_analog_sim.h -- AR2: simulated analog realization
 * provider with digital oracle parity (ARCH-0018 section 12, gate AR2).
 *
 * A software model of an analog in-memory matvec substrate. Every result it
 * produces is a SIMULATED_DEVELOPMENT result: no device acts, nothing here is
 * a physical result, and there is no API to label it otherwise.
 *
 * Shape of the path (one call, forge_asim_execute):
 *   1. describe   the provider states its error model as a V2 substrate
 *                 descriptor (declared model -> error_kind + error_abs);
 *   2. eligible   forge_v2_substrate_eligible against the contract, the
 *                 calibration artifact and the execution conditions, plus
 *                 the request's latency budget;
 *   3. admit      forge_v2_admit_plan: the eligible set must hold a digital
 *                 realization of the same contract, otherwise nothing runs;
 *   4. realize    the simulation injects the ACTUAL non-idealities (which a
 *                 test may make worse than the declared ones) for
 *                 contract.min_samples runs; it never sees the oracle;
 *   5. check      outputs are compared to the reference oracle named by the
 *                 contract; EXACT needs bit equality, bounded kinds need
 *                 |err| <= error_abs + error_rel_ppb * |ref| / 1e9 per element;
 *   6. publish    only a passing result is copied out; a refused result
 *                 still yields evidence, and the digital realization then
 *                 produces the answer under the same contract.
 *
 * Reference oracles (restated, not linked; the gate script cross-checks them
 * against the omega sources when an omega checkout is present):
 *   EXACT_U64  omega_matvec_reference          omega src/omega_matvec.h:74
 *              y = A * x, wrapping uint64 arithmetic.
 *   I32        omega_matmul_cpu_oracle_i32     omega src/omega_blackwell_matmul.h:50
 *              n = 1 column, wrapping uint32 arithmetic.
 * The f16 / bf16 oracles are not covered here (deferred).
 *
 * Integers only: bounds are in LSB units of the output, as spec section 6.4
 * requires. No heap, no hardware, no I/O.
 */
#ifndef FORGE_ANALOG_SIM_H
#define FORGE_ANALOG_SIM_H

#include <stddef.h>
#include <stdint.h>

#include "forge_substrate_v2.h"

#define FORGE_ASIM_MAX_ROWS 64u
#define FORGE_ASIM_MAX_COLS 64u
#define FORGE_ASIM_MAX_SAMPLES 64u
#define FORGE_ASIM_INPUT_BITS 8u     /* every A and x element must fit */
#define FORGE_ASIM_OUTPUT_BITS 24u   /* 8 + 8 + log2(64) */

/* Reference oracles, identified by the contract's reference_oracle_digest. */
enum {
    FORGE_ASIM_ORACLE_EXACT_U64 = 1,
    FORGE_ASIM_ORACLE_I32 = 2
};

/* Outcome of forge_asim_execute. Values >= 0 are decisions; < 0 are errors. */
enum {
    FORGE_ASIM_PUBLISHED_ANALOG = 0,      /* simulated analog result passed the contract */
    FORGE_ASIM_PUBLISHED_FALLBACK = 1,    /* analog refused; digital result published */
    FORGE_ASIM_ERR_ARG = -1,
    FORGE_ASIM_ERR_INPUT_RANGE = -2,      /* shape or element outside the declared limits */
    FORGE_ASIM_ERR_NO_FALLBACK = -3,      /* plan not admitted: no digital realization */
    FORGE_ASIM_ERR_EVIDENCE = -4          /* evidence record failed V2 validation */
};

/* Why the simulated analog realization was refused (0 = not refused). */
enum {
    FORGE_ASIM_REFUSE_NONE = 0,
    FORGE_ASIM_REFUSE_INELIGIBLE = 1,      /* forge_v2_substrate_eligible said no */
    FORGE_ASIM_REFUSE_LATENCY_DECLARED = 2,/* declared latency exceeds the request budget */
    FORGE_ASIM_REFUSE_EXACT_MISMATCH = 3,  /* EXACT contract, output digest != oracle digest */
    FORGE_ASIM_REFUSE_ERROR_BOUND = 4,     /* deterministic kind, some element out of bound */
    FORGE_ASIM_REFUSE_CONFIDENCE = 5,      /* stochastic kind, too few runs within bound */
    FORGE_ASIM_REFUSE_NONDETERMINISM = 6,  /* non-stochastic kind, runs disagree */
    FORGE_ASIM_REFUSE_LATENCY_ACTUAL = 7,  /* simulated latency exceeds the request budget */
    FORGE_ASIM_REFUSE_UNSUPPORTED = 8      /* oracle or norm this provider cannot check */
};

/* One error model. All magnitudes in output LSB units unless stated.
 * Zero everywhere means an ideal (exact) simulated substrate. */
typedef struct {
    uint32_t quant_step_lsb;        /* output quantization step; 0 or 1 = off */
    int64_t  bias_lsb;              /* constant offset */
    uint32_t noise_peak_lsb;        /* uniform noise in [-peak, +peak]; 0 = off */
    uint64_t noise_seed;            /* deterministic noise stream */
    int64_t  drift_lsb_per_hour;    /* offset growth with calibration age */
    int64_t  temp_lsb_per_k;        /* offset per kelvin away from the calibration temperature */
    uint64_t saturation_max;        /* outputs clamp to [0, saturation_max]; 0 = off */
    uint64_t latency_ns;            /* settling + conversion per run; 0 = none */
} ForgeAnalogSimModel;

/* The simulated provider and its world. */
typedef struct {
    ForgeAnalogSimModel declared;   /* what the provider claims (descriptor, eligibility) */
    ForgeAnalogSimModel actual;     /* what the simulation injects */
    uint32_t wrong_answer_row;      /* fault injection: row + 1 gets wrong_answer_delta; 0 = off */
    int64_t  wrong_answer_delta;
    ForgeCalibrationArtifact cal;   /* SIMULATED_MODEL calibration for this substrate */
    uint8_t  machine_identity[32];
    uint32_t include_digital;       /* 1: a digital CPU realization is in the eligible set */
} ForgeAsimSetup;

typedef struct {
    ForgeSemanticResultContract contract;  /* operation MATVEC, shape {M, N} */
    const uint64_t *A;              /* M x N, row-major */
    const uint64_t *x;              /* N */
    uint32_t M, N;
    ForgeV2Conditions now;
    uint64_t latency_budget_ns;     /* 0 = no budget */
} ForgeAsimRequest;

typedef struct {
    int      status;                /* FORGE_ASIM_PUBLISHED_* or FORGE_ASIM_ERR_* */
    uint32_t refuse_reason;         /* FORGE_ASIM_REFUSE_* for the analog realization */
    int      eligibility_rc;        /* forge_v2_substrate_eligible result */
    uint32_t published;             /* 1 when y holds an accepted result */
    uint64_t y[FORGE_ASIM_MAX_ROWS];/* zeroed unless published */
    uint32_t analog_ran;            /* 1 when the simulation executed */
    uint64_t analog_error_abs;      /* worst element error vs oracle over all runs */
    uint32_t analog_runs, analog_runs_within_bound;
    uint8_t  analog_substrate_digest[32];
    ForgeExecutionEvidenceV2 analog_evidence;   /* valid when analog_ran */
    uint8_t  analog_evidence_digest[32];
    ForgeExecutionEvidenceV2 final_evidence;    /* evidence of the published result */
    uint8_t  final_evidence_digest[32];
} ForgeAsimOutcome;

/* Oracles (restatements of the omega anchors above). */
void forge_asim_oracle_exact_u64(const uint64_t *A, const uint64_t *x, uint64_t *y, uint32_t M, uint32_t N);
int forge_asim_oracle_i32(const uint32_t *a, const uint32_t *x, uint32_t *y, uint32_t M, uint32_t N);
/* Stable identity of an oracle; returns FORGE_V2_OK or FORGE_V2_ERR_ARG. */
int forge_asim_oracle_digest(uint32_t oracle, uint8_t out[32]);

/* Error bound (LSB) that a model guarantees at a calibration age and a
 * temperature distance from the calibration point. Saturation is not part
 * of the bound: clipping is a range limit and is caught by the check. */
uint64_t forge_asim_model_bound(const ForgeAnalogSimModel *m, uint64_t cal_age_s, uint32_t temp_delta_mc);

/* The V2 substrate descriptor this provider declares for a declared model
 * and calibration: worst-case bound over the full calibration window and
 * envelope. error_kind is EXACT only when the declared model is ideal. */
int forge_asim_describe(const ForgeAnalogSimModel *declared, const ForgeCalibrationArtifact *cal,
                        ForgeSubstrateDescriptor *out);

/* Identity of the simulated substrate instance (calibrations must name it). */
void forge_asim_substrate_identity(uint8_t out[32]);

/* The digital CPU realization used as the fallback. */
void forge_asim_digital_descriptor(ForgeSubstrateDescriptor *out);

/* One simulated run. Never consults an oracle. Returns the simulated
 * latency in *latency_ns. */
int forge_asim_realize(const ForgeAnalogSimModel *m, uint32_t wrong_row, int64_t wrong_delta,
                       const uint64_t *A, const uint64_t *x, uint32_t M, uint32_t N,
                       uint32_t run_index, uint64_t cal_age_s, int32_t temp_delta_mc,
                       uint64_t *y, uint64_t *latency_ns);

/* The whole path: describe, eligibility, admission, realize, check,
 * publish or fall back. Every evidence record it writes carries
 * provenance_class = SIMULATED_DEVELOPMENT. */
int forge_asim_execute(const ForgeAsimSetup *setup, const ForgeAsimRequest *req, ForgeAsimOutcome *out);

/* Provenance used by every evidence record of this provider. Fixed. */
uint32_t forge_asim_provenance(void);

const char *forge_asim_refuse_name(uint32_t reason);

#endif
