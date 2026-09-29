/* tests/test_forge_v2_kat.c -- FORGE SUBSTRATE V2 host-only known-answer test.
 *
 * No hardware, no driver, no GPU. Links forge/v2/forge_substrate_v2.c and
 * the pinned root sha256_clean.c only.
 *
 *   test_forge_v2_kat                 run every check, print PASS/FAIL, exit 0 on all-pass
 *   test_forge_v2_kat --dump-vectors  print docs/forge-v2-golden-vectors.md to stdout
 *
 * Every vector here is a synthetic test fixture. None of them is a receipt of
 * a physical run, and no analog / neuromorphic / optical device exists on
 * this host.
 */
#include "forge_substrate_v2.h"

#include <stdio.h>
#include <string.h>

/* ---- FORGE MACHINE DESCRIPTOR V1 KAT stream (208 bytes) ----
 * The v1 serialization KAT reference record (forge_descriptor.c KAT values,
 * physics PR #13), as its canonical byte stream. V2 never re-serializes v1
 * fields: it only hashes this literal and carries the digest. */
static const uint8_t V1_KAT_STREAM[208] = {
    0xde,0x10,0x12,0x2e,0xde,0x10,0x00,0x00,0x30,0x30,0x30,0x66,0x3a,0x30,0x31,0x3a,
    0x30,0x30,0x2e,0x30,0x00,0x00,0x00,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
    0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,0xc0,0xce,0x00,0x00,0x04,0x0a,0x00,0x00,
    0x20,0x00,0x00,0x00,0x30,0x00,0x00,0x00,0x00,0x18,0x00,0x00,0x02,0x00,0x00,0x00,
    0x35,0x38,0x30,0x2e,0x31,0x37,0x33,0x2e,0x30,0x32,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x39,0x61,0x2e,0x30,0x62,0x2e,0x32,0x64,0x2e,0x30,0x30,0x2e,0x30,0x30,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x35,0x38,0x30,0x2e,0x31,0x37,0x33,0x2e,0x30,0x32,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0xf0,0xfe,0x6b,0x1e,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0x01,0x00,0x00,
    0x00,0x10,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0xc0,0x00,0x00,0x1f,0x00,0x00,0x00,
    0xff,0xff,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
_Static_assert(sizeof V1_KAT_STREAM == FORGE_V1_DESCRIPTOR_LEN, "v1 KAT stream must be 208 bytes");

static const char V1_KAT_DIGEST[] = "10d63d05f888eaba4fe473c5f17febe22f462f543ae0c2b005a9a8e5417e09fd";

/* ---- frozen V2 digests (machine-produced by --dump-vectors, then frozen) ---- */
static const char FROZEN_MACHINE[]     = "89cb5ba667a75ab2871a6ba0d2f812c84d95c376cbfdf3b9b987ecacda1106c0";
static const char FROZEN_CONTRACT[]    = "b32a3577b720087e5877997824123f1177cb7a4e72d31da92313f3e650335421";
static const char FROZEN_CALIBRATION[] = "f7fd8996c6818081ee918681bf9cf9a0be3e1c23fde8ac522eeb55b3a8565413";
static const char FROZEN_EVIDENCE[]    = "e1fbcb958b1e7060ba7ee2d366c3688a00bcf6e27f3f9ca71fb8d73c88f150f9";
static const char FROZEN_REF[]         = "a20384da09d1f46090e553651ba81d1355533d3becbe59c1a798f1321da4b7b5";
static const char FROZEN_ANALOG_MACHINE[] = "a1c00a5f917889455972a204ec7e83c44cce3ea2042e06efeb87981c67d88d2c";

#define KAT_EPOCH 1000000u      /* calibration epoch of the fixture, seconds */
#define KAT_WINDOW 3600u        /* one hour validity */
#define KAT_GENERATION 0x0000000500000007ull
/* measured conditions at time t: inside the fixture envelope */
#define AT(t) (&(ForgeV2Conditions){ (t), 25000, 800000 })

static int g_pass, g_fail;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (ok) g_pass++; else g_fail++;
}

static void to_hex(const uint8_t *p, size_t n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = hx[p[i] >> 4]; out[2 * i + 1] = hx[p[i] & 15]; }
    out[2 * n] = 0;
}

static int digest_is(const uint8_t d[32], const char *hex)
{
    char h[65];
    to_hex(d, 32, h);
    return strcmp(h, hex) == 0;
}

/* label -> 32-byte fixture identity (SHA-256 of an ASCII label) */
static void label_id(const char *label, uint8_t out[32])
{
    forge_v2_sha256((const uint8_t *)label, strlen(label), out);
}

/* ---- fixtures ---- */

static void cpu_substrate(ForgeSubstrateDescriptor *s)
{
    memset(s, 0, sizeof *s);
    label_id("FORGE-V2-KAT/substrate/digital-cpu", s->substrate_identity);
    s->substrate_class = FORGE_V2_SUBSTRATE_DIGITAL_CPU;
    s->operation_families = FORGE_V2_OP_MATVEC | FORGE_V2_OP_MATMUL | FORGE_V2_OP_ELEMENTWISE |
                            FORGE_V2_OP_REDUCTION | FORGE_V2_OP_GENERAL;
    s->input_repr = FORGE_V2_REPR_DIGITAL_INT; s->input_bits = 64;
    s->output_repr = FORGE_V2_REPR_DIGITAL_INT; s->output_bits = 64;
    s->state_model = FORGE_V2_STATE_VOLATILE;
    s->sync_model = FORGE_V2_SYNC_CLOCKED;
    s->memory_model = FORGE_V2_MEM_SHARED_COHERENT;
    s->memory_capacity_bytes = 128ull << 30;
    s->numerical.dynamic_range_mdb = 385000;   /* 64-bit integer, ~385 dB */
    s->numerical.resolution_bits = 64;
    s->error_kind = FORGE_V2_ERROR_EXACT;
    s->fault_detect_mask = 0;
    s->timing_min_latency_ns = 1;
    s->timing_deterministic = 0;
    s->limit_max_rows = 1u << 20; s->limit_max_cols = 1u << 20;
    s->limit_max_state_bytes = 64ull << 30;
    s->limit_max_concurrent = 20;
    s->evidence_capabilities = FORGE_V2_EVCAP_DURATION | FORGE_V2_EVCAP_ENERGY |
                               FORGE_V2_EVCAP_TEMPERATURE | FORGE_V2_EVCAP_REPEATS;
}

static void gpu_substrate(ForgeSubstrateDescriptor *s, const uint8_t v1_digest[32])
{
    memset(s, 0, sizeof *s);
    label_id("FORGE-V2-KAT/substrate/digital-gpu", s->substrate_identity);
    s->substrate_class = FORGE_V2_SUBSTRATE_DIGITAL_GPU;
    s->operation_families = FORGE_V2_OP_MATVEC | FORGE_V2_OP_MATMUL | FORGE_V2_OP_ELEMENTWISE |
                            FORGE_V2_OP_REDUCTION | FORGE_V2_OP_CONV;
    s->input_repr = FORGE_V2_REPR_DIGITAL_INT; s->input_bits = 32;
    s->output_repr = FORGE_V2_REPR_DIGITAL_INT; s->output_bits = 32;
    s->state_model = FORGE_V2_STATE_VOLATILE;
    s->sync_model = FORGE_V2_SYNC_BATCH_BARRIER;
    s->memory_model = FORGE_V2_MEM_SHARED_COHERENT;
    s->memory_capacity_bytes = 128ull << 30;
    s->numerical.dynamic_range_mdb = 192000;   /* 32-bit integer, ~192 dB */
    s->numerical.resolution_bits = 32;
    s->error_kind = FORGE_V2_ERROR_EXACT;
    s->timing_min_latency_ns = 1000;
    s->timing_setup_ns = 10000;
    s->limit_max_rows = 1u << 16; s->limit_max_cols = 1u << 16;
    s->limit_max_state_bytes = 64ull << 30;
    s->limit_max_concurrent = 1;
    s->evidence_capabilities = FORGE_V2_EVCAP_DURATION | FORGE_V2_EVCAP_HW_STATUS | FORGE_V2_EVCAP_FAULT;
    s->has_legacy_v1 = 1;
    memcpy(s->legacy_v1_descriptor_digest, v1_digest, 32);
}

/* Development fixture only: no such device exists on this host. */
static void analog_fixture_substrate(ForgeSubstrateDescriptor *s)
{
    memset(s, 0, sizeof *s);
    label_id("FORGE-V2-KAT/fixture/analog-in-memory-simulated", s->substrate_identity);
    s->substrate_class = FORGE_V2_SUBSTRATE_ANALOG_IN_MEMORY;
    s->operation_families = FORGE_V2_OP_MATVEC;
    s->input_repr = FORGE_V2_REPR_ANALOG_VOLTAGE; s->input_bits = 8;
    s->output_repr = FORGE_V2_REPR_ANALOG_CURRENT; s->output_bits = 8;
    s->state_model = FORGE_V2_STATE_PERSISTENT_WEIGHTS;
    s->sync_model = FORGE_V2_SYNC_BATCH_BARRIER;
    s->memory_model = FORGE_V2_MEM_IN_MEMORY_COMPUTE;
    s->memory_capacity_bytes = 1ull << 20;
    s->numerical.dynamic_range_mdb = 48000;
    s->numerical.resolution_bits = 8;
    s->numerical.repeatability_ppb = 2000000;    /* 0.2 % */
    s->numerical.drift_ppb_per_hour = 500000;
    s->numerical.temperature_ppb_per_k = 100000;
    s->numerical.settling_ns = 200;
    s->numerical.sample_rate_hz = 10000000;
    s->numerical.calibration_validity_s = KAT_WINDOW;
    s->calibration_required = 1;
    s->calibration_method_mask = (1u << FORGE_V2_CAL_REFERENCE_FIXTURE_SWEEP) | (1u << FORGE_V2_CAL_SIMULATED_MODEL);
    s->calibration_max_window_s = KAT_WINDOW;
    s->calibration_min_fixtures = 4;
    s->error_kind = FORGE_V2_ERROR_BOUNDED_STOCHASTIC;
    s->error_rel_ppb = 10000000;                 /* 1 % */
    s->error_abs = 2;
    s->fault_detect_mask = 1;
    s->timing_min_latency_ns = 500;
    s->timing_conversion_ns = 300;
    s->limit_max_rows = 256; s->limit_max_cols = 256;
    s->limit_max_state_bytes = 65536;
    s->limit_max_concurrent = 1;
    s->evidence_capabilities = FORGE_V2_EVCAP_KNOWN_MASK;
}

static void sort2(ForgeSubstrateDescriptor *a, ForgeSubstrateDescriptor *b)
{
    if (memcmp(a->substrate_identity, b->substrate_identity, 32) > 0) {
        ForgeSubstrateDescriptor t = *a; *a = *b; *b = t;
    }
}

static void reference_machine(ForgeMachineDescriptorV2 *m, const uint8_t v1_digest[32])
{
    memset(m, 0, sizeof *m);
    label_id("FORGE-V2-KAT/machine/first-qualified-machine", m->machine_identity);
    label_id("FORGE-V2-KAT/authority-domain/owner", m->authority_domain);
    m->memory_nominal_bytes = 128ull << 30;      /* declared nominal, not measured MemTotal */
    m->memory_numa_domains = 1;
    m->memory_coherency = FORGE_V2_COHERENCY_UNIFIED;
    m->interconnect_topology = FORGE_V2_TOPO_SINGLE_PACKAGE;
    m->interconnect_links = 1;
    m->transport_class = FORGE_V2_TRANSPORT_ON_PACKAGE;
    m->transport_bandwidth_mbps = 600000;
    m->transport_latency_ns = 1000;
    m->substrate_count = 2;
    cpu_substrate(&m->substrates[0]);
    gpu_substrate(&m->substrates[1], v1_digest);
    sort2(&m->substrates[0], &m->substrates[1]);
}

static void bounded_matvec_contract(ForgeSemanticResultContract *c)
{
    memset(c, 0, sizeof *c);
    c->operation_family = FORGE_V2_OP_MATVEC;
    label_id("FORGE-V2-KAT/oracle/matvec-exact-integer-reference", c->reference_oracle_digest);
    c->shape_rank = 1; c->shape_dim[0] = 256;
    c->error_kind = FORGE_V2_ERROR_BOUNDED_STOCHASTIC;
    c->error_rel_ppb = 20000000;                 /* 2 % */
    c->error_abs = 4;
    c->error_norm = FORGE_V2_NORM_ELEMENTWISE_MAX;
    c->confidence_ppm = 999000;
    c->min_samples = 32;
    c->allowed_nondeterminism = FORGE_V2_NONDET_STOCHASTIC_NOISE | FORGE_V2_NONDET_ROUNDING;
}

static void exact_matvec_contract(ForgeSemanticResultContract *c)
{
    memset(c, 0, sizeof *c);
    c->operation_family = FORGE_V2_OP_MATVEC;
    label_id("FORGE-V2-KAT/oracle/matvec-exact-integer-reference", c->reference_oracle_digest);
    c->shape_rank = 1; c->shape_dim[0] = 256;
    c->error_kind = FORGE_V2_ERROR_EXACT;
    c->error_norm = FORGE_V2_NORM_ELEMENTWISE_MAX;
    c->confidence_ppm = 1000000;
    c->min_samples = 1;
}

static void fixture_calibration(ForgeCalibrationArtifact *a, const ForgeSubstrateDescriptor *s)
{
    memset(a, 0, sizeof *a);
    memcpy(a->substrate_id, s->substrate_identity, 32);
    label_id("FORGE-V2-KAT/calibration/0001", a->calibration_id);
    a->method = FORGE_V2_CAL_SIMULATED_MODEL;
    a->epoch_s = KAT_EPOCH;
    a->validity_window_s = KAT_WINDOW;
    a->env_temp_min_mc = 15000; a->env_temp_max_mc = 45000;
    a->env_supply_min_uv = 790000; a->env_supply_max_uv = 810000;
    a->transfer_gain_ppb = -1500000;
    a->transfer_offset_lsb = -1;
    a->transfer_linearity_ppb = 800000;
    a->noise_rms_ppb = 1200000; a->noise_peak_ppb = 4000000; a->noise_samples = 4096;
    a->drift_ppb_per_hour = 400000; a->drift_temp_ppb_per_k = 90000; a->drift_observed_span_s = 7200;
    a->fixture_count = 8;
    label_id("FORGE-V2-KAT/calibration/fixtures", a->fixtures_digest);
    label_id("FORGE-V2-KAT/calibration/evidence-root", a->evidence_root);
}

static void fixture_evidence(ForgeExecutionEvidenceV2 *e, const uint8_t rid[32], const uint8_t machine_id[32],
                             const uint8_t substrate_id[32], const uint8_t contract_digest[32],
                             const uint8_t cal_digest[32])
{
    memset(e, 0, sizeof *e);
    memcpy(e->realization_identity, rid, 32);
    memcpy(e->machine_identity, machine_id, 32);
    memcpy(e->substrate_identity, substrate_id, 32);
    memcpy(e->semantic_contract_digest, contract_digest, 32);
    e->has_calibration = 1;
    memcpy(e->calibration_digest, cal_digest, 32);
    label_id("FORGE-V2-KAT/evidence/input", e->input_identity);
    label_id("FORGE-V2-KAT/evidence/output", e->output_identity);
    e->execution_epoch_s = KAT_EPOCH + 60;
    e->execution_duration_ns = 1800;
    e->conversion_duration_ns = 300;
    e->energy_nj = 0;
    e->energy_source = FORGE_V2_ENERGY_NOT_MEASURED;
    e->env_temperature_mc = 25000;
    e->env_supply_uv = 800000;
    e->measured_error_rel_ppb = 6000000;
    e->measured_error_abs = 1;
    e->confidence_ppm = 999500;
    e->repeat_runs = 32; e->repeat_passes = 32;
    e->repeat_mean_ns = 1800; e->repeat_spread_ns = 40; e->repeat_error_spread_ppb = 900000;
    e->hardware_status = 0;
    e->fault_state = FORGE_V2_FAULT_NONE;
    e->provenance_class = FORGE_V2_PROVENANCE_SIMULATED_DEVELOPMENT;
}

static void fixture_ref(ForgeCrossMachineRef *r, const uint8_t target[32])
{
    memset(r, 0, sizeof *r);
    label_id("FORGE-V2-KAT/ref/program", r->program_digest);
    label_id("FORGE-V2-KAT/ref/semantic-object", r->semantic_object_id);
    label_id("FORGE-V2-KAT/ref/ir", r->ir_reference);
    r->ir_size = 4096; r->range_offset = 512; r->range_length = 1024;
    r->target_kind = FORGE_V2_TARGET_SUBSTRATE;
    memcpy(r->target_digest, target, 32);
    r->generation = KAT_GENERATION;
    r->rights = FORGE_V2_RIGHT_READ | FORGE_V2_RIGHT_EXECUTE_PURE;
}

/* ---- all fixtures in one place ---- */

typedef struct {
    uint8_t v1_digest[32];
    ForgeMachineDescriptorV2 machine;
    uint8_t machine_digest[32];
    ForgeSubstrateDescriptor analog;
    uint8_t analog_digest[32];
    ForgeSemanticResultContract contract;
    uint8_t contract_digest[32];
    ForgeCalibrationArtifact cal;
    uint8_t cal_digest[32];
    uint8_t code_digest[32];
    uint8_t rid[32];
    ForgeExecutionEvidenceV2 ev;
    uint8_t ev_digest[32];
    ForgeCrossMachineRef ref;
    uint8_t ref_digest[32];
    ForgeMachineDescriptorV2 analog_machine;   /* Fabric Machine with only an analog substrate */
    uint8_t analog_machine_digest[32];
} Kat;

/* Development fixture: an external analog compute node reached over the
 * Fabric, whose only substrate is the simulated analog fixture. */
static void analog_only_machine(ForgeMachineDescriptorV2 *m, const ForgeSubstrateDescriptor *analog)
{
    memset(m, 0, sizeof *m);
    label_id("FORGE-V2-KAT/machine/external-analog-node-fixture", m->machine_identity);
    label_id("FORGE-V2-KAT/authority-domain/owner", m->authority_domain);
    m->memory_nominal_bytes = 1ull << 20;
    m->memory_numa_domains = 1;
    m->memory_coherency = FORGE_V2_COHERENCY_DISCRETE;
    m->interconnect_topology = FORGE_V2_TOPO_REMOTE;
    m->interconnect_links = 1;
    m->transport_class = FORGE_V2_TRANSPORT_ETHERNET;
    m->transport_bandwidth_mbps = 10000;
    m->transport_latency_ns = 50000;
    m->substrate_count = 1;
    m->substrates[0] = *analog;
}

static int build_kat(Kat *k)
{
    memset(k, 0, sizeof *k);
    forge_v2_sha256(V1_KAT_STREAM, sizeof V1_KAT_STREAM, k->v1_digest);
    reference_machine(&k->machine, k->v1_digest);
    if (forge_v2_digest_machine(&k->machine, k->machine_digest)) return -1;
    analog_fixture_substrate(&k->analog);
    if (forge_v2_digest_substrate(&k->analog, k->analog_digest)) return -1;
    bounded_matvec_contract(&k->contract);
    if (forge_v2_digest_contract(&k->contract, k->contract_digest)) return -1;
    fixture_calibration(&k->cal, &k->analog);
    if (forge_v2_digest_calibration(&k->cal, k->cal_digest)) return -1;
    label_id("FORGE-V2-KAT/code/analog-matvec-256", k->code_digest);
    forge_v2_realization_identity(k->contract_digest, k->machine.machine_identity, k->analog_digest,
                                  k->code_digest, k->cal_digest, k->rid);
    fixture_evidence(&k->ev, k->rid, k->machine.machine_identity, k->analog.substrate_identity,
                     k->contract_digest, k->cal_digest);
    if (forge_v2_digest_evidence(&k->ev, k->ev_digest)) return -1;
    fixture_ref(&k->ref, k->analog_digest);
    if (forge_v2_digest_ref(&k->ref, k->ref_digest)) return -1;
    analog_only_machine(&k->analog_machine, &k->analog);
    if (forge_v2_digest_machine(&k->analog_machine, k->analog_machine_digest)) return -1;
    return 0;
}

/* ---- byte-level helpers for negative tests ---- */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* offset of the TLV with this tag, or 0 */
static size_t find_tlv(const uint8_t *b, size_t n, uint32_t tag)
{
    size_t pos = FORGE_V2_HEADER_LEN;
    while (pos + 8 <= n) {
        if (rd32(b + pos) == tag) return pos;
        pos += 8 + rd32(b + pos + 4);
    }
    return 0;
}

/* remove the TLV at off (length 8+len), fixing body_len */
static size_t cut_tlv(uint8_t *b, size_t n, size_t off)
{
    size_t w = 8 + rd32(b + off + 4);
    memmove(b + off, b + off + w, n - off - w);
    n -= w;
    wr32(b + 8, (uint32_t)(n - FORGE_V2_HEADER_LEN));
    return n;
}

/* insert a TLV at off, fixing body_len */
static size_t put_tlv(uint8_t *b, size_t n, size_t off, uint32_t tag, const uint8_t *v, uint32_t vlen)
{
    memmove(b + off + 8 + vlen, b + off, n - off);
    wr32(b + off, tag);
    wr32(b + off + 4, vlen);
    memcpy(b + off + 8, v, vlen);
    n += 8 + vlen;
    wr32(b + 8, (uint32_t)(n - FORGE_V2_HEADER_LEN));
    return n;
}

/* ---- checks ---- */

static uint8_t g_buf[FORGE_V2_MAX_OBJECT_LEN];
static uint8_t g_tmp[FORGE_V2_MAX_OBJECT_LEN];

static int run_checks(void)
{
    Kat k;
    size_t n = 0, n2 = 0;
    uint8_t d[32];
    int rc;

    /* V1 wrap */
    uint8_t v1[32];
    forge_v2_sha256(V1_KAT_STREAM, sizeof V1_KAT_STREAM, v1);
    check("v1_kat_stream_digest_reproduced (208-byte literal -> 10d63d05...)", digest_is(v1, V1_KAT_DIGEST));

    check("kat_fixtures_build", build_kat(&k) == 0);
    {
        const ForgeSubstrateDescriptor *gpu = 0;
        for (uint32_t i = 0; i < k.machine.substrate_count; i++)
            if (k.machine.substrates[i].substrate_class == FORGE_V2_SUBSTRATE_DIGITAL_GPU) gpu = &k.machine.substrates[i];
        check("v1_digest_wrapped_in_digital_gpu_substrate",
              gpu && gpu->has_legacy_v1 && digest_is(gpu->legacy_v1_descriptor_digest, V1_KAT_DIGEST));
        /* wrap, not re-serialize: the v1 stream bytes never appear in V2 bytes */
        rc = forge_v2_encode_machine(&k.machine, g_buf, sizeof g_buf, &n);
        int found = 0;
        for (size_t i = 0; rc == 0 && i + 16 <= n; i++)
            if (memcmp(g_buf + i, V1_KAT_STREAM + 8, 16) == 0) found = 1;   /* v1 location field */
        check("v2_bytes_carry_no_v1_fields (no bus address in identity)", rc == 0 && !found);
    }

    /* frozen digests */
    check("frozen_v2_machine_descriptor_digest", digest_is(k.machine_digest, FROZEN_MACHINE));
    check("frozen_semantic_result_contract_digest", digest_is(k.contract_digest, FROZEN_CONTRACT));
    check("frozen_calibration_artifact_digest", digest_is(k.cal_digest, FROZEN_CALIBRATION));
    check("frozen_execution_evidence_v2_digest", digest_is(k.ev_digest, FROZEN_EVIDENCE));
    check("frozen_cross_machine_ref_digest", digest_is(k.ref_digest, FROZEN_REF));

    /* round trips (strict) */
    {
        ForgeMachineDescriptorV2 m2;
        rc = forge_v2_encode_machine(&k.machine, g_buf, sizeof g_buf, &n);
        int ok = rc == 0 && forge_v2_decode_machine(g_buf, n, 1, &m2) == 0 &&
                 forge_v2_encode_machine(&m2, g_tmp, sizeof g_tmp, &n2) == 0 && n2 == n &&
                 memcmp(g_buf, g_tmp, n) == 0 && forge_v2_digest_machine(&m2, d) == 0 &&
                 memcmp(d, k.machine_digest, 32) == 0 && m2.substrate_count == 2;
        check("roundtrip_machine_descriptor_strict", ok);
    }
    {
        ForgeSemanticResultContract c2; ForgeCalibrationArtifact a2; ForgeExecutionEvidenceV2 e2;
        ForgeCrossMachineRef r2; ForgeSubstrateDescriptor s2;
        int ok = 1;
        ok &= forge_v2_encode_contract(&k.contract, g_buf, sizeof g_buf, &n) == 0 &&
              forge_v2_decode_contract(g_buf, n, 1, &c2) == 0 && forge_v2_digest_contract(&c2, d) == 0 &&
              memcmp(d, k.contract_digest, 32) == 0;
        ok &= forge_v2_encode_calibration(&k.cal, g_buf, sizeof g_buf, &n) == 0 &&
              forge_v2_decode_calibration(g_buf, n, 1, &a2) == 0 && forge_v2_digest_calibration(&a2, d) == 0 &&
              memcmp(d, k.cal_digest, 32) == 0 && a2.transfer_gain_ppb == -1500000;
        ok &= forge_v2_encode_evidence(&k.ev, g_buf, sizeof g_buf, &n) == 0 &&
              forge_v2_decode_evidence(g_buf, n, 1, &e2) == 0 && forge_v2_digest_evidence(&e2, d) == 0 &&
              memcmp(d, k.ev_digest, 32) == 0;
        ok &= forge_v2_encode_ref(&k.ref, g_buf, sizeof g_buf, &n) == 0 &&
              forge_v2_decode_ref(g_buf, n, 1, &r2) == 0 && forge_v2_digest_ref(&r2, d) == 0 &&
              memcmp(d, k.ref_digest, 32) == 0 && r2.generation == KAT_GENERATION;
        ok &= forge_v2_encode_substrate(&k.analog, g_buf, sizeof g_buf, &n) == 0 &&
              forge_v2_decode_substrate(g_buf, n, 1, &s2) == 0 && forge_v2_digest_substrate(&s2, d) == 0 &&
              memcmp(d, k.analog_digest, 32) == 0;
        check("roundtrip_contract_calibration_evidence_ref_substrate_strict", ok);
    }

    /* unknown tag: strict refuses, lenient skips and yields the same object */
    {
        ForgeMachineDescriptorV2 m2;
        uint8_t v[4] = { 1, 2, 3, 4 };
        forge_v2_encode_machine(&k.machine, g_buf, sizeof g_buf, &n);
        n2 = put_tlv(g_buf, n, n, 0x01F0, v, 4);          /* after the last (0x0140) field */
        check("unknown_tag_rejected_strict", forge_v2_decode_machine(g_buf, n2, 1, &m2) == FORGE_V2_ERR_UNKNOWN_TAG);
        rc = forge_v2_decode_machine(g_buf, n2, 0, &m2);
        check("unknown_tag_skipped_lenient_same_digest",
              rc == 0 && forge_v2_digest_machine(&m2, d) == 0 && memcmp(d, k.machine_digest, 32) == 0);
    }

    /* canonical order and structure */
    {
        ForgeSemanticResultContract c2;
        forge_v2_encode_contract(&k.contract, g_buf, sizeof g_buf, &n);
        /* swap two adjacent 4-byte TLVs (0x0310 shape_rank, 0x0311 shape_dim0) */
        size_t o = find_tlv(g_buf, n, 0x0310);
        uint8_t a[12], b[12];
        memcpy(a, g_buf + o, 12); memcpy(b, g_buf + o + 12, 12);
        memcpy(g_buf + o, b, 12); memcpy(g_buf + o + 12, a, 12);
        check("out_of_order_tags_rejected", forge_v2_decode_contract(g_buf, n, 1, &c2) == FORGE_V2_ERR_ORDER);
        forge_v2_encode_contract(&k.contract, g_buf, sizeof g_buf, &n);
        n2 = cut_tlv(g_buf, n, find_tlv(g_buf, n, 0x0330));
        check("missing_required_field_rejected", forge_v2_decode_contract(g_buf, n2, 1, &c2) == FORGE_V2_ERR_MISSING);
        forge_v2_encode_contract(&k.contract, g_buf, sizeof g_buf, &n);
        g_buf[n - 1] ^= 0xff;  /* nondeterminism mask gets unknown bits */
        check("unknown_enum_bits_rejected", forge_v2_decode_contract(g_buf, n, 1, &c2) == FORGE_V2_ERR_VALUE);
        forge_v2_encode_contract(&k.contract, g_buf, sizeof g_buf, &n);
        check("truncated_object_rejected", forge_v2_decode_contract(g_buf, n - 1, 1, &c2) == FORGE_V2_ERR_LENGTH);
    }
    {
        ForgeMachineDescriptorV2 m = k.machine;
        m.substrates[1] = m.substrates[0];
        check("duplicate_substrate_identity_rejected",
              forge_v2_encode_machine(&m, g_buf, sizeof g_buf, &n) == FORGE_V2_ERR_DUPLICATE_ID);
        m = k.machine;
        ForgeSubstrateDescriptor t = m.substrates[0]; m.substrates[0] = m.substrates[1]; m.substrates[1] = t;
        check("substrates_out_of_identity_order_rejected",
              forge_v2_encode_machine(&m, g_buf, sizeof g_buf, &n) == FORGE_V2_ERR_ORDER);
        m = k.machine; m.substrate_count = 0;
        check("zero_substrates_rejected", forge_v2_encode_machine(&m, g_buf, sizeof g_buf, &n) == FORGE_V2_ERR_COUNT);
        {
            /* (a) a Fabric Machine whose only substrate is analog is valid */
            ForgeMachineDescriptorV2 am;
            rc = forge_v2_encode_machine(&k.analog_machine, g_buf, sizeof g_buf, &n);
            check("analog_only_machine_roundtrip_strict",
                  rc == 0 && forge_v2_decode_machine(g_buf, n, 1, &am) == 0 && am.substrate_count == 1 &&
                      am.substrates[0].substrate_class == FORGE_V2_SUBSTRATE_ANALOG_IN_MEMORY &&
                      forge_v2_digest_machine(&am, d) == 0 && memcmp(d, k.analog_machine_digest, 32) == 0);
            check("frozen_analog_only_machine_digest", digest_is(k.analog_machine_digest, FROZEN_ANALOG_MACHINE));
        }
        {
            /* (b)/(c) the digital fallback is a property of the eligible set */
            ForgeV2Candidate set[3];
            memset(set, 0, sizeof set);
            memcpy(set[0].contract_digest, k.contract_digest, 32);
            memcpy(set[0].machine_identity, k.analog_machine.machine_identity, 32);
            memcpy(set[0].substrate_digest, k.analog_digest, 32);
            set[0].substrate_class = FORGE_V2_SUBSTRATE_ANALOG_IN_MEMORY;
            set[1] = set[0];
            set[1].substrate_digest[0] ^= 1;   /* a second analog candidate */
            check("eligible_set_analog_only_has_no_digital_fallback",
                  forge_v2_eligible_set_has_digital_fallback(set, 2, k.contract_digest) == 0 &&
                      forge_v2_admit_plan(set, 2, k.contract_digest) == FORGE_V2_ERR_NO_DIGITAL_FALLBACK);
            /* a digital candidate for a different contract does not count */
            const ForgeSubstrateDescriptor *cpu = &k.machine.substrates[0];
            if (cpu->substrate_class != FORGE_V2_SUBSTRATE_DIGITAL_CPU) cpu = &k.machine.substrates[1];
            memcpy(set[2].contract_digest, k.contract_digest, 32);
            set[2].contract_digest[0] ^= 1;
            memcpy(set[2].machine_identity, k.machine.machine_identity, 32);
            forge_v2_digest_substrate(cpu, set[2].substrate_digest);
            set[2].substrate_class = cpu->substrate_class;
            check("digital_candidate_for_other_contract_not_a_fallback",
                  forge_v2_eligible_set_has_digital_fallback(set, 3, k.contract_digest) == 0);
            /* digital realization of the same contract on another Machine */
            memcpy(set[2].contract_digest, k.contract_digest, 32);
            check("eligible_set_digital_on_other_machine_is_fallback",
                  memcmp(set[2].machine_identity, set[0].machine_identity, 32) != 0 &&
                      forge_v2_eligible_set_has_digital_fallback(set, 3, k.contract_digest) == 1 &&
                      forge_v2_admit_plan(set, 3, k.contract_digest) == FORGE_V2_OK);
        }
        ForgeSubstrateDescriptor s = k.analog;
        s.substrate_class = FORGE_V2_SUBSTRATE_FUTURE_RESERVED_FIRST;
        check("reserved_substrate_class_rejected",
              forge_v2_encode_substrate(&s, g_buf, sizeof g_buf, &n) == FORGE_V2_ERR_VALUE);
        s = k.analog; s.has_legacy_v1 = 1; memcpy(s.legacy_v1_descriptor_digest, k.v1_digest, 32);
        check("legacy_v1_digest_only_on_digital_substrate",
              forge_v2_encode_substrate(&s, g_buf, sizeof g_buf, &n) == FORGE_V2_ERR_VALUE);
    }

    /* transport is cost metadata: descriptor digest moves, identity does not */
    {
        ForgeMachineDescriptorV2 m = k.machine;
        m.transport_class = FORGE_V2_TRANSPORT_RDMA_ETHERNET;
        uint8_t rid2[32];
        forge_v2_digest_machine(&m, d);
        forge_v2_realization_identity(k.contract_digest, m.machine_identity, k.analog_digest, k.code_digest,
                                      k.cal_digest, rid2);
        check("transport_changes_descriptor_digest_not_realization_identity",
              memcmp(d, k.machine_digest, 32) != 0 && memcmp(rid2, k.rid, 32) == 0);
    }

    /* calibration: fresh ok, stale / missing / future / foreign refused, digest bound into identity */
    {
        uint64_t fresh = KAT_EPOCH + 60, stale = KAT_EPOCH + KAT_WINDOW, future = KAT_EPOCH - 1;
        check("fresh_calibration_eligible",
              forge_v2_substrate_eligible(&k.analog, &k.contract, &k.cal, FORGE_V2_FAULT_NONE, AT(fresh)) == 0);
        check("stale_calibration_rejected",
              forge_v2_substrate_eligible(&k.analog, &k.contract, &k.cal, FORGE_V2_FAULT_NONE, AT(stale)) ==
                  FORGE_V2_ERR_CAL_STALE);
        check("missing_calibration_rejected",
              forge_v2_substrate_eligible(&k.analog, &k.contract, 0, FORGE_V2_FAULT_NONE, AT(fresh)) ==
                  FORGE_V2_ERR_CAL_MISSING);
        check("calibration_from_future_rejected",
              forge_v2_calibration_check(&k.analog, &k.cal, AT(future)) == FORGE_V2_ERR_CAL_STALE);
        ForgeCalibrationArtifact a = k.cal;
        a.validity_window_s = KAT_WINDOW * 10;   /* artifact over-claims its window */
        check("calibration_window_capped_by_substrate",
              forge_v2_calibration_check(&k.analog, &a, AT(KAT_EPOCH + KAT_WINDOW + 1)) == FORGE_V2_ERR_CAL_STALE);
        a = k.cal; a.substrate_id[0] ^= 1;
        check("calibration_for_other_substrate_rejected",
              forge_v2_calibration_check(&k.analog, &a, AT(fresh)) == FORGE_V2_ERR_CAL_MISMATCH);
        ForgeV2Conditions hot = { fresh, 50000, 800000 };   /* 50 C; envelope ends at 45 C */
        check("out_of_envelope_conditions_rejected",
              forge_v2_substrate_eligible(&k.analog, &k.contract, &k.cal, FORGE_V2_FAULT_NONE, &hot) ==
                  FORGE_V2_ERR_OUT_OF_ENVELOPE);
        a = k.cal; a.epoch_s += 1;
        uint8_t cd[32], rid2[32];
        forge_v2_digest_calibration(&a, cd);
        forge_v2_realization_identity(k.contract_digest, k.machine.machine_identity, k.analog_digest,
                                      k.code_digest, cd, rid2);
        check("recalibration_changes_realization_identity", memcmp(rid2, k.rid, 32) != 0);
    }

    /* failure behavior: faulted substrate excluded, digital fallback remains */
    {
        ForgeSemanticResultContract exact;
        exact_matvec_contract(&exact);
        const ForgeSubstrateDescriptor *cpu = 0;
        for (uint32_t i = 0; i < k.machine.substrate_count; i++)
            if (k.machine.substrates[i].substrate_class == FORGE_V2_SUBSTRATE_DIGITAL_CPU) cpu = &k.machine.substrates[i];
        check("faulted_substrate_excluded",
              forge_v2_substrate_eligible(&k.analog, &k.contract, &k.cal, FORGE_V2_FAULT_PERSISTENT, AT(KAT_EPOCH + 60)) ==
                  FORGE_V2_ERR_FAULTED);
        check("digital_fallback_eligible_for_bounded_and_exact",
              cpu && forge_v2_substrate_eligible(cpu, &k.contract, 0, FORGE_V2_FAULT_NONE, AT(KAT_EPOCH + 60)) == 0 &&
                  forge_v2_substrate_eligible(cpu, &exact, 0, FORGE_V2_FAULT_NONE, AT(KAT_EPOCH + 60)) == 0);
        check("stochastic_substrate_not_eligible_for_exact_contract",
              forge_v2_substrate_eligible(&k.analog, &exact, &k.cal, FORGE_V2_FAULT_NONE, AT(KAT_EPOCH + 60)) ==
                  FORGE_V2_ERR_VALUE);
        ForgeSemanticResultContract tight = k.contract;
        tight.error_rel_ppb = 1000000;   /* 0.1 %, tighter than the substrate's 1 % */
        check("substrate_bound_looser_than_contract_rejected",
              forge_v2_substrate_eligible(&k.analog, &tight, &k.cal, FORGE_V2_FAULT_NONE, AT(KAT_EPOCH + 60)) ==
                  FORGE_V2_ERR_VALUE);
    }

    /* generation: 32-bit refused, legacy version refused, full 64 bits compared */
    {
        ForgeCrossMachineRef r2;
        forge_v2_encode_ref(&k.ref, g_buf, sizeof g_buf, &n);
        size_t off = find_tlv(g_buf, n, 0x0730);
        uint8_t g32[4] = { 7, 0, 0, 0 };
        n2 = cut_tlv(g_buf, n, off);
        n2 = put_tlv(g_buf, n2, off, 0x0730, g32, 4);
        check("v1_32bit_generation_refused", forge_v2_decode_ref(g_buf, n2, 1, &r2) == FORGE_V2_ERR_GENERATION_32);
        forge_v2_encode_ref(&k.ref, g_buf, sizeof g_buf, &n);
        g_buf[6] = 1; g_buf[7] = 0;
        check("legacy_version_1_object_refused", forge_v2_decode_ref(g_buf, n, 1, &r2) == FORGE_V2_ERR_LEGACY_VERSION);
        check("authorize_pure_current_generation",
              forge_v2_authorize(&k.ref, FORGE_V2_FX_PURE, KAT_GENERATION) == 0);
        check("stale_generation_high_bits_refused",
              forge_v2_authorize(&k.ref, FORGE_V2_FX_PURE, KAT_GENERATION + (1ull << 32)) ==
                  FORGE_V2_ERR_STALE_GENERATION);
        check("irreversible_config_without_right_refused",
              forge_v2_authorize(&k.ref, FORGE_V2_FX_EXTERNAL_IRREVERSIBLE, KAT_GENERATION) == FORGE_V2_ERR_RIGHTS);
        ForgeCrossMachineRef r = k.ref;
        r.rights |= FORGE_V2_RIGHT_CONFIGURE_IRREVERSIBLE;
        check("irreversible_config_with_right_and_generation_allowed",
              forge_v2_authorize(&r, FORGE_V2_FX_EXTERNAL_IRREVERSIBLE, KAT_GENERATION) == 0);
        r = k.ref; r.range_offset = UINT64_MAX - 10;
        check("reference_range_overflow_refused", forge_v2_encode_ref(&r, g_buf, sizeof g_buf, &n) == FORGE_V2_ERR_RANGE);
    }

    /* provenance must be explicit */
    {
        ForgeExecutionEvidenceV2 e = k.ev, e2;
        e.provenance_class = FORGE_V2_PROVENANCE_INVALID;
        check("evidence_encode_without_provenance_refused",
              forge_v2_encode_evidence(&e, g_buf, sizeof g_buf, &n) == FORGE_V2_ERR_PROVENANCE);
        forge_v2_encode_evidence(&k.ev, g_buf, sizeof g_buf, &n);
        n2 = cut_tlv(g_buf, n, find_tlv(g_buf, n, 0x0670));
        check("evidence_decode_missing_provenance_refused",
              forge_v2_decode_evidence(g_buf, n2, 1, &e2) == FORGE_V2_ERR_PROVENANCE);
        forge_v2_encode_evidence(&k.ev, g_buf, sizeof g_buf, &n);
        wr32(g_buf + find_tlv(g_buf, n, 0x0670) + 8, 0);
        check("evidence_decode_zero_provenance_refused",
              forge_v2_decode_evidence(g_buf, n, 1, &e2) == FORGE_V2_ERR_PROVENANCE);
        forge_v2_encode_evidence(&k.ev, g_buf, sizeof g_buf, &n);
        check("simulated_provenance_explicit_in_kat_evidence",
              forge_v2_decode_evidence(g_buf, n, 1, &e2) == 0 &&
                  e2.provenance_class == FORGE_V2_PROVENANCE_SIMULATED_DEVELOPMENT);
    }

    printf("RESULT %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

/* ---- --dump-vectors ---- */

static void dump_bytes(const uint8_t *b, size_t n)
{
    char h[65];
    printf("```\n");
    for (size_t i = 0; i < n; i += 32) {
        size_t w = n - i < 32 ? n - i : 32;
        to_hex(b + i, w, h);
        printf("%s\n", h);
    }
    printf("```\n");
}

static void dump_digest(const char *label, const uint8_t d[32])
{
    char h[65];
    to_hex(d, 32, h);
    printf("%s: `%s`\n\n", label, h);
}

static int dump_vectors(void)
{
    Kat k;
    size_t n = 0;
    if (build_kat(&k) != 0) return 1;
    printf("# FORGE SUBSTRATE V2 golden vectors\n\n");
    printf("Generated by `build/test_forge_v2_kat --dump-vectors` (see tests/run_forge_v2_gates.sh).\n");
    printf("Do not edit by hand: the gate regenerates this file and fails on any difference.\n\n");
    printf("All vectors are synthetic test fixtures. None is a receipt of a physical run. The\n");
    printf("ANALOG_IN_MEMORY substrate below is a development fixture; no such device exists and\n");
    printf("no physical analog result is claimed. Canonical bytes are shown as hex, 32 bytes per line.\n\n");

    printf("## 1. FORGE MACHINE DESCRIPTOR V1 KAT stream (wrapped, not re-serialized)\n\n");
    printf("208-byte v1 canonical stream (the v1 KAT record):\n\n");
    dump_bytes(V1_KAT_STREAM, sizeof V1_KAT_STREAM);
    printf("\n");
    dump_digest("v1 descriptor digest (must equal the frozen v1 KAT digest)", k.v1_digest);

    printf("## 2. Reference ForgeMachineDescriptorV2 (first qualified machine, frozen)\n\n");
    printf("Two substrates: DIGITAL_CPU and DIGITAL_GPU; the DIGITAL_GPU substrate carries\n");
    printf("`legacy_v1_descriptor_digest` = the v1 digest above.\n\n");
    forge_v2_encode_machine(&k.machine, g_buf, sizeof g_buf, &n);
    printf("Canonical bytes (%zu bytes):\n\n", n);
    dump_bytes(g_buf, n);
    printf("\n");
    dump_digest("V2 machine descriptor digest (frozen)", k.machine_digest);
    for (uint32_t i = 0; i < k.machine.substrate_count; i++) {
        uint8_t d[32];
        forge_v2_digest_substrate(&k.machine.substrates[i], d);
        char lbl[64];
        snprintf(lbl, sizeof lbl, "Substrate %u (%s) descriptor digest", i,
                 k.machine.substrates[i].substrate_class == FORGE_V2_SUBSTRATE_DIGITAL_GPU ? "DIGITAL_GPU" : "DIGITAL_CPU");
        dump_digest(lbl, d);
    }

    printf("## 3. Development fixture substrate: ANALOG_IN_MEMORY (simulated)\n\n");
    forge_v2_encode_substrate(&k.analog, g_buf, sizeof g_buf, &n);
    printf("Canonical bytes (%zu bytes):\n\n", n);
    dump_bytes(g_buf, n);
    printf("\n");
    dump_digest("Fixture substrate descriptor digest", k.analog_digest);

    printf("## 4. SemanticResultContract (bounded stochastic matvec, 256 outputs)\n\n");
    forge_v2_encode_contract(&k.contract, g_buf, sizeof g_buf, &n);
    printf("Canonical bytes (%zu bytes):\n\n", n);
    dump_bytes(g_buf, n);
    printf("\n");
    dump_digest("Contract digest (frozen)", k.contract_digest);

    printf("## 5. CalibrationArtifact (fixture substrate, method SIMULATED_MODEL)\n\n");
    forge_v2_encode_calibration(&k.cal, g_buf, sizeof g_buf, &n);
    printf("Canonical bytes (%zu bytes):\n\n", n);
    dump_bytes(g_buf, n);
    printf("\n");
    dump_digest("Calibration digest (frozen)", k.cal_digest);

    printf("## 6. Realization identity (calibration digest bound in)\n\n");
    dump_digest("Code digest input (SHA-256 of a fixture label)", k.code_digest);
    dump_digest("Realization identity", k.rid);

    printf("## 7. ForgeExecutionEvidenceV2 (provenance_class = SIMULATED_DEVELOPMENT)\n\n");
    forge_v2_encode_evidence(&k.ev, g_buf, sizeof g_buf, &n);
    printf("Canonical bytes (%zu bytes):\n\n", n);
    dump_bytes(g_buf, n);
    printf("\n");
    dump_digest("Evidence digest (frozen)", k.ev_digest);

    printf("## 8. Cross-machine reference (64-bit generation 0x%016llx)\n\n",
           (unsigned long long)k.ref.generation);
    forge_v2_encode_ref(&k.ref, g_buf, sizeof g_buf, &n);
    printf("Canonical bytes (%zu bytes):\n\n", n);
    dump_bytes(g_buf, n);
    printf("\n");
    dump_digest("Reference digest (frozen)", k.ref_digest);

    printf("## 9. Analog-only Fabric Machine (development fixture, frozen)\n\n");
    printf("An external analog compute node whose only substrate is the simulated ANALOG_IN_MEMORY\n");
    printf("fixture of section 3. Valid: the digital fallback is required of the eligible set for a\n");
    printf("contract (any Machine), not of each Machine descriptor.\n\n");
    forge_v2_encode_machine(&k.analog_machine, g_buf, sizeof g_buf, &n);
    printf("Canonical bytes (%zu bytes):\n\n", n);
    dump_bytes(g_buf, n);
    printf("\n");
    dump_digest("Analog-only machine descriptor digest (frozen)", k.analog_machine_digest);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--dump-vectors") == 0) return dump_vectors();
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--dump-vectors]\n", argv[0]);
        return 2;
    }
    return run_checks();
}
