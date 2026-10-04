/*
 * tests/test_forge_form.c -- FORM0 gate: FORGE REALIZATION FORM V1 (host-only).
 *
 *   test_forge_form             run every check, print PASS/FAIL lines + RESULT
 *   test_forge_form --receipt   same checks, then print the deterministic JSON receipt
 *
 * Proves, on the host with SIMULATED_DEVELOPMENT provenance only:
 *   1. canonical encoding + frozen digest of the LINEAR_OPERATOR form;
 *   2. strict decode refuses every malformed, reordered, unknown-tag, wrong-width,
 *      out-of-range, reserved-kind and single-byte-mutated input, or reproduces
 *      the input byte for byte (canonical uniqueness);
 *   3. one MATVEC SemanticResultContract maps to one form; the form digest is
 *      never an input of the contract digest;
 *   4. the same contract is realized by two different methods (digital
 *      reference, simulated analog) with the SAME contract identity and
 *      DIFFERENT form-bound realization identities and evidence identities;
 *   5. digital fallback and refusal paths keep evidence.
 */
#include <stdio.h>
#include <string.h>

#include "forge_form.h"

static int g_pass, g_fail, g_quiet;
static int g_records, g_records_sim;

static void check(const char *name, int ok)
{
    if (ok) g_pass++; else g_fail++;
    if (!g_quiet || !ok) fprintf(g_quiet ? stderr : stdout, "%s %s\n", ok ? "PASS" : "FAIL", name);
}

#define MAX_CASES 64
typedef struct {
    const char *name;
    uint32_t backend, contract_kind;
    int status;
    uint32_t refuse, ran, runs, within;
    uint64_t error_abs;
    uint8_t contract[32], form[32], lowering[32], realization[32], evidence[32], record[32];
} CaseRow;
static CaseRow g_cases[MAX_CASES];
static int g_ncases;

#define EPOCH0 1790000000ull
#define WINDOW (24ull * 3600)

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
    forge_v2_sha256((const uint8_t *)"FORGE-FORM/test/calibration/1", 29, a->calibration_id);
    a->method = FORGE_V2_CAL_SIMULATED_MODEL;
    a->epoch_s = EPOCH0;
    a->validity_window_s = WINDOW;
    a->env_temp_min_mc = 20000; a->env_temp_max_mc = 30000;
    a->env_supply_min_uv = 900000; a->env_supply_max_uv = 1100000;
    a->noise_samples = 1024;
    a->fixture_count = 8;
    forge_v2_sha256((const uint8_t *)"FORGE-FORM/test/fixtures", 24, a->fixtures_digest);
    forge_v2_sha256((const uint8_t *)"FORGE-FORM/test/evidence-root", 29, a->evidence_root);
}
static void fixture_contract(ForgeSemanticResultContract *c, uint32_t kind, uint32_t oracle, uint64_t bound,
                             uint32_t M, uint32_t N)
{
    memset(c, 0, sizeof *c);
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
}
static void fixture_input(ForgeFormInput *in, uint32_t M, uint32_t N)
{
    memset(in, 0, sizeof *in);
    fill_inputs(M, N, 0x5eed0001ull + M * 131 + N);
    in->A = g_A; in->x = g_x; in->M = M; in->N = N;
    forge_v2_sha256((const uint8_t *)"FORGE-FORM/test/machine", 23, in->machine_identity);
    in->now.execution_epoch_s = EPOCH0 + 3600;
    in->now.temperature_mc = 25000;
    in->now.supply_uv = 1000000;
}

static const char *kind_name(uint32_t k)
{
    switch (k) {
    case FORGE_V2_ERROR_EXACT: return "EXACT";
    case FORGE_V2_ERROR_BOUNDED_DETERMINISTIC: return "BOUNDED_DETERMINISTIC";
    case FORGE_V2_ERROR_BOUNDED_STOCHASTIC: return "BOUNDED_STOCHASTIC";
    default: return "MEASURED_DISTRIBUTION";
    }
}
static const char *backend_name(uint32_t b)
{
    return b == FORGE_FORM_BACKEND_ANALOG_SIM ? "analog_sim" : "digital_reference";
}
static const char *status_name(int s)
{
    if (s == FORGE_FORM_PUBLISHED) return "published";
    if (s == FORGE_FORM_REFUSED) return "refused";
    return forge_form_strerror(s);
}
static void hex(const uint8_t *d) { for (int i = 0; i < 32; i++) printf("%02x", d[i]); }

/* Run one lowering, audit its evidence and record it as a receipt row. */
static ForgeFormOutcome g_out[MAX_CASES];
static const ForgeFormOutcome *run(const char *name, const ForgeRealizationForm *f,
                                   const ForgeSemanticResultContract *c, const ForgeFormLowering *lw,
                                   const ForgeFormInput *in)
{
    ForgeFormOutcome *op = &g_out[g_ncases < MAX_CASES ? g_ncases : MAX_CASES - 1];
    int rc = forge_form_realize(f, c, lw, in, op);
    check(name, rc == op->status);
    if (op->ran) {
        uint8_t buf[4096], d[32];
        size_t n;
        ForgeFormRealizationRecord back;
        g_records++;
        if (op->evidence.provenance_class == FORGE_V2_PROVENANCE_SIMULATED_DEVELOPMENT &&
            op->record.provenance_class == FORGE_V2_PROVENANCE_SIMULATED_DEVELOPMENT)
            g_records_sim++;
        check("evidence_strict_decodes", forge_v2_encode_evidence(&op->evidence, buf, sizeof buf, &n) == FORGE_V2_OK &&
                                             forge_v2_decode_evidence(buf, n, 1, &(ForgeExecutionEvidenceV2){ 0 }) ==
                                                 FORGE_V2_OK);
        forge_v2_sha256(buf, n, d);
        check("evidence_digest_matches", memcmp(d, op->evidence_digest, 32) == 0);
        check("record_strict_roundtrip", forge_form_encode_record(&op->record, buf, sizeof buf, &n) == FORGE_FORM_OK &&
                                             forge_form_decode_record(buf, n, 1, &back) == FORGE_FORM_OK &&
                                             memcmp(&back, &op->record, sizeof back) == 0);
        check("record_binds_form_and_contract", memcmp(op->record.form_digest, lw->form_digest, 32) == 0 &&
                                                    memcmp(op->record.contract_digest, op->evidence.semantic_contract_digest,
                                                           32) == 0 &&
                                                    memcmp(op->record.realization_identity, op->evidence.realization_identity,
                                                           32) == 0 &&
                                                    memcmp(op->record.evidence_digest, op->evidence_digest, 32) == 0);
        check("record_decision_matches_status",
              op->record.decision == (op->status == FORGE_FORM_PUBLISHED ? FORGE_FORM_DECISION_PUBLISHED
                                                                      : FORGE_FORM_DECISION_REFUSED));
        {
            /* the realization identity really is V2's, with the lowering digest in the code slot */
            uint8_t rid[32];
            forge_v2_realization_identity(op->evidence.semantic_contract_digest, in->machine_identity,
                                          lw->substrate_digest, lw->lowering_digest,
                                          lw->has_calibration ? lw->cal_digest : NULL, rid);
            check("realization_identity_is_v2_over_lowering_digest", memcmp(rid, op->evidence.realization_identity, 32) == 0);
        }
    }
    if (g_ncases < MAX_CASES) {
        CaseRow *r = &g_cases[g_ncases++];
        memset(r, 0, sizeof *r);
        r->name = name; r->backend = lw->backend; r->contract_kind = c->error_kind;
        r->status = op->status; r->refuse = op->refuse_reason; r->ran = op->ran; r->runs = op->runs;
        r->within = op->runs_within_bound; r->error_abs = op->error_abs;
        forge_v2_digest_contract(c, r->contract);
        memcpy(r->form, lw->form_digest, 32);
        memcpy(r->lowering, lw->lowering_digest, 32);
        if (op->ran) {
            memcpy(r->realization, op->evidence.realization_identity, 32);
            memcpy(r->evidence, op->evidence_digest, 32);
            memcpy(r->record, op->record_digest, 32);
        }
    }
    return op;
}

/* ---------------------------------------------------------------- */
/* 1. canonical encoding, frozen digest                               */
/* ---------------------------------------------------------------- */

/* Frozen KAT: LINEAR_OPERATOR, INT_WRAP_U64, 8 x 8, EXACT_MODULAR. */
static const char *const KAT_FORM_DIGEST = "6e1769e21b1c1df854f4832618e276f3c556a45500af62636a51e1e862cf71d8";

static void canonical_form(void)
{
    ForgeRealizationForm f = { FORGE_FORM_LINEAR_OPERATOR, FORGE_FORM_DOMAIN_INT_WRAP_U64, 8, 8,
                               FORGE_FORM_ACCUM_EXACT_MODULAR }, back;
    uint8_t buf[256], d[32], again[256];
    size_t n = 0, m = 0;
    char hexd[65];
    check("form_encode_ok", forge_form_encode(&f, buf, sizeof buf, &n) == FORGE_FORM_OK);
    check("form_wire_len_is_12_plus_5x12", n == 12 + 5 * 12);
    check("form_header_magic_kind_version", buf[0] == 'F' && buf[1] == 'G' && buf[2] == 'F' && buf[3] == 'M' &&
                                                buf[4] == 1 && buf[5] == 0 && buf[6] == 1 && buf[7] == 0);
    check("form_strict_decode_roundtrip", forge_form_decode(buf, n, 1, &back) == FORGE_FORM_OK &&
                                              memcmp(&back, &f, sizeof f) == 0);
    check("form_reencode_identical", forge_form_encode(&back, again, sizeof again, &m) == FORGE_FORM_OK && m == n &&
                                         memcmp(again, buf, n) == 0);
    check("form_digest_ok", forge_form_digest(&f, d) == FORGE_FORM_OK);
    for (int i = 0; i < 32; i++) snprintf(hexd + 2 * i, 3, "%02x", d[i]);
    if (!g_quiet) printf("INFO form_digest_kat %s\n", hexd);
    check("form_digest_kat_frozen", strcmp(hexd, KAT_FORM_DIGEST) == 0);
    check("form_encode_too_small_refused", forge_form_encode(&f, buf, 20, &n) == FORGE_FORM_ERR_SPACE);
    check("form_encode_null_refused", forge_form_encode(NULL, buf, sizeof buf, &n) == FORGE_FORM_ERR_ARG);
}

/* ---------------------------------------------------------------- */
/* 2. negative canonicalization                                       */
/* ---------------------------------------------------------------- */

static int dec(const uint8_t *b, size_t n) { ForgeRealizationForm f; return forge_form_decode(b, n, 1, &f); }

static void negative_decode(void)
{
    ForgeRealizationForm f = { FORGE_FORM_LINEAR_OPERATOR, FORGE_FORM_DOMAIN_INT_WRAP_U64, 8, 8,
                               FORGE_FORM_ACCUM_EXACT_MODULAR };
    uint8_t ok[256], m[300];
    size_t n = 0;
    forge_form_encode(&f, ok, sizeof ok, &n);

    memcpy(m, ok, n); m[0] = 'X';
    check("neg_bad_magic", dec(m, n) == FORGE_FORM_ERR_MAGIC);
    memcpy(m, ok, n); m[4] = 2;
    check("neg_wrong_kind", dec(m, n) == FORGE_FORM_ERR_KIND);
    memcpy(m, ok, n); m[6] = 0;
    check("neg_version_0_refused", dec(m, n) == FORGE_FORM_ERR_VERSION);
    memcpy(m, ok, n); m[6] = 2;
    check("neg_future_version_refused", dec(m, n) == FORGE_FORM_ERR_VERSION);
    memcpy(m, ok, n); m[8] = (uint8_t)(m[8] + 1);
    check("neg_body_len_mismatch", dec(m, n) == FORGE_FORM_ERR_LENGTH);
    check("neg_truncated_header", dec(ok, 11) == FORGE_FORM_ERR_TRUNCATED);
    { uint8_t t[300]; memcpy(t, ok, n); t[8] = (uint8_t)(n - 12 + 1); t[n] = 0;
      check("neg_trailing_byte_is_length_error", dec(t, n + 1) == FORGE_FORM_ERR_LENGTH || dec(t, n + 1) == FORGE_FORM_ERR_TRUNCATED); }
    { /* tag order: swap fields 1 and 2 */
      uint8_t t[300]; memcpy(t, ok, n); memcpy(t + 12, ok + 24, 12); memcpy(t + 24, ok + 12, 12);
      check("neg_tag_order_refused", dec(t, n) == FORGE_FORM_ERR_ORDER); }
    { /* duplicate tag: field 5 repeated as tag 5 again */
      uint8_t t[300]; memcpy(t, ok, n); memcpy(t + n, ok + n - 12, 12); t[8] = (uint8_t)(n - 12 + 12);
      check("neg_duplicate_tag_refused", dec(t, n + 12) == FORGE_FORM_ERR_ORDER); }
    { /* unknown tag 6 appended: strict refuses, lenient skips */
      uint8_t t[300]; ForgeRealizationForm g; memcpy(t, ok, n);
      t[n] = 6; t[n+1] = t[n+2] = t[n+3] = 0; t[n+4] = 4; t[n+5] = t[n+6] = t[n+7] = 0; memset(t + n + 8, 0x11, 4);
      t[8] = (uint8_t)(n - 12 + 12);
      check("neg_unknown_tag_strict_refused", dec(t, n + 12) == FORGE_FORM_ERR_UNKNOWN_TAG);
      check("neg_unknown_tag_lenient_skipped", forge_form_decode(t, n + 12, 0, &g) == FORGE_FORM_OK &&
                                                   memcmp(&g, &f, sizeof f) == 0); }
    { /* wrong width: tag 3 with len 8 */
      uint8_t t[300]; memcpy(t, ok, n); t[12 + 24 + 4] = 8;
      check("neg_wrong_width_refused", dec(t, n) == FORGE_FORM_ERR_WIDTH || dec(t, n) == FORGE_FORM_ERR_TRUNCATED); }
    { /* missing tag: drop field 5 */
      uint8_t t[300]; memcpy(t, ok, n - 12); t[8] = (uint8_t)(n - 12 - 12);
      check("neg_missing_field_refused", dec(t, n - 12) == FORGE_FORM_ERR_MISSING); }
    { ForgeRealizationForm g = f; g.form_kind = FORGE_FORM_ENERGY_FUNCTIONAL; uint8_t t[256];
      check("neg_energy_functional_specified_not_realizable",
            forge_form_encode(&g, t, sizeof t, &n) == FORGE_FORM_ERR_UNSUPPORTED_FORM);
      g.form_kind = FORGE_FORM_STOCHASTIC_DISTRIBUTION;
      check("neg_stochastic_distribution_reserved_refused",
            forge_form_encode(&g, t, sizeof t, &n) == FORGE_FORM_ERR_UNSUPPORTED_FORM);
      g.form_kind = 9;
      check("neg_unknown_form_kind_refused", forge_form_encode(&g, t, sizeof t, &n) == FORGE_FORM_ERR_VALUE);
      g = f; g.scalar_domain = 7;
      check("neg_unknown_domain_refused", forge_form_validate(&g) == FORGE_FORM_ERR_VALUE);
      g = f; g.operator_rows = 0;
      check("neg_zero_rows_refused", forge_form_validate(&g) == FORGE_FORM_ERR_VALUE);
      g = f; g.operator_cols = FORGE_FORM_MAX_DIM + 1;
      check("neg_cols_over_max_refused", forge_form_validate(&g) == FORGE_FORM_ERR_VALUE);
      g = f; g.accumulation = 2;
      check("neg_unknown_accumulation_refused", forge_form_validate(&g) == FORGE_FORM_ERR_VALUE); }
    { /* decoded wire with a reserved kind is refused too (not only at encode) */
      uint8_t t[300]; memcpy(t, ok, n); t[20] = FORGE_FORM_ENERGY_FUNCTIONAL;
      check("neg_wire_energy_functional_refused", dec(t, n) == FORGE_FORM_ERR_UNSUPPORTED_FORM); }
    forge_form_encode(&f, ok, sizeof ok, &n);
    {
        /* single-byte mutation sweep: every mutant is refused, or decodes to a
         * form whose canonical re-encoding is byte-identical to the mutant and
         * whose digest differs from the original (canonical uniqueness) */
        uint8_t d0[32];
        int bad = 0, accepted = 0;
        forge_form_digest(&f, d0);
        for (size_t i = 0; i < n; i++) {
            for (int bit = 0; bit < 8; bit++) {
                ForgeRealizationForm g;
                uint8_t t[256], again[256], d1[32];
                size_t m2 = 0;
                memcpy(t, ok, n);
                t[i] ^= (uint8_t)(1u << bit);
                if (forge_form_decode(t, n, 1, &g) != FORGE_FORM_OK) continue;
                accepted++;
                if (forge_form_encode(&g, again, sizeof again, &m2) != FORGE_FORM_OK || m2 != n ||
                    memcmp(again, t, n) != 0)
                    bad++;
                forge_form_digest(&g, d1);
                if (memcmp(d0, d1, 32) == 0) bad++;
            }
        }
        if (!g_quiet) printf("INFO mutation sweep: %zu bits, %d accepted (all canonical), %d bad\n", n * 8, accepted, bad);
        check("neg_single_byte_mutants_canonical_or_refused", bad == 0);
        check("neg_some_value_mutants_are_valid_other_forms", accepted > 0);
    }
    { /* a record with a zero digest or a non-simulated provenance is refused */
      ForgeFormRealizationRecord r; uint8_t t[512];
      memset(&r, 0x5a, sizeof r);
      r.provenance_class = FORGE_V2_PROVENANCE_SIMULATED_DEVELOPMENT; r.decision = FORGE_FORM_DECISION_PUBLISHED;
      check("record_encode_ok", forge_form_encode_record(&r, t, sizeof t, &n) == FORGE_FORM_OK);
      r.provenance_class = FORGE_V2_PROVENANCE_PHYSICAL;
      check("neg_record_physical_provenance_refused", forge_form_encode_record(&r, t, sizeof t, &n) == FORGE_FORM_ERR_VALUE);
      r.provenance_class = FORGE_V2_PROVENANCE_SIMULATED_DEVELOPMENT; memset(r.form_digest, 0, 32);
      check("neg_record_zero_form_digest_refused", forge_form_encode_record(&r, t, sizeof t, &n) == FORGE_FORM_ERR_VALUE); }
}

/* ---------------------------------------------------------------- */
/* 3. contract mapping and identity separation                        */
/* ---------------------------------------------------------------- */

static void contract_mapping(void)
{
    ForgeSemanticResultContract c, c2;
    ForgeRealizationForm f, f2;
    uint8_t cd[32], cd2[32], fd[32], fd2[32], cbytes[1024];
    size_t n = 0;
    fixture_contract(&c, FORGE_V2_ERROR_EXACT, FORGE_ASIM_ORACLE_EXACT_U64, 0, 8, 8);
    check("map_matvec_u64_to_linear_operator", forge_form_from_contract(&c, &f) == FORGE_FORM_OK &&
                                                   f.form_kind == FORGE_FORM_LINEAR_OPERATOR &&
                                                   f.scalar_domain == FORGE_FORM_DOMAIN_INT_WRAP_U64 &&
                                                   f.operator_rows == 8 && f.operator_cols == 8);
    forge_v2_digest_contract(&c, cd);
    forge_form_digest(&f, fd);
    /* the form digest is not an input of the contract digest */
    forge_v2_encode_contract(&c, cbytes, sizeof cbytes, &n);
    { int found = 0; for (size_t i = 0; i + 32 <= n; i++) if (memcmp(cbytes + i, fd, 32) == 0) found = 1;
      check("form_digest_not_in_contract_bytes", !found); }
    forge_v2_digest_contract(&c, cd2);
    check("contract_digest_unchanged_by_form_derivation", memcmp(cd, cd2, 32) == 0);
    /* a weaker contract (bounded) on the same operation maps to the SAME form */
    fixture_contract(&c2, FORGE_V2_ERROR_BOUNDED_DETERMINISTIC, FORGE_ASIM_ORACLE_EXACT_U64, 3, 8, 8);
    forge_v2_digest_contract(&c2, cd2);
    check("bounded_contract_has_different_contract_digest", memcmp(cd, cd2, 32) != 0);
    check("bounded_contract_maps_to_same_form", forge_form_from_contract(&c2, &f2) == FORGE_FORM_OK &&
                                                   forge_form_digest(&f2, fd2) == FORGE_FORM_OK &&
                                                   memcmp(fd, fd2, 32) == 0);
    /* a different oracle domain maps to a different form */
    fixture_contract(&c2, FORGE_V2_ERROR_EXACT, FORGE_ASIM_ORACLE_I32, 0, 8, 8);
    check("i32_oracle_maps_to_u32_wrap_form", forge_form_from_contract(&c2, &f2) == FORGE_FORM_OK &&
                                                 f2.scalar_domain == FORGE_FORM_DOMAIN_INT_WRAP_U32 &&
                                                 forge_form_digest(&f2, fd2) == FORGE_FORM_OK &&
                                                 memcmp(fd, fd2, 32) != 0);
    /* unmappable contracts */
    fixture_contract(&c2, FORGE_V2_ERROR_EXACT, FORGE_ASIM_ORACLE_EXACT_U64, 0, 8, 8);
    c2.operation_family = FORGE_V2_OP_MATMUL;
    check("map_non_matvec_refused", forge_form_from_contract(&c2, &f2) == FORGE_FORM_ERR_CONTRACT);
    fixture_contract(&c2, FORGE_V2_ERROR_EXACT, FORGE_ASIM_ORACLE_EXACT_U64, 0, 8, 8);
    c2.shape_rank = 1; c2.shape_dim[1] = 0;
    check("map_rank1_refused", forge_form_from_contract(&c2, &f2) == FORGE_FORM_ERR_CONTRACT);
    fixture_contract(&c2, FORGE_V2_ERROR_EXACT, FORGE_ASIM_ORACLE_EXACT_U64, 0, 8, 8);
    memset(c2.reference_oracle_digest, 0x77, 32);
    check("map_unknown_oracle_refused", forge_form_from_contract(&c2, &f2) == FORGE_FORM_ERR_CONTRACT);
    /* the form carries no substrate: two lowerings share one form digest */
    {
        ForgeFormLowering dl, al;
        ForgeAnalogSimModel ideal = { 0 };
        ForgeCalibrationArtifact cal;
        fixture_cal(&cal);
        check("lower_digital_ok", forge_form_lower_digital(&f, &dl) == FORGE_FORM_OK);
        check("lower_analog_sim_ok", forge_form_lower_analog_sim(&f, &ideal, &ideal, &cal, &al) == FORGE_FORM_OK);
        check("lowerings_share_form_digest", memcmp(dl.form_digest, al.form_digest, 32) == 0 &&
                                                 memcmp(dl.form_digest, fd, 32) == 0);
        check("lowerings_differ_in_substrate_and_lowering_digest",
              memcmp(dl.substrate_digest, al.substrate_digest, 32) != 0 &&
              memcmp(dl.lowering_digest, al.lowering_digest, 32) != 0);
        { uint8_t ld[32];
          forge_form_lowering_digest(fd, dl.code_digest, ld);
          check("lowering_digest_is_H_form_code", memcmp(ld, dl.lowering_digest, 32) == 0); }
        check("lowerings_substrate_classes_digital_vs_analog",
              dl.substrate.substrate_class == FORGE_V2_SUBSTRATE_DIGITAL_CPU &&
              al.substrate.substrate_class == FORGE_V2_SUBSTRATE_ANALOG_IN_MEMORY);
    }
}

/* ---------------------------------------------------------------- */
/* 4. two realizations of one contract                                */
/* ---------------------------------------------------------------- */

static uint8_t g_proof_contract[32], g_proof_form[32], g_proof_rid_d[32], g_proof_rid_a[32],
               g_proof_ev_d[32], g_proof_ev_a[32];
static int g_proof_ok;

static void two_realizations(void)
{
    ForgeSemanticResultContract c;
    ForgeRealizationForm f;
    ForgeFormLowering dl, al, bad;
    ForgeFormInput in;
    ForgeAnalogSimModel ideal = { 0 }, biased = { 0 }, noisy = { 0 };
    ForgeCalibrationArtifact cal;
    const ForgeFormOutcome *od, *oa;
    size_t nel = 0;
    uint8_t cd[32];

    fixture_cal(&cal);
    biased.bias_lsb = 2;
    noisy.noise_peak_lsb = 2; noisy.noise_seed = 42;

    /* EXACT contract, ideal simulated analog: both realizations publish */
    fixture_contract(&c, FORGE_V2_ERROR_EXACT, FORGE_ASIM_ORACLE_EXACT_U64, 0, 8, 8);
    fixture_input(&in, 8, 8);
    forge_form_from_contract(&c, &f);
    forge_form_lower_digital(&f, &dl);
    forge_form_lower_analog_sim(&f, &ideal, &ideal, &cal, &al);
    { const ForgeFormLowering *set[2] = { &al, &dl };
      check("admit_two_lowerings_with_digital", forge_form_admit(set, 2, &c, &in, &nel) == FORGE_FORM_OK && nel == 2);
      check("admit_analog_only_refused_no_fallback", forge_form_admit(set, 1, &c, &in, &nel) == FORGE_FORM_ERR_NO_FALLBACK && nel == 1); }
    od = run("exact_digital_reference_published", &f, &c, &dl, &in);
    check("exact_digital_published", od->status == FORGE_FORM_PUBLISHED && od->published && od->error_abs == 0);
    memcpy(g_proof_rid_d, od->evidence.realization_identity, 32);
    memcpy(g_proof_ev_d, od->evidence_digest, 32);
    { uint64_t ref[64]; forge_asim_oracle_exact_u64(in.A, in.x, ref, 8, 8);
      check("exact_digital_output_equals_oracle", memcmp(od->y, ref, 8 * sizeof ref[0]) == 0); }
    { uint8_t out_d[32]; memcpy(out_d, od->evidence.output_identity, 32);
      oa = run("exact_analog_sim_ideal_published", &f, &c, &al, &in);
      check("exact_analog_published", oa->status == FORGE_FORM_PUBLISHED && oa->published && oa->error_abs == 0);
      check("exact_same_output_identity", memcmp(out_d, oa->evidence.output_identity, 32) == 0); }
    memcpy(g_proof_rid_a, oa->evidence.realization_identity, 32);
    memcpy(g_proof_ev_a, oa->evidence_digest, 32);
    forge_v2_digest_contract(&c, cd);
    memcpy(g_proof_contract, cd, 32);
    memcpy(g_proof_form, dl.form_digest, 32);
    g_proof_ok = memcmp(g_proof_rid_d, g_proof_rid_a, 32) != 0 && memcmp(g_proof_ev_d, g_proof_ev_a, 32) != 0 &&
                 memcmp(od->evidence.semantic_contract_digest, cd, 32) == 0 &&
                 memcmp(oa->evidence.semantic_contract_digest, cd, 32) == 0 &&
                 memcmp(od->record.form_digest, oa->record.form_digest, 32) == 0 &&
                 memcmp(od->record.lowering_digest, oa->record.lowering_digest, 32) != 0 &&
                 memcmp(od->record.substrate_digest, oa->record.substrate_digest, 32) != 0 &&
                 memcmp(od->evidence.substrate_identity, oa->evidence.substrate_identity, 32) != 0 &&
                 od->evidence.has_calibration == 0 && oa->evidence.has_calibration == 1;
    check("proof_same_contract_digest_in_both_realizations",
          memcmp(od->evidence.semantic_contract_digest, oa->evidence.semantic_contract_digest, 32) == 0);
    check("proof_same_form_digest_in_both_records", memcmp(od->record.form_digest, oa->record.form_digest, 32) == 0);
    check("proof_different_realization_identities", memcmp(g_proof_rid_d, g_proof_rid_a, 32) != 0);
    check("proof_different_evidence_identities", memcmp(g_proof_ev_d, g_proof_ev_a, 32) != 0);
    check("proof_different_record_digests", memcmp(od->record_digest, oa->record_digest, 32) != 0);
    check("proof_two_realizations_same_contract", g_proof_ok);
    check("proof_both_simulated_development", od->evidence.provenance_class == forge_form_provenance() &&
                                                  oa->evidence.provenance_class == forge_form_provenance() &&
                                                  forge_form_provenance() == FORGE_V2_PROVENANCE_SIMULATED_DEVELOPMENT);

    /* BOUNDED_DETERMINISTIC: a biased simulated analog publishes a DIFFERENT
     * output that still satisfies the same contract as the digital reference */
    fixture_contract(&c, FORGE_V2_ERROR_BOUNDED_DETERMINISTIC, FORGE_ASIM_ORACLE_EXACT_U64, 3, 8, 8);
    forge_form_from_contract(&c, &f);
    forge_form_lower_digital(&f, &dl);
    forge_form_lower_analog_sim(&f, &biased, &biased, &cal, &al);
    od = run("bounded_digital_reference_published", &f, &c, &dl, &in);
    check("bounded_digital_zero_error", od->status == FORGE_FORM_PUBLISHED && od->error_abs == 0);
    { uint8_t out_d[32]; memcpy(out_d, od->evidence.output_identity, 32);
      oa = run("bounded_analog_sim_bias2_published", &f, &c, &al, &in);
      check("bounded_analog_published_with_error_2", oa->status == FORGE_FORM_PUBLISHED && oa->error_abs == 2 &&
                                                         oa->runs_within_bound == oa->runs);
      check("bounded_outputs_differ_same_contract", memcmp(out_d, oa->evidence.output_identity, 32) != 0 &&
                                                        memcmp(od->evidence.semantic_contract_digest,
                                                               oa->evidence.semantic_contract_digest, 32) == 0); }

    /* over the bound: declared bias 2 (eligible) but actual bias 5: analog
     * refused with evidence, digital answers */
    { ForgeAnalogSimModel worse = biased; worse.bias_lsb = 5;
      forge_form_lower_analog_sim(&f, &biased, &worse, &cal, &al); }
    oa = run("bounded_analog_sim_bias5_refused", &f, &c, &al, &in);
    check("over_bound_refused_error_bound", oa->status == FORGE_FORM_REFUSED && !oa->published &&
                                                oa->refuse_reason == FORGE_ASIM_REFUSE_ERROR_BOUND && oa->ran &&
                                                oa->record.decision == FORGE_FORM_DECISION_REFUSED);
    { int zero = 1; for (int i = 0; i < 64; i++) if (oa->y[i]) zero = 0; check("refused_publishes_nothing", zero); }
    od = run("bounded_digital_fallback_published", &f, &c, &dl, &in);
    check("fallback_digital_published_same_contract", od->status == FORGE_FORM_PUBLISHED &&
                                                          memcmp(od->evidence.semantic_contract_digest,
                                                                 oa->evidence.semantic_contract_digest, 32) == 0);

    /* declared ideal, actually biased: EXACT refused even if outputs matched */
    fixture_contract(&c, FORGE_V2_ERROR_EXACT, FORGE_ASIM_ORACLE_EXACT_U64, 0, 8, 8);
    forge_form_from_contract(&c, &f);
    forge_form_lower_analog_sim(&f, &ideal, &noisy, &cal, &al);
    oa = run("exact_analog_sim_hidden_noise_refused", &f, &c, &al, &in);
    check("exact_hidden_noise_refused", oa->status == FORGE_FORM_REFUSED &&
                                            oa->refuse_reason == FORGE_ASIM_REFUSE_EXACT_MISMATCH);

    /* BOUNDED_STOCHASTIC with noise within bound publishes */
    fixture_contract(&c, FORGE_V2_ERROR_BOUNDED_STOCHASTIC, FORGE_ASIM_ORACLE_EXACT_U64, 3, 8, 8);
    forge_form_from_contract(&c, &f);
    forge_form_lower_analog_sim(&f, &noisy, &noisy, &cal, &al);
    oa = run("stochastic_analog_sim_noise2_published", &f, &c, &al, &in);
    check("stochastic_published_within_confidence", oa->status == FORGE_FORM_PUBLISHED && oa->runs == 16 &&
                                                        oa->runs_within_bound == 16);
    forge_form_lower_digital(&f, &dl);
    od = run("stochastic_digital_reference_published", &f, &c, &dl, &in);
    check("stochastic_digital_published", od->status == FORGE_FORM_PUBLISHED && od->runs == 16);

    /* stale calibration: ineligible, never runs, no evidence */
    fixture_contract(&c, FORGE_V2_ERROR_BOUNDED_DETERMINISTIC, FORGE_ASIM_ORACLE_EXACT_U64, 3, 8, 8);
    forge_form_from_contract(&c, &f);
    biased.bias_lsb = 2;
    forge_form_lower_analog_sim(&f, &biased, &biased, &cal, &al);
    in.now.execution_epoch_s = EPOCH0 + WINDOW + 1;
    oa = run("stale_calibration_ineligible", &f, &c, &al, &in);
    check("stale_cal_not_run", oa->status == FORGE_FORM_REFUSED && oa->refuse_reason == FORGE_ASIM_REFUSE_INELIGIBLE &&
                                   !oa->ran && oa->eligibility_rc == FORGE_V2_ERR_CAL_STALE);
    in.now.execution_epoch_s = EPOCH0 + 3600;

    /* the digital reference is also eligible with a stale analog calibration (fallback stays) */
    forge_form_lower_digital(&f, &dl);
    od = run("digital_reference_unaffected_by_analog_calibration", &f, &c, &dl, &in);
    check("digital_published_regardless", od->status == FORGE_FORM_PUBLISHED);

    /* form / contract mismatch is refused before anything runs */
    { ForgeRealizationForm wrong = f; ForgeFormOutcome o; wrong.operator_rows = 4;
      forge_form_lower_digital(&wrong, &bad);
      check("mismatched_form_refused_err_contract",
            forge_form_realize(&wrong, &c, &bad, &in, &o) == FORGE_FORM_ERR_CONTRACT && !o.ran);
      check("lowering_of_other_form_refused",
            forge_form_realize(&f, &c, &bad, &in, &o) == FORGE_FORM_ERR_CONTRACT && !o.ran); }
    /* input outside the lowering's declared range */
    { ForgeFormOutcome o; uint64_t saved = g_A[0]; g_A[0] = 0x1FF;
      check("input_out_of_range_refused", forge_form_realize(&f, &c, &dl, &in, &o) == FORGE_FORM_ERR_INPUT_RANGE);
      g_A[0] = saved; }
    /* i32 oracle domain through both lowerings */
    fixture_contract(&c, FORGE_V2_ERROR_EXACT, FORGE_ASIM_ORACLE_I32, 0, 5, 7);
    fixture_input(&in, 5, 7);
    forge_form_from_contract(&c, &f);
    forge_form_lower_digital(&f, &dl);
    forge_form_lower_analog_sim(&f, &ideal, &ideal, &cal, &al);
    od = run("i32_digital_reference_published", &f, &c, &dl, &in);
    oa = run("i32_analog_sim_ideal_published", &f, &c, &al, &in);
    check("i32_both_published_same_contract_different_realizations",
          od->status == FORGE_FORM_PUBLISHED && oa->status == FORGE_FORM_PUBLISHED &&
          memcmp(od->evidence.semantic_contract_digest, oa->evidence.semantic_contract_digest, 32) == 0 &&
          memcmp(od->evidence.realization_identity, oa->evidence.realization_identity, 32) != 0);
}

/* ---------------------------------------------------------------- */
/* receipt                                                            */
/* ---------------------------------------------------------------- */

static void print_receipt(void)
{
    printf("{\n");
    printf("  \"gate\": \"FORM0_LINEAR_OPERATOR_HOST\",\n");
    printf("  \"workstream\": \"ARCH-0018 / ARCH-0019 extension; ADR 0034 Physical-Form Realization IR (PROPOSED)\",\n");
    printf("  \"provenance_class\": \"SIMULATED_DEVELOPMENT\",\n");
    printf("  \"note\": \"Host-only reference. No device acted. The digital reference ran on the host CPU as a "
           "development fixture; the analog lowering is the AR2 software model. Errors are simulated or host "
           "errors against the digital oracle.\",\n");
    printf("  \"status\": \"%s\",\n", g_fail == 0 ? "PASS" : "FAIL");
    printf("  \"checks_passed\": %d,\n", g_pass);
    printf("  \"checks_failed\": %d,\n", g_fail);
    printf("  \"evidence_records\": %d,\n", g_records);
    printf("  \"evidence_records_simulated_development\": %d,\n", g_records_sim);
    printf("  \"proof_two_realizations_same_contract\": \"%s\",\n", g_proof_ok ? "PASS" : "FAIL");
    printf("  \"proof\": {\"contract_digest\": \""); hex(g_proof_contract);
    printf("\", \"form_digest\": \""); hex(g_proof_form);
    printf("\", \"digital_reference\": {\"realization_identity\": \""); hex(g_proof_rid_d);
    printf("\", \"evidence_digest\": \""); hex(g_proof_ev_d);
    printf("\"}, \"analog_sim\": {\"realization_identity\": \""); hex(g_proof_rid_a);
    printf("\", \"evidence_digest\": \""); hex(g_proof_ev_a);
    printf("\"}},\n");
    printf("  \"cases\": [\n");
    for (int i = 0; i < g_ncases; i++) {
        const CaseRow *r = &g_cases[i];
        printf("    {\"name\": \"%s\", \"backend\": \"%s\", \"contract_kind\": \"%s\", \"outcome\": \"%s\", "
               "\"refusal\": \"%s\", \"ran\": %u, \"runs\": %u, \"runs_within_bound\": %u, "
               "\"error_vs_oracle_abs\": %llu, ",
               r->name, backend_name(r->backend), kind_name(r->contract_kind), status_name(r->status),
               forge_asim_refuse_name(r->refuse), r->ran, r->runs, r->within, (unsigned long long)r->error_abs);
        printf("\"contract_digest\": \""); hex(r->contract);
        printf("\", \"form_digest\": \""); hex(r->form);
        printf("\", \"lowering_digest\": \""); hex(r->lowering);
        printf("\", \"realization_identity\": \""); if (r->ran) hex(r->realization);
        printf("\", \"evidence_digest\": \""); if (r->ran) hex(r->evidence);
        printf("\", \"record_digest\": \""); if (r->ran) hex(r->record);
        printf("\", \"provenance_class\": \"SIMULATED_DEVELOPMENT\"}%s\n", i + 1 < g_ncases ? "," : "");
    }
    printf("  ]\n}\n");
}

int main(int argc, char **argv)
{
    int receipt = argc > 1 && strcmp(argv[1], "--receipt") == 0;
    g_quiet = receipt;
    canonical_form();
    negative_decode();
    contract_mapping();
    two_realizations();
    if (receipt) print_receipt();
    else printf("RESULT checks: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
