#ifndef PHYSICS_ACCEL_NATIVE_H
#define PHYSICS_ACCEL_NATIVE_H

#include "physics_accel.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define SMMU_V3_1_AUTO_BASE         0x13000000ULL
#define SMMU_V3_1_AUTO_SIZE         0x00020000ULL /* 128 KiB */
#define SMMU_V3_FEATURE_MASK        0x0196dfbfULL
#define SMMU_V3_OAS_BITS            40
#define GB10_STREAM_ID              0x0100 /* 256 decimal: Segment 15 BDF 01:00.0 */
#define GB10_PCI_BDF_STRING         "000f:01:00.0"
#define GB10_PCI_VENDOR_ID          0x10de
#define GB10_PCI_DEVICE_ID          0x2e12
#define GB10_BAR0_BASE              0x24000000ULL
#define GB10_BAR0_SIZE              0x04000000ULL /* 64 MiB */
#define GB10_DOORBELL_REG_OFFSET    0x00000040ULL

/* Native hardware probe results */
typedef struct {
    bool     present;
    bool     smmu_matched;
    bool     device_matched;
    uint32_t stream_id;
    uint64_t smmu_base;
    uint64_t bar0_base;
    uint64_t bar0_size;
    uint32_t iommu_group;
    uint32_t smmu_features;
    uint32_t oas_bits;
    char     pci_location[32];
} Smmuv3NativeProbe;

/* Native DMA mapping record */
typedef struct {
    uint64_t iova;
    uint64_t phys_base;
    uint64_t size_bytes;
    uint32_t permissions;
    bool     coherent;
    void    *vaddr;
} DmaHardwareMapping;

/* Native Hardware Command Ring */
typedef struct {
    uint64_t ring_phys_base;
    uint8_t *ring_vaddr;
    uint32_t slot_count;
    uint32_t slot_size;
    uint32_t head_index;
    uint32_t tail_index;
    bool     active;
} HardwareCommandRing;

/* Native Hardware Doorbell Context */
typedef struct {
    uint64_t mmio_phys_reg;
    volatile uint32_t *mmio_vaddr;
    uint64_t total_doorbell_writes;
    uint32_t last_token;
} HardwareDoorbell;

/* Native Hardware Completion Telemetry */
typedef struct {
    uint64_t start_cycles;
    uint64_t end_cycles;
    uint64_t elapsed_cycles;
    uint32_t completion_status;
    bool     observed_completion;
} HardwareCompletion;

/* Native Hardware Reset Outcome */
typedef struct {
    uint64_t reset_timestamp;
    uint32_t device_state_before;
    uint32_t device_state_after;
    bool     smmu_stream_isolated;
    bool     queues_revoked;
    bool     windows_unmapped;
    bool     hardware_idle_confirmed;
} HardwareResetOutcome;

/* Master Native Hardware Seam Execution Context */
typedef struct {
    Smmuv3NativeProbe    probe;
    DmaHardwareMapping   dma_window;
    uint64_t             initial_mapped_phys_base;
    uint64_t             initial_mapped_size;
    HardwareCommandRing  ring;
    HardwareDoorbell     doorbell;
    HardwareCompletion   completion;
    HardwareResetOutcome reset_outcome;
    bool                 proof_chain_passed;
} NativeHardwareSeamContext;

/* Core Native Seam Routines */
int physics_accel_native_probe(Smmuv3NativeProbe *out_probe);
int physics_accel_native_map_dma(const Smmuv3NativeProbe *probe,
                                 uint64_t iova,
                                 uint64_t size_bytes,
                                 uint32_t perms,
                                 DmaHardwareMapping *out_map);
int physics_accel_native_unmap_dma(DmaHardwareMapping *map);
int physics_accel_native_ring_init(DmaHardwareMapping *map,
                                   HardwareCommandRing *out_ring);
int physics_accel_native_submit_packet(HardwareCommandRing *ring,
                                       const void *cmd_bytes,
                                       uint32_t cmd_len,
                                       uint32_t *out_slot);
int physics_accel_native_ring_doorbell(HardwareDoorbell *doorbell,
                                      uint32_t token);
int physics_accel_native_observe_completion(HardwareCommandRing *ring,
                                            HardwareCompletion *out_comp);
int physics_accel_native_reset(Smmuv3NativeProbe *probe,
                               HardwareCommandRing *ring,
                               DmaHardwareMapping *map,
                               HardwareResetOutcome *out_reset);
int physics_accel_native_execute_proof_chain(PhysicsAcceleratorLink *link,
                                             NativeHardwareSeamContext *out_ctx,
                                             EffectReceipt *out_receipt);

#endif /* PHYSICS_ACCEL_NATIVE_H */
