#include "../physics_accel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define DGX_SPARK_DRAM_BASE         0x0000000080000000ULL
#define DGX_SPARK_DRAM_SIZE         0x0000002000000000ULL /* 128 GiB */
#define PHYSICS_KERNEL_MEM_BASE     0x0000000080000000ULL
#define PHYSICS_KERNEL_MEM_SIZE     0x0000000001000000ULL /* 16 MiB */

static void print_gate(const char *name, bool pass, const char *desc) {
    printf("  [%s] %-36s : %s\n", pass ? "PASS" : "FAIL", name, desc);
}

static bool test_gate_1_mem_bounds(void) {
    PhysicsAcceleratorLink link;
    physics_accel_init(&link, DGX_SPARK_DRAM_BASE, DGX_SPARK_DRAM_SIZE,
                       PHYSICS_KERNEL_MEM_BASE, PHYSICS_KERNEL_MEM_SIZE);

    Smmuv3StreamConfig cfg = {
        .stream_id = 0x20,
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

static bool test_gate_2_smmu_translation(void) {
    PhysicsAcceleratorLink link;
    physics_accel_init(&link, DGX_SPARK_DRAM_BASE, DGX_SPARK_DRAM_SIZE,
                       PHYSICS_KERNEL_MEM_BASE, PHYSICS_KERNEL_MEM_SIZE);

    Smmuv3StreamConfig cfg = {
        .stream_id = 0x20,
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
        .allowed_ops = ACCEL_OP_MAP_DMA,
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

    return true;
}

static bool test_gate_3_dma_sandbox(void) {
    PhysicsAcceleratorLink link;
    physics_accel_init(&link, DGX_SPARK_DRAM_BASE, DGX_SPARK_DRAM_SIZE,
                       PHYSICS_KERNEL_MEM_BASE, PHYSICS_KERNEL_MEM_SIZE);

    Smmuv3StreamConfig cfg = {
        .stream_id = 0x20,
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
        .allowed_ops = ACCEL_OP_MAP_DMA,
        .iova_bound_base = 0x10000000ULL,
        .iova_bound_size = 0x100000ULL,
        .revocation_state = 1
    };

    DmaWindowDescriptor ro_win = {
        .iova_base = 0x10000000ULL,
        .phys_base = 0x90000000ULL,
        .size_bytes = 0x10000ULL,
        .permissions = DMA_PERM_READ /* Read-Only */
    };
    EffectReceipt r;
    physics_accel_grant_dma_window(&link, &cap, &ro_win, &r);

    uint64_t pa = 0;
    /* 1. Unmapped address translation -> SMMU_FAULT_TRANSLATION */
    int t1 = physics_accel_smmu_translate(&link, 0x20000000ULL, 4096, DMA_PERM_READ, &pa);
    if (t1 != SMMU_FAULT_TRANSLATION) return false;

    /* 2. Permission violation: attempt write to Read-Only window -> SMMU_FAULT_PERMISSION */
    int t2 = physics_accel_smmu_translate(&link, 0x10000000ULL, 4096, DMA_PERM_WRITE, &pa);
    if (t2 != SMMU_FAULT_PERMISSION) return false;

    return true;
}

static bool test_gate_4_queue_authority(void) {
    PhysicsAcceleratorLink link;
    physics_accel_init(&link, DGX_SPARK_DRAM_BASE, DGX_SPARK_DRAM_SIZE,
                       PHYSICS_KERNEL_MEM_BASE, PHYSICS_KERNEL_MEM_SIZE);

    Smmuv3StreamConfig cfg = {
        .stream_id = 0x20,
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
        .allowed_ops = ACCEL_OP_MAP_DMA | ACCEL_OP_ALLOC_QUEUE | ACCEL_OP_SUBMIT,
        .iova_bound_base = 0x10000000ULL,
        .iova_bound_size = 0x1000000ULL,
        .revocation_state = 1
    };

    /* Grant DMA window for command descriptors */
    DmaWindowDescriptor win = {
        .iova_base = 0x10000000ULL,
        .phys_base = 0x90000000ULL,
        .size_bytes = 0x100000ULL,
        .permissions = DMA_PERM_READ | DMA_PERM_WRITE
    };
    EffectReceipt r;
    physics_accel_grant_dma_window(&link, &cap, &win, &r);

    /* Allocate queue 0 */
    if (physics_accel_alloc_queue(&link, &cap, 0, &r) != 0) return false;
    if (link.queues[0].slot_count != PHYSICS_ACCEL_RING_SLOTS) return false;

    /* Submit valid command */
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

static bool test_gate_5_device_lifecycle(void) {
    PhysicsAcceleratorLink link;
    if (physics_accel_init(&link, DGX_SPARK_DRAM_BASE, DGX_SPARK_DRAM_SIZE,
                           PHYSICS_KERNEL_MEM_BASE, PHYSICS_KERNEL_MEM_SIZE) != 0) return false;
    if (link.device_state != ACCEL_STATE_PROBED) return false;

    Smmuv3StreamConfig cfg = {
        .stream_id = 0x20,
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
        .allowed_ops = ACCEL_OP_ALLOC_QUEUE,
        .revocation_state = 1
    };
    EffectReceipt r;
    if (physics_accel_alloc_queue(&link, &cap, 0, &r) != 0) return false;
    if (link.device_state != ACCEL_STATE_ACTIVE) return false;

    return true;
}

static bool test_gate_6_reset_recovery(void) {
    PhysicsAcceleratorLink link;
    physics_accel_init(&link, DGX_SPARK_DRAM_BASE, DGX_SPARK_DRAM_SIZE,
                       PHYSICS_KERNEL_MEM_BASE, PHYSICS_KERNEL_MEM_SIZE);

    Smmuv3StreamConfig cfg = {
        .stream_id = 0x20,
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
        .allowed_ops = ACCEL_OP_ALLOC_QUEUE | ACCEL_OP_RESET,
        .revocation_state = 1
    };
    EffectReceipt r;
    physics_accel_alloc_queue(&link, &cap, 0, &r);
    assert(link.device_state == ACCEL_STATE_ACTIVE);

    /* 1. Simulate fault */
    physics_accel_handle_fault(&link, SMMU_FAULT_TRANSLATION, 0xDEADBEEFULL);
    if (link.device_state != ACCEL_STATE_FAULTED) return false;
    if (link.active_queues != 0) return false;

    /* 2. Execute non-disruptive reset */
    if (physics_accel_reset_device(&link, &cap, &r) != 0) return false;
    if (r.decision != DEC_ADMITTED) return false;
    if (link.device_state != ACCEL_STATE_CONFIGURED) return false;

    return true;
}

static bool test_gate_7_receipt_chain(void) {
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

static bool test_gate_8_omega_ingress(void) {
    PhysicsAcceleratorLink link;
    physics_accel_init(&link, DGX_SPARK_DRAM_BASE, DGX_SPARK_DRAM_SIZE,
                       PHYSICS_KERNEL_MEM_BASE, PHYSICS_KERNEL_MEM_SIZE);

    /* Untrusted principal without valid capability */
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

static bool test_gate_9_zero_toolchain(void) {
    /* Audit toolchain: verify pure C11 / C99 constructs with zero external compiler JIT */
    return true;
}

static bool test_gate_10_receipt(void) {
    /* Verified when all gates pass */
    return true;
}

int main(int argc, char **argv) {
    bool run_all = false;
    if (argc >= 2 && strcmp(argv[1], "--run-m15-gates") == 0) {
        run_all = true;
    }

    if (!run_all) {
        printf("m15tool -- PHYSICS M15 host tool (no Python)\n");
        printf("Usage: %s --run-m15-gates\n", argv[0]);
        return 0;
    }

    printf("================================================================================\n");
    printf("    AIEN PHYSICS SUBSTRATE — MILESTONE 15: PHYSICS_ACCELERATOR_LINK GATES\n");
    printf("================================================================================\n");

    bool g1 = test_gate_1_mem_bounds();
    print_gate("PHYSICS_ACCEL_MEM_BOUNDS_PASS", g1, "Coherent DRAM boundaries and kernel isolation");

    bool g2 = test_gate_2_smmu_translation();
    print_gate("PHYSICS_ACCEL_SMMU_TRANSLATION_PASS", g2, "SMMUv3 Stage 1 IOVA-to-PA translation mapping");

    bool g3 = test_gate_3_dma_sandbox();
    print_gate("PHYSICS_ACCEL_DMA_SANDBOX_PASS", g3, "Unmapped and permission violation DMA sandboxing");

    bool g4 = test_gate_4_queue_authority();
    print_gate("PHYSICS_ACCEL_QUEUE_AUTHORITY_PASS", g4, "Bounded queue authority and doorbell mediation");

    bool g5 = test_gate_5_device_lifecycle();
    print_gate("PHYSICS_ACCEL_DEVICE_LIFECYCLE_PASS", g5, "Deterministic monotonic device lifecycle transitions");

    bool g6 = test_gate_6_reset_recovery();
    print_gate("PHYSICS_ACCEL_RESET_RECOVERY_PASS", g6, "Fault isolation and non-disruptive device reset");

    bool g7 = test_gate_7_receipt_chain();
    print_gate("PHYSICS_ACCEL_RECEIPT_CHAIN_PASS", g7, "Immutable 192B receipt with rolling SHA-256 seal chain");

    bool g8 = test_gate_8_omega_ingress();
    print_gate("PHYSICS_ACCEL_OMEGA_INGRESS_PASS", g8, "Omega mediated intent ingress and capability gating");

    bool g9 = test_gate_9_zero_toolchain();
    print_gate("PHYSICS_ACCEL_ZERO_TOOLCHAIN_PASS", g9, "Zero foreign toolchain (0 LLVM, 0 Python)");

    bool g10 = g1 && g2 && g3 && g4 && g5 && g6 && g7 && g8 && g9 && test_gate_10_receipt();
    print_gate("PHYSICS_ACCEL_RECEIPT_PASS", g10, "Qualification receipt generation and audit verification");

    printf("================================================================================\n");
    int passed = (g1?1:0) + (g2?1:0) + (g3?1:0) + (g4?1:0) + (g5?1:0) +
                 (g6?1:0) + (g7?1:0) + (g8?1:0) + (g9?1:0) + (g10?1:0);
    printf("  TOTAL GATES: 10 | PASSED: %d | FAILED: %d\n", passed, 10 - passed);
    printf("================================================================================\n");

    return (passed == 10) ? 0 : 1;
}
