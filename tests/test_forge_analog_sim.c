/*
 * tests/test_forge_analog_sim.c -- AR2 gate checks for the simulated analog
 * provider (forge/analog-sim). Maps 1:1 onto the AR2 table of the analog
 * realization test plan: exact oracle, tolerance oracle, injected
 * non-idealities, contract strictness, fallback, provenance.
 *
 *   test_forge_analog_sim            run every check, print PASS/FAIL, exit 0 on all-pass
 *   test_forge_analog_sim --receipt  same checks, then print the deterministic JSON receipt
 *
 * Every result in this file is SIMULATED_DEVELOPMENT. No device acts.
 */
#include <stdio.h>
#include <string.h>

#include "forge_analog_sim.h"

static int g_pass, g_fail;
static int g_quiet;

static void check(const char *name, int ok)
{
    if (ok) g_pass++; else g_fail++;
    if (!g_quiet || !ok) fprintf(g_quiet ? stderr : stdout, "%s %s\n", ok ? "PASS" : "FAIL", name);
}

/* ---- receipt table ---- */

#define MAX_CASES 128
typedef struct {
    const char *name;
    uint32_t contract_kind, oracle;
    uint64_t contract_bound;
    int status;
    uint32_t refuse_reason;
    int eligibility_rc;
    uint32_t analog_ran, runs, runs_within;
    uint64_t error_abs;
    uint8_t analog_ev[32], final_ev[32];
    uint32_t final_is_digital;
} CaseRow;
static CaseRow g_cases[MAX_CASES];
static int g_ncases;

/* ---- fixtures ---- */

#define EPOCH0 1790000000ull        /* fixed authority-clock time of the fixture calibration */
#define WINDOW (24ull * 3600)       /* calibration validity: one day */

static uint64_t g_A[FORGE_ASIM_MAX_ROWS * FORGE_ASIM_MAX_COLS], g_x[FORGE_ASIM_MAX_COLS];

static uint64_t lcg(uint64_t *s)
{
    *s = *s * 6364136223846793005ull + 1442695040888963407ull;
    return *s >> 33;
}

static void fill_inputs(uint32_t M, uint32_t N, uint64_t seed)
{
    for (size_t k = 0; k < (size_t)M * N; k++) g_A[k] = lcg(&seed) & 0xFF;
    for (uint32_t j = 0; j < N; j++) g_x[j] = lcg(&seed) & 0xFF;
}

static void fixture_cal(ForgeCalibrationArtifact *a)
{
    memset(a, 0, sizeof *a);
    forge_asim_substrate_identity(a->substrate_id);
    forge_v2_sha256((const uint8_t *)"FORGE-ASIM/test/calibration/1", 29, a->calibration_id);
    a->method = FORGE_V2_CAL_SIMULATED_MODEL;
    a->epoch_s = EPOCH0;
    a->validity_window_s = WINDOW;
    a->env_temp_min_mc = 20000;
    a->env_temp_max_mc = 30000;   /* calibration point = 25 C */
    a->env_supply_min_uv = 900000;
    a->env_supply_max_uv = 1100000;
    a->noise_samples = 1024;
    a->fixture_count = 8;
    forge_v2_sha256((const uint8_t *)"FORGE-ASIM/test/fixtures", 24, a->fixtures_digest);
    forge_v2_sha256((const uint8_t *)"FORGE-ASIM/test/evidence-root", 29, a->evidence_root);
}

static void fixture_setup(ForgeAsimSetup *st)
{
    memset(st, 0, sizeof *st);
    fixture_cal(&st->cal);
    forge_v2_sha256((const uint8_t *)"FORGE-ASIM/test/machine", 23, st->machine_identity);
    st->include_digital = 1;
}

static void fixture_request(ForgeAsimRequest *rq, uint32_t kind, uint32_t oracle, uint64_t bound, uint32_t M,
                            uint32_t N)
{
    memset(rq, 0, sizeof *rq);
    ForgeSemanticResultContract *c = &rq->contract;
    c->operation_family = FORGE_V2_OP_MATVEC;
    forge_asim_oracle_digest(oracle, c->reference_oracle_digest);
    c->shape_rank = 2; c->shape_dim[0] = M; c->shape_dim[1] = N;
    c->error_kind = kind;
    c->error_abs = kind == FORGE_V2_ERROR_EXACT ? 0 : bound;
    c->error_norm = FORGE_V2_NORM_ELEMENTWISE_MAX;
    c->confidence_ppm = 1000000;
    c->min_samples = 4;
    c->allowed_nondeterminism = FORGE_V2_NONDET_NONE;
    if (kind == FORGE_V2_ERROR_BOUNDED_DETERMINISTIC) c->allowed_nondeterminism = FORGE_V2_NONDET_ROUNDING;
    if (kind == FORGE_V2_ERROR_BOUNDED_STOCHASTIC) {
        c->allowed_nondeterminism = FORGE_V2_NONDET_ROUNDING | FORGE_V2_NONDET_STOCHASTIC_NOISE;
        c->min_samples = 16;
    }
    rq->A = g_A; rq->x = g_x; rq->M = M; rq->N = N;
    rq->now.execution_epoch_s = EPOCH0 + 3600;   /* calibration one hour old */
    rq->now.temperature_mc = 25000;
    rq->now.supply_uv = 1000000;
}

static void oracle_u64(const ForgeAsimRequest *rq, uint64_t *ref)
{
    forge_asim_oracle_exact_u64(rq->A, rq->x, ref, rq->M, rq->N);
}

static int y_equals(const ForgeAsimOutcome *o, const uint64_t *ref, uint32_t M)
{
    return memcmp(o->y, ref, (size_t)M * sizeof ref[0]) == 0;
}

static int y_zero(const ForgeAsimOutcome *o)
{
    for (uint32_t i = 0; i < FORGE_ASIM_MAX_ROWS; i++) if (o->y[i]) return 0;
    return 1;
}

/* Provenance of every evidence record any case produced; strict decode. */
static int g_evidence_records, g_evidence_simulated, g_evidence_bad;

static void audit_evidence(const ForgeExecutionEvidenceV2 *e)
{
    uint8_t buf[4096];
    size_t n;
    ForgeExecutionEvidenceV2 back;
    g_evidence_records++;
    if (forge_v2_encode_evidence(e, buf, sizeof buf, &n) != FORGE_V2_OK ||
        forge_v2_decode_evidence(buf, n, 1, &back) != FORGE_V2_OK) {
        g_evidence_bad++;
        return;
    }
    if (back.provenance_class == FORGE_V2_PROVENANCE_SIMULATED_DEVELOPMENT) g_evidence_simulated++;
    else g_evidence_bad++;
}

static ForgeAsimOutcome g_o;

static const ForgeAsimOutcome *run(const char *name, const ForgeAsimSetup *st, const ForgeAsimRequest *rq)
{
    forge_asim_execute(st, rq, &g_o);
    if (g_o.analog_ran) audit_evidence(&g_o.analog_evidence);
    if (g_o.published) audit_evidence(&g_o.final_evidence);
    if (g_ncases >= MAX_CASES) check("receipt_case_table_has_room", 0);
    else {
        CaseRow *r = &g_cases[g_ncases++];
        memset(r, 0, sizeof *r);
        r->name = name;
        r->contract_kind = rq->contract.error_kind;
        r->oracle = 0;
        for (uint32_t o = FORGE_ASIM_ORACLE_EXACT_U64; o <= FORGE_ASIM_ORACLE_I32; o++) {
            uint8_t d[32];
            forge_asim_oracle_digest(o, d);
            if (memcmp(d, rq->contract.reference_oracle_digest, 32) == 0) r->oracle = o;
        }
        r->contract_bound = rq->contract.error_abs;
        r->status = g_o.status;
        r->refuse_reason = g_o.refuse_reason;
        r->eligibility_rc = g_o.eligibility_rc;
        r->analog_ran = g_o.analog_ran;
        r->runs = g_o.analog_runs;
        r->runs_within = g_o.analog_runs_within_bound;
        r->error_abs = g_o.analog_error_abs;
        if (g_o.analog_ran) memcpy(r->analog_ev, g_o.analog_evidence_digest, 32);
        if (g_o.published) memcpy(r->final_ev, g_o.final_evidence_digest, 32);
        r->final_is_digital = g_o.status == FORGE_ASIM_PUBLISHED_FALLBACK;
    }
    return &g_o;
}

/* refused analog, digital answer published under the same contract */
static int refused_with_fallback(const ForgeAsimOutcome *o, const ForgeAsimRequest *rq, uint32_t reason)
{
    uint64_t ref[FORGE_ASIM_MAX_ROWS];
    uint8_t cd[32];
    ForgeSubstrateDescriptor d;
    oracle_u64(rq, ref);
    forge_v2_digest_contract(&rq->contract, cd);
    forge_asim_digital_descriptor(&d);
    return o->status == FORGE_ASIM_PUBLISHED_FALLBACK && o->refuse_reason == reason && o->published == 1 &&
           y_equals(o, ref, rq->M) && memcmp(o->final_evidence.semantic_contract_digest, cd, 32) == 0 &&
           memcmp(o->final_evidence.substrate_identity, d.substrate_identity, 32) == 0;
}

static int published_analog(const ForgeAsimOutcome *o)
{
    return o->status == FORGE_ASIM_PUBLISHED_ANALOG && o->refuse_reason == FORGE_ASIM_REFUSE_NONE &&
           o->published == 1 && o->analog_ran == 1;
}

/* ---- AR2 checks ---- */

static void exact_oracle(void)
{
    static const uint32_t shapes[][2] = { {1, 1}, {7, 13}, {64, 64}, {64, 1}, {1, 64} };
    int all_ok = 1;
    for (size_t s = 0; s < sizeof shapes / sizeof shapes[0]; s++) {
        ForgeAsimSetup st; ForgeAsimRequest rq;
        uint64_t ref[FORGE_ASIM_MAX_ROWS];
        fixture_setup(&st);
        fill_inputs(shapes[s][0], shapes[s][1], 100 + s);
        fixture_request(&rq, FORGE_V2_ERROR_EXACT, FORGE_ASIM_ORACLE_EXACT_U64, 0, shapes[s][0], shapes[s][1]);
        const ForgeAsimOutcome *o = run("exact_ideal_sim_vs_u64_oracle", &st, &rq);
        oracle_u64(&rq, ref);
        if (!(published_analog(o) && y_equals(o, ref, rq.M) && o->analog_error_abs == 0)) all_ok = 0;
    }
    check("exact_ideal_sim_bit_equal_to_u64_oracle_all_shapes", all_ok);

    ForgeAsimSetup st; ForgeAsimRequest rq; ForgeSubstrateDescriptor d;
    fixture_setup(&st);
    forge_asim_describe(&st.declared, &st.cal, &d);
    check("ideal_sim_declares_exact_error_kind", d.error_kind == FORGE_V2_ERROR_EXACT && d.error_abs == 0 &&
                                                     d.substrate_class == FORGE_V2_SUBSTRATE_ANALOG_IN_MEMORY);
    fill_inputs(16, 16, 7);
    fixture_request(&rq, FORGE_V2_ERROR_EXACT, FORGE_ASIM_ORACLE_EXACT_U64, 0, 16, 16);
    const ForgeAsimOutcome *o = run("exact_output_identity_equals_oracle_digest", &st, &rq);
    ForgeAsimOutcome o1 = *o;
    const ForgeAsimOutcome *fb;
    ForgeAsimSetup st2 = st; st2.actual.bias_lsb = 1;   /* force a fallback to obtain the oracle digest */
    st2.declared = st2.actual;
    ForgeAsimRequest rq2 = rq; rq2.contract.error_kind = FORGE_V2_ERROR_EXACT;
    fb = run("exact_oracle_digest_via_digital", &st2, &rq2);
    check("exact_output_identity_equals_oracle_output_identity",
          published_analog(&o1) && fb->status == FORGE_ASIM_PUBLISHED_FALLBACK &&
              memcmp(o1.analog_evidence.output_identity, fb->final_evidence.output_identity, 32) == 0);

    /* oracle restatements agree where they must, and wrap where omega wraps */
    uint32_t a32[4] = { 0xFFFFFFFFu, 2, 3, 4 }, x32[2] = { 2, 1 }, y32[2];
    forge_asim_oracle_i32(a32, x32, y32, 2, 2);
    check("oracle_i32_wraps_u32_like_omega", y32[0] == 0u && y32[1] == 10u);
    uint64_t A64[1] = { 1ull << 63 }, x64[1] = { 2 }, y64[1];
    forge_asim_oracle_exact_u64(A64, x64, y64, 1, 1);
    check("oracle_u64_wraps_u64_like_omega", y64[0] == 0);
    uint8_t d1[32], d2[32];
    check("oracle_digests_distinct_and_unknown_refused",
          forge_asim_oracle_digest(FORGE_ASIM_ORACLE_EXACT_U64, d1) == 0 &&
              forge_asim_oracle_digest(FORGE_ASIM_ORACLE_I32, d2) == 0 && memcmp(d1, d2, 32) != 0 &&
              forge_asim_oracle_digest(99, d1) == FORGE_V2_ERR_ARG);
}

static void tolerance_oracle(void)
{
    ForgeAsimSetup st; ForgeAsimRequest rq; ForgeSubstrateDescriptor d;
    /* deterministic non-idealities against the i32 oracle */
    fixture_setup(&st);
    st.declared.quant_step_lsb = 8;
    st.declared.bias_lsb = 3;
    st.declared.drift_lsb_per_hour = 1;
    st.declared.temp_lsb_per_k = 1;
    st.actual = st.declared;
    forge_asim_describe(&st.declared, &st.cal, &d);
    fill_inputs(32, 32, 11);
    fixture_request(&rq, FORGE_V2_ERROR_BOUNDED_DETERMINISTIC, FORGE_ASIM_ORACLE_I32, d.error_abs, 32, 32);
    rq.now.temperature_mc = 28000;
    const ForgeAsimOutcome *o = run("tolerance_bounded_deterministic_i32", &st, &rq);
    check("tolerance_bounded_deterministic_vs_i32_oracle_within_declared_bound",
          d.error_kind == FORGE_V2_ERROR_BOUNDED_DETERMINISTIC && d.error_abs == 4 + 3 + 24 + 5 &&
              published_analog(o) && o->analog_error_abs > 0 && o->analog_error_abs <= d.error_abs);

    /* stochastic: noise, many runs, confidence counted */
    fixture_setup(&st);
    st.declared.noise_peak_lsb = 6;
    st.declared.noise_seed = 0xA2A2;
    st.declared.bias_lsb = -2;
    st.actual = st.declared;
    forge_asim_describe(&st.declared, &st.cal, &d);
    fixture_request(&rq, FORGE_V2_ERROR_BOUNDED_STOCHASTIC, FORGE_ASIM_ORACLE_I32, d.error_abs, 32, 32);
    o = run("tolerance_bounded_stochastic_i32", &st, &rq);
    check("tolerance_bounded_stochastic_vs_i32_oracle_all_runs_within_bound",
          d.error_kind == FORGE_V2_ERROR_BOUNDED_STOCHASTIC && published_analog(o) && o->analog_runs == 16 &&
              o->analog_runs_within_bound == 16 && o->analog_error_abs > 0 && o->analog_error_abs <= 8);

    /* the bound is error_abs + error_rel_ppb * |ref| / 1e9 per element: an
     * error above error_abs alone passes only because of the relative part,
     * and one LSB above the smallest per-element allowance is refused */
    fixture_setup(&st);
    st.declared.bias_lsb = 40;
    st.actual = st.declared;
    fill_inputs(8, 64, 12);
    fixture_request(&rq, FORGE_V2_ERROR_BOUNDED_DETERMINISTIC, FORGE_ASIM_ORACLE_EXACT_U64, 40, 8, 64);
    rq.contract.error_rel_ppb = 10000;   /* 10 ppm of the reference */
    uint64_t ref[FORGE_ASIM_MAX_ROWS], allow_min = UINT64_MAX;
    oracle_u64(&rq, ref);
    for (uint32_t i = 0; i < rq.M; i++) {
        uint64_t a = 40 + ref[i] * rq.contract.error_rel_ppb / 1000000000ull;
        if (a < allow_min) allow_min = a;
    }
    st.actual.bias_lsb = (int64_t)allow_min;
    o = run("tolerance_rel_part_admits_error_above_abs", &st, &rq);
    check("tolerance_rel_part_admits_error_above_abs",
          allow_min > 40 && published_analog(o) && o->analog_error_abs == allow_min);
    st.actual.bias_lsb = (int64_t)allow_min + 1;
    o = run("tolerance_one_lsb_over_bound_refused", &st, &rq);
    check("tolerance_one_lsb_over_bound_refused", refused_with_fallback(o, &rq, FORGE_ASIM_REFUSE_ERROR_BOUND) &&
                                                      o->analog_error_abs == allow_min + 1);
}

/* One non-ideality: declared model -> contract bound = declared bound.
 * Within: actual = declared, published. Out: actual worse, refused. */
static void one_kind(const char *within_name, const char *out_name, const ForgeAnalogSimModel *declared,
                     const ForgeAnalogSimModel *worse, uint32_t kind, uint32_t expect_reason)
{
    ForgeAsimSetup st; ForgeAsimRequest rq; ForgeSubstrateDescriptor d;
    fixture_setup(&st);
    st.declared = *declared;
    st.actual = *declared;
    forge_asim_describe(&st.declared, &st.cal, &d);
    fill_inputs(24, 40, 21);
    fixture_request(&rq, kind, FORGE_ASIM_ORACLE_I32, d.error_abs, 24, 40);
    rq.now.temperature_mc = 27000;
    rq.now.execution_epoch_s = EPOCH0 + 10 * 3600;
    rq.latency_budget_ns = 1000;
    const ForgeAsimOutcome *o = run(within_name, &st, &rq);
    check(within_name, published_analog(o) && o->analog_error_abs <= d.error_abs);
    st.actual = *worse;
    o = run(out_name, &st, &rq);
    check(out_name, refused_with_fallback(o, &rq, expect_reason) && o->analog_ran == 1);
}

static void injected_nonidealities(void)
{
    ForgeAnalogSimModel dec, bad;
    const uint32_t BD = FORGE_V2_ERROR_BOUNDED_DETERMINISTIC, BS = FORGE_V2_ERROR_BOUNDED_STOCHASTIC;

    memset(&dec, 0, sizeof dec); dec.quant_step_lsb = 4;
    bad = dec; bad.quant_step_lsb = 64;
    one_kind("inject_quantization_within_bound_published", "inject_quantization_over_bound_refused", &dec, &bad,
             BD, FORGE_ASIM_REFUSE_ERROR_BOUND);

    memset(&dec, 0, sizeof dec); dec.noise_peak_lsb = 3; dec.noise_seed = 77;
    bad = dec; bad.noise_peak_lsb = 300;
    one_kind("inject_noise_within_bound_published", "inject_noise_over_bound_refused", &dec, &bad, BS,
             FORGE_ASIM_REFUSE_CONFIDENCE);

    memset(&dec, 0, sizeof dec); dec.bias_lsb = 5;
    bad = dec; bad.bias_lsb = 9;
    one_kind("inject_bias_within_bound_published", "inject_bias_over_bound_refused", &dec, &bad, BD,
             FORGE_ASIM_REFUSE_ERROR_BOUND);

    memset(&dec, 0, sizeof dec); dec.drift_lsb_per_hour = 1;   /* window bound 24 LSB; at 10 h, 10 LSB */
    bad = dec; bad.drift_lsb_per_hour = 10;                    /* at 10 h, 100 LSB */
    one_kind("inject_drift_within_bound_published", "inject_drift_over_bound_refused", &dec, &bad, BD,
             FORGE_ASIM_REFUSE_ERROR_BOUND);

    memset(&dec, 0, sizeof dec); dec.saturation_max = (1ull << 24) - 1;   /* never clips these inputs */
    bad = dec; bad.saturation_max = 1000;                                 /* clips most rows */
    one_kind("inject_saturation_within_range_published", "inject_saturation_clipping_refused", &dec, &bad, BD,
             FORGE_ASIM_REFUSE_ERROR_BOUND);

    memset(&dec, 0, sizeof dec); dec.temp_lsb_per_k = 2;       /* envelope half width 5 K: bound 10 */
    bad = dec; bad.temp_lsb_per_k = 20;                        /* 2 K off: 40 LSB */
    one_kind("inject_temperature_within_bound_published", "inject_temperature_over_bound_refused", &dec, &bad,
             BD, FORGE_ASIM_REFUSE_ERROR_BOUND);

    memset(&dec, 0, sizeof dec); dec.bias_lsb = 1; dec.latency_ns = 500;
    bad = dec; bad.latency_ns = 5000;
    one_kind("inject_latency_within_budget_published", "inject_latency_over_budget_refused", &dec, &bad, BD,
             FORGE_ASIM_REFUSE_LATENCY_ACTUAL);

    memset(&dec, 0, sizeof dec);
    dec.quant_step_lsb = 4; dec.noise_peak_lsb = 2; dec.noise_seed = 5; dec.bias_lsb = -3;
    dec.drift_lsb_per_hour = 1; dec.temp_lsb_per_k = 1; dec.saturation_max = (1ull << 24) - 1; dec.latency_ns = 400;
    bad = dec; bad.bias_lsb = -60; bad.noise_peak_lsb = 40;
    one_kind("inject_combined_within_bound_published", "inject_combined_over_bound_refused", &dec, &bad, BS,
             FORGE_ASIM_REFUSE_CONFIDENCE);

    /* declared-level refusals (before anything runs) */
    ForgeAsimSetup st; ForgeAsimRequest rq; ForgeSubstrateDescriptor d;
    const ForgeAsimOutcome *o;
    fixture_setup(&st);
    st.declared.bias_lsb = 2; st.declared.latency_ns = 400; st.actual = st.declared;
    forge_asim_describe(&st.declared, &st.cal, &d);
    fill_inputs(8, 8, 31);

    fixture_request(&rq, BD, FORGE_ASIM_ORACLE_I32, d.error_abs, 8, 8);
    rq.now.execution_epoch_s = EPOCH0 + WINDOW - 1;
    o = run("inject_calibration_age_inside_window_published", &st, &rq);
    check("inject_calibration_age_inside_window_published", published_analog(o));
    rq.now.execution_epoch_s = EPOCH0 + WINDOW;
    o = run("inject_calibration_age_stale_refused", &st, &rq);
    check("inject_calibration_age_stale_refused_ineligible",
          refused_with_fallback(o, &rq, FORGE_ASIM_REFUSE_INELIGIBLE) &&
              o->eligibility_rc == FORGE_V2_ERR_CAL_STALE && o->analog_ran == 0);
    rq.now.execution_epoch_s = EPOCH0 - 1;
    o = run("inject_calibration_from_future_refused", &st, &rq);
    check("inject_calibration_from_future_refused_ineligible",
          refused_with_fallback(o, &rq, FORGE_ASIM_REFUSE_INELIGIBLE) && o->eligibility_rc == FORGE_V2_ERR_CAL_STALE);

    fixture_request(&rq, BD, FORGE_ASIM_ORACLE_I32, d.error_abs, 8, 8);
    rq.now.temperature_mc = 31000;
    o = run("inject_temperature_outside_envelope_refused", &st, &rq);
    check("inject_temperature_outside_envelope_refused_ineligible",
          refused_with_fallback(o, &rq, FORGE_ASIM_REFUSE_INELIGIBLE) &&
              o->eligibility_rc == FORGE_V2_ERR_OUT_OF_ENVELOPE && o->analog_ran == 0);

    fixture_request(&rq, BD, FORGE_ASIM_ORACLE_I32, d.error_abs, 8, 8);
    rq.latency_budget_ns = 300;
    o = run("inject_latency_declared_over_budget_refused", &st, &rq);
    check("inject_latency_declared_over_budget_refused",
          refused_with_fallback(o, &rq, FORGE_ASIM_REFUSE_LATENCY_DECLARED) && o->analog_ran == 0);

    fixture_request(&rq, BD, FORGE_ASIM_ORACLE_I32, d.error_abs - 1, 8, 8);
    o = run("declared_bound_over_contract_refused", &st, &rq);
    check("declared_bound_over_contract_refused_ineligible",
          refused_with_fallback(o, &rq, FORGE_ASIM_REFUSE_INELIGIBLE) && o->eligibility_rc == FORGE_V2_ERR_VALUE &&
              o->analog_ran == 0);
}

static void strictness(void)
{
    static const char *const names[] = {
        "exact_refuses_declared_quantization", "exact_refuses_declared_noise", "exact_refuses_declared_bias",
        "exact_refuses_declared_drift", "exact_refuses_declared_temperature", "exact_refuses_declared_saturation"
    };
    ForgeAsimSetup st; ForgeAsimRequest rq;
    int all_ok = 1;
    fill_inputs(12, 12, 41);
    for (int k = 0; k < 6; k++) {
        fixture_setup(&st);
        switch (k) {
        case 0: st.declared.quant_step_lsb = 2; break;
        case 1: st.declared.noise_peak_lsb = 1; break;
        case 2: st.declared.bias_lsb = 1; break;
        case 3: st.declared.drift_lsb_per_hour = 1; break;
        case 4: st.declared.temp_lsb_per_k = 1; break;
        default: st.declared.saturation_max = (1ull << 24) - 1; break;
        }
        st.actual = st.declared;
        fixture_request(&rq, FORGE_V2_ERROR_EXACT, FORGE_ASIM_ORACLE_EXACT_U64, 0, 12, 12);
        const ForgeAsimOutcome *o = run(names[k], &st, &rq);
        if (!(refused_with_fallback(o, &rq, FORGE_ASIM_REFUSE_INELIGIBLE) &&
              o->eligibility_rc == FORGE_V2_ERR_VALUE && o->analog_ran == 0))
            all_ok = 0;
    }
    check("exact_contract_refuses_every_declared_nonideality_at_eligibility", all_ok);

    /* a provider that declares itself ideal but is not: caught by the digest compare */
    fixture_setup(&st);
    st.actual.bias_lsb = 1;
    fixture_request(&rq, FORGE_V2_ERROR_EXACT, FORGE_ASIM_ORACLE_EXACT_U64, 0, 12, 12);
    const ForgeAsimOutcome *o = run("exact_false_ideal_claim_caught", &st, &rq);
    check("exact_false_ideal_claim_caught_by_output_digest",
          refused_with_fallback(o, &rq, FORGE_ASIM_REFUSE_EXACT_MISMATCH) && o->eligibility_rc == FORGE_V2_OK &&
              o->analog_ran == 1);

    /* injected quantization that happens to land on the oracle's values
     * (all outputs even, step 2) still never satisfies EXACT */
    fixture_setup(&st);
    st.actual.quant_step_lsb = 2;
    for (size_t k = 0; k < 12 * 12; k++) g_A[k] &= ~1ull;
    fixture_request(&rq, FORGE_V2_ERROR_EXACT, FORGE_ASIM_ORACLE_EXACT_U64, 0, 12, 12);
    o = run("exact_refuses_injected_nonideality_even_on_matching_output", &st, &rq);
    check("exact_refuses_injected_nonideality_even_on_matching_output",
          refused_with_fallback(o, &rq, FORGE_ASIM_REFUSE_EXACT_MISMATCH) && o->analog_ran == 1 &&
              o->analog_error_abs == 0);
    fill_inputs(12, 12, 41);

    /* a stochastic substrate is never offered for a deterministic contract */
    fixture_setup(&st);
    st.declared.noise_peak_lsb = 1; st.actual = st.declared;
    fixture_request(&rq, FORGE_V2_ERROR_BOUNDED_DETERMINISTIC, FORGE_ASIM_ORACLE_I32, 1000, 12, 12);
    o = run("deterministic_contract_refuses_stochastic_substrate", &st, &rq);
    check("deterministic_contract_refuses_stochastic_substrate",
          refused_with_fallback(o, &rq, FORGE_ASIM_REFUSE_INELIGIBLE) && o->eligibility_rc == FORGE_V2_ERR_VALUE);

    /* declared deterministic, actually noisy: runs disagree, refused */
    fixture_setup(&st);
    st.declared.bias_lsb = 1; st.actual = st.declared;
    st.actual.noise_peak_lsb = 1; st.actual.noise_seed = 3;
    fixture_request(&rq, FORGE_V2_ERROR_BOUNDED_DETERMINISTIC, FORGE_ASIM_ORACLE_I32, 1, 12, 12);
    rq.contract.error_abs = 1000;
    o = run("deterministic_contract_catches_hidden_noise", &st, &rq);
    check("deterministic_contract_catches_hidden_noise",
          refused_with_fallback(o, &rq, FORGE_ASIM_REFUSE_NONDETERMINISM) && o->analog_ran == 1);

    /* deliberately wrong simulated answers are caught under every contract kind */
    fixture_setup(&st);
    st.wrong_answer_row = 5; st.wrong_answer_delta = 1;
    fixture_request(&rq, FORGE_V2_ERROR_EXACT, FORGE_ASIM_ORACLE_EXACT_U64, 0, 12, 12);
    o = run("wrong_answer_exact_caught", &st, &rq);
    check("wrong_answer_off_by_one_caught_under_exact",
          refused_with_fallback(o, &rq, FORGE_ASIM_REFUSE_EXACT_MISMATCH) && o->analog_error_abs == 1);

    fixture_setup(&st);
    st.declared.bias_lsb = 2; st.actual = st.declared;
    st.wrong_answer_row = 12; st.wrong_answer_delta = -500;
    fixture_request(&rq, FORGE_V2_ERROR_BOUNDED_DETERMINISTIC, FORGE_ASIM_ORACLE_I32, 2, 12, 12);
    o = run("wrong_answer_bounded_deterministic_caught", &st, &rq);
    check("wrong_answer_caught_under_bounded_deterministic",
          refused_with_fallback(o, &rq, FORGE_ASIM_REFUSE_ERROR_BOUND) && o->analog_error_abs >= 498);

    fixture_setup(&st);
    st.declared.noise_peak_lsb = 2; st.declared.noise_seed = 9; st.actual = st.declared;
    st.wrong_answer_row = 1; st.wrong_answer_delta = 50;
    fixture_request(&rq, FORGE_V2_ERROR_BOUNDED_STOCHASTIC, FORGE_ASIM_ORACLE_I32, 2, 12, 12);
    rq.contract.confidence_ppm = 500000;
    o = run("wrong_answer_bounded_stochastic_caught", &st, &rq);
    check("wrong_answer_caught_under_bounded_stochastic",
          refused_with_fallback(o, &rq, FORGE_ASIM_REFUSE_CONFIDENCE) && o->analog_runs_within_bound == 0);
}

static void fallback(void)
{
    ForgeAsimSetup st; ForgeAsimRequest rq;
    const ForgeAsimOutcome *o;
    fill_inputs(10, 10, 51);

    fixture_setup(&st);
    st.include_digital = 0;
    fixture_request(&rq, FORGE_V2_ERROR_EXACT, FORGE_ASIM_ORACLE_EXACT_U64, 0, 10, 10);
    o = run("no_digital_fallback_plan_not_admitted", &st, &rq);
    check("no_digital_fallback_plan_not_admitted_even_when_analog_fits",
          o->status == FORGE_ASIM_ERR_NO_FALLBACK && o->published == 0 && o->analog_ran == 0 && y_zero(o));

    st.actual.bias_lsb = 7;
    o = run("no_digital_fallback_refused_nothing_published", &st, &rq);
    check("no_digital_fallback_refused_result_publishes_nothing",
          o->status == FORGE_ASIM_ERR_NO_FALLBACK && o->published == 0 && y_zero(o));

    fixture_setup(&st);
    st.actual.bias_lsb = 7;
    o = run("fallback_digital_answers_same_contract", &st, &rq);
    check("fallback_digital_answers_same_contract_with_evidence",
          refused_with_fallback(o, &rq, FORGE_ASIM_REFUSE_EXACT_MISMATCH) &&
              o->final_evidence.has_calibration == 0 && o->final_evidence.measured_error_abs == 0 &&
              memcmp(o->final_evidence_digest, o->analog_evidence_digest, 32) != 0);

    /* the refused analog run still left evidence of what it did */
    check("refused_analog_run_still_has_evidence",
          o->analog_ran == 1 && o->analog_evidence.measured_error_abs == 7 && o->analog_evidence.repeat_passes == 0 &&
              o->analog_evidence.has_calibration == 1);

    /* the digital fallback meets the same contract's sample count */
    fixture_setup(&st);
    st.declared.noise_peak_lsb = 2; st.declared.noise_seed = 4; st.actual = st.declared;
    st.actual.noise_peak_lsb = 500;
    fixture_request(&rq, FORGE_V2_ERROR_BOUNDED_STOCHASTIC, FORGE_ASIM_ORACLE_I32, 2, 10, 10);
    o = run("fallback_meets_stochastic_sample_count", &st, &rq);
    check("fallback_meets_contract_sample_count",
          refused_with_fallback(o, &rq, FORGE_ASIM_REFUSE_CONFIDENCE) &&
              o->final_evidence.repeat_runs == rq.contract.min_samples &&
              o->final_evidence.repeat_passes == rq.contract.min_samples);

    /* extreme but accepted model magnitudes stay defined (the gate also runs this under UBSan) */
    {
        ForgeAnalogSimModel m;
        uint64_t yy[FORGE_ASIM_MAX_ROWS], lat;
        memset(&m, 0, sizeof m);
        m.bias_lsb = (int64_t)1 << 32; m.drift_lsb_per_hour = (int64_t)1 << 32; m.temp_lsb_per_k = (int64_t)1 << 32;
        m.noise_peak_lsb = UINT32_MAX; m.quant_step_lsb = UINT32_MAX;
        int r1 = forge_asim_realize(&m, 1, (int64_t)1 << 32, g_A, g_x, 10, 10, 0, UINT64_MAX, INT32_MAX, yy, &lat);
        m.bias_lsb = -m.bias_lsb; m.drift_lsb_per_hour = -m.drift_lsb_per_hour;
        int r2 = forge_asim_realize(&m, 0, 0, g_A, g_x, 10, 10, 1, UINT64_MAX, INT32_MIN, yy, &lat);
        m.bias_lsb = ((int64_t)1 << 32) + 1;
        int r3 = forge_asim_realize(&m, 0, 0, g_A, g_x, 10, 10, 0, 0, 0, yy, &lat);
        uint64_t b = forge_asim_model_bound(&m, UINT64_MAX, UINT32_MAX);
        check("extreme_model_magnitudes_defined_and_over_limit_refused",
              r1 == FORGE_ASIM_PUBLISHED_ANALOG && r2 == FORGE_ASIM_PUBLISHED_ANALOG && r3 == FORGE_ASIM_ERR_ARG &&
                  b == UINT64_MAX);
    }

    /* input out of the declared limits is refused outright */
    g_A[0] = 256;
    fixture_setup(&st);
    o = run("input_out_of_declared_range_refused", &st, &rq);
    check("input_out_of_declared_range_refused", o->status == FORGE_ASIM_ERR_INPUT_RANGE && o->published == 0);
    g_A[0] = 1;
}

static void provenance_and_identity(void)
{
    ForgeAsimSetup st; ForgeAsimRequest rq;
    ForgeAsimOutcome a, b;
    check("provider_provenance_is_simulated_development",
          forge_asim_provenance() == FORGE_V2_PROVENANCE_SIMULATED_DEVELOPMENT &&
              forge_asim_provenance() != FORGE_V2_PROVENANCE_PHYSICAL);
    check("every_evidence_record_simulated_development_strict_roundtrip",
          g_evidence_records > 0 && g_evidence_bad == 0 && g_evidence_simulated == g_evidence_records);

    /* a gate that requires PHYSICAL refuses every record of this provider */
    fixture_setup(&st);
    fill_inputs(9, 9, 61);
    fixture_request(&rq, FORGE_V2_ERROR_EXACT, FORGE_ASIM_ORACLE_EXACT_U64, 0, 9, 9);
    forge_asim_execute(&st, &rq, &a);
    check("physical_requiring_gate_refuses_simulated_result",
          a.published && a.final_evidence.provenance_class != FORGE_V2_PROVENANCE_PHYSICAL);

    /* determinism: the same inputs give the same evidence digests */
    forge_asim_execute(&st, &rq, &b);
    check("same_inputs_same_evidence_digest", memcmp(a.final_evidence_digest, b.final_evidence_digest, 32) == 0);

    /* the calibration is bound into realization identity */
    st.cal.calibration_id[0] ^= 1;
    forge_asim_execute(&st, &rq, &b);
    check("calibration_change_moves_realization_identity",
          b.published && memcmp(a.final_evidence.realization_identity, b.final_evidence.realization_identity, 32) != 0);
}

/* ---- receipt ---- */

static void hex(const uint8_t *d)
{
    for (int i = 0; i < 32; i++) printf("%02x", d[i]);
}

static const char *kind_name(uint32_t k)
{
    switch (k) {
    case FORGE_V2_ERROR_EXACT: return "EXACT";
    case FORGE_V2_ERROR_BOUNDED_DETERMINISTIC: return "BOUNDED_DETERMINISTIC";
    case FORGE_V2_ERROR_BOUNDED_STOCHASTIC: return "BOUNDED_STOCHASTIC";
    default: return "OTHER";
    }
}

static const char *status_name(int s)
{
    switch (s) {
    case FORGE_ASIM_PUBLISHED_ANALOG: return "published_simulated_analog";
    case FORGE_ASIM_PUBLISHED_FALLBACK: return "published_digital_fallback";
    case FORGE_ASIM_ERR_NO_FALLBACK: return "not_admitted_no_digital_fallback";
    case FORGE_ASIM_ERR_INPUT_RANGE: return "input_out_of_range";
    default: return "error";
    }
}

static void print_receipt(void)
{
    printf("{\n");
    printf("  \"gate\": \"AR2_ANALOG_SIMULATION_PROVIDER\",\n");
    printf("  \"workstream\": \"ARCH-0018 AR2\",\n");
    printf("  \"provenance_class\": \"SIMULATED_DEVELOPMENT\",\n");
    printf("  \"note\": \"Software model of an analog in-memory matvec substrate. No device acted. "
           "Errors are simulated errors against the digital oracle.\",\n");
    printf("  \"status\": \"%s\",\n", g_fail == 0 ? "PASS" : "FAIL");
    printf("  \"checks_passed\": %d,\n", g_pass);
    printf("  \"checks_failed\": %d,\n", g_fail);
    printf("  \"evidence_records\": %d,\n", g_evidence_records);
    printf("  \"evidence_records_simulated_development\": %d,\n", g_evidence_simulated);
    printf("  \"cases\": [\n");
    for (int i = 0; i < g_ncases; i++) {
        const CaseRow *r = &g_cases[i];
        printf("    {\"name\": \"%s\", \"contract_kind\": \"%s\", \"oracle\": \"%s\", \"contract_error_abs\": %llu, "
               "\"outcome\": \"%s\", \"analog_refusal\": \"%s\", \"eligibility\": \"%s\", \"analog_ran\": %u, "
               "\"runs\": %u, \"runs_within_bound\": %u, \"simulated_error_vs_oracle_abs\": %llu, ",
               r->name, kind_name(r->contract_kind),
               r->oracle == FORGE_ASIM_ORACLE_I32 ? "omega_matmul_cpu_oracle_i32" : "omega_matvec_reference",
               (unsigned long long)r->contract_bound, status_name(r->status), forge_asim_refuse_name(r->refuse_reason),
               forge_v2_strerror(r->eligibility_rc), r->analog_ran, r->runs, r->runs_within,
               (unsigned long long)r->error_abs);
        printf("\"analog_evidence_digest\": \"");
        if (r->analog_ran) hex(r->analog_ev);
        printf("\", \"published_evidence_digest\": \"");
        if (r->status == FORGE_ASIM_PUBLISHED_ANALOG || r->status == FORGE_ASIM_PUBLISHED_FALLBACK) hex(r->final_ev);
        printf("\", \"provenance_class\": \"SIMULATED_DEVELOPMENT\"}%s\n", i + 1 < g_ncases ? "," : "");
    }
    printf("  ]\n}\n");
}

int main(int argc, char **argv)
{
    int receipt = argc > 1 && strcmp(argv[1], "--receipt") == 0;
    g_quiet = receipt;
    exact_oracle();
    tolerance_oracle();
    injected_nonidealities();
    strictness();
    fallback();
    provenance_and_identity();
    if (receipt) print_receipt();
    else printf("RESULT checks: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
