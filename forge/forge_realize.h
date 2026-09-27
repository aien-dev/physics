#ifndef FORGE_REALIZE_H
#define FORGE_REALIZE_H

#include "forge_types.h"
#include "nvrm.h"

/*
 * Canonical FORGE realization API:
 * Lowers an Omega realization request for the specified ForgeMachineDescriptor.
 * Does NOT submit to hardware.
 */
int forge_realize(const ForgeRealizationRequest *req, ForgeRealizationResult *out_res);

/*
 * Canonical AEGIS verification API (physics module entry point):
 * Verifies that the lowered realization satisfies all contracts and invariants
 * for the target descriptor before hardware submission is permitted.
 */
int aegis_verify_realization(const ForgeRealizationRequest    *req,
                             const ForgeRealizationResult     *res,
                             const ForgeMachineDescriptor     *desc,
                             ForgeVerifiedRealization        *out_verified);

/*
 * Canonical Hardware Submission API:
 * Strictly accepts only a ForgeVerifiedRealization.
 * Rejects unverified, mismatched descriptor, or stale realization requests.
 */
int forge_submit_realization(Nvrm                            *rm,
                             const ForgeVerifiedRealization  *verified_real,
                             const ForgeMachineDescriptor    *live_desc,
                             ForgeExecutionEvidence          *out_evidence);

#endif /* FORGE_REALIZE_H */
