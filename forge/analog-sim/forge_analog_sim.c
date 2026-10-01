/*
 * forge/analog-sim/forge_analog_sim.c -- AR2 simulated analog provider.
 * See forge_analog_sim.h. Every result here is SIMULATED_DEVELOPMENT.
 */
#include "forge_analog_sim.h"

#include <string.h>

#define ASIM_MAGNITUDE_LIMIT (1ll << 32)   /* model offsets beyond this are refused; keeps every sum inside int64 */

static const char *const ORACLE_LABEL_EXACT_U64 =
    "FORGE-ASIM/oracle/omega_matvec_reference/src/omega_matvec.h:74/u64-wrapping";
static const char *const ORACLE_LABEL_I32 =
    "FORGE-ASIM/oracle/omega_matmul_cpu_oracle_i32/src/omega_blackwell_matmul.h:50/u32-wrapping/n=1";
static const char *const SUBSTRATE_LABEL = "FORGE-ASIM/substrate/analog-in-memory/simulated/v1";
static const char *const DIGITAL_LABEL = "FORGE-ASIM/substrate/digital-cpu/v1";
static const char *const ANALOG_CODE_LABEL = "FORGE-ASIM/code/analog-matvec-simulation/v1";
static const char *const DIGITAL_CODE_LABEL = "FORGE-ASIM/code/digital-matvec-oracle/v1";

static void label_digest(const char *label, uint8_t out[32])
{
    forge_v2_sha256((const uint8_t *)label, strlen(label), out);
}

/* ---- oracles ---- */

/* Restatement of omega_matvec_reference (omega src/omega_matvec.c:265). */
void forge_asim_oracle_exact_u64(const uint64_t *A, const uint64_t *x, uint64_t *y, uint32_t M, uint32_t N)
{
    if (!A || !x || !y) return;
    for (uint32_t i = 0; i < M; ++i) {
        uint64_t acc = 0;
        for (uint32_t j = 0; j < N; ++j) acc += A[(size_t)i * N + j] * x[j];
        y[i] = acc;
    }
}

/* Restatement of omega_matmul_cpu_oracle_i32 (omega src/omega_blackwell_matmul.c:41)
 * with n = 1: c[i] = sum_p a[i*k + p] * b[p], wrapping uint32. */
int forge_asim_oracle_i32(const uint32_t *a, const uint32_t *x, uint32_t *y, uint32_t M, uint32_t N)
{
    if (!a || !x || !y || M == 0 || N == 0) return -1;
    for (uint32_t i = 0; i < M; i++) {
        uint32_t acc = 0;
        for (uint32_t p = 0; p < N; p++) acc += a[i * N + p] * x[p];
        y[i] = acc;
    }
    return 0;
}

int forge_asim_oracle_digest(uint32_t oracle, uint8_t out[32])
{
    if (!out) return FORGE_V2_ERR_ARG;
    switch (oracle) {
    case FORGE_ASIM_ORACLE_EXACT_U64: label_digest(ORACLE_LABEL_EXACT_U64, out); return FORGE_V2_OK;
    case FORGE_ASIM_ORACLE_I32: label_digest(ORACLE_LABEL_I32, out); return FORGE_V2_OK;
    default: return FORGE_V2_ERR_ARG;
    }
}

static uint32_t oracle_of(const uint8_t digest[32])
{
    uint8_t d[32];
    forge_asim_oracle_digest(FORGE_ASIM_ORACLE_EXACT_U64, d);
    if (memcmp(d, digest, 32) == 0) return FORGE_ASIM_ORACLE_EXACT_U64;
    forge_asim_oracle_digest(FORGE_ASIM_ORACLE_I32, d);
    if (memcmp(d, digest, 32) == 0) return FORGE_ASIM_ORACLE_I32;
    return 0;
}

/* ---- error model ---- */

static uint64_t mag(int64_t v) { return v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v; }

static uint64_t sat_add(uint64_t a, uint64_t b) { return a > UINT64_MAX - b ? UINT64_MAX : a + b; }

/* ceil(a * b / d) with a 128-bit product, saturating at UINT64_MAX only
 * after the division; d > 0 */
static uint64_t mul_div_ceil(uint64_t a, uint64_t b, uint64_t d)
{
    unsigned __int128 p = (unsigned __int128)a * b;
    unsigned __int128 q = p / d + (p % d != 0);
    return q > UINT64_MAX ? UINT64_MAX : (uint64_t)q;
}

/* signed a * b / d truncated toward zero, 128-bit product, saturating at
 * +/-2^61 after the division (three such terms still sum inside int64) */
static int64_t mul_div_trunc(int64_t a, int64_t b, int64_t d)
{
    __int128 q = ((__int128)a * b) / d;
    const __int128 lim = (__int128)1 << 61;
    return (int64_t)(q > lim ? lim : (q < -lim ? -lim : q));
}

static int model_ok(const ForgeAnalogSimModel *m)
{
    return mag(m->bias_lsb) <= (uint64_t)ASIM_MAGNITUDE_LIMIT &&
           mag(m->drift_lsb_per_hour) <= (uint64_t)ASIM_MAGNITUDE_LIMIT &&
           mag(m->temp_lsb_per_k) <= (uint64_t)ASIM_MAGNITUDE_LIMIT;
}

static int model_ideal(const ForgeAnalogSimModel *m)
{
    return m->quant_step_lsb <= 1 && m->bias_lsb == 0 && m->noise_peak_lsb == 0 && m->drift_lsb_per_hour == 0 &&
           m->temp_lsb_per_k == 0 && m->saturation_max == 0;
}

uint64_t forge_asim_model_bound(const ForgeAnalogSimModel *m, uint64_t cal_age_s, uint32_t temp_delta_mc)
{
    uint64_t b = 0;
    if (!m) return UINT64_MAX;
    if (m->quant_step_lsb > 1) b = m->quant_step_lsb / 2;
    b = sat_add(b, mag(m->bias_lsb));
    b = sat_add(b, m->noise_peak_lsb);
    b = sat_add(b, mul_div_ceil(mag(m->drift_lsb_per_hour), cal_age_s, 3600));
    b = sat_add(b, mul_div_ceil(mag(m->temp_lsb_per_k), temp_delta_mc, 1000));
    return b;
}

/* ---- descriptors ---- */

void forge_asim_substrate_identity(uint8_t out[32]) { label_digest(SUBSTRATE_LABEL, out); }

int forge_asim_describe(const ForgeAnalogSimModel *declared, const ForgeCalibrationArtifact *cal,
                        ForgeSubstrateDescriptor *s)
{
    if (!declared || !cal || !s || !model_ok(declared)) return FORGE_V2_ERR_ARG;
    if (cal->env_temp_max_mc < cal->env_temp_min_mc || cal->validity_window_s == 0) return FORGE_V2_ERR_ARG;
    memset(s, 0, sizeof *s);
    forge_asim_substrate_identity(s->substrate_identity);
    s->substrate_class = FORGE_V2_SUBSTRATE_ANALOG_IN_MEMORY;
    s->operation_families = FORGE_V2_OP_MATVEC;
    s->input_repr = FORGE_V2_REPR_ANALOG_VOLTAGE; s->input_bits = FORGE_ASIM_INPUT_BITS;
    s->output_repr = FORGE_V2_REPR_ANALOG_CURRENT; s->output_bits = FORGE_ASIM_OUTPUT_BITS;
    s->state_model = FORGE_V2_STATE_PERSISTENT_WEIGHTS;
    s->sync_model = FORGE_V2_SYNC_BATCH_BARRIER;
    s->memory_model = FORGE_V2_MEM_IN_MEMORY_COMPUTE;
    s->memory_capacity_bytes = (uint64_t)FORGE_ASIM_MAX_ROWS * FORGE_ASIM_MAX_COLS;
    s->numerical.dynamic_range_mdb = 144000;   /* 24 bits */
    s->numerical.resolution_bits = FORGE_ASIM_OUTPUT_BITS;
    s->numerical.settling_ns = declared->latency_ns;
    s->numerical.calibration_validity_s = cal->validity_window_s;
    s->calibration_required = 1;
    s->calibration_method_mask = 1u << FORGE_V2_CAL_SIMULATED_MODEL;
    s->calibration_max_window_s = cal->validity_window_s;
    s->calibration_min_fixtures = 4;
    /* worst case over the whole calibration window and envelope */
    uint32_t half_mc = (uint32_t)(((int64_t)cal->env_temp_max_mc - cal->env_temp_min_mc + 1) / 2);
    uint64_t bound = forge_asim_model_bound(declared, cal->validity_window_s, half_mc);
    if (model_ideal(declared)) {
        s->error_kind = FORGE_V2_ERROR_EXACT;
    } else {
        s->error_kind = declared->noise_peak_lsb ? FORGE_V2_ERROR_BOUNDED_STOCHASTIC
                                                 : FORGE_V2_ERROR_BOUNDED_DETERMINISTIC;
        s->error_abs = bound ? bound : 1;     /* a non-exact substrate never declares zero error */
    }
    s->fault_detect_mask = 0;
    s->timing_min_latency_ns = declared->latency_ns;
    s->timing_conversion_ns = declared->latency_ns;
    s->timing_deterministic = declared->noise_peak_lsb ? 0 : 1;
    s->limit_max_rows = FORGE_ASIM_MAX_ROWS;
    s->limit_max_cols = FORGE_ASIM_MAX_COLS;
    s->limit_max_state_bytes = (uint64_t)FORGE_ASIM_MAX_ROWS * FORGE_ASIM_MAX_COLS;
    s->limit_max_concurrent = 1;
    s->evidence_capabilities = FORGE_V2_EVCAP_DURATION | FORGE_V2_EVCAP_TEMPERATURE |
                               FORGE_V2_EVCAP_MEASURED_ERROR | FORGE_V2_EVCAP_REPEATS;
    return FORGE_V2_OK;
}

void forge_asim_digital_descriptor(ForgeSubstrateDescriptor *s)
{
    memset(s, 0, sizeof *s);
    label_digest(DIGITAL_LABEL, s->substrate_identity);
    s->substrate_class = FORGE_V2_SUBSTRATE_DIGITAL_CPU;
    s->operation_families = FORGE_V2_OP_MATVEC | FORGE_V2_OP_MATMUL;
    s->input_repr = FORGE_V2_REPR_DIGITAL_INT; s->input_bits = 64;
    s->output_repr = FORGE_V2_REPR_DIGITAL_INT; s->output_bits = 64;
    s->state_model = FORGE_V2_STATE_STATELESS;
    s->sync_model = FORGE_V2_SYNC_CLOCKED;
    s->memory_model = FORGE_V2_MEM_SHARED_COHERENT;
    s->numerical.resolution_bits = 64;
    s->error_kind = FORGE_V2_ERROR_EXACT;
    s->timing_deterministic = 1;
    s->limit_max_rows = FORGE_ASIM_MAX_ROWS;
    s->limit_max_cols = FORGE_ASIM_MAX_COLS;
    s->limit_max_concurrent = 1;
    s->evidence_capabilities = FORGE_V2_EVCAP_REPEATS;
}

/* ---- simulation ---- */

static uint64_t splitmix64(uint64_t z)
{
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static int shape_ok(const uint64_t *A, const uint64_t *x, uint32_t M, uint32_t N)
{
    const uint64_t lim = 1ull << FORGE_ASIM_INPUT_BITS;
    if (!A || !x || M == 0 || N == 0 || M > FORGE_ASIM_MAX_ROWS || N > FORGE_ASIM_MAX_COLS) return 0;
    for (size_t k = 0; k < (size_t)M * N; k++) if (A[k] >= lim) return 0;
    for (uint32_t j = 0; j < N; j++) if (x[j] >= lim) return 0;
    return 1;
}

int forge_asim_realize(const ForgeAnalogSimModel *m, uint32_t wrong_row, int64_t wrong_delta,
                       const uint64_t *A, const uint64_t *x, uint32_t M, uint32_t N,
                       uint32_t run_index, uint64_t cal_age_s, int32_t temp_delta_mc,
                       uint64_t *y, uint64_t *latency_ns)
{
    if (!m || !y || !latency_ns || !model_ok(m) || mag(wrong_delta) > (uint64_t)ASIM_MAGNITUDE_LIMIT)
        return FORGE_ASIM_ERR_ARG;
    if (!shape_ok(A, x, M, N)) return FORGE_ASIM_ERR_INPUT_RANGE;
    const uint64_t age_cap = 1ull << 40;
    int64_t age = (int64_t)(cal_age_s > age_cap ? age_cap : cal_age_s);
    int64_t offset = m->bias_lsb + mul_div_trunc(m->drift_lsb_per_hour, age, 3600) +
                     mul_div_trunc(m->temp_lsb_per_k, temp_delta_mc, 1000);
    for (uint32_t i = 0; i < M; i++) {
        /* the ideal crossbar column current: exact for inputs within limits */
        int64_t v = 0;
        for (uint32_t j = 0; j < N; j++) v += (int64_t)(A[(size_t)i * N + j] * x[j]);
        v += offset;
        if (m->noise_peak_lsb) {
            uint64_t r = splitmix64(m->noise_seed ^ splitmix64(((uint64_t)run_index << 32) | i));
            v += (int64_t)(r % (2ull * m->noise_peak_lsb + 1)) - (int64_t)m->noise_peak_lsb;
        }
        if (v < 0) v = 0;                                    /* no negative output current */
        if (m->quant_step_lsb > 1) {
            int64_t q = m->quant_step_lsb;
            v = ((v + q / 2) / q) * q;
        }
        if (m->saturation_max && (uint64_t)v > m->saturation_max) v = (int64_t)m->saturation_max;
        if (wrong_row && i == wrong_row - 1) {
            v += wrong_delta;
            if (v < 0) v = 0;
        }
        y[i] = (uint64_t)v;
    }
    *latency_ns = m->latency_ns;
    return FORGE_ASIM_PUBLISHED_ANALOG;
}

/* ---- evidence ---- */

uint32_t forge_asim_provenance(void) { return FORGE_V2_PROVENANCE_SIMULATED_DEVELOPMENT; }

const char *forge_asim_refuse_name(uint32_t r)
{
    switch (r) {
    case FORGE_ASIM_REFUSE_NONE: return "none";
    case FORGE_ASIM_REFUSE_INELIGIBLE: return "ineligible";
    case FORGE_ASIM_REFUSE_LATENCY_DECLARED: return "latency_declared_over_budget";
    case FORGE_ASIM_REFUSE_EXACT_MISMATCH: return "exact_digest_mismatch";
    case FORGE_ASIM_REFUSE_ERROR_BOUND: return "error_over_bound";
    case FORGE_ASIM_REFUSE_CONFIDENCE: return "confidence_not_met";
    case FORGE_ASIM_REFUSE_NONDETERMINISM: return "nondeterministic_runs";
    case FORGE_ASIM_REFUSE_LATENCY_ACTUAL: return "latency_actual_over_budget";
    case FORGE_ASIM_REFUSE_UNSUPPORTED: return "unsupported_contract";
    default: return "unknown";
    }
}

static size_t put_u64(uint8_t *p, uint64_t v)
{
    for (int b = 0; b < 8; b++) p[b] = (uint8_t)(v >> (8 * b));
    return 8;
}

static void vec_digest(const char *domain, const uint64_t *v, size_t n, uint8_t out[32])
{
    uint8_t buf[64 + 8 * FORGE_ASIM_MAX_ROWS];
    size_t len = strlen(domain), off;
    memcpy(buf, domain, len);
    off = len;
    off += put_u64(buf + off, n);
    for (size_t k = 0; k < n; k++) off += put_u64(buf + off, v[k]);
    forge_v2_sha256(buf, off, out);
}

static void input_digest(const uint64_t *A, const uint64_t *x, uint32_t M, uint32_t N, uint8_t out[32])
{
    static const char dom[] = "FORGE-ASIM/input/matvec/v1";
    uint8_t buf[sizeof dom + 16 + 8 * (FORGE_ASIM_MAX_ROWS * FORGE_ASIM_MAX_COLS + FORGE_ASIM_MAX_COLS)];
    size_t off = sizeof dom - 1;
    memcpy(buf, dom, off);
    off += put_u64(buf + off, M);
    off += put_u64(buf + off, N);
    for (size_t k = 0; k < (size_t)M * N; k++) off += put_u64(buf + off, A[k]);
    for (uint32_t j = 0; j < N; j++) off += put_u64(buf + off, x[j]);
    forge_v2_sha256(buf, off, out);
}

/* Fill the fields every record of this provider shares, then validate and
 * digest it through the V2 encoder. Provenance is fixed here and nowhere
 * else. */
static int seal_evidence(ForgeExecutionEvidenceV2 *e, uint8_t digest[32])
{
    uint8_t buf[4096];
    size_t n;
    e->provenance_class = forge_asim_provenance();
    e->energy_source = FORGE_V2_ENERGY_NOT_MEASURED;
    e->energy_nj = 0;
    if (forge_v2_encode_evidence(e, buf, sizeof buf, &n) != FORGE_V2_OK) return FORGE_ASIM_ERR_EVIDENCE;
    forge_v2_sha256(buf, n, digest);
    return 0;
}

/* ---- the whole path ---- */

static void compute_ref(uint32_t oracle, const ForgeAsimRequest *rq, uint64_t *ref)
{
    if (oracle == FORGE_ASIM_ORACLE_EXACT_U64) {
        forge_asim_oracle_exact_u64(rq->A, rq->x, ref, rq->M, rq->N);
    } else {
        uint32_t a32[FORGE_ASIM_MAX_ROWS * FORGE_ASIM_MAX_COLS], x32[FORGE_ASIM_MAX_COLS], y32[FORGE_ASIM_MAX_ROWS];
        for (size_t k = 0; k < (size_t)rq->M * rq->N; k++) a32[k] = (uint32_t)rq->A[k];
        for (uint32_t j = 0; j < rq->N; j++) x32[j] = (uint32_t)rq->x[j];
        forge_asim_oracle_i32(a32, x32, y32, rq->M, rq->N);
        for (uint32_t i = 0; i < rq->M; i++) ref[i] = y32[i];
    }
}

/* The digital realization: the contract's oracle on the CPU, repeated for
 * the contract's min_samples so its evidence meets the same contract. */
static int run_digital(const ForgeAsimSetup *st, const ForgeAsimRequest *rq, uint32_t oracle,
                       const uint8_t contract_digest[32], const uint8_t input_id[32], const uint64_t *ref,
                       ForgeAsimOutcome *out)
{
    ForgeSubstrateDescriptor d;
    uint8_t sd[32], code[32];
    uint64_t again[FORGE_ASIM_MAX_ROWS];
    uint32_t runs = rq->contract.min_samples, same = 0;
    ForgeExecutionEvidenceV2 *e = &out->final_evidence;
    for (uint32_t r = 0; r < runs; r++) {
        compute_ref(oracle, rq, again);
        if (memcmp(again, ref, (size_t)rq->M * sizeof ref[0]) == 0) same++;
    }
    if (same != runs) return FORGE_ASIM_ERR_EVIDENCE;   /* a digital oracle that disagrees with itself */
    forge_asim_digital_descriptor(&d);
    forge_v2_digest_substrate(&d, sd);
    label_digest(DIGITAL_CODE_LABEL, code);
    memset(e, 0, sizeof *e);
    forge_v2_realization_identity(contract_digest, st->machine_identity, sd, code, NULL, e->realization_identity);
    memcpy(e->machine_identity, st->machine_identity, 32);
    memcpy(e->substrate_identity, d.substrate_identity, 32);
    memcpy(e->semantic_contract_digest, contract_digest, 32);
    memcpy(e->input_identity, input_id, 32);
    vec_digest("FORGE-ASIM/output/matvec/v1", ref, rq->M, e->output_identity);
    e->execution_epoch_s = rq->now.execution_epoch_s;
    e->env_temperature_mc = rq->now.temperature_mc;
    e->env_supply_uv = rq->now.supply_uv;
    e->confidence_ppm = 1000000;
    e->repeat_runs = runs;
    e->repeat_passes = same;
    e->fault_state = FORGE_V2_FAULT_NONE;
    if (seal_evidence(e, out->final_evidence_digest) != 0) return FORGE_ASIM_ERR_EVIDENCE;
    memcpy(out->y, ref, (size_t)rq->M * sizeof ref[0]);
    out->published = 1;
    return FORGE_ASIM_PUBLISHED_FALLBACK;
}

int forge_asim_execute(const ForgeAsimSetup *st, const ForgeAsimRequest *rq, ForgeAsimOutcome *out)
{
    ForgeSubstrateDescriptor analog, digital;
    ForgeV2Candidate set[2];
    size_t nset = 0;
    uint8_t contract_digest[32], analog_digest[32], cal_digest[32], input_id[32], ref_id[32];
    uint64_t ref[FORGE_ASIM_MAX_ROWS];
    uint64_t first[FORGE_ASIM_MAX_ROWS], run_y[FORGE_ASIM_MAX_ROWS], chosen[FORGE_ASIM_MAX_ROWS];
    int rc;

    if (!out) return FORGE_ASIM_ERR_ARG;
    memset(out, 0, sizeof *out);
    out->status = FORGE_ASIM_ERR_ARG;
    if (!st || !rq) return out->status;
    const ForgeSemanticResultContract *c = &rq->contract;
    if (c->operation_family != FORGE_V2_OP_MATVEC || c->shape_rank != 2 || c->shape_dim[0] != rq->M ||
        c->shape_dim[1] != rq->N)
        return out->status;
    uint32_t oracle = oracle_of(c->reference_oracle_digest);
    if (!oracle || c->min_samples == 0 || c->min_samples > FORGE_ASIM_MAX_SAMPLES) return out->status;
    if (!shape_ok(rq->A, rq->x, rq->M, rq->N)) return out->status = FORGE_ASIM_ERR_INPUT_RANGE;
    if (forge_v2_digest_contract(c, contract_digest) != FORGE_V2_OK) return out->status;
    if (forge_asim_describe(&st->declared, &st->cal, &analog) != FORGE_V2_OK) return out->status;
    if (!model_ok(&st->actual)) return out->status;
    forge_v2_digest_substrate(&analog, analog_digest);
    memcpy(out->analog_substrate_digest, analog_digest, 32);
    if (forge_v2_digest_calibration(&st->cal, cal_digest) != FORGE_V2_OK) return out->status;

    /* the reference answer, from the oracle the contract names */
    compute_ref(oracle, rq, ref);
    input_digest(rq->A, rq->x, rq->M, rq->N, input_id);
    vec_digest("FORGE-ASIM/output/matvec/v1", ref, rq->M, ref_id);

    /* eligibility of the simulated analog realization */
    out->eligibility_rc = forge_v2_substrate_eligible(&analog, c, &st->cal, FORGE_V2_FAULT_NONE, &rq->now);
    if (out->eligibility_rc != FORGE_V2_OK) out->refuse_reason = FORGE_ASIM_REFUSE_INELIGIBLE;
    else if (rq->latency_budget_ns && st->declared.latency_ns > rq->latency_budget_ns)
        out->refuse_reason = FORGE_ASIM_REFUSE_LATENCY_DECLARED;
    else if (c->error_norm != FORGE_V2_NORM_ELEMENTWISE_MAX || c->error_kind == FORGE_V2_ERROR_MEASURED_DISTRIBUTION)
        out->refuse_reason = FORGE_ASIM_REFUSE_UNSUPPORTED;
    if (out->refuse_reason == FORGE_ASIM_REFUSE_NONE) {
        memcpy(set[nset].contract_digest, contract_digest, 32);
        memcpy(set[nset].machine_identity, st->machine_identity, 32);
        memcpy(set[nset].substrate_digest, analog_digest, 32);
        set[nset].substrate_class = analog.substrate_class;
        nset++;
    }
    forge_asim_digital_descriptor(&digital);
    if (st->include_digital &&
        forge_v2_substrate_eligible(&digital, c, NULL, FORGE_V2_FAULT_NONE, &rq->now) == FORGE_V2_OK) {
        memcpy(set[nset].contract_digest, contract_digest, 32);
        memcpy(set[nset].machine_identity, st->machine_identity, 32);
        forge_v2_digest_substrate(&digital, set[nset].substrate_digest);
        set[nset].substrate_class = digital.substrate_class;
        nset++;
    }
    /* admission: the eligible set must hold a digital realization */
    if (forge_v2_admit_plan(set, nset, contract_digest) != FORGE_V2_OK)
        return out->status = FORGE_ASIM_ERR_NO_FALLBACK;

    if (out->refuse_reason == FORGE_ASIM_REFUSE_NONE) {
        uint32_t runs = c->min_samples;
        uint64_t cal_age = rq->now.execution_epoch_s - st->cal.epoch_s;   /* eligible => not negative */
        int32_t mid = (int32_t)(((int64_t)st->cal.env_temp_min_mc + st->cal.env_temp_max_mc) / 2);
        int32_t dtemp = rq->now.temperature_mc - mid;
        uint64_t worst = 0, worst_rel = 0, lat = 0, lat_total = 0;
        uint32_t within_runs = 0, have_chosen = 0, differs = 0, exact_bad = 0, late = 0;
        for (uint32_t r = 0; r < runs; r++) {
            rc = forge_asim_realize(&st->actual, st->wrong_answer_row, st->wrong_answer_delta, rq->A, rq->x,
                                    rq->M, rq->N, r, cal_age, dtemp, run_y, &lat);
            if (rc != FORGE_ASIM_PUBLISHED_ANALOG) return out->status = rc;
            lat_total = sat_add(lat_total, lat);
            if (rq->latency_budget_ns && lat > rq->latency_budget_ns) late = 1;
            if (r == 0) memcpy(first, run_y, sizeof first);
            else if (memcmp(first, run_y, (size_t)rq->M * sizeof run_y[0]) != 0) differs = 1;
            if (c->error_kind == FORGE_V2_ERROR_EXACT) {
                uint8_t od[32];
                vec_digest("FORGE-ASIM/output/matvec/v1", run_y, rq->M, od);
                if (memcmp(od, ref_id, 32) != 0) exact_bad = 1;
            }
            uint32_t ok = 1;
            for (uint32_t i = 0; i < rq->M; i++) {
                uint64_t err = run_y[i] > ref[i] ? run_y[i] - ref[i] : ref[i] - run_y[i];
                uint64_t allow = sat_add(c->error_abs, (uint64_t)c->error_rel_ppb * ref[i] / 1000000000ull);
                if (c->error_kind == FORGE_V2_ERROR_EXACT) allow = 0;
                if (err > allow) ok = 0;
                if (err > worst) worst = err;
                uint64_t rel = ref[i] ? mul_div_ceil(err, 1000000000ull, ref[i]) : (err ? UINT32_MAX : 0);
                if (rel > worst_rel) worst_rel = rel;
            }
            if (ok) {
                within_runs++;
                if (!have_chosen) { memcpy(chosen, run_y, sizeof chosen); have_chosen = 1; }
            }
        }
        out->analog_ran = 1;
        out->analog_error_abs = worst;
        out->analog_runs = runs;
        out->analog_runs_within_bound = within_runs;

        if (late) out->refuse_reason = FORGE_ASIM_REFUSE_LATENCY_ACTUAL;
        /* EXACT is never satisfied once any non-ideality is injected, even
         * when the outputs happen to land on the oracle's values */
        else if (c->error_kind == FORGE_V2_ERROR_EXACT &&
                 (exact_bad || !model_ideal(&st->actual) || st->wrong_answer_row != 0))
            out->refuse_reason = FORGE_ASIM_REFUSE_EXACT_MISMATCH;
        else if (c->error_kind != FORGE_V2_ERROR_BOUNDED_STOCHASTIC && differs)
            out->refuse_reason = FORGE_ASIM_REFUSE_NONDETERMINISM;
        else if (c->error_kind != FORGE_V2_ERROR_BOUNDED_STOCHASTIC && within_runs != runs)
            out->refuse_reason = FORGE_ASIM_REFUSE_ERROR_BOUND;
        else if (c->error_kind == FORGE_V2_ERROR_BOUNDED_STOCHASTIC &&
                 (uint64_t)within_runs * 1000000ull < (uint64_t)c->confidence_ppm * runs)
            out->refuse_reason = FORGE_ASIM_REFUSE_CONFIDENCE;

        /* evidence of the simulated analog realization, refused or not */
        ForgeExecutionEvidenceV2 *e = &out->analog_evidence;
        uint8_t code[32];
        label_digest(ANALOG_CODE_LABEL, code);
        forge_v2_realization_identity(contract_digest, st->machine_identity, analog_digest, code, cal_digest,
                                      e->realization_identity);
        memcpy(e->machine_identity, st->machine_identity, 32);
        memcpy(e->substrate_identity, analog.substrate_identity, 32);
        memcpy(e->semantic_contract_digest, contract_digest, 32);
        e->has_calibration = 1;
        memcpy(e->calibration_digest, cal_digest, 32);
        memcpy(e->input_identity, input_id, 32);
        vec_digest("FORGE-ASIM/output/matvec/v1", have_chosen ? chosen : first, rq->M, e->output_identity);
        e->execution_epoch_s = rq->now.execution_epoch_s;
        e->execution_duration_ns = lat_total;
        e->conversion_duration_ns = lat;
        e->env_temperature_mc = rq->now.temperature_mc;
        e->env_supply_uv = rq->now.supply_uv;
        e->measured_error_abs = worst;   /* V2 field name; the value is a simulated error vs the oracle */
        e->measured_error_rel_ppb = worst_rel > UINT32_MAX ? UINT32_MAX : (uint32_t)worst_rel;
        e->confidence_ppm = (uint32_t)((uint64_t)within_runs * 1000000ull / runs);
        e->repeat_runs = runs;
        e->repeat_passes = within_runs;
        e->repeat_mean_ns = runs ? lat_total / runs : 0;
        e->fault_state = FORGE_V2_FAULT_NONE;
        if (seal_evidence(e, out->analog_evidence_digest) != 0) return out->status = FORGE_ASIM_ERR_EVIDENCE;

        if (out->refuse_reason == FORGE_ASIM_REFUSE_NONE) {
            out->final_evidence = *e;
            memcpy(out->final_evidence_digest, out->analog_evidence_digest, 32);
            memcpy(out->y, chosen, (size_t)rq->M * sizeof chosen[0]);
            out->published = 1;
            return out->status = FORGE_ASIM_PUBLISHED_ANALOG;
        }
    }
    /* refused: nothing of the analog run is published; the digital
     * realization answers under the same contract */
    if (!st->include_digital) return out->status = FORGE_ASIM_ERR_NO_FALLBACK;
    return out->status = run_digital(st, rq, oracle, contract_digest, input_id, ref, out);
}
