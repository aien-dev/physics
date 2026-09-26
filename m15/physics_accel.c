#include "physics_accel.h"
#include <string.h>
#include <stdatomic.h>

/* Forward declaration from sha256_clean.c */
void sha256_compute(const uint8_t *data, uint64_t len, uint8_t scratch_buf[128], uint32_t out_digest[8]);

/* Internal helper to compute SHA-256 and store canonical big-endian 32-byte digest */
static void compute_sha256_be(const void *data, uint64_t len, uint8_t out[32]) {
    uint8_t scratch[128] __attribute__((aligned(16)));
    uint32_t st[8];
    sha256_compute((const uint8_t *)data, len, scratch, st);
    for (int i = 0; i < 8; i++) {
        out[4 * i + 0] = (uint8_t)(st[i] >> 24);
        out[4 * i + 1] = (uint8_t)(st[i] >> 16);
        out[4 * i + 2] = (uint8_t)(st[i] >> 8);
        out[4 * i + 3] = (uint8_t)(st[i]);
    }
}

int physics_accel_init(PhysicsAcceleratorLink *link,
                       uint64_t dram_base,
                       uint64_t dram_size,
                       uint64_t kernel_reserved_base,
                       uint64_t kernel_reserved_size) {
    if (!link) return -1;
    memset(link, 0, sizeof(*link));

    link->link_version = PHYSICS_ACCEL_LINK_VERSION;
    link->device_state = ACCEL_STATE_PROBED;
    link->stream_id = 0x0100; /* Observed hardware Stream ID for GB10 (000f:01:00.0) on DGX Spark */
    link->coherent_dram_base = dram_base;
    link->coherent_dram_size = dram_size;
    link->kernel_reserved_base = kernel_reserved_base;
    link->kernel_reserved_size = kernel_reserved_size;
    link->mmio_doorbell_base = 0x24000000; /* Hardware BAR0 base */
    link->mmio_doorbell_size = 0x04000000; /* 64 MiB */
    link->machine_generation = 1;

    /* Initialize rolling cryptographic receipt chain with genesis zero */
    memset(link->last_receipt_digest, 0, 32);
    return 0;
}

int physics_accel_configure_smmu(PhysicsAcceleratorLink *link,
                                 const Smmuv3StreamConfig *config) {
    if (!link || !config) return -1;
    if (link->device_state != ACCEL_STATE_PROBED &&
        link->device_state != ACCEL_STATE_RECOVERED &&
        link->device_state != ACCEL_STATE_CONFIGURED) {
        return -2;
    }

    /* Validate SMMU geometry */
    if (config->granule_size != 4096) return -3; /* Mandate 4 KiB granule */
    if (config->t0sz != 16 && config->t0sz != 24) return -4; /* 48-bit or 40-bit IOVA */

    link->smmu_config = *config;
    link->stream_id = config->stream_id;
    link->device_state = ACCEL_STATE_CONFIGURED;
    return 0;
}

int physics_accel_validate_capability(const PhysicsAcceleratorLink *link,
                                      const AcceleratorCapability *cap,
                                      uint32_t required_op,
                                      uint64_t target_base,
                                      uint64_t target_size) {
    if (!link || !cap) return DEC_REJECTED_STRUCTURAL;
    if (cap->resource_type != RES_ACCELERATOR) return DEC_REJECTED_RESOURCE;
    if (cap->revocation_state != 1) return DEC_REJECTED_REVOKED;
    if ((cap->allowed_ops & required_op) == 0) return DEC_REJECTED_OPERATION;

    /* Check bounds if applicable */
    if (target_size > 0) {
        if (target_base < cap->iova_bound_base) return DEC_REJECTED_BOUNDS;
        if ((target_base + target_size) > (cap->iova_bound_base + cap->iova_bound_size)) {
            return DEC_REJECTED_BOUNDS;
        }
    }

    return DEC_ADMITTED;
}

void physics_accel_commit_receipt_measured(PhysicsAcceleratorLink *link,
                                           const EffectIntent *intent,
                                           uint32_t decision,
                                           uint32_t rejection_reason,
                                           uint64_t actual_effect,
                                           uint64_t output,
                                           uint64_t measurement,
                                           EffectReceipt *out_receipt) {
    if (!out_receipt) return;
    memset(out_receipt, 0, sizeof(*out_receipt));

    out_receipt->version = 1;
    out_receipt->length = sizeof(EffectReceipt);
    out_receipt->decision = decision;
    out_receipt->rejection_reason = rejection_reason;
    out_receipt->request_id = intent ? intent->request_id : 0;

    if (intent) {
        compute_sha256_be(intent, sizeof(EffectIntent), out_receipt->intent_digest);
        out_receipt->capability_slot = intent->capability_slot;
        out_receipt->capability_generation = intent->capability_generation;
    }

    out_receipt->actual_effect = actual_effect;
    out_receipt->output = output;
    out_receipt->machine_generation = link ? ++link->machine_generation : 0;
    out_receipt->measurement = measurement;

    if (link) {
        memcpy(out_receipt->previous_receipt_digest, link->last_receipt_digest, 32);
    }

    /* Compute seal over bytes 0..127 + previous_receipt_digest */
    uint8_t hash_input[160];
    memcpy(hash_input, out_receipt, 128);
    memcpy(hash_input + 128, out_receipt->previous_receipt_digest, 32);
    compute_sha256_be(hash_input, 160, out_receipt->receipt_digest);

    /* Update rolling chain in link */
    if (link) {
        memcpy(link->last_receipt_digest, out_receipt->receipt_digest, 32);
    }
}

void physics_accel_commit_receipt(PhysicsAcceleratorLink *link,
                                  const EffectIntent *intent,
                                  uint32_t decision,
                                  uint32_t rejection_reason,
                                  uint64_t actual_effect,
                                  uint64_t output,
                                  EffectReceipt *out_receipt) {
    uint64_t gen = link ? (link->machine_generation + 1) : 0;
    uint64_t synthetic_meas = 1000 + gen;
    physics_accel_commit_receipt_measured(link, intent, decision, rejection_reason,
                                          actual_effect, output, synthetic_meas, out_receipt);
}

int physics_accel_grant_dma_window(PhysicsAcceleratorLink *link,
                                   const AcceleratorCapability *cap,
                                   const DmaWindowDescriptor *window,
                                   EffectReceipt *out_receipt) {
    if (!link || !window) return -1;

    EffectIntent intent;
    memset(&intent, 0, sizeof(intent));
    intent.version = 1;
    intent.length = sizeof(intent);
    intent.request_id = 101;
    intent.principal_id = cap ? cap->principal_id : 0;
    intent.capability_slot = cap ? cap->slot : 0;
    intent.capability_generation = cap ? cap->generation : 0;
    intent.resource_type = RES_ACCELERATOR;
    intent.operation = ACCEL_OP_MAP_DMA;
    intent.target_base = window->iova_base;
    intent.target_size = window->size_bytes;
    intent.param0 = window->phys_base;

    int val = physics_accel_validate_capability(link, cap, ACCEL_OP_MAP_DMA,
                                                window->iova_base, window->size_bytes);
    if (val != DEC_ADMITTED) {
        physics_accel_commit_receipt(link, &intent, DEC_REJECTED, val, 0, 0, out_receipt);
        return -1;
    }

    /* Verify device state */
    if (link->device_state != ACCEL_STATE_CONFIGURED &&
        link->device_state != ACCEL_STATE_ACTIVE) {
        physics_accel_commit_receipt(link, &intent, DEC_REJECTED, DEC_REJECTED_STRUCTURAL, 0, 0, out_receipt);
        return -2;
    }

    /* Verify physical memory bounds against DRAM envelope */
    if (window->phys_base < link->coherent_dram_base ||
        (window->phys_base + window->size_bytes) > (link->coherent_dram_base + link->coherent_dram_size)) {
        physics_accel_commit_receipt(link, &intent, DEC_REJECTED, DEC_REJECTED_BOUNDS, 0, 0, out_receipt);
        return -3;
    }

    /* Verify physical memory does NOT overlap protected Physics kernel memory */
    if (window->phys_base < (link->kernel_reserved_base + link->kernel_reserved_size) &&
        (window->phys_base + window->size_bytes) > link->kernel_reserved_base) {
        physics_accel_commit_receipt(link, &intent, DEC_REJECTED, DEC_REJECTED_CONSTRAINT, 0, 0, out_receipt);
        return -4;
    }

    /* Check window capacity */
    if (link->active_window_count >= PHYSICS_ACCEL_MAX_WINDOWS) {
        physics_accel_commit_receipt(link, &intent, DEC_REJECTED, DEC_REJECTED_EXHAUSTED, 0, 0, out_receipt);
        return -5;
    }

    /* Check for overlapping IOVA windows */
    for (uint32_t i = 0; i < link->active_window_count; i++) {
        const DmaWindowDescriptor *w = &link->active_windows[i];
        if (window->iova_base < (w->iova_base + w->size_bytes) &&
            (window->iova_base + window->size_bytes) > w->iova_base) {
            physics_accel_commit_receipt(link, &intent, DEC_REJECTED, DEC_REJECTED_REPLAY_CONFLICT, 0, 0, out_receipt);
            return -6;
        }
    }

    /* Register active window */
    DmaWindowDescriptor *new_win = &link->active_windows[link->active_window_count++];
    *new_win = *window;
    new_win->window_id = link->active_window_count;
    new_win->stream_id = link->stream_id;

    physics_accel_commit_receipt(link, &intent, DEC_ADMITTED, 0, window->phys_base, window->size_bytes, out_receipt);
    return 0;
}

int physics_accel_revoke_dma_window(PhysicsAcceleratorLink *link,
                                    const AcceleratorCapability *cap,
                                    uint64_t iova_base,
                                    EffectReceipt *out_receipt) {
    if (!link) return -1;

    EffectIntent intent;
    memset(&intent, 0, sizeof(intent));
    intent.version = 1;
    intent.length = sizeof(intent);
    intent.request_id = 102;
    intent.principal_id = cap ? cap->principal_id : 0;
    intent.capability_slot = cap ? cap->slot : 0;
    intent.capability_generation = cap ? cap->generation : 0;
    intent.resource_type = RES_ACCELERATOR;
    intent.operation = ACCEL_OP_UNMAP_DMA;
    intent.target_base = iova_base;

    int val = physics_accel_validate_capability(link, cap, ACCEL_OP_UNMAP_DMA, 0, 0);
    if (val != DEC_ADMITTED) {
        physics_accel_commit_receipt(link, &intent, DEC_REJECTED, val, 0, 0, out_receipt);
        return -1;
    }

    int found_idx = -1;
    for (uint32_t i = 0; i < link->active_window_count; i++) {
        if (link->active_windows[i].iova_base == iova_base) {
            found_idx = (int)i;
            break;
        }
    }

    if (found_idx < 0) {
        physics_accel_commit_receipt(link, &intent, DEC_REJECTED, DEC_REJECTED_BOUNDS, 0, 0, out_receipt);
        return -2;
    }

    /* Shift remaining windows */
    for (uint32_t i = (uint32_t)found_idx; i + 1 < link->active_window_count; i++) {
        link->active_windows[i] = link->active_windows[i + 1];
    }
    link->active_window_count--;

    physics_accel_commit_receipt(link, &intent, DEC_ADMITTED, 0, iova_base, 0, out_receipt);
    return 0;
}

int physics_accel_alloc_queue(PhysicsAcceleratorLink *link,
                              const AcceleratorCapability *cap,
                              uint32_t queue_id,
                              EffectReceipt *out_receipt) {
    if (!link) return -1;

    EffectIntent intent;
    memset(&intent, 0, sizeof(intent));
    intent.version = 1;
    intent.length = sizeof(intent);
    intent.request_id = 103;
    intent.principal_id = cap ? cap->principal_id : 0;
    intent.capability_slot = cap ? cap->slot : 0;
    intent.capability_generation = cap ? cap->generation : 0;
    intent.resource_type = RES_ACCELERATOR;
    intent.operation = ACCEL_OP_ALLOC_QUEUE;
    intent.target_base = queue_id;

    int val = physics_accel_validate_capability(link, cap, ACCEL_OP_ALLOC_QUEUE, 0, 0);
    if (val != DEC_ADMITTED) {
        physics_accel_commit_receipt(link, &intent, DEC_REJECTED, val, 0, 0, out_receipt);
        return -1;
    }

    if (queue_id >= PHYSICS_ACCEL_MAX_QUEUES) {
        physics_accel_commit_receipt(link, &intent, DEC_REJECTED, DEC_REJECTED_BOUNDS, 0, 0, out_receipt);
        return -2;
    }

    AcceleratorQueue *q = &link->queues[queue_id];
    if (q->active) {
        physics_accel_commit_receipt(link, &intent, DEC_REJECTED, DEC_REJECTED_REPLAY_CONFLICT, 0, 0, out_receipt);
        return -3;
    }

    q->queue_id = queue_id;
    q->head_index = 0;
    q->tail_index = 0;
    q->slot_count = PHYSICS_ACCEL_RING_SLOTS;
    q->ring_phys_base = link->coherent_dram_base + 0x100000 + (queue_id * 0x10000);
    q->doorbell_mmio_reg = link->mmio_doorbell_base + (queue_id * 0x1000);
    q->active = true;
    memset(q->ring_memory, 0, sizeof(q->ring_memory));

    link->active_queues++;
    link->device_state = ACCEL_STATE_ACTIVE;

    physics_accel_commit_receipt(link, &intent, DEC_ADMITTED, 0, q->ring_phys_base, q->slot_count, out_receipt);
    return 0;
}

int physics_accel_smmu_translate(const PhysicsAcceleratorLink *link,
                                 uint64_t iova,
                                 uint64_t size,
                                 uint32_t access_perms,
                                 uint64_t *out_phys_addr) {
    if (!link || size == 0) return SMMU_FAULT_BAD_STE;

    if (!link->smmu_config.stage1_enabled) {
        return SMMU_FAULT_STREAM_DISABLED;
    }

    for (uint32_t i = 0; i < link->active_window_count; i++) {
        const DmaWindowDescriptor *w = &link->active_windows[i];
        if (iova >= w->iova_base && (iova + size) <= (w->iova_base + w->size_bytes)) {
            /* Check permission match */
            if ((access_perms & DMA_PERM_WRITE) && !(w->permissions & DMA_PERM_WRITE)) {
                return SMMU_FAULT_PERMISSION;
            }
            if (out_phys_addr) {
                uint64_t offset = iova - w->iova_base;
                *out_phys_addr = w->phys_base + offset;
            }
            return SMMU_FAULT_NONE;
        }
    }

    return SMMU_FAULT_TRANSLATION;
}

int physics_accel_submit_command(PhysicsAcceleratorLink *link,
                                 const AcceleratorCapability *cap,
                                 const EffectIntent *intent,
                                 EffectReceipt *out_receipt) {
    if (!link || !intent) return -1;

    int val = physics_accel_validate_capability(link, cap, ACCEL_OP_SUBMIT, 0, 0);
    if (val != DEC_ADMITTED) {
        physics_accel_commit_receipt(link, intent, DEC_REJECTED, val, 0, 0, out_receipt);
        return -1;
    }

    if (link->device_state != ACCEL_STATE_ACTIVE) {
        physics_accel_commit_receipt(link, intent, DEC_REJECTED, DEC_REJECTED_STRUCTURAL, 0, 0, out_receipt);
        return -2;
    }

    uint32_t q_id = (uint32_t)intent->target_base;
    if (q_id >= PHYSICS_ACCEL_MAX_QUEUES || !link->queues[q_id].active) {
        physics_accel_commit_receipt(link, intent, DEC_REJECTED, DEC_REJECTED_BOUNDS, 0, 0, out_receipt);
        return -3;
    }

    /* Translate and validate command descriptor IOVA */
    uint64_t phys_cmd = 0;
    int trans = physics_accel_smmu_translate(link, intent->param0, intent->target_size,
                                             DMA_PERM_READ, &phys_cmd);
    if (trans != SMMU_FAULT_NONE) {
        /* SMMU fault triggered */
        physics_accel_handle_fault(link, trans, intent->param0);
        physics_accel_commit_receipt(link, intent, DEC_REJECTED, DEC_REJECTED_CONSTRAINT, 0, 0, out_receipt);
        return -4;
    }

    AcceleratorQueue *q = &link->queues[q_id];
    uint32_t slot = q->tail_index;
    q->tail_index = (q->tail_index + 1) % q->slot_count;

    /* Write command token into ring memory */
    uint8_t *slot_ptr = q->ring_memory + (slot * PHYSICS_ACCEL_SLOT_SIZE_BYTES);
    memcpy(slot_ptr, &phys_cmd, sizeof(phys_cmd));
    memcpy(slot_ptr + 8, &intent->target_size, sizeof(intent->target_size));

    /* Sequential consistency memory barrier ensuring descriptor visibility */
    atomic_thread_fence(memory_order_seq_cst);

    link->total_submissions++;
    physics_accel_commit_receipt(link, intent, DEC_ADMITTED, 0, phys_cmd, slot, out_receipt);
    return 0;
}

int physics_accel_handle_fault(PhysicsAcceleratorLink *link,
                               uint32_t fault_syndrome,
                               uint64_t fault_iova) {
    (void)fault_iova;
    if (!link) return -1;

    link->device_state = ACCEL_STATE_FAULTED;
    link->fault_count++;

    /* Isolate all active queues */
    for (uint32_t i = 0; i < PHYSICS_ACCEL_MAX_QUEUES; i++) {
        if (link->queues[i].active) {
            link->queues[i].active = false;
        }
    }
    link->active_queues = 0;

    return (int)fault_syndrome;
}

int physics_accel_reset_device(PhysicsAcceleratorLink *link,
                               const AcceleratorCapability *cap,
                               EffectReceipt *out_receipt) {
    if (!link) return -1;

    EffectIntent intent;
    memset(&intent, 0, sizeof(intent));
    intent.version = 1;
    intent.length = sizeof(intent);
    intent.request_id = 104;
    intent.principal_id = cap ? cap->principal_id : 0;
    intent.capability_slot = cap ? cap->slot : 0;
    intent.capability_generation = cap ? cap->generation : 0;
    intent.resource_type = RES_ACCELERATOR;
    intent.operation = ACCEL_OP_RESET;

    int val = physics_accel_validate_capability(link, cap, ACCEL_OP_RESET, 0, 0);
    if (val != DEC_ADMITTED) {
        physics_accel_commit_receipt(link, &intent, DEC_REJECTED, val, 0, 0, out_receipt);
        return -1;
    }

    link->device_state = ACCEL_STATE_RESETTING;

    /* Reset software rings and queues */
    for (uint32_t i = 0; i < PHYSICS_ACCEL_MAX_QUEUES; i++) {
        link->queues[i].active = false;
        link->queues[i].head_index = 0;
        link->queues[i].tail_index = 0;
        memset(link->queues[i].ring_memory, 0, sizeof(link->queues[i].ring_memory));
    }
    link->active_queues = 0;

    /* Clear windows */
    link->active_window_count = 0;
    memset(link->active_windows, 0, sizeof(link->active_windows));

    /* State transition: RECOVERED -> CONFIGURED */
    link->device_state = ACCEL_STATE_RECOVERED;
    link->device_state = ACCEL_STATE_CONFIGURED;

    physics_accel_commit_receipt(link, &intent, DEC_ADMITTED, 0, 0x1, 0, out_receipt);
    return 0;
}
