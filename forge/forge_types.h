#ifndef FORGE_TYPES_H
#define FORGE_TYPES_H

#include "forge_descriptor.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FORGE_DIGEST_LEN          32
#define FORGE_COMMIT_LEN          20
#define FORGE_MAX_MACHINE_CODE    65536

/*
 * Omega Realization Request:
 * Submitted from Omega to FORGE specifying the program to realize
 * targeted to a specific normalized ForgeMachineDescriptor.
 */
typedef struct {
    uint8_t                program_digest[FORGE_DIGEST_LEN];
    uint32_t               opcode_count;
    const uint8_t         *ir_payload;
    size_t                 ir_size;
    ForgeMachineDescriptor target_machine;
    uint8_t                target_descriptor_digest[FORGE_DIGEST_LEN];
} ForgeRealizationRequest;

/*
 * Raw Forge Realization Result:
 * The output of FORGE lowering prior to AEGIS invariant verification.
 * Must NOT be submitted directly to hardware.
 */
typedef struct {
    uint8_t  request_digest[FORGE_DIGEST_LEN];
    uint8_t  realization_id[FORGE_DIGEST_LEN];
    uint8_t  target_descriptor_digest[FORGE_DIGEST_LEN];
    uint8_t  code_digest[FORGE_DIGEST_LEN];
    uint8_t *machine_code;
    size_t   machine_code_size;
    uint32_t gpr_count;
    uint32_t shared_mem_bytes;
    bool     is_lowered;
} ForgeRealizationResult;

/*
 * Aegis Verified Realization:
 * The typed proof that AEGIS invariant verification succeeded.
 * Only instances constructed by aegis_verify_realization() with
 * verified == true and valid verification_token are accepted
 * by forge_submit_realization().
 * Contains an immutable sealed copy and hash of the verified machine code
 * to prevent post-verification code mutation attacks.
 */
typedef struct {
    ForgeRealizationResult realization;
    uint8_t                sealed_code[FORGE_MAX_MACHINE_CODE];
    size_t                 sealed_code_size;
    uint8_t                sealed_code_hash[FORGE_DIGEST_LEN];
    uint8_t                verification_token[FORGE_DIGEST_LEN];
    uint8_t                verified_descriptor_digest[FORGE_DIGEST_LEN];
    bool                   verified;
} ForgeVerifiedRealization;

/*
 * Forge Execution Evidence:
 * Structured observational evidence emitted upon physical hardware execution.
 */
typedef struct {
    uint8_t  candidate_commit[FORGE_COMMIT_LEN];
    uint8_t  physics_commit[FORGE_COMMIT_LEN];
    uint8_t  descriptor_digest[FORGE_DIGEST_LEN];
    uint8_t  code_digest[FORGE_DIGEST_LEN];
    uint8_t  realization_id[FORGE_DIGEST_LEN];
    uint32_t submission_marker_payload;
    uint64_t execution_duration_ns;
    bool     hardware_success;
} ForgeExecutionEvidence;

/* Seam error codes */
#define FORGE_SEAM_OK                    0
#define FORGE_SEAM_ERR_UNVERIFIED       -101
#define FORGE_SEAM_ERR_DESC_MISMATCH    -102
#define FORGE_SEAM_ERR_STALE_IDENTITY   -103
#define FORGE_SEAM_ERR_VERIFY_FAILED    -104
#define FORGE_SEAM_ERR_INVALID_ARG      -105
#define FORGE_SEAM_ERR_CAPACITY         -106
#define FORGE_SEAM_ERR_NVRM             -107
#define FORGE_SEAM_ERR_CODE_MUTATED     -108
#define FORGE_SEAM_ERR_NULL_NVRM        -109
#define FORGE_SEAM_ERR_SUBMIT_FAILED    -110

#endif /* FORGE_TYPES_H */
