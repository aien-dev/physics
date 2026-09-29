/* forge/v2/forge_substrate_v2.h -- FORGE SUBSTRATE V2 reference model.
 *
 * Specification: docs/FORGE_SUBSTRATE_V2_SPEC.md
 * Architecture decision: ARCH-0018 "Substrate-Neutral Physical Realization"
 * (PROPOSED, aien-dev/aien-architecture docs/adr/0018-...).
 *
 * This is a host-only reference: canonical serializer, SHA-256 digest and
 * strict decoder/validator. It touches no hardware and allocates no memory.
 *
 * Wire form of every top-level object (all integers little-endian):
 *   header  : magic "FGV2" (4 bytes) | kind u16 | version u16 | body_len u32
 *   body    : TLV fields, each  tag u32 | len u32 | value[len]
 * Fields appear in ascending tag order; a repeatable field repeats its tag
 * with strictly ascending element sort key. Each value is exactly one
 * primitive (u32, u64, i32, i64, 32-byte digest) or one nested object.
 * The object digest is SHA-256 over header+body. A digest is derived, never
 * stored inside the bytes it covers.
 *
 * The FORGE MACHINE DESCRIPTOR V1 (208-byte GB10 record) is never
 * re-serialized here: a V2 substrate refers to it only by its 32-byte digest
 * (legacy_v1_descriptor_digest).
 */
#ifndef FORGE_SUBSTRATE_V2_H
#define FORGE_SUBSTRATE_V2_H

#include <stddef.h>
#include <stdint.h>

#define FORGE_V2_MAGIC0 'F'
#define FORGE_V2_MAGIC1 'G'
#define FORGE_V2_MAGIC2 'V'
#define FORGE_V2_MAGIC3 '2'
#define FORGE_V2_HEADER_LEN 12u
#define FORGE_V2_TLV_HEAD_LEN 8u
#define FORGE_V2_WIRE_VERSION 2u      /* every V2 object carries version 2 */
#define FORGE_V2_LEGACY_VERSION 1u    /* refused: legacy / 32-bit generation era */
#define FORGE_V2_MAX_OBJECT_LEN 65536u
#define FORGE_V2_MAX_SUBSTRATES 8u
#define FORGE_V2_DIGEST_LEN 32u
#define FORGE_V1_DESCRIPTOR_LEN 208u

/* ---- object kinds (header kind field) ---- */
enum {
    FORGE_V2_KIND_MACHINE_DESCRIPTOR  = 0x0001,
    FORGE_V2_KIND_SUBSTRATE           = 0x0002,
    FORGE_V2_KIND_RESULT_CONTRACT     = 0x0003,
    FORGE_V2_KIND_NUMERICAL_PROFILE   = 0x0004,
    FORGE_V2_KIND_CALIBRATION         = 0x0005,
    FORGE_V2_KIND_EXECUTION_EVIDENCE  = 0x0006,
    FORGE_V2_KIND_CROSS_MACHINE_REF   = 0x0007
};

/* ---- errors ---- */
enum {
    FORGE_V2_OK = 0,
    FORGE_V2_ERR_ARG = -1,
    FORGE_V2_ERR_SPACE = -2,          /* output buffer too small */
    FORGE_V2_ERR_TRUNCATED = -3,
    FORGE_V2_ERR_MAGIC = -4,
    FORGE_V2_ERR_KIND = -5,
    FORGE_V2_ERR_VERSION = -6,        /* unknown future version */
    FORGE_V2_ERR_LEGACY_VERSION = -7, /* version 1 object refused */
    FORGE_V2_ERR_LENGTH = -8,         /* body_len / object size mismatch */
    FORGE_V2_ERR_ORDER = -9,          /* tags not ascending / duplicate */
    FORGE_V2_ERR_UNKNOWN_TAG = -10,   /* strict mode */
    FORGE_V2_ERR_WIDTH = -11,         /* field length wrong for its type */
    FORGE_V2_ERR_MISSING = -12,       /* required field absent */
    FORGE_V2_ERR_COUNT = -13,         /* repeat count out of range */
    FORGE_V2_ERR_VALUE = -14,         /* enum / range / cross-field rule */
    FORGE_V2_ERR_NONCANONICAL = -15,  /* decode+encode does not reproduce input */
    FORGE_V2_ERR_GENERATION_32 = -16, /* 32-bit capability generation refused */
    FORGE_V2_ERR_PROVENANCE = -17,    /* provenance_class missing or zero */
    FORGE_V2_ERR_CAL_STALE = -18,     /* calibration window expired */
    FORGE_V2_ERR_CAL_MISSING = -19,   /* substrate needs calibration, none bound */
    FORGE_V2_ERR_CAL_MISMATCH = -20,  /* calibration names another substrate */
    FORGE_V2_ERR_FAULTED = -21,       /* substrate faulted or unavailable */
    FORGE_V2_ERR_STALE_GENERATION = -22,
    FORGE_V2_ERR_RIGHTS = -23,
    FORGE_V2_ERR_RANGE = -24,         /* offset/length outside referenced object */
    FORGE_V2_ERR_DUPLICATE_ID = -25,
    FORGE_V2_ERR_OUT_OF_ENVELOPE = -26, /* conditions outside the calibration envelope */
    FORGE_V2_ERR_NO_DIGITAL_FALLBACK = -27 /* eligible set holds no digital realization */
};

/* ---- substrate classes (vendor-neutral; numeric ids are frozen) ---- */
enum {
    FORGE_V2_SUBSTRATE_INVALID               = 0x0000,
    FORGE_V2_SUBSTRATE_DIGITAL_CPU           = 0x0001,
    FORGE_V2_SUBSTRATE_DIGITAL_GPU           = 0x0002,
    FORGE_V2_SUBSTRATE_ANALOG_IN_MEMORY      = 0x0010,
    FORGE_V2_SUBSTRATE_NEUROMORPHIC_SPIKING  = 0x0020,
    FORGE_V2_SUBSTRATE_FPGA_CGRA_DATAFLOW    = 0x0030,
    FORGE_V2_SUBSTRATE_OPTICAL_PHOTONIC      = 0x0040,
    FORGE_V2_SUBSTRATE_FUTURE_RESERVED_FIRST = 0x8000,
    FORGE_V2_SUBSTRATE_FUTURE_RESERVED_LAST  = 0xFFFF
};

/* ---- operation families (bit mask in a substrate, single bit in a contract) ---- */
enum {
    FORGE_V2_OP_MATVEC       = 1u << 0,
    FORGE_V2_OP_MATMUL       = 1u << 1,
    FORGE_V2_OP_CONV         = 1u << 2,
    FORGE_V2_OP_ELEMENTWISE  = 1u << 3,
    FORGE_V2_OP_REDUCTION    = 1u << 4,
    FORGE_V2_OP_SPIKE_INTEGRATE = 1u << 5,
    FORGE_V2_OP_TRANSFORM    = 1u << 6,  /* FFT-like linear transforms */
    FORGE_V2_OP_SAMPLE       = 1u << 7,  /* draw from a distribution */
    FORGE_V2_OP_GENERAL      = 1u << 8,  /* general-purpose program */
    FORGE_V2_OP_KNOWN_MASK   = 0x1FFu
};

/* ---- input / output representation ---- */
enum {
    FORGE_V2_REPR_DIGITAL_INT = 1, FORGE_V2_REPR_DIGITAL_FLOAT = 2,
    FORGE_V2_REPR_FIXED_POINT = 3, FORGE_V2_REPR_ANALOG_VOLTAGE = 4,
    FORGE_V2_REPR_ANALOG_CURRENT = 5, FORGE_V2_REPR_SPIKE_TRAIN = 6,
    FORGE_V2_REPR_OPTICAL_INTENSITY = 7, FORGE_V2_REPR_OPTICAL_PHASE = 8,
    FORGE_V2_REPR_LAST = 8
};

enum { /* state model */
    FORGE_V2_STATE_STATELESS = 1, FORGE_V2_STATE_VOLATILE = 2,
    FORGE_V2_STATE_PERSISTENT_WEIGHTS = 3, FORGE_V2_STATE_DYNAMICAL = 4,
    FORGE_V2_STATE_LAST = 4
};

enum { /* synchronization model */
    FORGE_V2_SYNC_CLOCKED = 1, FORGE_V2_SYNC_ASYNC_EVENT = 2,
    FORGE_V2_SYNC_BATCH_BARRIER = 3, FORGE_V2_SYNC_FREE_RUNNING = 4,
    FORGE_V2_SYNC_LAST = 4
};

enum { /* memory model */
    FORGE_V2_MEM_SHARED_COHERENT = 1, FORGE_V2_MEM_DEVICE_LOCAL = 2,
    FORGE_V2_MEM_IN_MEMORY_COMPUTE = 3, FORGE_V2_MEM_NONE = 4,
    FORGE_V2_MEM_LAST = 4
};

enum { /* machine memory coherency */
    FORGE_V2_COHERENCY_UNIFIED = 1, FORGE_V2_COHERENCY_NONCOHERENT = 2,
    FORGE_V2_COHERENCY_DISCRETE = 3, FORGE_V2_COHERENCY_LAST = 3
};

enum { /* interconnect topology */
    FORGE_V2_TOPO_SINGLE_PACKAGE = 1, FORGE_V2_TOPO_SHARED_BUS = 2,
    FORGE_V2_TOPO_SWITCHED = 3, FORGE_V2_TOPO_MESH = 4,
    FORGE_V2_TOPO_REMOTE = 5, FORGE_V2_TOPO_LAST = 5
};

enum { /* transport class: cost metadata only, never semantic identity */
    FORGE_V2_TRANSPORT_ON_PACKAGE = 1, FORGE_V2_TRANSPORT_PCIE = 2,
    FORGE_V2_TRANSPORT_USB = 3, FORGE_V2_TRANSPORT_ETHERNET = 4,
    FORGE_V2_TRANSPORT_RDMA_ETHERNET = 5, FORGE_V2_TRANSPORT_BOARD_TO_BOARD = 6,
    FORGE_V2_TRANSPORT_REMOTE_MACHINE = 7, FORGE_V2_TRANSPORT_LAST = 7
};

enum { /* acceptable error kind */
    FORGE_V2_ERROR_EXACT = 1,
    FORGE_V2_ERROR_BOUNDED_DETERMINISTIC = 2,
    FORGE_V2_ERROR_BOUNDED_STOCHASTIC = 3,
    FORGE_V2_ERROR_MEASURED_DISTRIBUTION = 4,
    FORGE_V2_ERROR_LAST = 4
};

enum { /* norm the error bound is measured in */
    FORGE_V2_NORM_ELEMENTWISE_MAX = 1, FORGE_V2_NORM_L2_RELATIVE = 2,
    FORGE_V2_NORM_LAST = 2
};

enum { /* allowed nondeterminism (mask) */
    FORGE_V2_NONDET_NONE = 0,
    FORGE_V2_NONDET_REDUCTION_ORDER = 1u << 0,
    FORGE_V2_NONDET_ROUNDING = 1u << 1,
    FORGE_V2_NONDET_STOCHASTIC_NOISE = 1u << 2,
    FORGE_V2_NONDET_TIMING = 1u << 3,
    FORGE_V2_NONDET_KNOWN_MASK = 0xFu
};

enum { /* evidence capabilities (mask) */
    FORGE_V2_EVCAP_DURATION = 1u << 0, FORGE_V2_EVCAP_ENERGY = 1u << 1,
    FORGE_V2_EVCAP_TEMPERATURE = 1u << 2, FORGE_V2_EVCAP_MEASURED_ERROR = 1u << 3,
    FORGE_V2_EVCAP_REPEATS = 1u << 4, FORGE_V2_EVCAP_HW_STATUS = 1u << 5,
    FORGE_V2_EVCAP_FAULT = 1u << 6, FORGE_V2_EVCAP_KNOWN_MASK = 0x7Fu
};

enum { /* calibration method */
    FORGE_V2_CAL_REFERENCE_FIXTURE_SWEEP = 1, FORGE_V2_CAL_SELF_TEST = 2,
    FORGE_V2_CAL_FACTORY_TABLE = 3, FORGE_V2_CAL_SIMULATED_MODEL = 4,
    FORGE_V2_CAL_LAST = 4
};

enum { /* energy source in evidence */
    FORGE_V2_ENERGY_NOT_MEASURED = 0, FORGE_V2_ENERGY_MEASURED = 1,
    FORGE_V2_ENERGY_ESTIMATED = 2, FORGE_V2_ENERGY_LAST = 2
};

enum { /* fault state */
    FORGE_V2_FAULT_NONE = 0, FORGE_V2_FAULT_TRANSIENT = 1,
    FORGE_V2_FAULT_PERSISTENT = 2, FORGE_V2_FAULT_UNAVAILABLE = 3,
    FORGE_V2_FAULT_LAST = 3
};

enum { /* provenance class: mandatory, zero is invalid */
    FORGE_V2_PROVENANCE_INVALID = 0,
    FORGE_V2_PROVENANCE_PHYSICAL = 1,
    FORGE_V2_PROVENANCE_SIMULATED_DEVELOPMENT = 2,
    FORGE_V2_PROVENANCE_LAST = 2
};

enum { /* cross-machine reference target kind */
    FORGE_V2_TARGET_MACHINE = 1, FORGE_V2_TARGET_SUBSTRATE = 2
};

/* Effect classes: same bit meaning as omega CQ_FX_* (rx_capq.h). */
enum {
    FORGE_V2_FX_PURE                  = 0x001,
    FORGE_V2_FX_READ                  = 0x002,
    FORGE_V2_FX_EXTERNAL_WRITE        = 0x040,
    FORGE_V2_FX_EXTERNAL_IRREVERSIBLE = 0x080
};

enum { /* rights carried by a cross-machine reference (mask) */
    FORGE_V2_RIGHT_READ = 1u << 0,
    FORGE_V2_RIGHT_EXECUTE_PURE = 1u << 1,
    FORGE_V2_RIGHT_CONFIGURE_REVERSIBLE = 1u << 2,
    FORGE_V2_RIGHT_CONFIGURE_IRREVERSIBLE = 1u << 3,
    FORGE_V2_RIGHT_KNOWN_MASK = 0xFu
};

/* ---- objects ---- */

typedef struct {
    uint32_t dynamic_range_mdb;        /* dB x 1000 */
    uint32_t resolution_bits;
    uint32_t repeatability_ppb;
    uint32_t drift_ppb_per_hour;
    uint32_t temperature_ppb_per_k;
    uint64_t settling_ns;
    uint64_t sample_rate_hz;
    uint64_t calibration_validity_s;   /* 0 = no calibration required */
} ForgeNumericalProfile;

typedef struct {
    uint8_t  substrate_identity[32];
    uint32_t substrate_class;
    uint64_t operation_families;
    uint32_t input_repr, input_bits;
    uint32_t output_repr, output_bits;
    uint32_t state_model;
    uint32_t sync_model;
    uint32_t memory_model;
    uint64_t memory_capacity_bytes;    /* declared nominal, not measured */
    ForgeNumericalProfile numerical;
    uint32_t calibration_required;     /* 0 or 1 */
    uint32_t calibration_method_mask;  /* bit (1 << method) */
    uint64_t calibration_max_window_s;
    uint32_t calibration_min_fixtures;
    uint32_t error_kind;               /* best achievable FORGE_V2_ERROR_* */
    uint32_t error_rel_ppb;
    uint64_t error_abs;
    uint32_t fault_detect_mask;
    uint64_t timing_min_latency_ns;
    uint64_t timing_setup_ns;
    uint64_t timing_conversion_ns;
    uint32_t timing_deterministic;     /* 0 or 1 */
    uint32_t limit_max_rows;
    uint32_t limit_max_cols;
    uint64_t limit_max_state_bytes;
    uint32_t limit_max_concurrent;
    uint32_t evidence_capabilities;
    uint8_t  has_legacy_v1;            /* not serialized: presence of the tag */
    uint8_t  legacy_v1_descriptor_digest[32];
} ForgeSubstrateDescriptor;

typedef struct {
    uint8_t  machine_identity[32];
    uint8_t  authority_domain[32];
    uint64_t memory_nominal_bytes;
    uint32_t memory_numa_domains;
    uint32_t memory_coherency;
    uint32_t interconnect_topology;
    uint32_t interconnect_links;
    uint32_t transport_class;
    uint32_t transport_bandwidth_mbps;
    uint32_t transport_latency_ns;
    uint32_t substrate_count;          /* 1..FORGE_V2_MAX_SUBSTRATES */
    ForgeSubstrateDescriptor substrates[FORGE_V2_MAX_SUBSTRATES];
} ForgeMachineDescriptorV2;

typedef struct {
    uint32_t operation_family;         /* exactly one FORGE_V2_OP_* bit */
    uint8_t  reference_oracle_digest[32]; /* identity of the oracle that defines "correct" */
    uint32_t shape_rank;               /* 1..4 */
    uint32_t shape_dim[4];             /* unused dims must be 0 */
    uint32_t error_kind;
    uint32_t error_rel_ppb;
    uint64_t error_abs;
    uint32_t error_norm;               /* FORGE_V2_NORM_* */
    uint32_t confidence_ppm;           /* 1..1000000 */
    uint32_t min_samples;              /* >= 1; stochastic kinds: runs needed for acceptance */
    uint32_t allowed_nondeterminism;
} ForgeSemanticResultContract;

typedef struct {
    uint8_t  substrate_id[32];
    uint8_t  calibration_id[32];
    uint32_t method;
    uint64_t epoch_s;                  /* calibration time on the authority clock */
    uint64_t validity_window_s;        /* > 0 */
    int32_t  env_temp_min_mc;          /* milli-degC */
    int32_t  env_temp_max_mc;
    uint32_t env_supply_min_uv;
    uint32_t env_supply_max_uv;
    int64_t  transfer_gain_ppb;        /* deviation from ideal gain */
    int64_t  transfer_offset_lsb;
    uint32_t transfer_linearity_ppb;
    uint32_t noise_rms_ppb;
    uint32_t noise_peak_ppb;
    uint32_t noise_samples;
    uint32_t drift_ppb_per_hour;
    uint32_t drift_temp_ppb_per_k;
    uint64_t drift_observed_span_s;
    uint32_t fixture_count;
    uint8_t  fixtures_digest[32];
    uint8_t  evidence_root[32];
} ForgeCalibrationArtifact;

typedef struct {
    uint8_t  realization_identity[32];
    uint8_t  machine_identity[32];
    uint8_t  substrate_identity[32];
    uint8_t  semantic_contract_digest[32];
    uint8_t  has_calibration;          /* presence of the calibration tag */
    uint8_t  calibration_digest[32];
    uint8_t  input_identity[32];
    uint8_t  output_identity[32];
    uint64_t execution_epoch_s;
    uint64_t execution_duration_ns;
    uint64_t conversion_duration_ns;
    uint64_t energy_nj;
    uint32_t energy_source;
    int32_t  env_temperature_mc;
    uint32_t env_supply_uv;
    uint32_t measured_error_rel_ppb;
    uint64_t measured_error_abs;
    uint32_t confidence_ppm;
    uint32_t repeat_runs;
    uint32_t repeat_passes;
    uint64_t repeat_mean_ns;
    uint64_t repeat_spread_ns;
    uint32_t repeat_error_spread_ppb;
    uint32_t hardware_status;
    uint32_t fault_state;
    uint32_t provenance_class;         /* mandatory */
} ForgeExecutionEvidenceV2;

typedef struct {
    uint8_t  program_digest[32];
    uint8_t  semantic_object_id[32];
    uint8_t  ir_reference[32];         /* content digest of the IR bytes */
    uint64_t ir_size;
    uint64_t range_offset;
    uint64_t range_length;
    uint32_t target_kind;              /* FORGE_V2_TARGET_* */
    uint8_t  target_digest[32];
    uint64_t generation;               /* full 64-bit capability generation */
    uint64_t rights;
} ForgeCrossMachineRef;

/* ---- API ---- */

/* SHA-256 of bytes, standard 32-byte big-endian digest form. */
void forge_v2_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/* Encode: writes canonical bytes into buf (cap bytes); *out_len = size.
 * Refuses objects that fail validation. */
int forge_v2_encode_machine(const ForgeMachineDescriptorV2 *m, uint8_t *buf, size_t cap, size_t *out_len);
int forge_v2_encode_substrate(const ForgeSubstrateDescriptor *s, uint8_t *buf, size_t cap, size_t *out_len);
int forge_v2_encode_contract(const ForgeSemanticResultContract *c, uint8_t *buf, size_t cap, size_t *out_len);
int forge_v2_encode_profile(const ForgeNumericalProfile *p, uint8_t *buf, size_t cap, size_t *out_len);
int forge_v2_encode_calibration(const ForgeCalibrationArtifact *a, uint8_t *buf, size_t cap, size_t *out_len);
int forge_v2_encode_evidence(const ForgeExecutionEvidenceV2 *e, uint8_t *buf, size_t cap, size_t *out_len);
int forge_v2_encode_ref(const ForgeCrossMachineRef *r, uint8_t *buf, size_t cap, size_t *out_len);

/* Decode + validate. strict != 0: unknown tags refused and the input must
 * be byte-identical to the canonical re-encoding. strict == 0: unknown tags
 * (still in ascending order) are skipped; everything else is enforced. */
int forge_v2_decode_machine(const uint8_t *buf, size_t len, int strict, ForgeMachineDescriptorV2 *m);
int forge_v2_decode_substrate(const uint8_t *buf, size_t len, int strict, ForgeSubstrateDescriptor *s);
int forge_v2_decode_contract(const uint8_t *buf, size_t len, int strict, ForgeSemanticResultContract *c);
int forge_v2_decode_profile(const uint8_t *buf, size_t len, int strict, ForgeNumericalProfile *p);
int forge_v2_decode_calibration(const uint8_t *buf, size_t len, int strict, ForgeCalibrationArtifact *a);
int forge_v2_decode_evidence(const uint8_t *buf, size_t len, int strict, ForgeExecutionEvidenceV2 *e);
int forge_v2_decode_ref(const uint8_t *buf, size_t len, int strict, ForgeCrossMachineRef *r);

/* Digest of the canonical encoding (encode + SHA-256). */
int forge_v2_digest_machine(const ForgeMachineDescriptorV2 *m, uint8_t out[32]);
int forge_v2_digest_substrate(const ForgeSubstrateDescriptor *s, uint8_t out[32]);
int forge_v2_digest_contract(const ForgeSemanticResultContract *c, uint8_t out[32]);
int forge_v2_digest_calibration(const ForgeCalibrationArtifact *a, uint8_t out[32]);
int forge_v2_digest_evidence(const ForgeExecutionEvidenceV2 *e, uint8_t out[32]);
int forge_v2_digest_ref(const ForgeCrossMachineRef *r, uint8_t out[32]);

/* Realization identity. calibration_digest may be NULL only when the
 * substrate needs no calibration. Domain-separated SHA-256 over
 * "FORGE-V2-REALIZE" | contract | machine_identity | substrate descriptor
 * digest | code | has_cal | cal. machine_identity (not the machine
 * descriptor digest) is used so transport metadata never moves identity. */
void forge_v2_realization_identity(const uint8_t contract_digest[32],
                                   const uint8_t machine_identity[32],
                                   const uint8_t substrate_digest[32],
                                   const uint8_t code_digest[32],
                                   const uint8_t *calibration_digest,
                                   uint8_t out[32]);

/* Measured operating conditions at execution time (evidence, not identity). */
typedef struct {
    uint64_t execution_epoch_s;
    int32_t  temperature_mc;           /* milli-degC */
    uint32_t supply_uv;
} ForgeV2Conditions;

/* Calibration freshness: FORGE_V2_OK when the artifact names this substrate,
 * satisfies its requirements, epoch_s <= now < epoch_s + window (window
 * capped by the substrate), and the conditions lie inside the calibrated
 * envelope. Stale =>  FORGE_V2_ERR_CAL_STALE; outside envelope =>
 * FORGE_V2_ERR_OUT_OF_ENVELOPE. Both mean "not eligible", never "degraded". */
int forge_v2_calibration_check(const ForgeSubstrateDescriptor *s,
                               const ForgeCalibrationArtifact *a,
                               const ForgeV2Conditions *now);

/* Eligibility of one substrate for one contract (the check AEGIS runs).
 * cal may be NULL when the substrate needs no calibration. */
int forge_v2_substrate_eligible(const ForgeSubstrateDescriptor *s,
                                const ForgeSemanticResultContract *c,
                                const ForgeCalibrationArtifact *cal,
                                uint32_t fault_state,
                                const ForgeV2Conditions *now);

/* One eligible realization candidate, on any Machine of the Fabric. */
typedef struct {
    uint8_t  contract_digest[32];      /* SemanticResultContract digest it satisfies */
    uint8_t  machine_identity[32];     /* Machine that hosts the substrate */
    uint8_t  substrate_digest[32];
    uint32_t substrate_class;
} ForgeV2Candidate;

/* Digital fallback rule. Returns 1 when some candidate in the eligible set
 * (on any Machine) is on a DIGITAL_CPU or DIGITAL_GPU substrate and carries
 * the same contract digest; 0 otherwise (also for NULL or empty input).
 * Pure. A single Machine need not hold a digital substrate. */
int forge_v2_eligible_set_has_digital_fallback(const ForgeV2Candidate *set, size_t n,
                                               const uint8_t contract_digest[32]);

/* Admission of a realization plan for one contract: FORGE_V2_OK when the
 * eligible set holds a digital fallback for that contract, otherwise
 * FORGE_V2_ERR_NO_DIGITAL_FALLBACK (AEGIS refuses the plan). */
int forge_v2_admit_plan(const ForgeV2Candidate *set, size_t n, const uint8_t contract_digest[32]);

/* Authority for a use of a cross-machine reference. effect_class is one
 * FORGE_V2_FX_* value. Hardware-state-changing effects need the matching
 * CONFIGURE right and a generation equal to current_generation. */
int forge_v2_authorize(const ForgeCrossMachineRef *r, uint32_t effect_class,
                       uint64_t current_generation);

const char *forge_v2_strerror(int err);

#endif
