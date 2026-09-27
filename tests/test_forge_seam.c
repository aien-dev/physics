#include "forge_descriptor.h"
#include "forge_realize.h"
#include "forge_types.h"
#include "nvrm.h"

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

static int check_git_diff_no_renames_or_deletions(void) {
    FILE *fp = popen("git diff --name-status a477aeef700559a086eb61c12e070b262681963e", "r");
    if (!fp) return 0;
    char line[512];
    int ok = 1;
    while (fgets(line, sizeof(line), fp)) {
        char status = line[0];
        if (status == 'D' || status == 'R') {
            fprintf(stderr, "[-] Forbidden git status '%c': %s", status, line);
            ok = 0;
        }
    }
    pclose(fp);
    return ok;
}

static int check_historical_files_unmodified(void) {
    FILE *fp = popen("git diff --exit-code a477aeef700559a086eb61c12e070b262681963e -- physics.bin physics.audit physics.s nvrm/ m16/", "r");
    if (!fp) return 0;
    int rc = pclose(fp);
    return (rc == 0);
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

    /* Open real physical Nvrm GPU substrate */
    Nvrm rm;
    memset(&rm, 0, sizeof(rm));
    int rm_rc = nvrm_open(&rm);
    if (rm_rc != 0) {
        fprintf(stderr, "FATAL: nvrm_open failed: %d\n", rm_rc);
        return 1;
    }

    /* 5. FORGE_TYPED_BOUNDARY_ENFORCED (real native GB10 physical submission) */
    ForgeExecutionEvidence evidence;
    memset(&evidence, 0, sizeof(evidence));
    int submit_rc = forge_submit_realization(&rm, &verified_real, &desc, &evidence);
    int hw_ok = (submit_rc == FORGE_SEAM_OK) &&
                evidence.hardware_success &&
                (evidence.submission_marker_payload == 0x44444444U) &&
                (evidence.execution_duration_ns > 0) &&
                (memcmp(evidence.descriptor_digest, desc_digest, 32) == 0) &&
                (memcmp(evidence.code_digest, res.code_digest, 32) == 0);
    report("FORGE_TYPED_BOUNDARY_ENFORCED", hw_ok);

    /* 6. HISTORICAL_NAMES_PRESERVED */
    int hist_ok = check_historical_files_unmodified();
    report("HISTORICAL_NAMES_PRESERVED", hist_ok);

    /* 7. NO_EXISTING_FILE_RENAMED */
    int no_renames_ok = check_git_diff_no_renames_or_deletions();
    report("NO_EXISTING_FILE_RENAMED", no_renames_ok);

    /* --- NEGATIVE TESTS --- */
    printf("\n[*] Running Gate 3 Negative Tests...\n");

    /* Neg 0: NULL Nvrm rejected */
    ForgeExecutionEvidence neg_ev;
    int neg_null_rm = forge_submit_realization(NULL, &verified_real, &desc, &neg_ev);
    report("NEG_NULL_NVRM_REJECTED", neg_null_rm == FORGE_SEAM_ERR_NULL_NVRM);

    /* Neg 1: Unverified realization cannot be submitted */
    ForgeVerifiedRealization unverified = verified_real;
    unverified.verified = false;
    int neg_unverified_rc = forge_submit_realization(&rm, &unverified, &desc, &neg_ev);
    report("NEG_UNVERIFIED_REALIZATION_REJECTED", neg_unverified_rc == FORGE_SEAM_ERR_UNVERIFIED);

    /* Neg 2: Descriptor mismatch fails submission */
    ForgeMachineDescriptor mismatched_desc = desc;
    mismatched_desc.rm_sm_version = 0x0a00; /* altered SM version */
    int neg_desc_rc = forge_submit_realization(&rm, &verified_real, &mismatched_desc, &neg_ev);
    report("NEG_DESCRIPTOR_MISMATCH_REJECTED", neg_desc_rc == FORGE_SEAM_ERR_DESC_MISMATCH);

    /* Neg 3: Stale realization identity fails submission */
    ForgeVerifiedRealization stale_real = verified_real;
    stale_real.realization.realization_id[0] ^= 0xff; /* corrupted identity */
    int neg_stale_rc = forge_submit_realization(&rm, &stale_real, &desc, &neg_ev);
    report("NEG_STALE_REALIZATION_IDENTITY_REJECTED",
           neg_stale_rc == FORGE_SEAM_ERR_STALE_IDENTITY || neg_stale_rc == FORGE_SEAM_ERR_VERIFY_FAILED);

    /* Neg 4: Verification failure prevents hardware submission */
    uint8_t bad_code[1024];
    memcpy(bad_code, res.machine_code, res.machine_code_size);
    bad_code[0] ^= 0xff;
    ForgeRealizationResult corrupted_res = res;
    corrupted_res.machine_code = bad_code;
    ForgeVerifiedRealization bad_verified;
    int bad_verify_rc = aegis_verify_realization(&req, &corrupted_res, &desc, &bad_verified);
    report("NEG_VERIFICATION_FAILURE_PREVENTS_SUBMISSION", bad_verify_rc != FORGE_SEAM_OK);

    /* Neg 5: Post-verification machine code mutation MUST REFUSE submission */
    uint8_t mutated_code[1024];
    memcpy(mutated_code, verified_real.sealed_code, verified_real.sealed_code_size);
    mutated_code[0] ^= 0xff;
    ForgeVerifiedRealization mutated_verified = verified_real;
    mutated_verified.realization.machine_code = mutated_code;
    int neg_mutate_rc = forge_submit_realization(&rm, &mutated_verified, &desc, &neg_ev);
    report("NEG_POST_VERIFICATION_MUTATION_REFUSED", neg_mutate_rc == FORGE_SEAM_ERR_CODE_MUTATED);

    /* Clean up */
    nvrm_close(&rm);
    if (res.machine_code) free(res.machine_code);

    printf("\nGate 3 Results: TOTAL=%d PASSED=%d FAILED=%d\n", g_total, g_passed, g_failed);
    return (g_failed == 0) ? 0 : 1;
}
