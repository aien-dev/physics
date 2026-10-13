/*
 * Host-only checks of the AEGIS verifier and the submit-side refusals of the
 * FORGE seam (physics #53). No GPU and no NVRM: the descriptor is the frozen
 * KAT descriptor, and every submit call here is refused before any hardware
 * access. Run: tests/run_forge_aegis_host_gates.sh
 */
#include "forge_descriptor.h"
#include "forge_realize.h"
#include "forge_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_total = 0, g_passed = 0, g_failed = 0;

static void report(const char *name, int ok) {
    g_total++;
    if (ok) {
        g_passed++;
        printf("[PASS] %s\n", name);
    } else {
        g_failed++;
        printf("[FAIL] %s\n", name);
    }
}

/* Same values as the frozen KAT in forge_descriptor_run_kat(). */
static void kat_descriptor(ForgeMachineDescriptor *d) {
    memset(d, 0, sizeof(*d));
    d->pci_vendor_id = 0x10de;
    d->pci_device_id = 0x2e12;
    d->pci_subsystem_vendor_id = 0x10de;
    d->pci_subsystem_device_id = 0x0000;
    snprintf(d->pci_bdf, sizeof(d->pci_bdf), "000f:01:00.0");
    for (int i = 0; i < 16; i++) d->gpu_uuid[i] = (uint8_t)(i + 1);
    d->rm_compute_class = 0x0000cec0;
    d->rm_sm_version = 0x00000a04;
    d->warp_size = 32;
    d->max_warps_per_sm = 48;
    d->gpu_core_count = 6144;
    d->sm_per_tpc = 2;
    snprintf(d->driver_version, sizeof(d->driver_version), "580.173.02");
    snprintf(d->vbios_version, sizeof(d->vbios_version), "9a.0b.2d.00.00");
    snprintf(d->firmware_version, sizeof(d->firmware_version), "580.173.02");
    d->total_system_memory_bytes = 130660888576ULL;
    d->dma_mask = 0x1ffffffffffULL;
    d->page_size = 4096;
    d->max_threads_per_block = 1024;
    d->shared_mem_per_block_bytes = 49152;
    d->features = 0x1f;
    d->observation_flags = 0x1ffff;
}

static void make_request(ForgeRealizationRequest *req, const ForgeMachineDescriptor *desc,
                         const uint8_t desc_digest[32], uint8_t program_byte, uint32_t opcodes,
                         const uint8_t *ir, size_t ir_size) {
    memset(req, 0, sizeof(*req));
    memset(req->program_digest, program_byte, 32);
    req->opcode_count = opcodes;
    req->ir_payload = ir;
    req->ir_size = ir_size;
    req->target_machine = *desc;
    memcpy(req->target_descriptor_digest, desc_digest, 32);
}

int main(void) {
    printf("=== FORGE AEGIS verifier host checks (no GPU) ===\n");

    ForgeMachineDescriptor desc;
    kat_descriptor(&desc);
    uint8_t desc_digest[32];
    report("KAT_DESCRIPTOR_DIGEST", forge_descriptor_compute_digest(&desc, desc_digest) == 0);

    uint8_t ir[64];
    memset(ir, 0x5a, sizeof(ir));

    ForgeRealizationRequest req_a, req_b, req_c;
    make_request(&req_a, &desc, desc_digest, 0x11, 4, ir, sizeof(ir));
    make_request(&req_b, &desc, desc_digest, 0x22, 4, ir, sizeof(ir)); /* other program */
    make_request(&req_c, &desc, desc_digest, 0x11, 5, ir, sizeof(ir)); /* other opcode count */

    ForgeRealizationResult res_a;
    report("REALIZE_OK", forge_realize(&req_a, &res_a) == FORGE_SEAM_OK && res_a.is_lowered);

    static ForgeVerifiedRealization verified;
    report("VERIFY_OWN_REQUEST_ACCEPTED",
           aegis_verify_realization(&req_a, &res_a, &desc, &verified) == FORGE_SEAM_OK && verified.verified);

    static ForgeVerifiedRealization replay;
    int rc_b = aegis_verify_realization(&req_b, &res_a, &desc, &replay);
    report("NEG_RESULT_REPLAYED_TO_OTHER_PROGRAM_REFUSED", rc_b == FORGE_SEAM_ERR_VERIFY_FAILED && !replay.verified);
    int rc_c = aegis_verify_realization(&req_c, &res_a, &desc, &replay);
    report("NEG_RESULT_REPLAYED_TO_OTHER_OPCODE_COUNT_REFUSED", rc_c == FORGE_SEAM_ERR_VERIFY_FAILED && !replay.verified);

    /* A result that names another target machine than its request. */
    ForgeRealizationRequest req_other_target = req_a;
    req_other_target.target_descriptor_digest[0] ^= 0xff;
    int rc_t = aegis_verify_realization(&req_other_target, &res_a, &desc, &replay);
    report("NEG_REQUEST_FOR_OTHER_TARGET_REFUSED", rc_t != FORGE_SEAM_OK && !replay.verified);

    /* Existing invariants, now covered on the host. */
    uint8_t bad_code[FORGE_MAX_MACHINE_CODE];
    memcpy(bad_code, res_a.machine_code, res_a.machine_code_size);
    bad_code[0] ^= 0xff;
    ForgeRealizationResult corrupted = res_a;
    corrupted.machine_code = bad_code;
    report("NEG_CODE_DIGEST_MISMATCH_REFUSED",
           aegis_verify_realization(&req_a, &corrupted, &desc, &replay) == FORGE_SEAM_ERR_VERIFY_FAILED);

    ForgeRealizationResult stale = res_a;
    stale.realization_id[0] ^= 0xff;
    report("NEG_STALE_IDENTITY_REFUSED",
           aegis_verify_realization(&req_a, &stale, &desc, &replay) == FORGE_SEAM_ERR_STALE_IDENTITY);

    ForgeMachineDescriptor other_desc = desc;
    other_desc.rm_sm_version = 0x0a00;
    report("NEG_OTHER_DESCRIPTOR_REFUSED",
           aegis_verify_realization(&req_a, &res_a, &other_desc, &replay) == FORGE_SEAM_ERR_DESC_MISMATCH);

    /* Submit-side refusals; all return before any NVRM call. */
    ForgeExecutionEvidence ev;
    static ForgeVerifiedRealization unverified;
    unverified = verified;
    unverified.verified = false;
    report("NEG_SUBMIT_UNVERIFIED_REFUSED",
           forge_submit_realization(NULL, &unverified, &desc, &ev) == FORGE_SEAM_ERR_UNVERIFIED);

    static ForgeVerifiedRealization forged;
    forged = verified;
    forged.verification_token[0] ^= 0xff;
    report("NEG_SUBMIT_BAD_TOKEN_REFUSED",
           forge_submit_realization(NULL, &forged, &desc, &ev) == FORGE_SEAM_ERR_VERIFY_FAILED);

    uint8_t mutated_code[FORGE_MAX_MACHINE_CODE];
    memcpy(mutated_code, verified.sealed_code, verified.sealed_code_size);
    mutated_code[0] ^= 0xff;
    static ForgeVerifiedRealization mutated;
    mutated = verified;
    mutated.realization.machine_code = mutated_code;
    report("NEG_SUBMIT_POST_VERIFY_MUTATION_REFUSED",
           forge_submit_realization(NULL, &mutated, &desc, &ev) == FORGE_SEAM_ERR_CODE_MUTATED);

    report("NEG_SUBMIT_WITHOUT_NVRM_REFUSED",
           forge_submit_realization(NULL, &verified, &desc, &ev) == FORGE_SEAM_ERR_NULL_NVRM);

    free(res_a.machine_code);
    printf("FORGE AEGIS host results: TOTAL=%d PASSED=%d FAILED=%d\n", g_total, g_passed, g_failed);
    return (g_failed == 0) ? 0 : 1;
}
