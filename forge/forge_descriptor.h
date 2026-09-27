#ifndef FORGE_DESCRIPTOR_H
#define FORGE_DESCRIPTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FORGE_DESC_BDF_LEN        16
#define FORGE_DESC_UUID_LEN       16
#define FORGE_DESC_VER_LEN        32
#define FORGE_DESC_DIGEST_LEN     32

#define FORGE_FEATURE_UNIFIED_MEMORY (1U << 0)
#define FORGE_FEATURE_COMPUTE_B      (1U << 1)
#define FORGE_FEATURE_GPFIFO_A       (1U << 2)
#define FORGE_FEATURE_USERMODE_A     (1U << 3)
#define FORGE_FEATURE_PAGE_FAULTING  (1U << 4)

/*
 * Canonical binary layout for ForgeMachineDescriptor serialization.
 * Explicitly packed and little-endian to guarantee identical SHA-256
 * digest across all toolchains and platforms.
 */
#pragma pack(push, 1)
typedef struct {
    uint16_t pci_vendor_id;
    uint16_t pci_device_id;
    uint16_t pci_subsystem_vendor_id;
    uint16_t pci_subsystem_device_id;
    char     pci_bdf[FORGE_DESC_BDF_LEN];
    uint8_t  gpu_uuid[FORGE_DESC_UUID_LEN];
    uint32_t rm_compute_class;
    uint32_t rm_sm_version;
    char     driver_version[FORGE_DESC_VER_LEN];
    char     vbios_version[FORGE_DESC_VER_LEN];
    uint64_t total_system_memory_bytes;
    uint64_t dma_mask;
    uint32_t page_size;
    uint32_t warp_size;
    uint32_t max_threads_per_block;
    uint32_t shared_mem_per_block_bytes;
    uint32_t features;
    uint8_t  reserved[16];
} ForgeMachineDescriptor;
#pragma pack(pop)

/* Error codes for descriptor and probe operations */
#define FORGE_PROBE_OK                   0
#define FORGE_PROBE_ERR_SYSFS           -1
#define FORGE_PROBE_ERR_PROCFS          -2
#define FORGE_PROBE_ERR_NVRM            -3
#define FORGE_PROBE_ERR_MISSING_FIELD   -4
#define FORGE_PROBE_ERR_MALFORMED_PCI   -5
#define FORGE_PROBE_ERR_UNSUPPORTED_CC  -6
#define FORGE_PROBE_ERR_INVALID_ARG     -7
#define FORGE_PROBE_ERR_DIGEST_MISMATCH -8
#define FORGE_PROBE_ERR_UNBOUND_ALIAS   -9

/*
 * Probes authoritative direct host interfaces (sysfs, procfs, RM ioctl via Nvrm)
 * to construct a fully measured ForgeMachineDescriptor.
 * Zero dependency on libcuda or libcudart.
 */
int forge_probe_hardware(ForgeMachineDescriptor *out_desc);

/*
 * Deterministically normalizes descriptor fields:
 * ensures zeroed trailing bytes in strings/arrays, valid value bounds.
 */
int forge_descriptor_normalize(ForgeMachineDescriptor *desc);

/*
 * Computes canonical binary SHA-256 digest:
 * descriptor_digest = SHA256(canonical binary serialization)
 */
int forge_descriptor_compute_digest(const ForgeMachineDescriptor *desc, uint8_t out_digest[FORGE_DESC_DIGEST_LEN]);

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
