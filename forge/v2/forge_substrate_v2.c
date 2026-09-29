/* forge/v2/forge_substrate_v2.c -- FORGE SUBSTRATE V2 reference serializer,
 * digest and strict decoder/validator. Host-only: no hardware access, no
 * heap allocation. See docs/FORGE_SUBSTRATE_V2_SPEC.md.
 */
#include "forge_substrate_v2.h"
#include "forge_sha256.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* primitives                                                          */
/* ------------------------------------------------------------------ */

void forge_v2_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    uint8_t scratch[128];
    uint32_t st[8];
    sha256_compute(data, (uint64_t)len, scratch, st);
    for (unsigned i = 0; i < 8; i++) {
        out[4 * i + 0] = (uint8_t)(st[i] >> 24);
        out[4 * i + 1] = (uint8_t)(st[i] >> 16);
        out[4 * i + 2] = (uint8_t)(st[i] >> 8);
        out[4 * i + 3] = (uint8_t)(st[i]);
    }
}

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_u64(uint8_t *p, uint64_t v) { put_u32(p, (uint32_t)v); put_u32(p + 4, (uint32_t)(v >> 32)); }
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get_u64(const uint8_t *p) { return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32); }

static int is_zero(const uint8_t *p, size_t n)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < n; i++) acc |= p[i];
    return acc == 0;
}

/* ------------------------------------------------------------------ */
/* schema tables                                                       */
/* ------------------------------------------------------------------ */

enum { T_U32 = 1, T_U64, T_I32, T_I64, T_D32, T_NEST, T_NEST_REP };
enum { F_OPT = 1, F_GEN64 = 2, F_PROV = 4 };

typedef struct Schema Schema;
typedef struct {
    uint32_t tag;
    uint8_t type;
    uint8_t flags;
    uint32_t max;         /* T_NEST_REP: max element count */
    size_t off;           /* value offset in the struct */
    size_t aux;           /* F_OPT: presence byte; T_NEST_REP: u32 count */
    size_t stride;        /* T_NEST_REP: element size */
    const Schema *sub;    /* T_NEST / T_NEST_REP */
} Field;

struct Schema {
    uint16_t kind;
    const Field *f;
    size_t n;
    int (*validate)(const void *obj);
};

#define FLD(tag, ty, st, m) { tag, ty, 0, 0, offsetof(st, m), 0, 0, 0 }
#define OPT(tag, ty, st, m, p) { tag, ty, F_OPT, 0, offsetof(st, m), offsetof(st, p), 0, 0 }

static int val_profile(const void *o);
static int val_substrate(const void *o);
static int val_machine(const void *o);
static int val_contract(const void *o);
static int val_calibration(const void *o);
static int val_evidence(const void *o);
static int val_ref(const void *o);

#define P ForgeNumericalProfile
static const Field F_PROFILE[] = {
    FLD(0x0401, T_U32, P, dynamic_range_mdb),
    FLD(0x0402, T_U32, P, resolution_bits),
    FLD(0x0403, T_U32, P, repeatability_ppb),
    FLD(0x0404, T_U32, P, drift_ppb_per_hour),
    FLD(0x0405, T_U32, P, temperature_ppb_per_k),
    FLD(0x0406, T_U64, P, settling_ns),
    FLD(0x0407, T_U64, P, sample_rate_hz),
    FLD(0x0408, T_U64, P, calibration_validity_s),
};
#undef P
static const Schema S_PROFILE = { FORGE_V2_KIND_NUMERICAL_PROFILE, F_PROFILE,
                                  sizeof F_PROFILE / sizeof F_PROFILE[0], val_profile };

#define S ForgeSubstrateDescriptor
static const Field F_SUBSTRATE[] = {
    FLD(0x0201, T_D32, S, substrate_identity),
    FLD(0x0202, T_U32, S, substrate_class),
    FLD(0x0203, T_U64, S, operation_families),
    FLD(0x0210, T_U32, S, input_repr),
    FLD(0x0211, T_U32, S, input_bits),
    FLD(0x0212, T_U32, S, output_repr),
    FLD(0x0213, T_U32, S, output_bits),
    FLD(0x0220, T_U32, S, state_model),
    FLD(0x0221, T_U32, S, sync_model),
    FLD(0x0230, T_U32, S, memory_model),
    FLD(0x0231, T_U64, S, memory_capacity_bytes),
    { 0x0240, T_NEST, 0, 0, offsetof(S, numerical), 0, 0, &S_PROFILE },
    FLD(0x0250, T_U32, S, calibration_required),
    FLD(0x0251, T_U32, S, calibration_method_mask),
    FLD(0x0252, T_U64, S, calibration_max_window_s),
    FLD(0x0253, T_U32, S, calibration_min_fixtures),
    FLD(0x0260, T_U32, S, error_kind),
    FLD(0x0261, T_U32, S, error_rel_ppb),
    FLD(0x0262, T_U64, S, error_abs),
    FLD(0x0263, T_U32, S, fault_detect_mask),
    FLD(0x0270, T_U64, S, timing_min_latency_ns),
    FLD(0x0271, T_U64, S, timing_setup_ns),
    FLD(0x0272, T_U64, S, timing_conversion_ns),
    FLD(0x0273, T_U32, S, timing_deterministic),
    FLD(0x0280, T_U32, S, limit_max_rows),
    FLD(0x0281, T_U32, S, limit_max_cols),
    FLD(0x0282, T_U64, S, limit_max_state_bytes),
    FLD(0x0283, T_U32, S, limit_max_concurrent),
    FLD(0x0290, T_U32, S, evidence_capabilities),
    OPT(0x02F0, T_D32, S, legacy_v1_descriptor_digest, has_legacy_v1),
};
#undef S
static const Schema S_SUBSTRATE = { FORGE_V2_KIND_SUBSTRATE, F_SUBSTRATE,
                                    sizeof F_SUBSTRATE / sizeof F_SUBSTRATE[0], val_substrate };

#define M ForgeMachineDescriptorV2
static const Field F_MACHINE[] = {
    FLD(0x0101, T_D32, M, machine_identity),
    FLD(0x0102, T_D32, M, authority_domain),
    FLD(0x0110, T_U64, M, memory_nominal_bytes),
    FLD(0x0111, T_U32, M, memory_numa_domains),
    FLD(0x0112, T_U32, M, memory_coherency),
    FLD(0x0120, T_U32, M, interconnect_topology),
    FLD(0x0121, T_U32, M, interconnect_links),
    FLD(0x0130, T_U32, M, transport_class),
    FLD(0x0131, T_U32, M, transport_bandwidth_mbps),
    FLD(0x0132, T_U32, M, transport_latency_ns),
    { 0x0140, T_NEST_REP, 0, FORGE_V2_MAX_SUBSTRATES, offsetof(M, substrates),
      offsetof(M, substrate_count), sizeof(ForgeSubstrateDescriptor), &S_SUBSTRATE },
};
#undef M
static const Schema S_MACHINE = { FORGE_V2_KIND_MACHINE_DESCRIPTOR, F_MACHINE,
                                  sizeof F_MACHINE / sizeof F_MACHINE[0], val_machine };

#define C ForgeSemanticResultContract
static const Field F_CONTRACT[] = {
    FLD(0x0301, T_U32, C, operation_family),
    FLD(0x0302, T_D32, C, reference_oracle_digest),
    FLD(0x0310, T_U32, C, shape_rank),
    FLD(0x0311, T_U32, C, shape_dim[0]),
    FLD(0x0312, T_U32, C, shape_dim[1]),
    FLD(0x0313, T_U32, C, shape_dim[2]),
    FLD(0x0314, T_U32, C, shape_dim[3]),
    FLD(0x0320, T_U32, C, error_kind),
    FLD(0x0321, T_U32, C, error_rel_ppb),
    FLD(0x0322, T_U64, C, error_abs),
    FLD(0x0323, T_U32, C, error_norm),
    FLD(0x0330, T_U32, C, confidence_ppm),
    FLD(0x0331, T_U32, C, min_samples),
    FLD(0x0340, T_U32, C, allowed_nondeterminism),
};
#undef C
static const Schema S_CONTRACT = { FORGE_V2_KIND_RESULT_CONTRACT, F_CONTRACT,
                                   sizeof F_CONTRACT / sizeof F_CONTRACT[0], val_contract };

#define A ForgeCalibrationArtifact
static const Field F_CALIBRATION[] = {
    FLD(0x0501, T_D32, A, substrate_id),
    FLD(0x0502, T_D32, A, calibration_id),
    FLD(0x0503, T_U32, A, method),
    FLD(0x0504, T_U64, A, epoch_s),
    FLD(0x0505, T_U64, A, validity_window_s),
    FLD(0x0510, T_I32, A, env_temp_min_mc),
    FLD(0x0511, T_I32, A, env_temp_max_mc),
    FLD(0x0512, T_U32, A, env_supply_min_uv),
    FLD(0x0513, T_U32, A, env_supply_max_uv),
    FLD(0x0520, T_I64, A, transfer_gain_ppb),
    FLD(0x0521, T_I64, A, transfer_offset_lsb),
    FLD(0x0522, T_U32, A, transfer_linearity_ppb),
    FLD(0x0530, T_U32, A, noise_rms_ppb),
    FLD(0x0531, T_U32, A, noise_peak_ppb),
    FLD(0x0532, T_U32, A, noise_samples),
    FLD(0x0540, T_U32, A, drift_ppb_per_hour),
    FLD(0x0541, T_U32, A, drift_temp_ppb_per_k),
    FLD(0x0542, T_U64, A, drift_observed_span_s),
    FLD(0x0550, T_U32, A, fixture_count),
    FLD(0x0551, T_D32, A, fixtures_digest),
    FLD(0x0560, T_D32, A, evidence_root),
};
#undef A
static const Schema S_CALIBRATION = { FORGE_V2_KIND_CALIBRATION, F_CALIBRATION,
                                      sizeof F_CALIBRATION / sizeof F_CALIBRATION[0], val_calibration };

#define E ForgeExecutionEvidenceV2
static const Field F_EVIDENCE[] = {
    FLD(0x0601, T_D32, E, realization_identity),
    FLD(0x0602, T_D32, E, machine_identity),
    FLD(0x0603, T_D32, E, substrate_identity),
    FLD(0x0604, T_D32, E, semantic_contract_digest),
    OPT(0x0605, T_D32, E, calibration_digest, has_calibration),
    FLD(0x0606, T_D32, E, input_identity),
    FLD(0x0607, T_D32, E, output_identity),
    FLD(0x0610, T_U64, E, execution_epoch_s),
    FLD(0x0611, T_U64, E, execution_duration_ns),
    FLD(0x0612, T_U64, E, conversion_duration_ns),
    FLD(0x0620, T_U64, E, energy_nj),
    FLD(0x0621, T_U32, E, energy_source),
    FLD(0x0630, T_I32, E, env_temperature_mc),
    FLD(0x0631, T_U32, E, env_supply_uv),
    FLD(0x0640, T_U32, E, measured_error_rel_ppb),
    FLD(0x0641, T_U64, E, measured_error_abs),
    FLD(0x0642, T_U32, E, confidence_ppm),
    FLD(0x0650, T_U32, E, repeat_runs),
    FLD(0x0651, T_U32, E, repeat_passes),
    FLD(0x0652, T_U64, E, repeat_mean_ns),
    FLD(0x0653, T_U64, E, repeat_spread_ns),
    FLD(0x0654, T_U32, E, repeat_error_spread_ppb),
    FLD(0x0660, T_U32, E, hardware_status),
    FLD(0x0661, T_U32, E, fault_state),
    { 0x0670, T_U32, F_PROV, 0, offsetof(E, provenance_class), 0, 0, 0 },
};
#undef E
static const Schema S_EVIDENCE = { FORGE_V2_KIND_EXECUTION_EVIDENCE, F_EVIDENCE,
                                   sizeof F_EVIDENCE / sizeof F_EVIDENCE[0], val_evidence };

#define R ForgeCrossMachineRef
static const Field F_REF[] = {
    FLD(0x0701, T_D32, R, program_digest),
    FLD(0x0702, T_D32, R, semantic_object_id),
    FLD(0x0703, T_D32, R, ir_reference),
    FLD(0x0710, T_U64, R, ir_size),
    FLD(0x0711, T_U64, R, range_offset),
    FLD(0x0712, T_U64, R, range_length),
    FLD(0x0720, T_U32, R, target_kind),
    FLD(0x0721, T_D32, R, target_digest),
    { 0x0730, T_U64, F_GEN64, 0, offsetof(R, generation), 0, 0, 0 },
    FLD(0x0740, T_U64, R, rights),
};
#undef R
static const Schema S_REF = { FORGE_V2_KIND_CROSS_MACHINE_REF, F_REF,
                              sizeof F_REF / sizeof F_REF[0], val_ref };

/* ------------------------------------------------------------------ */
/* validators                                                          */
/* ------------------------------------------------------------------ */

static int in_range(uint32_t v, uint32_t lo, uint32_t hi) { return v >= lo && v <= hi; }

static int class_known(uint32_t c)
{
    return c == FORGE_V2_SUBSTRATE_DIGITAL_CPU || c == FORGE_V2_SUBSTRATE_DIGITAL_GPU ||
           c == FORGE_V2_SUBSTRATE_ANALOG_IN_MEMORY || c == FORGE_V2_SUBSTRATE_NEUROMORPHIC_SPIKING ||
           c == FORGE_V2_SUBSTRATE_FPGA_CGRA_DATAFLOW || c == FORGE_V2_SUBSTRATE_OPTICAL_PHOTONIC;
}

static int class_digital(uint32_t c)
{
    return c == FORGE_V2_SUBSTRATE_DIGITAL_CPU || c == FORGE_V2_SUBSTRATE_DIGITAL_GPU;
}

/* shared rule for (kind, rel bound, abs bound) triples */
static int error_triple_ok(uint32_t kind, uint32_t rel, uint64_t abs)
{
    if (!in_range(kind, FORGE_V2_ERROR_EXACT, FORGE_V2_ERROR_LAST)) return 0;
    if (kind == FORGE_V2_ERROR_EXACT) return rel == 0 && abs == 0;
    if (kind == FORGE_V2_ERROR_MEASURED_DISTRIBUTION) return 1;
    return rel != 0 || abs != 0;
}

static int val_profile(const void *o)
{
    const ForgeNumericalProfile *p = o;
    if (!in_range(p->resolution_bits, 1, 128)) return FORGE_V2_ERR_VALUE;
    return FORGE_V2_OK;
}

static int val_substrate(const void *o)
{
    const ForgeSubstrateDescriptor *s = o;
    int rc;
    if (is_zero(s->substrate_identity, 32)) return FORGE_V2_ERR_VALUE;
    if (!class_known(s->substrate_class)) return FORGE_V2_ERR_VALUE;
    if (s->operation_families == 0 || (s->operation_families & ~(uint64_t)FORGE_V2_OP_KNOWN_MASK))
        return FORGE_V2_ERR_VALUE;
    if (!in_range(s->input_repr, 1, FORGE_V2_REPR_LAST) || !in_range(s->output_repr, 1, FORGE_V2_REPR_LAST))
        return FORGE_V2_ERR_VALUE;
    if (!in_range(s->input_bits, 1, 128) || !in_range(s->output_bits, 1, 128)) return FORGE_V2_ERR_VALUE;
    if (!in_range(s->state_model, 1, FORGE_V2_STATE_LAST)) return FORGE_V2_ERR_VALUE;
    if (!in_range(s->sync_model, 1, FORGE_V2_SYNC_LAST)) return FORGE_V2_ERR_VALUE;
    if (!in_range(s->memory_model, 1, FORGE_V2_MEM_LAST)) return FORGE_V2_ERR_VALUE;
    if ((rc = val_profile(&s->numerical)) != FORGE_V2_OK) return rc;
    if (s->calibration_required > 1) return FORGE_V2_ERR_VALUE;
    if (s->calibration_required) {
        uint32_t known = 0;
        for (uint32_t m = 1; m <= FORGE_V2_CAL_LAST; m++) known |= 1u << m;
        if (s->calibration_method_mask == 0 || (s->calibration_method_mask & ~known)) return FORGE_V2_ERR_VALUE;
        if (s->calibration_max_window_s == 0 || s->calibration_min_fixtures == 0) return FORGE_V2_ERR_VALUE;
        if (s->numerical.calibration_validity_s == 0 ||
            s->numerical.calibration_validity_s > s->calibration_max_window_s)
            return FORGE_V2_ERR_VALUE;
    } else {
        if (s->calibration_method_mask || s->calibration_max_window_s || s->calibration_min_fixtures ||
            s->numerical.calibration_validity_s)
            return FORGE_V2_ERR_VALUE;
    }
    if (!error_triple_ok(s->error_kind, s->error_rel_ppb, s->error_abs)) return FORGE_V2_ERR_VALUE;
    if (s->timing_deterministic > 1) return FORGE_V2_ERR_VALUE;
    if (s->limit_max_concurrent == 0) return FORGE_V2_ERR_VALUE;
    if (s->evidence_capabilities & ~(uint32_t)FORGE_V2_EVCAP_KNOWN_MASK) return FORGE_V2_ERR_VALUE;
    if (s->has_legacy_v1 > 1) return FORGE_V2_ERR_VALUE;
    if (s->has_legacy_v1) {
        /* a v1 record describes a digital device; nothing else may wrap one */
        if (!class_digital(s->substrate_class)) return FORGE_V2_ERR_VALUE;
        if (is_zero(s->legacy_v1_descriptor_digest, 32)) return FORGE_V2_ERR_VALUE;
    } else if (!is_zero(s->legacy_v1_descriptor_digest, 32)) {
        return FORGE_V2_ERR_VALUE;
    }
    return FORGE_V2_OK;
}

static int val_machine(const void *o)
{
    const ForgeMachineDescriptorV2 *m = o;
    int digital = 0, rc;
    if (is_zero(m->machine_identity, 32) || is_zero(m->authority_domain, 32)) return FORGE_V2_ERR_VALUE;
    if (!in_range(m->memory_coherency, 1, FORGE_V2_COHERENCY_LAST)) return FORGE_V2_ERR_VALUE;
    if (!in_range(m->interconnect_topology, 1, FORGE_V2_TOPO_LAST)) return FORGE_V2_ERR_VALUE;
    if (!in_range(m->transport_class, 1, FORGE_V2_TRANSPORT_LAST)) return FORGE_V2_ERR_VALUE;
    if (m->substrate_count < 1 || m->substrate_count > FORGE_V2_MAX_SUBSTRATES) return FORGE_V2_ERR_COUNT;
    for (uint32_t i = 0; i < m->substrate_count; i++) {
        if ((rc = val_substrate(&m->substrates[i])) != FORGE_V2_OK) return rc;
        if (class_digital(m->substrates[i].substrate_class)) digital = 1;
        if (i > 0) {
            int c = memcmp(m->substrates[i - 1].substrate_identity, m->substrates[i].substrate_identity, 32);
            if (c == 0) return FORGE_V2_ERR_DUPLICATE_ID;
            if (c > 0) return FORGE_V2_ERR_ORDER;
        }
    }
    /* the digital fallback must always exist on the machine */
    if (!digital) return FORGE_V2_ERR_VALUE;
    return FORGE_V2_OK;
}

static int single_bit(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

static int val_contract(const void *o)
{
    const ForgeSemanticResultContract *c = o;
    if (!single_bit(c->operation_family) || (c->operation_family & ~(uint32_t)FORGE_V2_OP_KNOWN_MASK))
        return FORGE_V2_ERR_VALUE;
    if (is_zero(c->reference_oracle_digest, 32)) return FORGE_V2_ERR_VALUE;
    if (!in_range(c->error_norm, 1, FORGE_V2_NORM_LAST)) return FORGE_V2_ERR_VALUE;
    if (c->min_samples == 0) return FORGE_V2_ERR_VALUE;
    if (!in_range(c->shape_rank, 1, 4)) return FORGE_V2_ERR_VALUE;
    for (uint32_t i = 0; i < 4; i++) {
        if (i < c->shape_rank && c->shape_dim[i] == 0) return FORGE_V2_ERR_VALUE;
        if (i >= c->shape_rank && c->shape_dim[i] != 0) return FORGE_V2_ERR_VALUE;
    }
    if (!error_triple_ok(c->error_kind, c->error_rel_ppb, c->error_abs)) return FORGE_V2_ERR_VALUE;
    if (!in_range(c->confidence_ppm, 1, 1000000)) return FORGE_V2_ERR_VALUE;
    if (c->allowed_nondeterminism & ~(uint32_t)FORGE_V2_NONDET_KNOWN_MASK) return FORGE_V2_ERR_VALUE;
    switch (c->error_kind) {
    case FORGE_V2_ERROR_EXACT:
        if (c->confidence_ppm != 1000000) return FORGE_V2_ERR_VALUE;
        if (c->allowed_nondeterminism & (FORGE_V2_NONDET_ROUNDING | FORGE_V2_NONDET_STOCHASTIC_NOISE))
            return FORGE_V2_ERR_VALUE;
        break;
    case FORGE_V2_ERROR_BOUNDED_DETERMINISTIC:
        if (c->confidence_ppm != 1000000) return FORGE_V2_ERR_VALUE;
        if (c->allowed_nondeterminism & FORGE_V2_NONDET_STOCHASTIC_NOISE) return FORGE_V2_ERR_VALUE;
        break;
    default: /* stochastic kinds must admit noise explicitly */
        if (!(c->allowed_nondeterminism & FORGE_V2_NONDET_STOCHASTIC_NOISE)) return FORGE_V2_ERR_VALUE;
        if (c->min_samples < 2) return FORGE_V2_ERR_VALUE;   /* one run cannot show a spread */
        break;
    }
    return FORGE_V2_OK;
}

static int val_calibration(const void *o)
{
    const ForgeCalibrationArtifact *a = o;
    if (is_zero(a->substrate_id, 32) || is_zero(a->calibration_id, 32)) return FORGE_V2_ERR_VALUE;
    if (!in_range(a->method, 1, FORGE_V2_CAL_LAST)) return FORGE_V2_ERR_VALUE;
    if (a->validity_window_s == 0 || a->epoch_s > UINT64_MAX - a->validity_window_s) return FORGE_V2_ERR_VALUE;
    if (a->env_temp_min_mc > a->env_temp_max_mc) return FORGE_V2_ERR_VALUE;
    if (a->env_supply_min_uv > a->env_supply_max_uv) return FORGE_V2_ERR_VALUE;
    if (a->fixture_count == 0 || is_zero(a->fixtures_digest, 32)) return FORGE_V2_ERR_VALUE;
    if (is_zero(a->evidence_root, 32)) return FORGE_V2_ERR_VALUE;
    return FORGE_V2_OK;
}

static int val_evidence(const void *o)
{
    const ForgeExecutionEvidenceV2 *e = o;
    if (!in_range(e->provenance_class, FORGE_V2_PROVENANCE_PHYSICAL, FORGE_V2_PROVENANCE_LAST))
        return FORGE_V2_ERR_PROVENANCE;
    if (is_zero(e->realization_identity, 32) || is_zero(e->machine_identity, 32) ||
        is_zero(e->substrate_identity, 32) || is_zero(e->semantic_contract_digest, 32) ||
        is_zero(e->input_identity, 32) || is_zero(e->output_identity, 32))
        return FORGE_V2_ERR_VALUE;
    if (e->has_calibration > 1) return FORGE_V2_ERR_VALUE;
    if (e->has_calibration ? is_zero(e->calibration_digest, 32) : !is_zero(e->calibration_digest, 32))
        return FORGE_V2_ERR_VALUE;
    if (e->energy_source > FORGE_V2_ENERGY_LAST) return FORGE_V2_ERR_VALUE;
    if (e->energy_source == FORGE_V2_ENERGY_NOT_MEASURED && e->energy_nj != 0) return FORGE_V2_ERR_VALUE;
    if (e->confidence_ppm > 1000000) return FORGE_V2_ERR_VALUE;
    if (e->repeat_passes > e->repeat_runs) return FORGE_V2_ERR_VALUE;
    if (e->fault_state > FORGE_V2_FAULT_LAST) return FORGE_V2_ERR_VALUE;
    return FORGE_V2_OK;
}

static int val_ref(const void *o)
{
    const ForgeCrossMachineRef *r = o;
    if (is_zero(r->program_digest, 32) || is_zero(r->semantic_object_id, 32) ||
        is_zero(r->ir_reference, 32) || is_zero(r->target_digest, 32))
        return FORGE_V2_ERR_VALUE;
    if (r->range_length == 0 || r->range_offset > UINT64_MAX - r->range_length ||
        r->range_offset + r->range_length > r->ir_size)
        return FORGE_V2_ERR_RANGE;
    if (r->target_kind != FORGE_V2_TARGET_MACHINE && r->target_kind != FORGE_V2_TARGET_SUBSTRATE)
        return FORGE_V2_ERR_VALUE;
    if (r->generation == 0) return FORGE_V2_ERR_STALE_GENERATION;
    if (r->rights == 0 || (r->rights & ~(uint64_t)FORGE_V2_RIGHT_KNOWN_MASK)) return FORGE_V2_ERR_VALUE;
    return FORGE_V2_OK;
}

/* ------------------------------------------------------------------ */
/* generic encoder / decoder                                           */
/* ------------------------------------------------------------------ */

static size_t type_width(uint8_t t)
{
    switch (t) {
    case T_U32: case T_I32: return 4;
    case T_U64: case T_I64: return 8;
    case T_D32: return 32;
    default: return 0;
    }
}

static int enc_obj(const Schema *sc, const void *obj, uint8_t *buf, size_t cap, size_t *out_len);

static int enc_value(const Field *f, const uint8_t *src, uint8_t *buf, size_t cap, size_t *pos)
{
    size_t w, n = 0;
    int rc;
    if (cap - *pos < FORGE_V2_TLV_HEAD_LEN) return FORGE_V2_ERR_SPACE;
    uint8_t *head = buf + *pos;
    uint8_t *val = head + FORGE_V2_TLV_HEAD_LEN;
    size_t room = cap - *pos - FORGE_V2_TLV_HEAD_LEN;
    if (f->type == T_NEST || f->type == T_NEST_REP) {
        if ((rc = enc_obj(f->sub, src, val, room, &n)) != FORGE_V2_OK) return rc;
    } else {
        w = type_width(f->type);
        if (room < w) return FORGE_V2_ERR_SPACE;
        switch (f->type) {
        case T_U32: { uint32_t v; memcpy(&v, src, 4); put_u32(val, v); break; }
        case T_I32: { int32_t v; memcpy(&v, src, 4); put_u32(val, (uint32_t)v); break; }
        case T_U64: { uint64_t v; memcpy(&v, src, 8); put_u64(val, v); break; }
        case T_I64: { int64_t v; memcpy(&v, src, 8); put_u64(val, (uint64_t)v); break; }
        default: memcpy(val, src, 32); break;
        }
        n = w;
    }
    put_u32(head, f->tag);
    put_u32(head + 4, (uint32_t)n);
    *pos += FORGE_V2_TLV_HEAD_LEN + n;
    return FORGE_V2_OK;
}

static int enc_obj(const Schema *sc, const void *obj, uint8_t *buf, size_t cap, size_t *out_len)
{
    const uint8_t *base = obj;
    size_t pos = FORGE_V2_HEADER_LEN;
    int rc;
    if (!obj || !buf || !out_len) return FORGE_V2_ERR_ARG;
    if ((rc = sc->validate(obj)) != FORGE_V2_OK) return rc;
    if (cap < FORGE_V2_HEADER_LEN) return FORGE_V2_ERR_SPACE;
    if (cap > FORGE_V2_MAX_OBJECT_LEN) cap = FORGE_V2_MAX_OBJECT_LEN;
    for (size_t i = 0; i < sc->n; i++) {
        const Field *f = &sc->f[i];
        if ((f->flags & F_OPT) && base[f->aux] == 0) continue;
        if (f->type == T_NEST_REP) {
            uint32_t count;
            memcpy(&count, base + f->aux, 4);
            for (uint32_t k = 0; k < count; k++)
                if ((rc = enc_value(f, base + f->off + (size_t)k * f->stride, buf, cap, &pos)) != FORGE_V2_OK)
                    return rc;
        } else if ((rc = enc_value(f, base + f->off, buf, cap, &pos)) != FORGE_V2_OK) {
            return rc;
        }
    }
    buf[0] = FORGE_V2_MAGIC0; buf[1] = FORGE_V2_MAGIC1; buf[2] = FORGE_V2_MAGIC2; buf[3] = FORGE_V2_MAGIC3;
    put_u16(buf + 4, sc->kind);
    put_u16(buf + 6, FORGE_V2_WIRE_VERSION);
    put_u32(buf + 8, (uint32_t)(pos - FORGE_V2_HEADER_LEN));
    *out_len = pos;
    return FORGE_V2_OK;
}

static int check_header(const Schema *sc, const uint8_t *buf, size_t len)
{
    if (!buf) return FORGE_V2_ERR_ARG;
    if (len < FORGE_V2_HEADER_LEN) return FORGE_V2_ERR_TRUNCATED;
    if (len > FORGE_V2_MAX_OBJECT_LEN) return FORGE_V2_ERR_LENGTH;
    if (buf[0] != FORGE_V2_MAGIC0 || buf[1] != FORGE_V2_MAGIC1 || buf[2] != FORGE_V2_MAGIC2 ||
        buf[3] != FORGE_V2_MAGIC3)
        return FORGE_V2_ERR_MAGIC;
    if (get_u16(buf + 4) != sc->kind) return FORGE_V2_ERR_KIND;
    uint16_t ver = get_u16(buf + 6);
    if (ver == FORGE_V2_LEGACY_VERSION) return FORGE_V2_ERR_LEGACY_VERSION;
    if (ver != FORGE_V2_WIRE_VERSION) return FORGE_V2_ERR_VERSION;
    if ((size_t)get_u32(buf + 8) != len - FORGE_V2_HEADER_LEN) return FORGE_V2_ERR_LENGTH;
    return FORGE_V2_OK;
}

static int dec_obj(const Schema *sc, const uint8_t *buf, size_t len, int strict, void *obj)
{
    uint8_t *base = obj;
    uint8_t seen[64];
    uint32_t prev_tag = 0;
    size_t pos = FORGE_V2_HEADER_LEN;
    int rc;
    if (!obj) return FORGE_V2_ERR_ARG;
    if ((rc = check_header(sc, buf, len)) != FORGE_V2_OK) return rc;
    if (sc->n > sizeof seen) return FORGE_V2_ERR_ARG;
    memset(seen, 0, sizeof seen);
    while (pos < len) {
        if (len - pos < FORGE_V2_TLV_HEAD_LEN) return FORGE_V2_ERR_TRUNCATED;
        uint32_t tag = get_u32(buf + pos);
        size_t flen = get_u32(buf + pos + 4);
        const uint8_t *val = buf + pos + FORGE_V2_TLV_HEAD_LEN;
        if (flen > len - pos - FORGE_V2_TLV_HEAD_LEN) return FORGE_V2_ERR_TRUNCATED;
        size_t idx = sc->n;
        for (size_t i = 0; i < sc->n; i++)
            if (sc->f[i].tag == tag) { idx = i; break; }
        int repeat_ok = idx < sc->n && sc->f[idx].type == T_NEST_REP;
        if (tag < prev_tag || (tag == prev_tag && !repeat_ok)) return FORGE_V2_ERR_ORDER;
        prev_tag = tag;
        pos += FORGE_V2_TLV_HEAD_LEN + flen;
        if (idx == sc->n) {
            if (strict) return FORGE_V2_ERR_UNKNOWN_TAG;
            continue;
        }
        const Field *f = &sc->f[idx];
        if (f->type == T_NEST) {
            if ((rc = dec_obj(f->sub, val, flen, strict, base + f->off)) != FORGE_V2_OK) return rc;
        } else if (f->type == T_NEST_REP) {
            uint32_t count;
            memcpy(&count, base + f->aux, 4);
            if (count >= f->max) return FORGE_V2_ERR_COUNT;
            if ((rc = dec_obj(f->sub, val, flen, strict, base + f->off + (size_t)count * f->stride)) != FORGE_V2_OK)
                return rc;
            count++;
            memcpy(base + f->aux, &count, 4);
        } else {
            size_t w = type_width(f->type);
            if (flen != w) {
                if ((f->flags & F_GEN64) && flen == 4) return FORGE_V2_ERR_GENERATION_32;
                return FORGE_V2_ERR_WIDTH;
            }
            uint8_t *dst = base + f->off;
            switch (f->type) {
            case T_U32: { uint32_t v = get_u32(val); memcpy(dst, &v, 4); break; }
            case T_I32: { int32_t v = (int32_t)get_u32(val); memcpy(dst, &v, 4); break; }
            case T_U64: { uint64_t v = get_u64(val); memcpy(dst, &v, 8); break; }
            case T_I64: { int64_t v = (int64_t)get_u64(val); memcpy(dst, &v, 8); break; }
            default: memcpy(dst, val, 32); break;
            }
            if (f->flags & F_OPT) base[f->aux] = 1;
        }
        seen[idx] = 1;
    }
    for (size_t i = 0; i < sc->n; i++) {
        if (seen[i] || (sc->f[i].flags & F_OPT)) continue;
        if (sc->f[i].flags & F_PROV) return FORGE_V2_ERR_PROVENANCE;
        if (sc->f[i].type == T_NEST_REP) return FORGE_V2_ERR_COUNT;
        return FORGE_V2_ERR_MISSING;
    }
    return sc->validate(obj);
}

/* Public decode: structural decode + validation; in strict mode the input
 * must also equal the canonical re-encoding byte for byte. */
static int dec_public(const Schema *sc, const uint8_t *buf, size_t len, int strict, void *obj, size_t obj_size)
{
    uint8_t again[FORGE_V2_MAX_OBJECT_LEN];
    size_t n = 0;
    int rc;
    if (!obj) return FORGE_V2_ERR_ARG;
    memset(obj, 0, obj_size);
    if ((rc = dec_obj(sc, buf, len, strict, obj)) != FORGE_V2_OK) {
        memset(obj, 0, obj_size);
        return rc;
    }
    if (strict) {
        rc = enc_obj(sc, obj, again, sizeof again, &n);
        if (rc == FORGE_V2_OK && (n != len || memcmp(again, buf, len) != 0)) rc = FORGE_V2_ERR_NONCANONICAL;
        if (rc != FORGE_V2_OK) {
            memset(obj, 0, obj_size);
            return rc;
        }
    }
    return FORGE_V2_OK;
}

static int digest_obj(const Schema *sc, const void *obj, uint8_t out[32])
{
    uint8_t buf[FORGE_V2_MAX_OBJECT_LEN];
    size_t n = 0;
    int rc;
    if (!out) return FORGE_V2_ERR_ARG;
    if ((rc = enc_obj(sc, obj, buf, sizeof buf, &n)) != FORGE_V2_OK) return rc;
    forge_v2_sha256(buf, n, out);
    return FORGE_V2_OK;
}

/* ------------------------------------------------------------------ */
/* public wrappers                                                     */
/* ------------------------------------------------------------------ */

#define WRAP(name, type, schema)                                                              \
    int forge_v2_encode_##name(const type *o, uint8_t *buf, size_t cap, size_t *out_len)      \
    { return enc_obj(&schema, o, buf, cap, out_len); }                                        \
    int forge_v2_decode_##name(const uint8_t *buf, size_t len, int strict, type *o)           \
    { return dec_public(&schema, buf, len, strict, o, sizeof *o); }

WRAP(machine, ForgeMachineDescriptorV2, S_MACHINE)
WRAP(substrate, ForgeSubstrateDescriptor, S_SUBSTRATE)
WRAP(contract, ForgeSemanticResultContract, S_CONTRACT)
WRAP(profile, ForgeNumericalProfile, S_PROFILE)
WRAP(calibration, ForgeCalibrationArtifact, S_CALIBRATION)
WRAP(evidence, ForgeExecutionEvidenceV2, S_EVIDENCE)
WRAP(ref, ForgeCrossMachineRef, S_REF)
#undef WRAP

int forge_v2_digest_machine(const ForgeMachineDescriptorV2 *m, uint8_t out[32]) { return digest_obj(&S_MACHINE, m, out); }
int forge_v2_digest_substrate(const ForgeSubstrateDescriptor *s, uint8_t out[32]) { return digest_obj(&S_SUBSTRATE, s, out); }
int forge_v2_digest_contract(const ForgeSemanticResultContract *c, uint8_t out[32]) { return digest_obj(&S_CONTRACT, c, out); }
int forge_v2_digest_calibration(const ForgeCalibrationArtifact *a, uint8_t out[32]) { return digest_obj(&S_CALIBRATION, a, out); }
int forge_v2_digest_evidence(const ForgeExecutionEvidenceV2 *e, uint8_t out[32]) { return digest_obj(&S_EVIDENCE, e, out); }
int forge_v2_digest_ref(const ForgeCrossMachineRef *r, uint8_t out[32]) { return digest_obj(&S_REF, r, out); }

/* ------------------------------------------------------------------ */
/* realization identity, calibration, eligibility, authority           */
/* ------------------------------------------------------------------ */

void forge_v2_realization_identity(const uint8_t contract_digest[32], const uint8_t machine_identity[32],
                                   const uint8_t substrate_digest[32], const uint8_t code_digest[32],
                                   const uint8_t *calibration_digest, uint8_t out[32])
{
    static const char domain[16] = { 'F','O','R','G','E','-','V','2','-','R','E','A','L','I','Z','E' };
    uint8_t in[16 + 32 * 4 + 1 + 32];
    memcpy(in, domain, 16);
    memcpy(in + 16, contract_digest, 32);
    memcpy(in + 48, machine_identity, 32);
    memcpy(in + 80, substrate_digest, 32);
    memcpy(in + 112, code_digest, 32);
    in[144] = calibration_digest ? 1 : 0;
    if (calibration_digest) memcpy(in + 145, calibration_digest, 32);
    else memset(in + 145, 0, 32);
    forge_v2_sha256(in, sizeof in, out);
}

int forge_v2_calibration_check(const ForgeSubstrateDescriptor *s, const ForgeCalibrationArtifact *a,
                               const ForgeV2Conditions *now)
{
    if (!s || !now) return FORGE_V2_ERR_ARG;
    if (!s->calibration_required) return FORGE_V2_OK;
    if (!a) return FORGE_V2_ERR_CAL_MISSING;
    if (val_calibration(a) != FORGE_V2_OK) return FORGE_V2_ERR_VALUE;
    if (memcmp(a->substrate_id, s->substrate_identity, 32) != 0) return FORGE_V2_ERR_CAL_MISMATCH;
    if (!(s->calibration_method_mask & (1u << a->method))) return FORGE_V2_ERR_VALUE;
    if (a->fixture_count < s->calibration_min_fixtures) return FORGE_V2_ERR_VALUE;
    /* the artifact may not claim a longer window than the substrate allows */
    uint64_t window = a->validity_window_s;
    if (window > s->calibration_max_window_s) window = s->calibration_max_window_s;
    if (now->execution_epoch_s < a->epoch_s) return FORGE_V2_ERR_CAL_STALE;   /* from the future */
    if (now->execution_epoch_s - a->epoch_s >= window) return FORGE_V2_ERR_CAL_STALE;
    /* conditions outside the calibrated envelope invalidate the calibration */
    if (now->temperature_mc < a->env_temp_min_mc || now->temperature_mc > a->env_temp_max_mc ||
        now->supply_uv < a->env_supply_min_uv || now->supply_uv > a->env_supply_max_uv)
        return FORGE_V2_ERR_OUT_OF_ENVELOPE;
    return FORGE_V2_OK;
}

int forge_v2_substrate_eligible(const ForgeSubstrateDescriptor *s, const ForgeSemanticResultContract *c,
                                const ForgeCalibrationArtifact *cal, uint32_t fault_state,
                                const ForgeV2Conditions *now)
{
    int rc;
    if (!s || !c) return FORGE_V2_ERR_ARG;
    if (val_substrate(s) != FORGE_V2_OK || val_contract(c) != FORGE_V2_OK) return FORGE_V2_ERR_VALUE;
    if (fault_state != FORGE_V2_FAULT_NONE) return FORGE_V2_ERR_FAULTED;
    if (!(s->operation_families & c->operation_family)) return FORGE_V2_ERR_VALUE;
    /* the substrate's error class must be no looser than the contract's */
    if (s->error_kind > c->error_kind) return FORGE_V2_ERR_VALUE;
    /* acceptance is |err| <= abs + rel_ppb * |reference| / 1e9, so a
     * non-exact substrate fits only if both of its guaranteed bounds are
     * within the contract's (a MEASURED_DISTRIBUTION contract with no
     * bounds leaves the judgement to the measured evidence) */
    if (s->error_kind != FORGE_V2_ERROR_EXACT && (c->error_rel_ppb != 0 || c->error_abs != 0)) {
        if (s->error_rel_ppb > c->error_rel_ppb || s->error_abs > c->error_abs) return FORGE_V2_ERR_VALUE;
    }
    if ((rc = forge_v2_calibration_check(s, cal, now)) != FORGE_V2_OK) return rc;
    return FORGE_V2_OK;
}

int forge_v2_authorize(const ForgeCrossMachineRef *r, uint32_t effect_class, uint64_t current_generation)
{
    uint64_t need;
    int rc;
    if (!r) return FORGE_V2_ERR_ARG;
    if ((rc = val_ref(r)) != FORGE_V2_OK) return rc;
    switch (effect_class) {
    case FORGE_V2_FX_PURE: need = FORGE_V2_RIGHT_EXECUTE_PURE; break;
    case FORGE_V2_FX_READ: need = FORGE_V2_RIGHT_READ; break;
    case FORGE_V2_FX_EXTERNAL_WRITE: need = FORGE_V2_RIGHT_CONFIGURE_REVERSIBLE; break;
    case FORGE_V2_FX_EXTERNAL_IRREVERSIBLE: need = FORGE_V2_RIGHT_CONFIGURE_IRREVERSIBLE; break;
    default: return FORGE_V2_ERR_ARG;
    }
    /* no stale handles: the full 64-bit generation must match exactly */
    if (r->generation != current_generation) return FORGE_V2_ERR_STALE_GENERATION;
    if (!(r->rights & need)) return FORGE_V2_ERR_RIGHTS;
    return FORGE_V2_OK;
}

const char *forge_v2_strerror(int err)
{
    switch (err) {
    case FORGE_V2_OK: return "ok";
    case FORGE_V2_ERR_ARG: return "bad argument";
    case FORGE_V2_ERR_SPACE: return "output buffer too small";
    case FORGE_V2_ERR_TRUNCATED: return "truncated input";
    case FORGE_V2_ERR_MAGIC: return "bad magic";
    case FORGE_V2_ERR_KIND: return "wrong object kind";
    case FORGE_V2_ERR_VERSION: return "unknown version";
    case FORGE_V2_ERR_LEGACY_VERSION: return "legacy version 1 refused";
    case FORGE_V2_ERR_LENGTH: return "length mismatch";
    case FORGE_V2_ERR_ORDER: return "fields out of canonical order";
    case FORGE_V2_ERR_UNKNOWN_TAG: return "unknown tag (strict)";
    case FORGE_V2_ERR_WIDTH: return "field width mismatch";
    case FORGE_V2_ERR_MISSING: return "required field missing";
    case FORGE_V2_ERR_COUNT: return "repeat count out of range";
    case FORGE_V2_ERR_VALUE: return "value rule violated";
    case FORGE_V2_ERR_NONCANONICAL: return "non-canonical encoding";
    case FORGE_V2_ERR_GENERATION_32: return "32-bit capability generation refused";
    case FORGE_V2_ERR_PROVENANCE: return "provenance_class missing or invalid";
    case FORGE_V2_ERR_CAL_STALE: return "calibration stale";
    case FORGE_V2_ERR_CAL_MISSING: return "calibration required but absent";
    case FORGE_V2_ERR_CAL_MISMATCH: return "calibration names another substrate";
    case FORGE_V2_ERR_FAULTED: return "substrate faulted or unavailable";
    case FORGE_V2_ERR_STALE_GENERATION: return "stale capability generation";
    case FORGE_V2_ERR_RIGHTS: return "right not held";
    case FORGE_V2_ERR_RANGE: return "offset/length outside referenced object";
    case FORGE_V2_ERR_DUPLICATE_ID: return "duplicate substrate identity";
    case FORGE_V2_ERR_OUT_OF_ENVELOPE: return "conditions outside calibration envelope";
    default: return "unknown error";
    }
}
