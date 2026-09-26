#ifndef PHYSICS_ACCEL_H
#define PHYSICS_ACCEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PHYSICS_ACCEL_LINK_VERSION       0x00010000
#define PHYSICS_ACCEL_MAX_WINDOWS        16
#define PHYSICS_ACCEL_MAX_QUEUES         4
#define PHYSICS_ACCEL_RING_SLOTS         64
#define PHYSICS_ACCEL_SLOT_SIZE_BYTES    128

/* Resource Type for Accelerator Capability (M3 extension) */
#define RES_ACCELERATOR                  0x00000005

/* Accelerator Operation Bitmasks */
#define ACCEL_OP_PROBE                   0x00000001
#define ACCEL_OP_MAP_DMA                 0x00000002
#define ACCEL_OP_UNMAP_DMA               0x00000004
#define ACCEL_OP_ALLOC_QUEUE             0x00000008
#define ACCEL_OP_SUBMIT                  0x00000010
#define ACCEL_OP_SYNC                    0x00000020
#define ACCEL_OP_RESET                   0x00000040

/* DMA Permission Flags */
#define DMA_PERM_READ                    0x00000001
#define DMA_PERM_WRITE                   0x00000002
#define DMA_PERM_COHERENT                0x00000004

/* Decision Codes in EffectReceipt */
#define DEC_ADMITTED                     1
#define DEC_REJECTED                     2
#define DEC_REJECTED_STRUCTURAL          2
#define DEC_REJECTED_UNKNOWN_CAP         3
#define DEC_REJECTED_STALE_GENERATION    4
#define DEC_REJECTED_REVOKED             5
#define DEC_REJECTED_WRONG_PRINCIPAL     6
#define DEC_REJECTED_OPERATION           7
#define DEC_REJECTED_RESOURCE            8
#define DEC_REJECTED_BOUNDS              9
#define DEC_REJECTED_CONSTRAINT          10
#define DEC_REJECTED_REPLAY_CONFLICT     11
#define DEC_REJECTED_EXHAUSTED           12
#define DEC_EXECUTION_FAILED             13

/* SMMUv3 Fault Syndromes */
#define SMMU_FAULT_NONE                  0
#define SMMU_FAULT_TRANSLATION           1  /* F_TRANSLATION: unmapped IOVA */
#define SMMU_FAULT_PERMISSION            2  /* F_PERMISSION: write to read-only */
#define SMMU_FAULT_STREAM_DISABLED       3  /* Stream disabled in STE */
#define SMMU_FAULT_BAD_STE               4  /* C_BAD_STE: invalid STE format */
#define SMMU_FAULT_BAD_CD                5  /* C_BAD_CD: invalid Context Descriptor */

/* Accelerator Lifecycle States */
typedef enum {
    ACCEL_STATE_UNINITIALIZED = 0,
    ACCEL_STATE_PROBED        = 1,
    ACCEL_STATE_CONFIGURED    = 2,
    ACCEL_STATE_ACTIVE        = 3,
    ACCEL_STATE_FAULTED       = 4,
    ACCEL_STATE_RESETTING     = 5,
    ACCEL_STATE_RECOVERED     = 6
} AcceleratorDeviceState;

/* DMA Window Descriptor for SMMUv3 Translation Mapping */
typedef struct {
    uint64_t iova_base;
    uint64_t phys_base;
    uint64_t size_bytes;
    uint32_t permissions; /* DMA_PERM_* */
    uint32_t stream_id;
    uint32_t window_id;
    uint32_t reserved;
} DmaWindowDescriptor;

/* SMMUv3 Stream Configuration */
typedef struct {
    uint32_t stream_id;
    uint32_t ste_index;
    uint64_t cd_table_base;
    uint64_t ttbr0_base;
    uint64_t mair_value;
    uint32_t t0sz;
    uint32_t granule_size;
    bool stage1_enabled;
    bool fault_trap_enabled;
} Smmuv3StreamConfig;

/* Accelerator Capability Record */
typedef struct {
    uint32_t slot;
    uint32_t generation;
    uint64_t principal_id;
    uint32_t resource_type; /* RES_ACCELERATOR */
    uint32_t allowed_ops;   /* Bitmask of ACCEL_OP_* */
    uint64_t iova_bound_base;
    uint64_t iova_bound_size;
    uint32_t queue_id_mask;
    uint32_t revocation_state; /* 1 = ACTIVE, 2 = REVOKED */
    uint8_t  provenance_digest[32];
} AcceleratorCapability;

/* Canonical EffectIntent (64 bytes, little-endian, 64-bit aligned) */
typedef struct {
    uint16_t version;               /* 1 */
    uint16_t length;                /* 64 */
    uint32_t reserved0;             /* 0 */
    uint64_t request_id;            /* Caller-generated request ID */
    uint64_t principal_id;          /* Principal ID (e.g. Omega) */
    uint32_t capability_slot;       /* Capability slot index */
    uint32_t capability_generation; /* Capability generation */
    uint32_t resource_type;         /* RES_ACCELERATOR */
    uint32_t operation;             /* ACCEL_OP_* */
    uint64_t target_base;           /* Target IOVA or Queue ID */
    uint64_t target_size;           /* Byte span or command length */
    uint64_t param0;                /* Descriptor address or doorbell parameter */
} EffectIntent;

/* Canonical EffectReceipt (192 bytes, 64-bit aligned) */
typedef struct {
    uint16_t version;               /* 1 */
    uint16_t length;                /* 192 */
    uint32_t decision;              /* 1 = ADMITTED, 2 = REJECTED */
    uint32_t rejection_reason;      /* Specific reason code if rejected */
    uint32_t reserved0;             /* 0 */
    uint64_t request_id;            /* Mirrored from EffectIntent */
    uint8_t  intent_digest[32];     /* SHA-256 of 64-byte EffectIntent */
    uint64_t actual_effect;         /* Physical result or mapped address */
    uint64_t output;                /* Submitted packet count or status */
    uint32_t capability_slot;       /* Presented slot */
    uint32_t capability_generation; /* Presented generation */
    uint64_t machine_generation;    /* Monotonic machine counter */
    uint64_t measurement;           /* Timestamp / cycle measurement */
    uint8_t  previous_receipt_digest[32]; /* Rolling SHA-256 seal chain */
    uint8_t  receipt_digest[32];    /* SHA-256 seal over fields + previous */
    uint8_t  reserved1[32];         /* Padding to 192 bytes */
} EffectReceipt;

/* Command Queue State within Physics */
typedef struct {
    uint32_t queue_id;
    uint32_t head_index;
    uint32_t tail_index;
    uint32_t slot_count;
    uint64_t ring_phys_base;
    uint64_t doorbell_mmio_reg;
    bool     active;
    uint8_t  ring_memory[PHYSICS_ACCEL_RING_SLOTS * PHYSICS_ACCEL_SLOT_SIZE_BYTES];
} AcceleratorQueue;

/* Master Physics Accelerator Link Context */
typedef struct {
    uint32_t link_version;
    uint32_t device_state;          /* AcceleratorDeviceState */
    uint32_t stream_id;
    uint32_t active_queues;
    uint64_t mmio_doorbell_base;
    uint64_t mmio_doorbell_size;
    uint64_t coherent_dram_base;    /* 0x80000000 on DGX Spark */
    uint64_t coherent_dram_size;    /* 128 GiB on DGX Spark */
    uint64_t kernel_reserved_base;  /* Protected host kernel memory */
    uint64_t kernel_reserved_size;
    DmaWindowDescriptor active_windows[PHYSICS_ACCEL_MAX_WINDOWS];
    uint32_t active_window_count;
    AcceleratorQueue queues[PHYSICS_ACCEL_MAX_QUEUES];
    Smmuv3StreamConfig smmu_config;
    uint8_t  last_receipt_digest[32];
    uint64_t fault_count;
    uint64_t total_submissions;
    uint64_t machine_generation;
} PhysicsAcceleratorLink;

/* ========================================================================= */
/* Physics Server Authority APIs                                             */
/* ========================================================================= */

/* Initialize the Physics Accelerator Link with coherent DRAM boundaries */
int physics_accel_init(PhysicsAcceleratorLink *link,
                       uint64_t dram_base,
                       uint64_t dram_size,
                       uint64_t kernel_reserved_base,
                       uint64_t kernel_reserved_size);

/* Configure SMMUv3 Stream Table and Context Descriptors for accelerator */
int physics_accel_configure_smmu(PhysicsAcceleratorLink *link,
                                 const Smmuv3StreamConfig *config);

/* Validate accelerator capability token */
int physics_accel_validate_capability(const PhysicsAcceleratorLink *link,
                                      const AcceleratorCapability *cap,
                                      uint32_t required_op,
                                      uint64_t target_base,
                                      uint64_t target_size);

/* Grant a bounded DMA window and install SMMUv3 Stage 1 translation */
int physics_accel_grant_dma_window(PhysicsAcceleratorLink *link,
                                   const AcceleratorCapability *cap,
                                   const DmaWindowDescriptor *window,
                                   EffectReceipt *out_receipt);

/* Revoke a DMA window and invalidate SMMUv3 translation entries */
int physics_accel_revoke_dma_window(PhysicsAcceleratorLink *link,
                                    const AcceleratorCapability *cap,
                                    uint64_t iova_base,
                                    EffectReceipt *out_receipt);

/* Allocate a bounded command submission queue ring */
int physics_accel_alloc_queue(PhysicsAcceleratorLink *link,
                              const AcceleratorCapability *cap,
                              uint32_t queue_id,
                              EffectReceipt *out_receipt);

/* Mediate command packet validation, ring insertion, and doorbell ringing */
int physics_accel_submit_command(PhysicsAcceleratorLink *link,
                                 const AcceleratorCapability *cap,
                                 const EffectIntent *intent,
                                 EffectReceipt *out_receipt);

/* Perform SMMUv3 IOVA address translation and permission check */
int physics_accel_smmu_translate(const PhysicsAcceleratorLink *link,
                                 uint64_t iova,
                                 uint64_t size,
                                 uint32_t access_perms,
                                 uint64_t *out_phys_addr);

/* Trap and isolate an accelerator fault (SMMU abort, timeout, illegal insn) */
int physics_accel_handle_fault(PhysicsAcceleratorLink *link,
                               uint32_t fault_syndrome,
                               uint64_t fault_iova);

/* Perform non-disruptive device reset and recovery */
int physics_accel_reset_device(PhysicsAcceleratorLink *link,
                               const AcceleratorCapability *cap,
                               EffectReceipt *out_receipt);

/* Cryptographic receipt commit helper */
void physics_accel_commit_receipt(PhysicsAcceleratorLink *link,
                                  const EffectIntent *intent,
                                  uint32_t decision,
                                  uint32_t rejection_reason,
                                  uint64_t actual_effect,
                                  uint64_t output,
                                  EffectReceipt *out_receipt);

#endif /* PHYSICS_ACCEL_H */
