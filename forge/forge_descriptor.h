#ifndef FORGE_DESCRIPTOR_H
#define FORGE_DESCRIPTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FORGE_DESC_BDF_LEN        16
#define FORGE_DESC_UUID_LEN       16
#define FORGE_DESC_VER_LEN        32
#define FORGE_DESC_DIGEST_LEN     32
#define FORGE_DESC_SERIALIZED_LEN 208

/* Feature flags derived strictly from verified accelerator objects */
#define FORGE_FEATURE_UNIFIED_MEMORY (1U << 0)
#define FORGE_FEATURE_COMPUTE_B      (1U << 1)
#define FORGE_FEATURE_GPFIFO_A       (1U << 2)
#define FORGE_FEATURE_USERMODE_A     (1U << 3)
#define FORGE_FEATURE_PAGE_FAULTING  (1U << 4)

/* Explicit observation metadata flags: records exactly what was observed vs derived */
#define FORGE_OBS_PCI_IDENTITY       (1U << 0)
#define FORGE_OBS_GPU_UUID           (1U << 1)
#define FORGE_OBS_RM_COMPUTE_CLASS   (1U << 2)
#define FORGE_OBS_RM_SM_VERSION      (1U << 3)
#define FORGE_OBS_WARP_SIZE          (1U << 4)
#define FORGE_OBS_WARPS_PER_SM       (1U << 5)
#define FORGE_OBS_CORE_COUNT         (1U << 6)
#define FORGE_OBS_SM_PER_TPC         (1U << 7)
#define FORGE_OBS_DRIVER_VERSION     (1U << 8)
#define FORGE_OBS_VBIOS_VERSION      (1U << 9)
#define FORGE_OBS_FIRMWARE_VERSION   (1U << 10)
#define FORGE_OBS_TOTAL_MEMORY       (1U << 11)
#define FORGE_OBS_DMA_MASK           (1U << 12)
#define FORGE_OBS_PAGE_SIZE          (1U << 13)
#define FORGE_DERIVED_THREADS_PER_BLK (1U << 14)
#define FORGE_DERIVED_SHARED_MEM     (1U << 15)
#define FORGE_DERIVED_FEATURES       (1U << 16)

/*
 * ForgeMachineDescriptor captures the authoritative hardware identity.
 * Every field is either measured from a physical interface (RM ioctl, procfs, sysfs)
 * or explicitly classified as a derived architectural property.
 */
typedef struct {
    /* 1. Directly Observed PCI & Device Identities */
    uint16_t pci_vendor_id;
    uint16_t pci_device_id;
    uint16_t pci_subsystem_vendor_id;
    uint16_t pci_subsystem_device_id;
    char     pci_bdf[FORGE_DESC_BDF_LEN];
    uint8_t  gpu_uuid[FORGE_DESC_UUID_LEN];

    /* 2. Directly Observed RM Subdevice State */
    uint32_t rm_compute_class;
    uint32_t rm_sm_version;
    uint32_t warp_size;
    uint32_t max_warps_per_sm;
    uint32_t gpu_core_count;
    uint32_t sm_per_tpc;

    /* 3. Directly Observed Driver and Firmware Strings */
    char     driver_version[FORGE_DESC_VER_LEN];
    char     vbios_version[FORGE_DESC_VER_LEN];
    char     firmware_version[FORGE_DESC_VER_LEN];

    /* 4. Directly Observed Memory and Bus Substrate */
    uint64_t total_system_memory_bytes;
    uint64_t dma_mask;
    uint32_t page_size;

    /* 5. Classified Derived Architectural Invariants (derived from observed SM version) */
    uint32_t max_threads_per_block;
    uint32_t shared_mem_per_block_bytes;
    uint32_t features;

    /* 6. Provenance & Observation Metadata */
    uint32_t observation_flags;
} ForgeMachineDescriptor;

/* Error codes for descriptor and probe operations */
#define FORGE_PROBE_OK                    0
#define FORGE_PROBE_ERR_SYSFS            -1
#define FORGE_PROBE_ERR_PROCFS           -2
#define FORGE_PROBE_ERR_NVRM             -3
#define FORGE_PROBE_ERR_MISSING_FIELD    -4
#define FORGE_PROBE_ERR_MALFORMED_PCI    -5
#define FORGE_PROBE_ERR_UNSUPPORTED_CC   -6
#define FORGE_PROBE_ERR_INVALID_ARG      -7
#define FORGE_PROBE_ERR_DIGEST_MISMATCH  -8
#define FORGE_PROBE_ERR_UNBOUND_ALIAS    -9
#define FORGE_PROBE_ERR_DEVICE_MISMATCH  -10
#define FORGE_PROBE_ERR_KAT_FAILURE      -11

/*
 * Probes authoritative direct host interfaces (sysfs, procfs, RM ioctl via Nvrm)
 * to construct a fully measured ForgeMachineDescriptor bound to the opened GPU.
 * Zero dependency on libcuda or libcudart.
 */
int forge_probe_hardware(ForgeMachineDescriptor *out_desc);

/*
 * Deterministically normalizes descriptor fields:
 * ensures zeroed trailing bytes in strings/arrays, valid value bounds.
 */
int forge_descriptor_normalize(ForgeMachineDescriptor *desc);

/*
 * Explicit canonical binary serialization of the descriptor.
 * Serializes integers into little-endian format and strings into fixed-width
 * null-padded fields in a strictly frozen order (208 bytes total).
 */
size_t forge_descriptor_serialize(const ForgeMachineDescriptor *desc, uint8_t *out_buf, size_t max_len);

/*
 * Computes canonical binary SHA-256 digest:
 * descriptor_digest = SHA256(canonical binary serialization)
 */
int forge_descriptor_compute_digest(const ForgeMachineDescriptor *desc, uint8_t out_digest[FORGE_DESC_DIGEST_LEN]);

/*
 * Runs the serialization Known Answer Test (KAT).
 * Verifies that a frozen reference descriptor serializes to an exact byte stream
 * and produces the exact frozen reference SHA-256 digest.
 */
int forge_descriptor_run_kat(void);

/*
 * Returns derived display alias string (e.g. "sm_121") strictly based on
 * observed rm_compute_class and rm_sm_version. Never hardcoded as truth.
 * Returns NULL if descriptor does not map to a recognized architecture alias.
 */
const char *forge_descriptor_derived_alias(const ForgeMachineDescriptor *desc);

/*
 * Verifies that a given display alias strictly binds to the measured descriptor.
 * Rejects unbound, mismatched, or hardcoded alias claims.
 */
int forge_descriptor_verify_alias(const ForgeMachineDescriptor *desc, const char *alias);

/*
 * Formats canonical digest as a lowercase 64-character hexadecimal string.
 */
void forge_descriptor_digest_hex(const uint8_t digest[FORGE_DESC_DIGEST_LEN], char out_hex[65]);

#endif /* FORGE_DESCRIPTOR_H */
