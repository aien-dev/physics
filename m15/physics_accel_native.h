#ifndef PHYSICS_ACCEL_NATIVE_H
#define PHYSICS_ACCEL_NATIVE_H

#include "physics_accel.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define SMMU_V3_1_AUTO_BASE         0x13000000ULL
#define SMMU_V3_1_AUTO_SIZE         0x00020000ULL /* 128 KiB */
#define GB10_STREAM_ID              0x0100 /* 256 decimal: Segment 15 BDF 01:00.0 */
#define GB10_PCI_BDF_STRING         "000f:01:00.0"
#define GB10_PCI_VENDOR_ID          0x10de
#define GB10_PCI_DEVICE_ID          0x2e12
#define GB10_BAR0_BASE              0x24000000ULL
#define GB10_BAR0_SIZE              0x04000000ULL /* 64 MiB */
#define DGX_SPARK_DRAM_BASE         0x0000000080000000ULL
#define DGX_SPARK_DRAM_SIZE         0x0000002000000000ULL /* 128 GiB */

/* ========================================================================= */
/* 1. HARDWARE OBSERVATION: Live Host Device Probe Record                     */
/* ========================================================================= */

typedef struct {
    bool     present;
    bool     smmu_matched;
    bool     device_matched;
    uint32_t stream_id;
    uint64_t smmu_base;
    char     smmu_name[64];
    uint64_t bar0_base;
    uint64_t bar0_size;
    uint32_t iommu_group;
    char     pci_bdf[32];
    uint16_t vendor_id;
    uint16_t device_id;
    char     device_name[64];
    bool     coherent_unified_dram;
} Smmuv3NativeProbe;

/* ========================================================================= */
/* 2. HARDWARE OWNERSHIP BOUNDARIES: Explicit Subsystem Governance           */
/* ========================================================================= */

typedef struct {
    const char *subsystem;
    const char *current_owner;
    const char *physics_status;
    const char *transfer_plan;
} HardwareOwnershipBoundary;

/* ========================================================================= */
/* 3. CORE ROUTINES & CLASSIFICATIONS                                        */
/* ========================================================================= */

/* CLASSIFICATION: HARDWARE OBSERVATION
 * Inspects live Linux sysfs for physical device, SMMUv3 base, BDF, and BAR0.
 * Performs NO register writes and makes NO unproven architectural claims. */
int physics_accel_native_probe(Smmuv3NativeProbe *out_probe);

/* CLASSIFICATION: HARDWARE BOUNDARY CHARACTERIZATION
 * Verifies live DGX Spark accelerator topology against physical reality. */
int physics_accel_native_verify_hardware_boundary(Smmuv3NativeProbe *out_probe);

/* CLASSIFICATION: HARDWARE OWNERSHIP BOUNDARY RECORD
 * Returns canonical table documenting active subsystem ownership. */
const HardwareOwnershipBoundary *physics_accel_get_hardware_ownership_boundaries(size_t *out_count);

/* CLASSIFICATION: HARDWARE BOUNDARY RECEIPT COMMIT
 * Commits formal 192-byte EffectReceipt binding measured hardware topology. */
int physics_accel_native_commit_boundary_receipt(PhysicsAcceleratorLink *link,
                                                const Smmuv3NativeProbe *probe,
                                                EffectReceipt *out_receipt);

/* ========================================================================= */
/* 4. MODEL / DISCOVERY PLACEHOLDERS (Intentionally deferred to M16)         */
/* The following functions represent software simulations or discovery       */
/* scaffolding. They are EXPLICITLY NOT QUALIFIED AS PHYSICAL CONTROL.       */
/* Native Blackwell submission protocol is the subject of Milestone 16.      */
/* ========================================================================= */

typedef struct {
    uint64_t iova;
    uint64_t phys_base;
    uint64_t size_bytes;
    uint32_t permissions;
    bool     coherent;
    void    *vaddr;
} DmaModelMapping;

typedef struct {
    uint64_t ring_phys_base;
    uint8_t *ring_vaddr;
    uint32_t slot_count;
    uint32_t slot_size;
    uint32_t head_index;
    uint32_t tail_index;
    bool     active;
} ModelCommandRing;

/* CLASSIFICATION: MODEL / PLACEHOLDER (Hardware translation owned by Linux SMMU driver) */
int physics_accel_model_map_dma(const Smmuv3NativeProbe *probe,
                                uint64_t iova,
                                uint64_t size_bytes,
                                uint32_t perms,
                                DmaModelMapping *out_map);

/* CLASSIFICATION: MODEL / PLACEHOLDER */
int physics_accel_model_unmap_dma(DmaModelMapping *map);

/* CLASSIFICATION: MODEL / PLACEHOLDER (Queue layout deferred to M16) */
int physics_accel_model_ring_init(DmaModelMapping *map,
                                  ModelCommandRing *out_ring);

/* CLASSIFICATION: MODEL / PLACEHOLDER (No guessed MMIO writes in M15) */
int physics_accel_model_ring_doorbell(uint64_t doorbell_phys_reg, uint32_t token);

/* CLASSIFICATION: MODEL / PLACEHOLDER (Completion mechanism deferred to M16) */
int physics_accel_model_observe_completion(ModelCommandRing *ring);

/* CLASSIFICATION: MODEL / PLACEHOLDER (Raw reset registers deferred to M16) */
int physics_accel_model_reset(Smmuv3NativeProbe *probe,
                              ModelCommandRing *ring,
                              DmaModelMapping *map);

#endif /* PHYSICS_ACCEL_NATIVE_H */
