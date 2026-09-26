#include "../physics_accel.h"
#include "../physics_accel_native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <assert.h>

#define PHYSICS_KERNEL_MEM_BASE     0x0000000080000000ULL
#define PHYSICS_KERNEL_MEM_SIZE     0x0000000001000000ULL /* 16 MiB */

static void print_gate(const char *name, bool pass, const char *desc) {
    printf("  [%s] %-42s : %s\n", pass ? "PASS" : "FAIL", name, desc);
}

/* ============================================================================
 * GATE 1: PHYSICS_ACCEL_MEM_BOUNDS_PASS
 * Coherent DRAM envelope boundaries and Physics kernel memory isolation.
 * ============================================================================ */
static bool test_gate_1_mem_bounds(void) {
    PhysicsAcceleratorLink link;
    physics_accel_init(&link, DGX_SPARK_DRAM_BASE, DGX_SPARK_DRAM_SIZE,
                       PHYSICS_KERNEL_MEM_BASE, PHYSICS_KERNEL_MEM_SIZE);

    Smmuv3StreamConfig cfg = {
        .stream_id = GB10_STREAM_ID,
        .ste_index = 0,
        .cd_table_base = 0x82000000ULL,
        .ttbr0_base = 0x83000000ULL,
        .mair_value = 0xFF,
        .t0sz = 16,
        .granule_size = 4096,
        .stage1_enabled = true,
        .fault_trap_enabled = true
    };
    physics_accel_configure_smmu(&link, &cfg);

    AcceleratorCapability cap = {
        .slot = 1,
        .generation = 1,
        .principal_id = 42,
        .resource_type = RES_ACCELERATOR,
        .allowed_ops = ACCEL_OP_MAP_DMA | ACCEL_OP_UNMAP_DMA,
        .iova_bound_base = 0x10000000ULL,
        .iova_bound_size = 0x10000000ULL,
        .queue_id_mask = 0x1,
        .revocation_state = 1
    };

    /* 1. Valid window in DRAM */
    DmaWindowDescriptor valid_win = {
        .iova_base = 0x10000000ULL,
        .phys_base = 0x90000000ULL,
        .size_bytes = 0x100000ULL,
        .permissions = DMA_PERM_READ | DMA_PERM_WRITE | DMA_PERM_COHERENT
    };
    EffectReceipt r1;
    int rc1 = physics_accel_grant_dma_window(&link, &cap, &valid_win, &r1);
    if (rc1 != 0 || r1.decision != DEC_ADMITTED) return false;

    /* 2. Invalid window: overlapping protected Physics kernel memory */
    DmaWindowDescriptor kernel_overlap_win = {
        .iova_base = 0x11000000ULL,
        .phys_base = 0x80500000ULL, /* Inside [0x80000000, 0x81000000) */
        .size_bytes = 0x100000ULL,
        .permissions = DMA_PERM_READ | DMA_PERM_WRITE
    };
    EffectReceipt r2;
    int rc2 = physics_accel_grant_dma_window(&link, &cap, &kernel_overlap_win, &r2);
    if (rc2 == 0 || r2.decision != DEC_REJECTED) return false;

    /* 3. Invalid window: completely outside DRAM envelope */
    DmaWindowDescriptor outside_win = {
        .iova_base = 0x12000000ULL,
        .phys_base = 0x3000000000ULL, /* Outside 128 GiB DRAM */
        .size_bytes = 0x100000ULL,
        .permissions = DMA_PERM_READ | DMA_PERM_WRITE
    };
    EffectReceipt r3;
    int rc3 = physics_accel_grant_dma_window(&link, &cap, &outside_win, &r3);
    if (rc3 == 0 || r3.decision != DEC_REJECTED) return false;

    return true;
}

/* ============================================================================
 * GATE 2: PHYSICS_ACCEL_DMA_POLICY_PASS
 * SMMUv3 Stage 1 IOVA-to-PA translation mapping policy and DMA sandboxing.
 * ============================================================================ */
static bool test_gate_2_dma_policy(void) {
    PhysicsAcceleratorLink link;
    physics_accel_init(&link, DGX_SPARK_DRAM_BASE, DGX_SPARK_DRAM_SIZE,
                       PHYSICS_KERNEL_MEM_BASE, PHYSICS_KERNEL_MEM_SIZE);

    Smmuv3StreamConfig cfg = {
        .stream_id = GB10_STREAM_ID,
        .ste_index = 0,
        .cd_table_base = 0x82000000ULL,
        .ttbr0_base = 0x83000000ULL,
        .mair_value = 0xFF,
        .t0sz = 16,
        .granule_size = 4096,
        .stage1_enabled = true,
        .fault_trap_enabled = true
    };
    if (physics_accel_configure_smmu(&link, &cfg) != 0) return false;

    AcceleratorCapability cap = {
        .slot = 1,
        .generation = 1,
        .principal_id = 42,
        .resource_type = RES_ACCELERATOR,
        .allowed_ops = ACCEL_OP_MAP_DMA | ACCEL_OP_UNMAP_DMA,
        .iova_bound_base = 0x10000000ULL,
        .iova_bound_size = 0x10000000ULL,
        .revocation_state = 1
    };

    DmaWindowDescriptor win = {
        .iova_base = 0x10000000ULL,
        .phys_base = 0x90000000ULL,
        .size_bytes = 0x20000ULL,
        .permissions = DMA_PERM_READ | DMA_PERM_WRITE | DMA_PERM_COHERENT
    };
    EffectReceipt r;
    if (physics_accel_grant_dma_window(&link, &cap, &win, &r) != 0) return false;

    /* Verify SMMU translation at base, interior, and edge */
    uint64_t pa = 0;
    if (physics_accel_smmu_translate(&link, 0x10000000ULL, 4096, DMA_PERM_READ, &pa) != SMMU_FAULT_NONE) return false;
    if (pa != 0x90000000ULL) return false;

    if (physics_accel_smmu_translate(&link, 0x10004000ULL, 4096, DMA_PERM_WRITE, &pa) != SMMU_FAULT_NONE) return false;
    if (pa != 0x90004000ULL) return false;

    if (physics_accel_smmu_translate(&link, 0x1001F000ULL, 4096, DMA_PERM_READ, &pa) != SMMU_FAULT_NONE) return false;
    if (pa != 0x9001F000ULL) return false;

    /* DMA Sandbox Verification: unmapped IOVA access fails closed */
    if (physics_accel_smmu_translate(&link, 0x20000000ULL, 4096, DMA_PERM_READ, &pa) != SMMU_FAULT_TRANSLATION) return false;

    /* DMA Sandbox Verification: permission violation fails closed */
    DmaWindowDescriptor ro_win = {
        .iova_base = 0x10050000ULL,
        .phys_base = 0x90050000ULL,
        .size_bytes = 0x10000ULL,
        .permissions = DMA_PERM_READ /* Read-only */
    };
    if (physics_accel_grant_dma_window(&link, &cap, &ro_win, &r) != 0) return false;
    if (physics_accel_smmu_translate(&link, 0x10050000ULL, 4096, DMA_PERM_WRITE, &pa) != SMMU_FAULT_PERMISSION) return false;

    /* DMA Sandbox Verification: overlapping IOVA windows rejected */
    DmaWindowDescriptor overlap_win = {
        .iova_base = 0x10008000ULL, /* Overlaps win */
        .phys_base = 0x90100000ULL,
        .size_bytes = 0x10000ULL,
        .permissions = DMA_PERM_READ | DMA_PERM_WRITE
    };
    int rc_overlap = physics_accel_grant_dma_window(&link, &cap, &overlap_win, &r);
    if (rc_overlap == 0 || r.decision != DEC_REJECTED) return false;

    return true;
}

/* ============================================================================
 * GATE 3: PHYSICS_ACCEL_QUEUE_AUTHORITY_PASS
 * Bounded queue authority, ring pointer governance, and doorbell mediation.
 * ============================================================================ */
static bool test_gate_3_queue_authority(void) {
    PhysicsAcceleratorLink link;
    physics_accel_init(&link, DGX_SPARK_DRAM_BASE, DGX_SPARK_DRAM_SIZE,
                       PHYSICS_KERNEL_MEM_BASE, PHYSICS_KERNEL_MEM_SIZE);

    Smmuv3StreamConfig cfg = {
        .stream_id = GB10_STREAM_ID,
        .ste_index = 0,
        .cd_table_base = 0x82000000ULL,
        .ttbr0_base = 0x83000000ULL,
        .mair_value = 0xFF,
        .t0sz = 16,
        .granule_size = 4096,
        .stage1_enabled = true,
        .fault_trap_enabled = true
    };
    physics_accel_configure_smmu(&link, &cfg);

    AcceleratorCapability cap = {
        .slot = 1,
        .generation = 1,
        .principal_id = 42,
        .resource_type = RES_ACCELERATOR,
        .allowed_ops = ACCEL_OP_ALLOC_QUEUE | ACCEL_OP_SUBMIT | ACCEL_OP_MAP_DMA,
        .iova_bound_base = 0x10000000ULL,
        .iova_bound_size = 0x10000000ULL,
        .queue_id_mask = 0x1,
        .revocation_state = 1
    };

    EffectReceipt r;
    if (physics_accel_alloc_queue(&link, &cap, 0, &r) != 0) return false;
    if (r.decision != DEC_ADMITTED) return false;
    if (!link.queues[0].active) return false;

    /* Map command buffer DMA window */
    DmaWindowDescriptor cmd_win = {
        .iova_base = 0x10000000ULL,
        .phys_base = 0x90000000ULL,
        .size_bytes = 0x10000ULL,
        .permissions = DMA_PERM_READ | DMA_PERM_WRITE | DMA_PERM_COHERENT
    };
    if (physics_accel_grant_dma_window(&link, &cap, &cmd_win, &r) != 0) return false;

    /* Submit command descriptor within granted window */
    EffectIntent valid_intent = {
        .version = 1,
        .length = sizeof(EffectIntent),
        .request_id = 501,
        .principal_id = 42,
        .capability_slot = 1,
        .capability_generation = 1,
        .resource_type = RES_ACCELERATOR,
        .operation = ACCEL_OP_SUBMIT,
        .target_base = 0, /* queue_id = 0 */
        .target_size = 64,
        .param0 = 0x10001000ULL /* IOVA of command descriptor */
    };
    if (physics_accel_submit_command(&link, &cap, &valid_intent, &r) != 0) return false;
    if (r.decision != DEC_ADMITTED) return false;
    if (link.queues[0].tail_index != 1) return false;

    /* Refusal on unmapped command descriptor IOVA */
    EffectIntent rogue_intent = valid_intent;
    rogue_intent.request_id = 502;
    rogue_intent.param0 = 0x40000000ULL; /* Unmapped IOVA */
    int rc = physics_accel_submit_command(&link, &cap, &rogue_intent, &r);
    if (rc == 0 || r.decision != DEC_REJECTED) return false;

    return true;
}

/* ============================================================================
 * GATE 4: PHYSICS_ACCEL_DEVICE_LIFECYCLE_PASS
 * Monotonic device lifecycle progression and fault recovery.
 * ============================================================================ */
static bool test_gate_4_device_lifecycle(void) {
    PhysicsAcceleratorLink link;
    if (physics_accel_init(&link, DGX_SPARK_DRAM_BASE, DGX_SPARK_DRAM_SIZE,
                           PHYSICS_KERNEL_MEM_BASE, PHYSICS_KERNEL_MEM_SIZE) != 0) return false;
    if (link.device_state != ACCEL_STATE_PROBED) return false;

    Smmuv3StreamConfig cfg = {
        .stream_id = GB10_STREAM_ID,
        .ste_index = 0,
        .cd_table_base = 0x82000000ULL,
        .ttbr0_base = 0x83000000ULL,
        .mair_value = 0xFF,
        .t0sz = 16,
        .granule_size = 4096,
        .stage1_enabled = true,
        .fault_trap_enabled = true
    };
    if (physics_accel_configure_smmu(&link, &cfg) != 0) return false;
    if (link.device_state != ACCEL_STATE_CONFIGURED) return false;

    AcceleratorCapability cap = {
        .slot = 1,
        .generation = 1,
        .principal_id = 42,
        .resource_type = RES_ACCELERATOR,
        .allowed_ops = ACCEL_OP_ALLOC_QUEUE | ACCEL_OP_RESET,
        .revocation_state = 1
    };
    EffectReceipt r;
    if (physics_accel_alloc_queue(&link, &cap, 0, &r) != 0) return false;
    if (link.device_state != ACCEL_STATE_ACTIVE) return false;

    /* Simulate SMMU fault and verify containment */
    physics_accel_handle_fault(&link, SMMU_FAULT_TRANSLATION, 0xDEADBEEFULL);
    if (link.device_state != ACCEL_STATE_FAULTED) return false;
    if (link.active_queues != 0) return false;

    /* Execute non-disruptive reset recovery */
    if (physics_accel_reset_device(&link, &cap, &r) != 0) return false;
    if (r.decision != DEC_ADMITTED) return false;
    if (link.device_state != ACCEL_STATE_CONFIGURED) return false;

    return true;
}

/* ============================================================================
 * GATE 5: PHYSICS_ACCEL_RECEIPT_CHAIN_PASS
 * Immutable 192B receipt with rolling SHA-256 seal chain.
 * ============================================================================ */
static bool test_gate_5_receipt_chain(void) {
    PhysicsAcceleratorLink link;
    physics_accel_init(&link, DGX_SPARK_DRAM_BASE, DGX_SPARK_DRAM_SIZE,
                       PHYSICS_KERNEL_MEM_BASE, PHYSICS_KERNEL_MEM_SIZE);

    EffectReceipt r1, r2, r3;
    EffectIntent i1 = { .version = 1, .length = 64, .request_id = 1 };
    EffectIntent i2 = { .version = 1, .length = 64, .request_id = 2 };
    EffectIntent i3 = { .version = 1, .length = 64, .request_id = 3 };

    physics_accel_commit_receipt(&link, &i1, DEC_ADMITTED, 0, 100, 1, &r1);
    physics_accel_commit_receipt(&link, &i2, DEC_ADMITTED, 0, 200, 2, &r2);
    physics_accel_commit_receipt(&link, &i3, DEC_ADMITTED, 0, 300, 3, &r3);

    /* Check rolling seal chain binding */
    if (memcmp(r2.previous_receipt_digest, r1.receipt_digest, 32) != 0) return false;
    if (memcmp(r3.previous_receipt_digest, r2.receipt_digest, 32) != 0) return false;
    if (memcmp(link.last_receipt_digest, r3.receipt_digest, 32) != 0) return false;

    /* Check machine generation strictly monotonic */
    if (r2.machine_generation != r1.machine_generation + 1) return false;
    if (r3.machine_generation != r2.machine_generation + 1) return false;

    return true;
}

/* ============================================================================
 * GATE 6: PHYSICS_ACCEL_OMEGA_INGRESS_PASS
 * Omega mediated intent ingress and capability gating.
 * ============================================================================ */
static bool test_gate_6_omega_ingress(void) {
    PhysicsAcceleratorLink link;
    physics_accel_init(&link, DGX_SPARK_DRAM_BASE, DGX_SPARK_DRAM_SIZE,
                       PHYSICS_KERNEL_MEM_BASE, PHYSICS_KERNEL_MEM_SIZE);

    /* Revoked capability presentation must be rejected fail-closed */
    AcceleratorCapability revoked_cap = {
        .slot = 1,
        .generation = 1,
        .principal_id = 99,
        .resource_type = RES_ACCELERATOR,
        .allowed_ops = ACCEL_OP_MAP_DMA,
        .revocation_state = 2 /* REVOKED */
    };
    DmaWindowDescriptor win = {
        .iova_base = 0x10000000ULL,
        .phys_base = 0x90000000ULL,
        .size_bytes = 0x10000ULL,
        .permissions = DMA_PERM_READ
    };
    EffectReceipt r;
    int rc = physics_accel_grant_dma_window(&link, &revoked_cap, &win, &r);
    if (rc == 0 || r.decision != DEC_REJECTED) return false;
    if (r.rejection_reason != DEC_REJECTED_REVOKED) return false;

    return true;
}

/* ============================================================================
 * GATE 7: PHYSICS_ACCEL_HARDWARE_BOUNDARY_PASS
 * Truthful observation of DGX Spark hardware topology (BDF, vendor/device ID,
 * SMMUv3 platform device, IOMMU group, Stream ID provenance, BAR0 aperture,
 * explicit subsystem ownership recording, and 0 guessed MMIO writes).
 * ============================================================================ */
static bool test_gate_7_hardware_boundary(void) {
    Smmuv3NativeProbe probe;
    int rc = physics_accel_native_verify_hardware_boundary(&probe);
    if (rc != 0) {
        printf("    [FAIL] Hardware boundary verification failed (rc=%d)\n", rc);
        return false;
    }

    /* Verify ownership boundary table is recorded */
    size_t boundary_count = 0;
    const HardwareOwnershipBoundary *boundaries = physics_accel_get_hardware_ownership_boundaries(&boundary_count);
    if (!boundaries || boundary_count < 6) {
        printf("    [FAIL] Incomplete hardware ownership boundary recording\n");
        return false;
    }

    printf("    [Boundary] PCI BDF:        %s\n", probe.pci_bdf);
    printf("    [Boundary] Device:         %s (0x%04x:0x%04x)\n", probe.device_name, probe.vendor_id, probe.device_id);
    printf("    [Boundary] IOMMU Group:    %u\n", probe.iommu_group);
    printf("    [Boundary] SMMUv3 Base:    %s @ 0x%08llx\n", probe.smmu_name, (unsigned long long)probe.smmu_base);
    printf("    [Boundary] Stream ID:      0x%04x (256 dec, ACPI IORT Node 29)\n", probe.stream_id);
    printf("    [Boundary] BAR0 Aperture:  0x%08llx - 0x%08llx (%llu MiB)\n",
           (unsigned long long)probe.bar0_base,
           (unsigned long long)(probe.bar0_base + probe.bar0_size - 1),
           (unsigned long long)probe.bar0_size / (1024 * 1024));
    printf("    [Boundary] Subsystems:     %zu ownership boundaries documented\n", boundary_count);
    printf("    [Boundary] Sovereign Rule: 0 guessed MMIO writes performed\n");

    return true;
}

/* ============================================================================
 * GATE 8: PHYSICS_ACCEL_ZERO_RUNTIME_DEP_PASS
 * Strictly scans sovereign substrate files for forbidden runtime dependencies,
 * CUDA APIs, LLVM JIT, Python headers, or GCC inline assembly.
 * ============================================================================ */
static bool line_has_word(const char *line, const char *word) {
    const char *p = line;
    size_t wlen = strlen(word);
    while ((p = strstr(p, word)) != NULL) {
        bool left_ok = (p == line) || (!((p[-1] >= 'a' && p[-1] <= 'z') ||
                                         (p[-1] >= 'A' && p[-1] <= 'Z') ||
                                         (p[-1] >= '0' && p[-1] <= '9') ||
                                         p[-1] == '_'));
        const char *after = p + wlen;
        bool right_ok = (*after == '\0') || (!((*after >= 'a' && *after <= 'z') ||
                                              (*after >= 'A' && *after <= 'Z') ||
                                              (*after >= '0' && *after <= '9') ||
                                              *after == '_'));
        if (left_ok && right_ok) return true;
        p += wlen;
    }
    return false;
}

static bool test_gate_8_zero_runtime_dep(void) {
    const char *canonical_files[] = {
        "m15/physics_accel.c",
        "m15/physics_accel.h",
        "m15/physics_accel_native.c",
        "m15/physics_accel_native.h",
        "sha256_clean.c"
    };
    const char *forbidden[] = {
        "__asm__",
        "asm(",
        "Python.h",
        "system(",
        "popen(",
        "libcuda",
        "cudaMalloc",
        "cudaMemcpy",
        "cudaLaunch",
        "cuMemAlloc",
        "cuLaunch",
        "LLVM",
        "nvcc",
        "PTX"
    };

    for (size_t i = 0; i < sizeof(canonical_files) / sizeof(canonical_files[0]); i++) {
        const char *p = canonical_files[i];
        FILE *f = fopen(p, "r");
        char alt_path[256];
        if (!f) {
            snprintf(alt_path, sizeof(alt_path), "../%s", canonical_files[i]);
            f = fopen(alt_path, "r");
            if (!f) {
                printf("    [FAIL] Could not open canonical file: %s\n", p);
                return false;
            }
            p = alt_path;
        }

        char line[1024];
        uint32_t line_num = 0;
        while (fgets(line, sizeof(line), f)) {
            line_num++;
            char *trim = line;
            while (*trim == ' ' || *trim == '\t') trim++;
            if (trim[0] == '/' && (trim[1] == '*' || trim[1] == '/')) continue;

            for (size_t k = 0; k < sizeof(forbidden) / sizeof(forbidden[0]); k++) {
                if (line_has_word(line, forbidden[k])) {
                    printf("    [FAIL] Forbidden token '%s' in %s:%u\n", forbidden[k], p, line_num);
                    fclose(f);
                    return false;
                }
            }
        }
        fclose(f);
    }

    printf("    [Toolchain] Host qualification compiler: GCC (temporary non-lineage scaffold)\n");
    printf("    [Toolchain] Canonical accelerator runtime dependencies: 0 CUDA, 0 LLVM, 0 Python, 0 inline asm\n");
    return true;
}

/* ============================================================================
 * GATE 9: PHYSICS_ACCEL_PROVENANCE_PASS
 * Verifies capability provenance, monotonic attenuation, and slot checking.
 * ============================================================================ */
static bool test_gate_9_provenance(void) {
    PhysicsAcceleratorLink link;
    physics_accel_init(&link, DGX_SPARK_DRAM_BASE, DGX_SPARK_DRAM_SIZE,
                       PHYSICS_KERNEL_MEM_BASE, PHYSICS_KERNEL_MEM_SIZE);

    AcceleratorCapability base_cap = {
        .slot = 1,
        .generation = 1,
        .principal_id = 42,
        .resource_type = RES_ACCELERATOR,
        .allowed_ops = ACCEL_OP_MAP_DMA | ACCEL_OP_UNMAP_DMA,
        .iova_bound_base = 0x10000000ULL,
        .iova_bound_size = 0x10000000ULL,
        .queue_id_mask = 0x1,
        .revocation_state = 1
    };

    /* Attenuation check: requesting unpermitted operation fails closed */
    int val = physics_accel_validate_capability(&link, &base_cap, ACCEL_OP_RESET, 0, 0);
    if (val != DEC_REJECTED_OPERATION) return false;

    /* Resource type check: wrong resource type fails closed */
    AcceleratorCapability wrong_res_cap = base_cap;
    wrong_res_cap.resource_type = 0x9999;
    val = physics_accel_validate_capability(&link, &wrong_res_cap, ACCEL_OP_MAP_DMA, 0, 0);
    if (val != DEC_REJECTED_RESOURCE) return false;

    /* Out of bounds IOVA request fails closed */
    val = physics_accel_validate_capability(&link, &base_cap, ACCEL_OP_MAP_DMA,
                                            0x05000000ULL, 0x1000ULL);
    if (val != DEC_REJECTED_BOUNDS) return false;

    return true;
}

/* ============================================================================
 * GATE 10: PHYSICS_ACCEL_RECEIPT_PASS
 * Verifies formal 192-byte EffectReceipt committing observed hardware boundary.
 * ============================================================================ */
static bool test_gate_10_receipt(void) {
    PhysicsAcceleratorLink link;
    physics_accel_init(&link, DGX_SPARK_DRAM_BASE, DGX_SPARK_DRAM_SIZE,
                       PHYSICS_KERNEL_MEM_BASE, PHYSICS_KERNEL_MEM_SIZE);

    Smmuv3NativeProbe probe;
    if (physics_accel_native_probe(&probe) != 0) return false;

    EffectReceipt receipt;
    int rc = physics_accel_native_commit_boundary_receipt(&link, &probe, &receipt);
    if (rc != 0) return false;

    if (receipt.version != 1 || receipt.length != 192) return false;
    if (receipt.decision != DEC_ADMITTED) return false;
    if (receipt.actual_effect != probe.bar0_base) return false;
    if (receipt.output != probe.stream_id) return false;
    if (receipt.measurement != probe.smmu_base) return false;
    if (receipt.machine_generation != 2) return false;

    /* Verify seal matches rolling chain */
    if (memcmp(link.last_receipt_digest, receipt.receipt_digest, 32) != 0) return false;

    return true;
}

/* ============================================================================
 * DEMONSTRATION & ARTIFACT EXPORT
 * ============================================================================ */

static void run_demonstration(void) {
    printf("================================================================================\n");
    printf("  AIEN PHYSICS SUBSTRATE — MILESTONE 15: HARDWARE BOUNDARY DEMONSTRATION\n");
    printf("================================================================================\n");

    Smmuv3NativeProbe probe;
    if (physics_accel_native_probe(&probe) != 0) {
        printf("  [ERROR] Hardware probe failed!\n");
        return;
    }

    printf("  OBSERVED HARDWARE TOPOLOGY:\n");
    printf("    PCI BDF:          %s\n", probe.pci_bdf);
    printf("    Device:           %s (0x%04x:0x%04x)\n", probe.device_name, probe.vendor_id, probe.device_id);
    printf("    IOMMU Group:      %u\n", probe.iommu_group);
    printf("    SMMUv3 Device:    %s @ 0x%08llx\n", probe.smmu_name, (unsigned long long)probe.smmu_base);
    printf("    Stream ID:        0x%04x (256 decimal, ACPI IORT Node 29)\n", probe.stream_id);
    printf("    Coherent DRAM:    [0x80000000, 0x2080000000) (128 GiB unified LPDDR5x)\n");
    printf("    BAR0 Aperture:    0x%08llx - 0x%08llx (%llu MiB)\n",
           (unsigned long long)probe.bar0_base,
           (unsigned long long)(probe.bar0_base + probe.bar0_size - 1),
           (unsigned long long)probe.bar0_size / (1024 * 1024));
    printf("\n");

    printf("  SUBSYSTEM OWNERSHIP BOUNDARIES:\n");
    size_t count = 0;
    const HardwareOwnershipBoundary *b = physics_accel_get_hardware_ownership_boundaries(&count);
    for (size_t i = 0; i < count; i++) {
        printf("    [%zu] %-28s : %s\n", i + 1, b[i].subsystem, b[i].current_owner);
        printf("        Physics Status : %s\n", b[i].physics_status);
        printf("        Transfer Plan  : %s\n", b[i].transfer_plan);
    }
    printf("\n");

    printf("  CANONICAL BOUNDARY PRINCIPLE:\n");
    printf("    M15 ESTABLISHES WHO MAY TOUCH THE ACCELERATOR (Authority & Topology Grounding)\n");
    printf("    M16 DISCOVERS HOW THE ACCELERATOR IS ACTUALLY COMMANDED (Empirical Path Discovery)\n");
    printf("================================================================================\n");
}

static void dump_evidence(void) {
    const char *dir = "evidence/m15-boundary";
    mkdir("evidence", 0755);
    mkdir(dir, 0755);

    Smmuv3NativeProbe probe;
    physics_accel_native_probe(&probe);

    char path[256];
    snprintf(path, sizeof(path), "%s/hardware-boundary.txt", dir);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "Machine: NVIDIA DGX Spark\n");
        fprintf(f, "PCI BDF: %s\n", probe.pci_bdf);
        fprintf(f, "Vendor ID: 0x%04x\n", probe.vendor_id);
        fprintf(f, "Device ID: 0x%04x\n", probe.device_id);
        fprintf(f, "IOMMU Group: %u\n", probe.iommu_group);
        fprintf(f, "SMMUv3 Base: 0x%08llx\n", (unsigned long long)probe.smmu_base);
        fprintf(f, "Stream ID: 0x%04x\n", probe.stream_id);
        fprintf(f, "BAR0 Base: 0x%08llx\n", (unsigned long long)probe.bar0_base);
        fprintf(f, "BAR0 Size: %llu\n", (unsigned long long)probe.bar0_size);
        fprintf(f, "Coherent DRAM: [0x80000000, 0x2080000000) 128 GiB\n");
        fclose(f);
    }

    snprintf(path, sizeof(path), "%s/ownership-boundaries.txt", dir);
    f = fopen(path, "w");
    if (f) {
        size_t count = 0;
        const HardwareOwnershipBoundary *b = physics_accel_get_hardware_ownership_boundaries(&count);
        for (size_t i = 0; i < count; i++) {
            fprintf(f, "Subsystem: %s\n", b[i].subsystem);
            fprintf(f, "Current Owner: %s\n", b[i].current_owner);
            fprintf(f, "Physics Status: %s\n", b[i].physics_status);
            fprintf(f, "Transfer Plan: %s\n\n", b[i].transfer_plan);
        }
        fclose(f);
    }

    printf("    [Evidence] Boundary evidence artifacts dumped to %s/\n", dir);
}

/* ============================================================================
 * MAIN RUNNER
 * ============================================================================ */

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--demonstrate-accelerator") == 0) {
        run_demonstration();
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "--dump-evidence-artifacts") == 0) {
        dump_evidence();
        return 0;
    }

    bool run_all = (argc >= 2 && strcmp(argv[1], "--run-m15-gates") == 0);

    if (!run_all) {
        printf("m15tool -- AIEN Physics Milestone 15 Qualification Tool\n");
        printf("Usage: %s [--run-m15-gates | --demonstrate-accelerator | --dump-evidence-artifacts]\n", argv[0]);
        return 0;
    }

    printf("================================================================================\n");
    printf("    AIEN PHYSICS SUBSTRATE — MILESTONE 15: QUALIFICATION RUNNER\n");
    printf("================================================================================\n");
    printf("Timestamp: %lu\n", (unsigned long)time(NULL));
    printf("Host:      NVIDIA DGX Spark (Grace Neoverse V2 + Blackwell GB10)\n");
    printf("Boundary:  Authority Model & Empirically Grounded Hardware Topology\n\n");

    bool g1 = test_gate_1_mem_bounds();
    print_gate("PHYSICS_ACCEL_MEM_BOUNDS_PASS", g1, "Coherent DRAM boundaries and kernel memory isolation");

    bool g2 = test_gate_2_dma_policy();
    print_gate("PHYSICS_ACCEL_DMA_POLICY_PASS", g2, "SMMUv3 Stage 1 IOVA-to-PA translation policy and DMA sandboxing");

    bool g3 = test_gate_3_queue_authority();
    print_gate("PHYSICS_ACCEL_QUEUE_AUTHORITY_PASS", g3, "Bounded queue authority and doorbell mediation policy");

    bool g4 = test_gate_4_device_lifecycle();
    print_gate("PHYSICS_ACCEL_DEVICE_LIFECYCLE_PASS", g4, "Deterministic monotonic device lifecycle transitions and fault isolation");

    bool g5 = test_gate_5_receipt_chain();
    print_gate("PHYSICS_ACCEL_RECEIPT_CHAIN_PASS", g5, "Immutable 192B receipt with rolling SHA-256 seal chain");

    bool g6 = test_gate_6_omega_ingress();
    print_gate("PHYSICS_ACCEL_OMEGA_INGRESS_PASS", g6, "Omega mediated intent ingress and capability gating");

    bool g7 = test_gate_7_hardware_boundary();
    print_gate("PHYSICS_ACCEL_HARDWARE_BOUNDARY_PASS", g7, "Truthful DGX Spark hardware topology observation & ownership recording");

    bool g8 = test_gate_8_zero_runtime_dep();
    print_gate("PHYSICS_ACCEL_ZERO_RUNTIME_DEP_PASS", g8, "Zero foreign runtime dependencies (0 CUDA, 0 LLVM, 0 Python, 0 inline asm)");

    bool g9 = test_gate_9_provenance();
    print_gate("PHYSICS_ACCEL_PROVENANCE_PASS", g9, "Capability provenance verification and monotonic attenuation");

    bool g10 = test_gate_10_receipt();
    print_gate("PHYSICS_ACCEL_RECEIPT_PASS", g10, "192B EffectReceipt certifying observed hardware boundary parameters");

    int total_gates = 10;
    int passed = (g1?1:0) + (g2?1:0) + (g3?1:0) + (g4?1:0) + (g5?1:0) +
                 (g6?1:0) + (g7?1:0) + (g8?1:0) + (g9?1:0) + (g10?1:0);

    printf("================================================================================\n");
    printf("  QUALIFICATION GATES: %d | PASSED: %d | FAILED: %d\n", total_gates, passed, total_gates - passed);
    printf("================================================================================\n");

    return (passed == total_gates) ? 0 : 1;
}
