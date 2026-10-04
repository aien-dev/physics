/*
 * forge/form/forge_form.c -- FORGE REALIZATION FORM V1 reference: canonical
 * encoding, digests, contract mapping, two lowerings (digital reference,
 * simulated analog) and realization with V2 evidence. Host-only: no
 * hardware, no heap, no I/O. See forge_form.h.
 */
#include "forge_form.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* primitives                                                          */
/* ------------------------------------------------------------------ */

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
static uint64_t sat_add(uint64_t a, uint64_t b) { return a > UINT64_MAX - b ? UINT64_MAX : a + b; }
static uint64_t mul_div_ceil(uint64_t a, uint64_t b, uint64_t d)
{
    unsigned __int128 p = (unsigned __int128)a * b;
    unsigned __int128 q = (p + d - 1) / d;
    return q > UINT64_MAX ? UINT64_MAX : (uint64_t)q;
}
static void label_digest(const char *label, uint8_t out[32])
{
    forge_v2_sha256((const uint8_t *)label, strlen(label), out);
}

/* ------------------------------------------------------------------ */
/* schema: every field required, fixed width, ascending tags           */
/* ------------------------------------------------------------------ */

enum { T_U32 = 1, T_D32 = 2 };
typedef struct { uint32_t tag; uint8_t type; size_t off; } Field;

#define FORM_NFIELDS 5u
static const Field FORM_FIELDS[FORM_NFIELDS] = {
    { 1, T_U32, offsetof(ForgeRealizationForm, form_kind) },
    { 2, T_U32, offsetof(ForgeRealizationForm, scalar_domain) },
    { 3, T_U32, offsetof(ForgeRealizationForm, operator_rows) },
    { 4, T_U32, offsetof(ForgeRealizationForm, operator_cols) },
    { 5, T_U32, offsetof(ForgeRealizationForm, accumulation) },
};

#define RECORD_NFIELDS 8u
static const Field RECORD_FIELDS[RECORD_NFIELDS] = {
    { 1, T_D32, offsetof(ForgeFormRealizationRecord, form_digest) },
    { 2, T_D32, offsetof(ForgeFormRealizationRecord, contract_digest) },
    { 3, T_D32, offsetof(ForgeFormRealizationRecord, substrate_digest) },
    { 4, T_D32, offsetof(ForgeFormRealizationRecord, lowering_digest) },
    { 5, T_D32, offsetof(ForgeFormRealizationRecord, realization_identity) },
    { 6, T_D32, offsetof(ForgeFormRealizationRecord, evidence_digest) },
    { 7, T_U32, offsetof(ForgeFormRealizationRecord, provenance_class) },
    { 8, T_U32, offsetof(ForgeFormRealizationRecord, decision) },
};

static size_t field_width(uint8_t type) { return type == T_U32 ? 4u : 32u; }

static int is_zero32(const uint8_t *p)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < 32; i++) acc |= p[i];
    return acc == 0;
}

/* ---- validation ---- */

int forge_form_validate(const ForgeRealizationForm *f)
{
    if (!f) return FORGE_FORM_ERR_ARG;
    switch (f->form_kind) {
    case FORGE_FORM_LINEAR_OPERATOR: break;
    case FORGE_FORM_ENERGY_FUNCTIONAL:
    case FORGE_FORM_STOCHASTIC_DISTRIBUTION: return FORGE_FORM_ERR_UNSUPPORTED_FORM;
    default: return FORGE_FORM_ERR_VALUE;
    }
    if (f->scalar_domain != FORGE_FORM_DOMAIN_INT_WRAP_U64 && f->scalar_domain != FORGE_FORM_DOMAIN_INT_WRAP_U32)
        return FORGE_FORM_ERR_VALUE;
    if (f->operator_rows == 0 || f->operator_rows > FORGE_FORM_MAX_DIM) return FORGE_FORM_ERR_VALUE;
    if (f->operator_cols == 0 || f->operator_cols > FORGE_FORM_MAX_DIM) return FORGE_FORM_ERR_VALUE;
    if (f->accumulation != FORGE_FORM_ACCUM_EXACT_MODULAR) return FORGE_FORM_ERR_VALUE;
    return FORGE_FORM_OK;
}

static int record_validate(const ForgeFormRealizationRecord *r)
{
    if (!r) return FORGE_FORM_ERR_ARG;
    if (is_zero32(r->form_digest) || is_zero32(r->contract_digest) || is_zero32(r->substrate_digest) ||
        is_zero32(r->lowering_digest) || is_zero32(r->realization_identity) || is_zero32(r->evidence_digest))
        return FORGE_FORM_ERR_VALUE;
    if (r->provenance_class != FORGE_V2_PROVENANCE_SIMULATED_DEVELOPMENT) return FORGE_FORM_ERR_VALUE;
    if (r->decision != FORGE_FORM_DECISION_PUBLISHED && r->decision != FORGE_FORM_DECISION_REFUSED)
        return FORGE_FORM_ERR_VALUE;
    return FORGE_FORM_OK;
}

/* ---- generic encode / decode ---- */

static int enc_obj(uint16_t kind, const Field *fields, size_t nf, const void *obj, uint8_t *buf, size_t cap,
                   size_t *out_len)
{
    size_t n = FORGE_FORM_HEADER_LEN;
    const uint8_t *base = (const uint8_t *)obj;
    if (!buf || !out_len) return FORGE_FORM_ERR_ARG;
    for (size_t i = 0; i < nf; i++) {
        size_t w = field_width(fields[i].type);
        if (n + FORGE_FORM_TLV_HEAD_LEN + w > cap) return FORGE_FORM_ERR_SPACE;
        put_u32(buf + n, fields[i].tag);
        put_u32(buf + n + 4, (uint32_t)w);
        if (fields[i].type == T_U32) {
            uint32_t v;
            memcpy(&v, base + fields[i].off, sizeof v);
            put_u32(buf + n + 8, v);
        } else {
            memcpy(buf + n + 8, base + fields[i].off, 32);
        }
        n += FORGE_FORM_TLV_HEAD_LEN + w;
    }
    if (cap < FORGE_FORM_HEADER_LEN) return FORGE_FORM_ERR_SPACE;
    buf[0] = FORGE_FORM_MAGIC0; buf[1] = FORGE_FORM_MAGIC1; buf[2] = FORGE_FORM_MAGIC2; buf[3] = FORGE_FORM_MAGIC3;
    put_u16(buf + 4, kind);
    put_u16(buf + 6, FORGE_FORM_WIRE_VERSION);
    put_u32(buf + 8, (uint32_t)(n - FORGE_FORM_HEADER_LEN));
    *out_len = n;
    return FORGE_FORM_OK;
}

static int dec_obj(uint16_t kind, const Field *fields, size_t nf, const uint8_t *buf, size_t len, int strict,
                   void *obj, size_t obj_size)
{
    uint8_t *base = (uint8_t *)obj;
    uint32_t prev_tag = 0;
    uint8_t present[16] = { 0 };
    size_t n = FORGE_FORM_HEADER_LEN;
    if (!buf || !obj) return FORGE_FORM_ERR_ARG;
    memset(obj, 0, obj_size);
    if (len < FORGE_FORM_HEADER_LEN) return FORGE_FORM_ERR_TRUNCATED;
    if (len > FORGE_FORM_MAX_OBJECT_LEN) return FORGE_FORM_ERR_LENGTH;
    if (buf[0] != FORGE_FORM_MAGIC0 || buf[1] != FORGE_FORM_MAGIC1 || buf[2] != FORGE_FORM_MAGIC2 ||
        buf[3] != FORGE_FORM_MAGIC3)
        return FORGE_FORM_ERR_MAGIC;
    if (get_u16(buf + 4) != kind) return FORGE_FORM_ERR_KIND;
    if (get_u16(buf + 6) != FORGE_FORM_WIRE_VERSION) return FORGE_FORM_ERR_VERSION;
    if ((size_t)get_u32(buf + 8) != len - FORGE_FORM_HEADER_LEN) return FORGE_FORM_ERR_LENGTH;
    while (n < len) {
        uint32_t tag, flen;
        size_t i;
        if (len - n < FORGE_FORM_TLV_HEAD_LEN) return FORGE_FORM_ERR_TRUNCATED;
        tag = get_u32(buf + n);
        flen = get_u32(buf + n + 4);
        if (tag <= prev_tag) return FORGE_FORM_ERR_ORDER;   /* duplicate or descending */
        prev_tag = tag;
        n += FORGE_FORM_TLV_HEAD_LEN;
        if (len - n < flen) return FORGE_FORM_ERR_TRUNCATED;
        for (i = 0; i < nf; i++) if (fields[i].tag == tag) break;
        if (i == nf) {
            if (strict) return FORGE_FORM_ERR_UNKNOWN_TAG;
            n += flen;
            continue;
        }
        if (flen != field_width(fields[i].type)) return FORGE_FORM_ERR_WIDTH;
        if (fields[i].type == T_U32) {
            uint32_t v = get_u32(buf + n);
            memcpy(base + fields[i].off, &v, sizeof v);
        } else {
            memcpy(base + fields[i].off, buf + n, 32);
        }
        present[i] = 1;
        n += flen;
    }
    for (size_t i = 0; i < nf; i++) if (!present[i]) return FORGE_FORM_ERR_MISSING;
    return FORGE_FORM_OK;
}

static int dec_public(uint16_t kind, const Field *fields, size_t nf, const uint8_t *buf, size_t len, int strict,
                      void *obj, size_t obj_size, int (*validate)(const void *))
{
    int rc = dec_obj(kind, fields, nf, buf, len, strict, obj, obj_size);
    if (rc != FORGE_FORM_OK) { if (obj) memset(obj, 0, obj_size); return rc; }
    rc = validate(obj);
    if (rc != FORGE_FORM_OK) { memset(obj, 0, obj_size); return rc; }
    if (strict) {
        uint8_t again[FORGE_FORM_MAX_OBJECT_LEN];
        size_t m = 0;
        rc = enc_obj(kind, fields, nf, obj, again, sizeof again, &m);
        if (rc != FORGE_FORM_OK || m != len || memcmp(again, buf, len) != 0) {
            memset(obj, 0, obj_size);
            return FORGE_FORM_ERR_NONCANONICAL;
        }
    }
    return FORGE_FORM_OK;
}

static int validate_form_v(const void *o) { return forge_form_validate((const ForgeRealizationForm *)o); }
static int validate_record_v(const void *o) { return record_validate((const ForgeFormRealizationRecord *)o); }

int forge_form_encode(const ForgeRealizationForm *f, uint8_t *buf, size_t cap, size_t *out_len)
{
    int rc = forge_form_validate(f);
    if (rc != FORGE_FORM_OK) return rc;
    return enc_obj(FORGE_FORM_KIND_FORM, FORM_FIELDS, FORM_NFIELDS, f, buf, cap, out_len);
}

int forge_form_decode(const uint8_t *buf, size_t len, int strict, ForgeRealizationForm *f)
{
    return dec_public(FORGE_FORM_KIND_FORM, FORM_FIELDS, FORM_NFIELDS, buf, len, strict, f, sizeof *f,
                      validate_form_v);
}

int forge_form_digest(const ForgeRealizationForm *f, uint8_t out[32])
{
    uint8_t buf[256];
    size_t n = 0;
    int rc = forge_form_encode(f, buf, sizeof buf, &n);
    if (rc != FORGE_FORM_OK) return rc;
    forge_v2_sha256(buf, n, out);
    return FORGE_FORM_OK;
}

int forge_form_encode_record(const ForgeFormRealizationRecord *r, uint8_t *buf, size_t cap, size_t *out_len)
{
    int rc = record_validate(r);
    if (rc != FORGE_FORM_OK) return rc;
    return enc_obj(FORGE_FORM_KIND_REALIZATION_RECORD, RECORD_FIELDS, RECORD_NFIELDS, r, buf, cap, out_len);
}

int forge_form_decode_record(const uint8_t *buf, size_t len, int strict, ForgeFormRealizationRecord *r)
{
    return dec_public(FORGE_FORM_KIND_REALIZATION_RECORD, RECORD_FIELDS, RECORD_NFIELDS, buf, len, strict, r,
                      sizeof *r, validate_record_v);
}

int forge_form_digest_record(const ForgeFormRealizationRecord *r, uint8_t out[32])
{
    uint8_t buf[512];
    size_t n = 0;
    int rc = forge_form_encode_record(r, buf, sizeof buf, &n);
    if (rc != FORGE_FORM_OK) return rc;
    forge_v2_sha256(buf, n, out);
    return FORGE_FORM_OK;
}

void forge_form_lowering_digest(const uint8_t form_digest[32], const uint8_t code_digest[32], uint8_t out[32])
{
    static const char domain[19] = { 'F','O','R','G','E','-','F','O','R','M','-','L','O','W','E','R','-','V','1' };
    uint8_t in[19 + 64];
    memcpy(in, domain, 19);
    memcpy(in + 19, form_digest, 32);
    memcpy(in + 51, code_digest, 32);
    forge_v2_sha256(in, sizeof in, out);
}

/* ------------------------------------------------------------------ */
/* contract mapping                                                    */
/* ------------------------------------------------------------------ */

static uint32_t oracle_of(const uint8_t digest[32])
{
    uint8_t d[32];
    if (forge_asim_oracle_digest(FORGE_ASIM_ORACLE_EXACT_U64, d) == FORGE_V2_OK && memcmp(d, digest, 32) == 0)
        return FORGE_ASIM_ORACLE_EXACT_U64;
    if (forge_asim_oracle_digest(FORGE_ASIM_ORACLE_I32, d) == FORGE_V2_OK && memcmp(d, digest, 32) == 0)
        return FORGE_ASIM_ORACLE_I32;
    return 0;
}

int forge_form_from_contract(const ForgeSemanticResultContract *c, ForgeRealizationForm *out)
{
    uint32_t oracle;
    if (!c || !out) return FORGE_FORM_ERR_ARG;
    memset(out, 0, sizeof *out);
    if (c->operation_family != FORGE_V2_OP_MATVEC) return FORGE_FORM_ERR_CONTRACT;
    if (c->shape_rank != 2 || c->shape_dim[0] == 0 || c->shape_dim[1] == 0 || c->shape_dim[2] || c->shape_dim[3])
        return FORGE_FORM_ERR_CONTRACT;
    oracle = oracle_of(c->reference_oracle_digest);
    if (!oracle) return FORGE_FORM_ERR_CONTRACT;
    out->form_kind = FORGE_FORM_LINEAR_OPERATOR;
    out->scalar_domain = oracle == FORGE_ASIM_ORACLE_EXACT_U64 ? FORGE_FORM_DOMAIN_INT_WRAP_U64
                                                               : FORGE_FORM_DOMAIN_INT_WRAP_U32;
    out->operator_rows = c->shape_dim[0];
    out->operator_cols = c->shape_dim[1];
    out->accumulation = FORGE_FORM_ACCUM_EXACT_MODULAR;
    return forge_form_validate(out);
}

/* ------------------------------------------------------------------ */
/* lowerings                                                           */
/* ------------------------------------------------------------------ */

static const char *const DIGITAL_CODE_LABEL = "FORGE-FORM/lowering/linear-operator/digital-reference/v1";
static const char *const ANALOG_CODE_LABEL = "FORGE-FORM/lowering/linear-operator/analog-sim/v1";

static int finish_lowering(const ForgeRealizationForm *f, ForgeFormLowering *out)
{
    if (forge_form_digest(f, out->form_digest) != FORGE_FORM_OK) return FORGE_FORM_ERR_ARG;
    if (forge_v2_digest_substrate(&out->substrate, out->substrate_digest) != FORGE_V2_OK)
        return FORGE_FORM_ERR_EVIDENCE;
    forge_form_lowering_digest(out->form_digest, out->code_digest, out->lowering_digest);
    return FORGE_FORM_OK;
}

int forge_form_lower_digital(const ForgeRealizationForm *f, ForgeFormLowering *out)
{
    int rc;
    if (!out) return FORGE_FORM_ERR_ARG;
    memset(out, 0, sizeof *out);
    if ((rc = forge_form_validate(f)) != FORGE_FORM_OK) return rc;
    out->backend = FORGE_FORM_BACKEND_DIGITAL_REFERENCE;
    forge_asim_digital_descriptor(&out->substrate);
    label_digest(DIGITAL_CODE_LABEL, out->code_digest);
    return finish_lowering(f, out);
}

int forge_form_lower_analog_sim(const ForgeRealizationForm *f, const ForgeAnalogSimModel *declared,
                                const ForgeAnalogSimModel *actual, const ForgeCalibrationArtifact *cal,
                                ForgeFormLowering *out)
{
    int rc;
    if (!out) return FORGE_FORM_ERR_ARG;
    memset(out, 0, sizeof *out);
    if (!declared || !actual || !cal) return FORGE_FORM_ERR_ARG;
    if ((rc = forge_form_validate(f)) != FORGE_FORM_OK) return rc;
    out->backend = FORGE_FORM_BACKEND_ANALOG_SIM;
    out->declared = *declared;
    out->actual = *actual;
    out->cal = *cal;
    out->has_calibration = 1;
    if (forge_asim_describe(declared, cal, &out->substrate) != FORGE_V2_OK) return FORGE_FORM_ERR_ARG;
    if (forge_v2_digest_calibration(cal, out->cal_digest) != FORGE_V2_OK) return FORGE_FORM_ERR_ARG;
    label_digest(ANALOG_CODE_LABEL, out->code_digest);
    return finish_lowering(f, out);
}

/* ------------------------------------------------------------------ */
/* admission and realization                                           */
/* ------------------------------------------------------------------ */

uint32_t forge_form_provenance(void) { return FORGE_V2_PROVENANCE_SIMULATED_DEVELOPMENT; }

static int eligible(const ForgeFormLowering *lw, const ForgeSemanticResultContract *c, const ForgeV2Conditions *now)
{
    return forge_v2_substrate_eligible(&lw->substrate, c, lw->has_calibration ? &lw->cal : NULL,
                                       FORGE_V2_FAULT_NONE, now);
}

int forge_form_admit(const ForgeFormLowering *const *lw, size_t n, const ForgeSemanticResultContract *c,
                     const ForgeFormInput *in, size_t *n_eligible)
{
    ForgeV2Candidate set[8];
    uint8_t contract_digest[32];
    size_t nset = 0;
    if (!lw || !c || !in || n > 8) return FORGE_FORM_ERR_ARG;
    if (forge_v2_digest_contract(c, contract_digest) != FORGE_V2_OK) return FORGE_FORM_ERR_CONTRACT;
    for (size_t i = 0; i < n; i++) {
        if (!lw[i]) return FORGE_FORM_ERR_ARG;
        if (eligible(lw[i], c, &in->now) != FORGE_V2_OK) continue;
        memcpy(set[nset].contract_digest, contract_digest, 32);
        memcpy(set[nset].machine_identity, in->machine_identity, 32);
        memcpy(set[nset].substrate_digest, lw[i]->substrate_digest, 32);
        set[nset].substrate_class = lw[i]->substrate.substrate_class;
        nset++;
    }
    if (n_eligible) *n_eligible = nset;
    return forge_v2_admit_plan(set, nset, contract_digest) == FORGE_V2_OK ? FORGE_FORM_OK
                                                                          : FORGE_FORM_ERR_NO_FALLBACK;
}

static size_t put_u64_at(uint8_t *p, uint64_t v) { put_u64(p, v); return 8; }

static void vec_digest(const char *domain, const uint64_t *v, size_t n, uint8_t out[32])
{
    /* SHA-256(domain | n (u64) | v[0..n) ) via a fixed stack buffer */
    uint8_t buf[64 + 8 + 8 * FORGE_ASIM_MAX_COLS];
    size_t k = strlen(domain);
    memcpy(buf, domain, k);
    k += put_u64_at(buf + k, (uint64_t)n);
    for (size_t i = 0; i < n; i++) k += put_u64_at(buf + k, v[i]);
    forge_v2_sha256(buf, k, out);
}

static void input_digest(const uint64_t *A, const uint64_t *x, uint32_t M, uint32_t N, uint8_t out[32])
{
    uint8_t da[32], dx[32], in[19 + 8 + 64];
    /* A is hashed row by row to stay inside the fixed buffer */
    uint8_t rows[32 * FORGE_ASIM_MAX_ROWS];
    for (uint32_t i = 0; i < M; i++) vec_digest("FORGE-FORM/input/row/v1", A + (size_t)i * N, N, rows + 32 * i);
    forge_v2_sha256(rows, 32 * (size_t)M, da);
    vec_digest("FORGE-FORM/input/x/v1", x, N, dx);
    memcpy(in, "FORGE-FORM/input/v1", 19);
    put_u32(in + 19, M); put_u32(in + 23, N);
    memcpy(in + 27, da, 32); memcpy(in + 59, dx, 32);
    forge_v2_sha256(in, 91, out);
}

static int model_ideal(const ForgeAnalogSimModel *m)
{
    return m->quant_step_lsb <= 1 && m->bias_lsb == 0 && m->noise_peak_lsb == 0 && m->drift_lsb_per_hour == 0 &&
           m->temp_lsb_per_k == 0 && m->saturation_max == 0;
}

static int shape_ok(const ForgeFormInput *in)
{
    if (!in->A || !in->x || in->M == 0 || in->N == 0 || in->M > FORGE_ASIM_MAX_ROWS || in->N > FORGE_ASIM_MAX_COLS)
        return 0;
    for (size_t k = 0; k < (size_t)in->M * in->N; k++) if (in->A[k] >> FORGE_ASIM_INPUT_BITS) return 0;
    for (uint32_t j = 0; j < in->N; j++) if (in->x[j] >> FORGE_ASIM_INPUT_BITS) return 0;
    return 1;
}

static void compute_ref(uint32_t oracle, const ForgeFormInput *in, uint64_t *ref)
{
    if (oracle == FORGE_ASIM_ORACLE_EXACT_U64) {
        forge_asim_oracle_exact_u64(in->A, in->x, ref, in->M, in->N);
    } else {
        uint32_t a32[FORGE_ASIM_MAX_ROWS * FORGE_ASIM_MAX_COLS], x32[FORGE_ASIM_MAX_COLS], y32[FORGE_ASIM_MAX_ROWS];
        for (size_t k = 0; k < (size_t)in->M * in->N; k++) a32[k] = (uint32_t)in->A[k];
        for (uint32_t j = 0; j < in->N; j++) x32[j] = (uint32_t)in->x[j];
        forge_asim_oracle_i32(a32, x32, y32, in->M, in->N);
        for (uint32_t i = 0; i < in->M; i++) ref[i] = y32[i];
    }
}

/* One run of the lowering. Never consults the oracle. */
static int run_once(const ForgeFormLowering *lw, const ForgeFormInput *in, uint32_t r, uint32_t oracle,
                    uint64_t *y, uint64_t *latency_ns)
{
    if (lw->backend == FORGE_FORM_BACKEND_DIGITAL_REFERENCE) {
        compute_ref(oracle, in, y);   /* the digital reference IS the oracle's arithmetic */
        *latency_ns = 0;
        return FORGE_FORM_OK;
    }
    if (lw->backend == FORGE_FORM_BACKEND_ANALOG_SIM) {
        uint64_t cal_age = in->now.execution_epoch_s - lw->cal.epoch_s;   /* eligible => not negative */
        int32_t mid = (int32_t)(((int64_t)lw->cal.env_temp_min_mc + lw->cal.env_temp_max_mc) / 2);
        int32_t dtemp = in->now.temperature_mc - mid;
        int rc = forge_asim_realize(&lw->actual, lw->wrong_row, lw->wrong_delta, in->A, in->x, in->M, in->N, r,
                                    cal_age, dtemp, y, latency_ns);
        return rc == FORGE_ASIM_PUBLISHED_ANALOG ? FORGE_FORM_OK : FORGE_FORM_ERR_INPUT_RANGE;
    }
    return FORGE_FORM_ERR_ARG;
}

static int seal(ForgeFormOutcome *out, const ForgeFormLowering *lw, const uint8_t contract_digest[32],
                uint32_t decision)
{
    uint8_t buf[4096];
    size_t n;
    ForgeExecutionEvidenceV2 *e = &out->evidence;
    ForgeFormRealizationRecord *rec = &out->record;
    e->provenance_class = forge_form_provenance();
    e->energy_source = FORGE_V2_ENERGY_NOT_MEASURED;
    e->energy_nj = 0;
    if (forge_v2_encode_evidence(e, buf, sizeof buf, &n) != FORGE_V2_OK) return FORGE_FORM_ERR_EVIDENCE;
    forge_v2_sha256(buf, n, out->evidence_digest);
    memset(rec, 0, sizeof *rec);
    memcpy(rec->form_digest, lw->form_digest, 32);
    memcpy(rec->contract_digest, contract_digest, 32);
    memcpy(rec->substrate_digest, lw->substrate_digest, 32);
    memcpy(rec->lowering_digest, lw->lowering_digest, 32);
    memcpy(rec->realization_identity, e->realization_identity, 32);
    memcpy(rec->evidence_digest, out->evidence_digest, 32);
    rec->provenance_class = forge_form_provenance();
    rec->decision = decision;
    if (forge_form_digest_record(rec, out->record_digest) != FORGE_FORM_OK) return FORGE_FORM_ERR_EVIDENCE;
    return FORGE_FORM_OK;
}

int forge_form_realize(const ForgeRealizationForm *f, const ForgeSemanticResultContract *c,
                       const ForgeFormLowering *lw, const ForgeFormInput *in, ForgeFormOutcome *out)
{
    ForgeRealizationForm derived;
    uint8_t contract_digest[32], form_digest[32], input_id[32], ref_id[32];
    uint64_t ref[FORGE_ASIM_MAX_ROWS], first[FORGE_ASIM_MAX_ROWS], run_y[FORGE_ASIM_MAX_ROWS],
             chosen[FORGE_ASIM_MAX_ROWS];
    uint64_t worst = 0, worst_rel = 0, lat = 0, lat_total = 0;
    uint32_t oracle, runs, within_runs = 0, have_chosen = 0, differs = 0, exact_bad = 0;
    int rc;

    if (!out) return FORGE_FORM_ERR_ARG;
    memset(out, 0, sizeof *out);
    out->status = FORGE_FORM_ERR_ARG;
    if (!f || !c || !lw || !in) return out->status;
    if (forge_form_validate(f) != FORGE_FORM_OK) return out->status;
    /* the form must be exactly the one this contract maps to */
    if (forge_form_from_contract(c, &derived) != FORGE_FORM_OK || memcmp(&derived, f, sizeof derived) != 0)
        return out->status = FORGE_FORM_ERR_CONTRACT;
    if (forge_form_digest(f, form_digest) != FORGE_FORM_OK || memcmp(form_digest, lw->form_digest, 32) != 0)
        return out->status = FORGE_FORM_ERR_CONTRACT;
    if (in->M != f->operator_rows || in->N != f->operator_cols) return out->status = FORGE_FORM_ERR_CONTRACT;
    oracle = oracle_of(c->reference_oracle_digest);
    runs = c->min_samples;
    if (!oracle || runs == 0 || runs > FORGE_ASIM_MAX_SAMPLES) return out->status;
    if (!shape_ok(in)) return out->status = FORGE_FORM_ERR_INPUT_RANGE;
    if (forge_v2_digest_contract(c, contract_digest) != FORGE_V2_OK) return out->status = FORGE_FORM_ERR_CONTRACT;

    out->eligibility_rc = eligible(lw, c, &in->now);
    if (out->eligibility_rc != FORGE_V2_OK) {
        out->refuse_reason = FORGE_ASIM_REFUSE_INELIGIBLE;
        return out->status = FORGE_FORM_REFUSED;
    }
    if (c->error_norm != FORGE_V2_NORM_ELEMENTWISE_MAX || c->error_kind == FORGE_V2_ERROR_MEASURED_DISTRIBUTION) {
        out->refuse_reason = FORGE_ASIM_REFUSE_UNSUPPORTED;
        return out->status = FORGE_FORM_REFUSED;
    }

    compute_ref(oracle, in, ref);
    input_digest(in->A, in->x, in->M, in->N, input_id);
    vec_digest("FORGE-FORM/output/linear-operator/v1", ref, in->M, ref_id);

    for (uint32_t r = 0; r < runs; r++) {
        rc = run_once(lw, in, r, oracle, run_y, &lat);
        if (rc != FORGE_FORM_OK) return out->status = rc;
        lat_total = sat_add(lat_total, lat);
        if (r == 0) memcpy(first, run_y, sizeof first);
        else if (memcmp(first, run_y, (size_t)in->M * sizeof run_y[0]) != 0) differs = 1;
        if (c->error_kind == FORGE_V2_ERROR_EXACT) {
            uint8_t od[32];
            vec_digest("FORGE-FORM/output/linear-operator/v1", run_y, in->M, od);
            if (memcmp(od, ref_id, 32) != 0) exact_bad = 1;
        }
        uint32_t ok = 1;
        for (uint32_t i = 0; i < in->M; i++) {
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
    out->ran = 1;
    out->runs = runs;
    out->runs_within_bound = within_runs;
    out->error_abs = worst;

    if (c->error_kind == FORGE_V2_ERROR_EXACT &&
        (exact_bad || (lw->backend == FORGE_FORM_BACKEND_ANALOG_SIM && (!model_ideal(&lw->actual) || lw->wrong_row))))
        out->refuse_reason = FORGE_ASIM_REFUSE_EXACT_MISMATCH;
    else if (c->error_kind != FORGE_V2_ERROR_BOUNDED_STOCHASTIC && differs)
        out->refuse_reason = FORGE_ASIM_REFUSE_NONDETERMINISM;
    else if (c->error_kind != FORGE_V2_ERROR_BOUNDED_STOCHASTIC && within_runs != runs)
        out->refuse_reason = FORGE_ASIM_REFUSE_ERROR_BOUND;
    else if (c->error_kind == FORGE_V2_ERROR_BOUNDED_STOCHASTIC &&
             (uint64_t)within_runs * 1000000ull < (uint64_t)c->confidence_ppm * runs)
        out->refuse_reason = FORGE_ASIM_REFUSE_CONFIDENCE;

    /* evidence, published or refused; the form digest enters only via the lowering digest */
    ForgeExecutionEvidenceV2 *e = &out->evidence;
    forge_v2_realization_identity(contract_digest, in->machine_identity, lw->substrate_digest, lw->lowering_digest,
                                  lw->has_calibration ? lw->cal_digest : NULL, e->realization_identity);
    memcpy(e->machine_identity, in->machine_identity, 32);
    memcpy(e->substrate_identity, lw->substrate.substrate_identity, 32);
    memcpy(e->semantic_contract_digest, contract_digest, 32);
    e->has_calibration = lw->has_calibration;
    if (lw->has_calibration) memcpy(e->calibration_digest, lw->cal_digest, 32);
    memcpy(e->input_identity, input_id, 32);
    vec_digest("FORGE-FORM/output/linear-operator/v1", have_chosen ? chosen : first, in->M, e->output_identity);
    e->execution_epoch_s = in->now.execution_epoch_s;
    e->execution_duration_ns = lat_total;
    e->conversion_duration_ns = lat;
    e->env_temperature_mc = in->now.temperature_mc;
    e->env_supply_uv = in->now.supply_uv;
    e->measured_error_abs = worst;   /* V2 field name; here a simulated / host error vs the oracle */
    e->measured_error_rel_ppb = worst_rel > UINT32_MAX ? UINT32_MAX : (uint32_t)worst_rel;
    e->confidence_ppm = (uint32_t)((uint64_t)within_runs * 1000000ull / runs);
    e->repeat_runs = runs;
    e->repeat_passes = within_runs;
    e->repeat_mean_ns = runs ? lat_total / runs : 0;
    e->fault_state = FORGE_V2_FAULT_NONE;

    if (out->refuse_reason == FORGE_ASIM_REFUSE_NONE) {
        if ((rc = seal(out, lw, contract_digest, FORGE_FORM_DECISION_PUBLISHED)) != FORGE_FORM_OK)
            return out->status = rc;
        memcpy(out->y, chosen, (size_t)in->M * sizeof chosen[0]);
        out->published = 1;
        return out->status = FORGE_FORM_PUBLISHED;
    }
    if ((rc = seal(out, lw, contract_digest, FORGE_FORM_DECISION_REFUSED)) != FORGE_FORM_OK)
        return out->status = rc;
    return out->status = FORGE_FORM_REFUSED;
}

const char *forge_form_strerror(int err)
{
    switch (err) {
    case FORGE_FORM_OK: return "ok";
    case FORGE_FORM_REFUSED: return "refused";
    case FORGE_FORM_ERR_ARG: return "bad argument";
    case FORGE_FORM_ERR_SPACE: return "output buffer too small";
    case FORGE_FORM_ERR_TRUNCATED: return "truncated";
    case FORGE_FORM_ERR_MAGIC: return "bad magic";
    case FORGE_FORM_ERR_KIND: return "wrong object kind";
    case FORGE_FORM_ERR_VERSION: return "unsupported wire version";
    case FORGE_FORM_ERR_LENGTH: return "body length mismatch";
    case FORGE_FORM_ERR_ORDER: return "tags not strictly ascending";
    case FORGE_FORM_ERR_UNKNOWN_TAG: return "unknown tag (strict)";
    case FORGE_FORM_ERR_WIDTH: return "field width wrong";
    case FORGE_FORM_ERR_MISSING: return "required field missing";
    case FORGE_FORM_ERR_VALUE: return "value out of range";
    case FORGE_FORM_ERR_NONCANONICAL: return "non-canonical encoding";
    case FORGE_FORM_ERR_UNSUPPORTED_FORM: return "form kind specified but not realizable here";
    case FORGE_FORM_ERR_CONTRACT: return "contract does not map to this form";
    case FORGE_FORM_ERR_NO_FALLBACK: return "eligible set holds no digital realization";
    case FORGE_FORM_ERR_EVIDENCE: return "evidence or record failed validation";
    case FORGE_FORM_ERR_INPUT_RANGE: return "input outside lowering limits";
    default: return "unknown";
    }
}
