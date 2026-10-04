/*
 * forge/form/forge_form.h -- FORGE REALIZATION FORM V1 (host-only reference).
 *
 * Architecture: extends ARCH-0018 (substrate-neutral physical realization)
 * and ARCH-0019 (mixed-algebra realization); proposed as ADR 0034
 * "Physical-Form Realization IR" (PROPOSED, not accepted).
 *
 * A realization form is a REALIZATION-SIDE artifact: the mathematical
 * structure that one or more physical substrates can embody. It sits below
 * an Omega semantic operation and its SemanticResultContract, and beside
 * the substrate-specific lowering. It is not an Omega program, not a new
 * layer, not an authority, and it carries no substrate, vendor, device,
 * calibration, address, clock or temperature field.
 *
 *   Omega semantic operation + SemanticResultContract   (semantic identity)
 *                      |
 *                      v   forge_form_from_contract
 *             ForgeRealizationForm                      (form digest)
 *                      |
 *        +-------------+--------------+
 *        v                            v   forge_form_lower_*
 *   digital reference lowering    simulated analog lowering   (lowering digest
 *        |                            |                        = H(form, code))
 *        v                            v   forge_form_realize
 *   V2 realization identity       V2 realization identity    (realization identity
 *   V2 evidence + record          V2 evidence + record        and evidence identity)
 *
 * Identity rules (the point of this module):
 *   - The form digest NEVER enters the semantic contract digest.
 *   - The form digest enters realization identity only through the
 *     code_digest slot of forge_v2_realization_identity (FORGE V2 bytes and
 *     digests are untouched).
 *   - Every realization of one form under one contract carries the SAME
 *     semantic_contract_digest and DIFFERENT realization identities.
 *
 * Provenance: every evidence record and realization record produced here is
 * SIMULATED_DEVELOPMENT (host fixture; no provisioned Machine measured
 * anything). There is no API to label a result PHYSICAL.
 *
 * Wire form (same discipline as FORGE V2, separate magic so the two
 * families never collide):
 *   header : magic "FGFM" | kind u16 | version u16 (=1) | body_len u32
 *   body   : TLV fields, tag u32 | len u32 | value[len], strictly ascending
 *            tags, every field required, fixed widths, little-endian.
 *   digest : SHA-256 over header+body.
 *
 * Integers only, no heap, no hardware, no I/O.
 */
#ifndef FORGE_FORM_H
#define FORGE_FORM_H

#include <stddef.h>
#include <stdint.h>

#include "forge_substrate_v2.h"
#include "forge_analog_sim.h"

#define FORGE_FORM_MAGIC0 'F'
#define FORGE_FORM_MAGIC1 'G'
#define FORGE_FORM_MAGIC2 'F'
#define FORGE_FORM_MAGIC3 'M'
#define FORGE_FORM_HEADER_LEN 12u
#define FORGE_FORM_TLV_HEAD_LEN 8u
#define FORGE_FORM_WIRE_VERSION 1u
#define FORGE_FORM_MAX_OBJECT_LEN 4096u
#define FORGE_FORM_MAX_DIM 65536u

/* ---- object kinds ---- */
enum {
    FORGE_FORM_KIND_FORM               = 0x0001,
    FORGE_FORM_KIND_REALIZATION_RECORD = 0x0002
};

/* ---- form kinds (numeric ids frozen; only LINEAR_OPERATOR is realizable) ---- */
enum {
    FORGE_FORM_INVALID                 = 0,
    FORGE_FORM_LINEAR_OPERATOR         = 1,  /* y = A x over a declared scalar domain */
    FORGE_FORM_ENERGY_FUNCTIONAL       = 2,  /* SPECIFIED in ADR 0034, NOT IMPLEMENTED: decode refuses */
    FORGE_FORM_STOCHASTIC_DISTRIBUTION = 3   /* RESERVED, NOT IMPLEMENTED: decode refuses */
};

/* ---- scalar domains (ARCH-0019: the algebraic domain a realization computes in) ---- */
enum {
    FORGE_FORM_DOMAIN_INVALID      = 0,
    FORGE_FORM_DOMAIN_INT_WRAP_U64 = 1,  /* integers mod 2^64, wrapping (omega_matvec_reference) */
    FORGE_FORM_DOMAIN_INT_WRAP_U32 = 2   /* integers mod 2^32, wrapping (omega_matmul_cpu_oracle_i32, n = 1) */
};

/* ---- accumulation semantics ---- */
enum {
    FORGE_FORM_ACCUM_INVALID       = 0,
    FORGE_FORM_ACCUM_EXACT_MODULAR = 1   /* exact modular sum; order-free in a wrapping integer ring */
};

/* ---- lowering backends (realization side; NOT a form field) ---- */
enum {
    FORGE_FORM_BACKEND_DIGITAL_REFERENCE = 1,
    FORGE_FORM_BACKEND_ANALOG_SIM        = 2
};

/* ---- realization record decision ---- */
enum {
    FORGE_FORM_DECISION_PUBLISHED = 1,
    FORGE_FORM_DECISION_REFUSED   = 2
};

/* ---- errors / statuses ---- */
enum {
    FORGE_FORM_OK = 0,
    FORGE_FORM_PUBLISHED = 0,            /* forge_form_realize: result satisfied the contract */
    FORGE_FORM_REFUSED = 1,              /* forge_form_realize: nothing published, evidence kept */
    FORGE_FORM_ERR_ARG = -1,
    FORGE_FORM_ERR_SPACE = -2,
    FORGE_FORM_ERR_TRUNCATED = -3,
    FORGE_FORM_ERR_MAGIC = -4,
    FORGE_FORM_ERR_KIND = -5,
    FORGE_FORM_ERR_VERSION = -6,
    FORGE_FORM_ERR_LENGTH = -7,
    FORGE_FORM_ERR_ORDER = -8,
    FORGE_FORM_ERR_UNKNOWN_TAG = -9,
    FORGE_FORM_ERR_WIDTH = -10,
    FORGE_FORM_ERR_MISSING = -11,
    FORGE_FORM_ERR_VALUE = -12,
    FORGE_FORM_ERR_NONCANONICAL = -13,
    FORGE_FORM_ERR_UNSUPPORTED_FORM = -14, /* specified or reserved form kind with no realization here */
    FORGE_FORM_ERR_CONTRACT = -15,         /* contract cannot be mapped to / does not match the form */
    FORGE_FORM_ERR_NO_FALLBACK = -16,      /* eligible set holds no digital realization */
    FORGE_FORM_ERR_EVIDENCE = -17,         /* evidence or record failed validation */
    FORGE_FORM_ERR_INPUT_RANGE = -18       /* inputs outside what the lowering can take */
};

/* ---- objects ---- */

/* The form: mathematical structure only. */
typedef struct {
    uint32_t form_kind;        /* FORGE_FORM_LINEAR_OPERATOR */
    uint32_t scalar_domain;    /* FORGE_FORM_DOMAIN_* */
    uint32_t operator_rows;    /* M, 1..FORGE_FORM_MAX_DIM */
    uint32_t operator_cols;    /* N, 1..FORGE_FORM_MAX_DIM */
    uint32_t accumulation;     /* FORGE_FORM_ACCUM_* */
} ForgeRealizationForm;

/* Evidence-side binding of one realization of one form under one contract. */
typedef struct {
    uint8_t  form_digest[32];
    uint8_t  contract_digest[32];
    uint8_t  substrate_digest[32];
    uint8_t  lowering_digest[32];
    uint8_t  realization_identity[32];
    uint8_t  evidence_digest[32];
    uint32_t provenance_class;  /* always SIMULATED_DEVELOPMENT here */
    uint32_t decision;          /* FORGE_FORM_DECISION_* */
} ForgeFormRealizationRecord;

/* A lowering: one way to embody the form on one substrate. */
typedef struct {
    uint32_t backend;                  /* FORGE_FORM_BACKEND_* */
    ForgeSubstrateDescriptor substrate;
    uint8_t  substrate_digest[32];
    uint8_t  code_digest[32];          /* identity of the lowering code (label digest) */
    uint8_t  form_digest[32];
    uint8_t  lowering_digest[32];      /* H("FORGE-FORM-LOWER-V1" | form_digest | code_digest) */
    uint8_t  has_calibration;
    ForgeCalibrationArtifact cal;
    uint8_t  cal_digest[32];
    /* analog-sim backend only */
    ForgeAnalogSimModel declared, actual;
    uint32_t wrong_row;
    int64_t  wrong_delta;
} ForgeFormLowering;

typedef struct {
    const uint64_t *A;                 /* M x N row-major */
    const uint64_t *x;                 /* N */
    uint32_t M, N;                     /* must equal the form's rows / cols */
    uint8_t  machine_identity[32];
    ForgeV2Conditions now;
} ForgeFormInput;

typedef struct {
    int      status;                   /* FORGE_FORM_PUBLISHED / FORGE_FORM_REFUSED / < 0 */
    uint32_t refuse_reason;            /* FORGE_ASIM_REFUSE_* vocabulary */
    int      eligibility_rc;
    uint32_t ran, runs, runs_within_bound;
    uint64_t error_abs;                /* worst element error vs the contract's oracle */
    uint32_t published;
    uint64_t y[FORGE_ASIM_MAX_ROWS];   /* zeroed unless published */
    ForgeExecutionEvidenceV2 evidence; /* valid when ran */
    uint8_t  evidence_digest[32];
    ForgeFormRealizationRecord record; /* valid when ran */
    uint8_t  record_digest[32];
} ForgeFormOutcome;

/* ---- encoding / identity ---- */
int forge_form_encode(const ForgeRealizationForm *f, uint8_t *buf, size_t cap, size_t *out_len);
int forge_form_decode(const uint8_t *buf, size_t len, int strict, ForgeRealizationForm *f);
int forge_form_digest(const ForgeRealizationForm *f, uint8_t out[32]);
int forge_form_validate(const ForgeRealizationForm *f);

int forge_form_encode_record(const ForgeFormRealizationRecord *r, uint8_t *buf, size_t cap, size_t *out_len);
int forge_form_decode_record(const uint8_t *buf, size_t len, int strict, ForgeFormRealizationRecord *r);
int forge_form_digest_record(const ForgeFormRealizationRecord *r, uint8_t out[32]);

/* Lowering digest: SHA-256("FORGE-FORM-LOWER-V1" | form_digest | code_digest). */
void forge_form_lowering_digest(const uint8_t form_digest[32], const uint8_t code_digest[32], uint8_t out[32]);

/* ---- contract mapping (Omega / V2 contract -> form) ---- */
/* MATVEC, rank 2, known oracle -> LINEAR_OPERATOR; anything else FORGE_FORM_ERR_CONTRACT. */
int forge_form_from_contract(const ForgeSemanticResultContract *c, ForgeRealizationForm *out);

/* ---- lowerings ---- */
int forge_form_lower_digital(const ForgeRealizationForm *f, ForgeFormLowering *out);
int forge_form_lower_analog_sim(const ForgeRealizationForm *f, const ForgeAnalogSimModel *declared,
                                const ForgeAnalogSimModel *actual, const ForgeCalibrationArtifact *cal,
                                ForgeFormLowering *out);

/* ---- admission and realization ---- */
/* Eligible set over the given lowerings; FORGE_FORM_OK when it holds a
 * digital realization of this contract (forge_v2_admit_plan), otherwise
 * FORGE_FORM_ERR_NO_FALLBACK. *n_eligible receives the eligible count. */
int forge_form_admit(const ForgeFormLowering *const *lw, size_t n, const ForgeSemanticResultContract *c,
                     const ForgeFormInput *in, size_t *n_eligible);

/* Realize the form under the contract through one lowering: eligibility,
 * contract.min_samples runs, check against the oracle the contract names,
 * V2 evidence + realization record sealed whether published or refused. */
int forge_form_realize(const ForgeRealizationForm *f, const ForgeSemanticResultContract *c,
                       const ForgeFormLowering *lw, const ForgeFormInput *in, ForgeFormOutcome *out);

/* Fixed provenance of everything this module seals. */
uint32_t forge_form_provenance(void);

const char *forge_form_strerror(int err);

#endif
