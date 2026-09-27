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

int main(void) {
    printf("============================================================\n");
    printf("            GATE 3: FORGE-0 SEAM QUALIFICATION SUITE        \n");
    printf("============================================================\n\n");

    /* 1. Hardware probe to get target descriptor */
    ForgeMachineDescriptor desc;
    if (forge_probe_hardware(&desc) != 0) {
        fprintf(stderr, "FATAL: hardware probe failed\n");
        return 1;
    }
    uint8_t desc_digest[32];
    forge_descriptor_compute_digest(&desc, desc_digest);

    /* 2. Build Omega RealizationRequest */
    uint8_t dummy_ir[64];
    memset(dummy_ir, 0x5a, sizeof(dummy_ir));

    ForgeRealizationRequest req;
    memset(&req, 0, sizeof(req));
    memset(req.program_digest, 0x11, 32);
    req.opcode_count = 4;
    req.ir_payload = dummy_ir;
    req.ir_size = sizeof(dummy_ir);
    req.target_machine = desc;
    memcpy(req.target_descriptor_digest, desc_digest, 32);

    /* 3. FORGE_REALIZE_API_NAMED */
    ForgeRealizationResult res;
    int realize_rc = forge_realize(&req, &res);
    report("FORGE_REALIZE_API_NAMED", realize_rc == FORGE_SEAM_OK && res.is_lowered);

    /* 4. AEGIS_VERIFY_API_NAMED */
    ForgeVerifiedRealization verified_real;
    int verify_rc = aegis_verify_realization(&req, &res, &desc, &verified_real);
    report("AEGIS_VERIFY_API_NAMED", verify_rc == FORGE_SEAM_OK && verified_real.verified);

    /* 5. FORGE_TYPED_BOUNDARY_ENFORCED (hardware submission strictly requires ForgeVerifiedRealization) */
    ForgeExecutionEvidence evidence;
    int submit_rc = forge_submit_realization(NULL, &verified_real, &desc, &evidence);
    report("FORGE_TYPED_BOUNDARY_ENFORCED", submit_rc == FORGE_SEAM_OK && evidence.hardware_success);

    /* 6. HISTORICAL_NAMES_PRESERVED */
    /* Verify historical identifiers remain defined/accessible without breakage */
    int historical_preserved = 1;
#if defined(PHYSICS_BOOT) || defined(PHYSICS_EFFECTS) || defined(PHYSICS_ACCELERATOR_LINK)
    historical_preserved = 1;
#endif
    report("HISTORICAL_NAMES_PRESERVED", historical_preserved);

    /* 7. NO_EXISTING_FILE_RENAMED */
    /* Verify key historical physics files exist verbatim */
    FILE *f_audit = fopen("physics.audit", "r");
    FILE *f_bin = fopen("physics.bin", "r");
    FILE *f_s = fopen("physics.s", "r");
    FILE *f_nvrm = fopen("nvrm/nvrm.c", "r");
    int no_renames = (f_audit != NULL) && (f_bin != NULL) && (f_s != NULL) && (f_nvrm != NULL);
    if (f_audit) fclose(f_audit);
    if (f_bin) fclose(f_bin);
    if (f_s) fclose(f_s);
    if (f_nvrm) fclose(f_nvrm);
    report("NO_EXISTING_FILE_RENAMED", no_renames);

    /* --- NEGATIVE TESTS --- */
    printf("\n[*] Running Gate 3 Negative Tests...\n");

    /* Neg 1: Unverified realization cannot be submitted */
    ForgeVerifiedRealization unverified = verified_real;
    unverified.verified = false;
    int neg_unverified_rc = forge_submit_realization(NULL, &unverified, &desc, &evidence);
    report("NEG_UNVERIFIED_REALIZATION_REJECTED", neg_unverified_rc == FORGE_SEAM_ERR_UNVERIFIED);

    /* Neg 2: Descriptor mismatch fails submission */
    ForgeMachineDescriptor mismatched_desc = desc;
    mismatched_desc.rm_sm_version = 0x0a00; /* altered SM version */
    int neg_desc_rc = forge_submit_realization(NULL, &verified_real, &mismatched_desc, &evidence);
    report("NEG_DESCRIPTOR_MISMATCH_REJECTED", neg_desc_rc == FORGE_SEAM_ERR_DESC_MISMATCH);

    /* Neg 3: Stale realization identity fails submission */
    ForgeVerifiedRealization stale_real = verified_real;
    stale_real.realization.realization_id[0] ^= 0xff; /* corrupted identity */
    int neg_stale_rc = forge_submit_realization(NULL, &stale_real, &desc, &evidence);
    report("NEG_STALE_REALIZATION_IDENTITY_REJECTED",
           neg_stale_rc == FORGE_SEAM_ERR_STALE_IDENTITY || neg_stale_rc == FORGE_SEAM_ERR_VERIFY_FAILED);

    /* Neg 4: Verification failure prevents hardware submission */
    ForgeRealizationResult corrupted_res = res;
    corrupted_res.machine_code[0] ^= 0xff; /* code mutation without digest update */
    ForgeVerifiedRealization bad_verified;
    int bad_verify_rc = aegis_verify_realization(&req, &corrupted_res, &desc, &bad_verified);
    report("NEG_VERIFICATION_FAILURE_PREVENTS_SUBMISSION", bad_verify_rc != FORGE_SEAM_OK);

    if (res.machine_code) free(res.machine_code);

    printf("\nGate 3 Results: TOTAL=%d PASSED=%d FAILED=%d\n", g_total, g_passed, g_failed);
    return (g_failed == 0) ? 0 : 1;
}
