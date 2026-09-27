#include "forge_descriptor.h"
#include "nvrm.h"
#include "nvos.h"
#include "nv-ioctl.h"
#include "nv-ioctl-numbers.h"
#include "nv_escape.h"
#include "ctrl/ctrl2080/ctrl2080gr.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define NV_IOWR(nr, sz) _IOC(_IOC_READ | _IOC_WRITE, NV_IOCTL_MAGIC, (nr), (sz))

/* Forward declaration of clean SHA-256 implementation */
void sha256_compute(const uint8_t *data, uint64_t len, uint8_t scratch_buf[128], uint32_t out_digest[8]);

static void sha256_bytes(const uint8_t *data, size_t len, uint8_t out[32]) {
    uint8_t scratch[128];
    uint32_t words[8];
    sha256_compute(data, len, scratch, words);
    for (int i = 0; i < 8; i++) {
        out[i * 4 + 0] = (uint8_t)((words[i] >> 24) & 0xff);
        out[i * 4 + 1] = (uint8_t)((words[i] >> 16) & 0xff);
        out[i * 4 + 2] = (uint8_t)((words[i] >> 8) & 0xff);
        out[i * 4 + 3] = (uint8_t)(words[i] & 0xff);
    }
}

static inline void put_u16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static inline void put_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static inline void put_u64_le(uint8_t *p, uint64_t v) {
    put_u32_le(p, (uint32_t)(v & 0xffffffffULL));
    put_u32_le(p + 4, (uint32_t)((v >> 32) & 0xffffffffULL));
}

static int read_sysfs_u32(const char *path, uint32_t *out) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    uint32_t val = 0;
    int rc = fscanf(f, "0x%x", &val);
    if (rc != 1) {
        rewind(f);
        rc = fscanf(f, "%u", &val);
    }
    fclose(f);
    if (rc != 1) return -1;
    *out = val;
    return 0;
}

static int read_proc_driver_version(char *out_ver, size_t max_len) {
    FILE *f = fopen("/proc/driver/nvidia/version", "r");
    if (!f) return -1;
    char line[512];
    int found = 0;
    if (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "NVRM version:");
        if (p) {
            char *v = strstr(p, "580.");
            if (!v) v = strstr(p, "5");
            if (v) {
                size_t i = 0;
                while (v[i] && !isspace((unsigned char)v[i]) && i < max_len - 1) {
                    out_ver[i] = v[i];
                    i++;
                }
                out_ver[i] = '\0';
                found = 1;
            }
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

static int read_proc_gpu_info(const char *bdf, uint32_t expected_minor, const uint8_t expected_uuid[16],
                              char *out_vbios, size_t max_vbios,
                              char *out_fw, size_t max_fw,
                              uint64_t *out_dma_mask) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/driver/nvidia/gpus/%s/information", bdf);
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[256];
    int verified_minor = 0;
    int verified_uuid = 0;
    int found_vbios = 0;
    int found_fw = 0;
    int found_dma = 0;

    char expected_uuid_str[64];
    snprintf(expected_uuid_str, sizeof(expected_uuid_str),
             "GPU-%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             expected_uuid[0], expected_uuid[1], expected_uuid[2], expected_uuid[3],
             expected_uuid[4], expected_uuid[5], expected_uuid[6], expected_uuid[7],
             expected_uuid[8], expected_uuid[9], expected_uuid[10], expected_uuid[11],
             expected_uuid[12], expected_uuid[13], expected_uuid[14], expected_uuid[15]);

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Device Minor:", 13) == 0) {
            uint32_t m = 0;
            if (sscanf(line + 13, "%u", &m) == 1 && m == expected_minor) {
                verified_minor = 1;
            }
        } else if (strncmp(line, "GPU UUID:", 9) == 0) {
            char *p = line + 9;
            while (*p && isspace((unsigned char)*p)) p++;
            if (strncasecmp(p, expected_uuid_str, strlen(expected_uuid_str)) == 0) {
                verified_uuid = 1;
            }
        } else if (strncmp(line, "Video BIOS:", 11) == 0) {
            char *p = line + 11;
            while (*p && isspace((unsigned char)*p)) p++;
            size_t i = 0;
            while (p[i] && !isspace((unsigned char)p[i]) && i < max_vbios - 1) {
                out_vbios[i] = p[i];
                i++;
            }
            out_vbios[i] = '\0';
            found_vbios = 1;
        } else if (strncmp(line, "GPU Firmware:", 13) == 0) {
            char *p = line + 13;
            while (*p && isspace((unsigned char)*p)) p++;
            size_t i = 0;
            while (p[i] && !isspace((unsigned char)p[i]) && i < max_fw - 1) {
                out_fw[i] = p[i];
                i++;
            }
            out_fw[i] = '\0';
            found_fw = 1;
        } else if (strncmp(line, "DMA Mask:", 9) == 0) {
            char *p = line + 9;
            while (*p && isspace((unsigned char)*p)) p++;
            uint64_t mask = 0;
            if (sscanf(p, "0x%" SCNx64, &mask) == 1 || sscanf(p, "%" SCNu64, &mask) == 1) {
                *out_dma_mask = mask;
                found_dma = 1;
            }
        }
    }
    fclose(f);

    if (!verified_minor || !verified_uuid) {
        return FORGE_PROBE_ERR_DEVICE_MISMATCH;
    }
    if (!found_vbios || !found_fw) {
        return FORGE_PROBE_ERR_PROCFS;
    }
    if (!found_dma) {
        *out_dma_mask = 0; /* explicitly record unobserved rather than guessing */
    }
    return 0;
}

static int read_total_memory(uint64_t *out_bytes) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemTotal:", 9) == 0) {
            uint64_t kb = 0;
            if (sscanf(line + 9, "%" SCNu64, &kb) == 1) {
                *out_bytes = kb * 1024ULL;
                found = 1;
                break;
            }
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

int forge_probe_hardware(ForgeMachineDescriptor *out) {
    if (!out) return FORGE_PROBE_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    /* 1. Open NVRM client to bind to the active physical GPU */
    Nvrm rm;
    if (nvrm_open(&rm) != 0) {
        return FORGE_PROBE_ERR_NVRM;
    }

    /* 2. Query NV_ESC_CARD_INFO from NVRM to get exact BDF of the opened GPU */
    nv_ioctl_card_info_t cards[64];
    memset(cards, 0, sizeof(cards));
    if (ioctl(rm.fd_ctl, NV_IOWR(NV_ESC_CARD_INFO, sizeof(cards)), cards) != 0) {
        nvrm_close(&rm);
        return FORGE_PROBE_ERR_NVRM;
    }

    int found = -1;
    for (int i = 0; i < 64; i++) {
        if (cards[i].valid && cards[i].gpu_id == rm.gpu_id) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        nvrm_close(&rm);
        return FORGE_PROBE_ERR_DEVICE_MISMATCH;
    }

    /* Record exact PCI BDF observed from RM card info */
    snprintf(out->pci_bdf, sizeof(out->pci_bdf), "%04x:%02x:%02x.%d",
             cards[found].pci_info.domain, cards[found].pci_info.bus,
             cards[found].pci_info.slot, cards[found].pci_info.function);
    out->pci_vendor_id = cards[found].pci_info.vendor_id;
    out->pci_device_id = cards[found].pci_info.device_id;
    out->rm_compute_class = rm.compute_class;
    out->rm_sm_version = rm.sm_version;
    memcpy(out->gpu_uuid, rm.gpu_uuid, 16);

    out->observation_flags |= FORGE_OBS_PCI_IDENTITY |
                              FORGE_OBS_GPU_UUID |
                              FORGE_OBS_RM_COMPUTE_CLASS |
                              FORGE_OBS_RM_SM_VERSION;

    /* 3. Query RM GR Info properties (warp size, warps per SM, core count, SM per TPC) */
    NV2080_CTRL_GR_INFO list[8];
    memset(list, 0, sizeof(list));
    list[0].index = NV2080_CTRL_GR_INFO_INDEX_MAX_THREADS_PER_WARP;
    list[1].index = NV2080_CTRL_GR_INFO_INDEX_MAX_WARPS_PER_SM;
    list[2].index = NV2080_CTRL_GR_INFO_INDEX_GPU_CORE_COUNT;
    list[3].index = NV2080_CTRL_GR_INFO_INDEX_LITTER_NUM_SM_PER_TPC;

    NV2080_CTRL_GR_GET_INFO_PARAMS gp;
    memset(&gp, 0, sizeof(gp));
    gp.grInfoListSize = 4;
    gp.grInfoList = (NvP64)(uintptr_t)list;

    NVOS54_PARAMETERS p;
    memset(&p, 0, sizeof(p));
    p.hClient = rm.root;
    p.hObject = rm.subdevice;
    p.cmd = NV2080_CTRL_CMD_GR_GET_INFO;
    p.params = (NvP64)(uintptr_t)&gp;
    p.paramsSize = sizeof(gp);

    if (ioctl(rm.fd_ctl, NV_IOWR(NV_ESC_RM_CONTROL, sizeof(p)), &p) == 0 && p.status == 0) {
        out->warp_size = list[0].data;
        out->max_warps_per_sm = list[1].data;
        out->gpu_core_count = list[2].data;
        out->sm_per_tpc = list[3].data;
        out->observation_flags |= FORGE_OBS_WARP_SIZE |
                                  FORGE_OBS_WARPS_PER_SM |
                                  FORGE_OBS_CORE_COUNT |
                                  FORGE_OBS_SM_PER_TPC;
    } else {
        out->warp_size = 0; /* unobserved */
    }

    nvrm_close(&rm);

    /* 4. Validate RM compute class: must be Blackwell Compute B (0xcec0) */
    if (out->rm_compute_class != 0xcec0) {
        return FORGE_PROBE_ERR_UNSUPPORTED_CC;
    }

    /* 5. Cross-validate with sysfs for the SAME observed BDF */
    char path[512];
    uint32_t val = 0;

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", out->pci_bdf);
    if (read_sysfs_u32(path, &val) != 0 || val != out->pci_vendor_id) return FORGE_PROBE_ERR_MALFORMED_PCI;

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/device", out->pci_bdf);
    if (read_sysfs_u32(path, &val) != 0 || val != out->pci_device_id) return FORGE_PROBE_ERR_DEVICE_MISMATCH;

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/subsystem_vendor", out->pci_bdf);
    if (read_sysfs_u32(path, &val) == 0) out->pci_subsystem_vendor_id = (uint16_t)val;

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/subsystem_device", out->pci_bdf);
    if (read_sysfs_u32(path, &val) == 0) out->pci_subsystem_device_id = (uint16_t)val;

    /* 6. Read and cross-validate with procfs for the SAME observed BDF */
    if (read_proc_driver_version(out->driver_version, sizeof(out->driver_version)) != 0) {
        return FORGE_PROBE_ERR_PROCFS;
    }
    out->observation_flags |= FORGE_OBS_DRIVER_VERSION;

    if (read_proc_gpu_info(out->pci_bdf, 0, out->gpu_uuid,
                           out->vbios_version, sizeof(out->vbios_version),
                           out->firmware_version, sizeof(out->firmware_version),
                           &out->dma_mask) != 0) {
        return FORGE_PROBE_ERR_PROCFS;
    }
    out->observation_flags |= FORGE_OBS_VBIOS_VERSION | FORGE_OBS_FIRMWARE_VERSION;
    if (out->dma_mask != 0) out->observation_flags |= FORGE_OBS_DMA_MASK;

    /* 7. Read host memory topology */
    if (read_total_memory(&out->total_system_memory_bytes) != 0) {
        return FORGE_PROBE_ERR_MISSING_FIELD;
    }
    out->observation_flags |= FORGE_OBS_TOTAL_MEMORY;

    out->page_size = (uint32_t)sysconf(_SC_PAGESIZE);
    if (out->page_size > 0) out->observation_flags |= FORGE_OBS_PAGE_SIZE;

    /* 8. Classified Derived Architectural Invariants (derived from observed SM version) */
    if (out->rm_sm_version == 0x0a04) {
        out->max_threads_per_block = 1024;
        out->shared_mem_per_block_bytes = 49152;
        out->observation_flags |= FORGE_DERIVED_THREADS_PER_BLK | FORGE_DERIVED_SHARED_MEM;
    } else {
        out->max_threads_per_block = 0;
        out->shared_mem_per_block_bytes = 0;
    }

    out->features = FORGE_FEATURE_UNIFIED_MEMORY |
                    FORGE_FEATURE_COMPUTE_B |
                    FORGE_FEATURE_GPFIFO_A |
                    FORGE_FEATURE_USERMODE_A |
                    FORGE_FEATURE_PAGE_FAULTING;
    out->observation_flags |= FORGE_DERIVED_FEATURES;

    return forge_descriptor_normalize(out);
}

int forge_descriptor_normalize(ForgeMachineDescriptor *desc) {
    if (!desc) return FORGE_PROBE_ERR_INVALID_ARG;

    /* Check required identities */
    if (desc->pci_vendor_id != 0x10de) return FORGE_PROBE_ERR_MALFORMED_PCI;
    if (desc->pci_device_id == 0) return FORGE_PROBE_ERR_MALFORMED_PCI;
    if (desc->pci_bdf[0] == '\0') return FORGE_PROBE_ERR_MISSING_FIELD;
    if (desc->rm_compute_class == 0) return FORGE_PROBE_ERR_MISSING_FIELD;
    if (desc->rm_sm_version == 0) return FORGE_PROBE_ERR_MISSING_FIELD;
    if (desc->driver_version[0] == '\0') return FORGE_PROBE_ERR_MISSING_FIELD;
    if (desc->firmware_version[0] == '\0') return FORGE_PROBE_ERR_MISSING_FIELD;

    /* Ensure strict null termination and zero unused trailing buffer space */
    desc->pci_bdf[FORGE_DESC_BDF_LEN - 1] = '\0';
    for (size_t i = strlen(desc->pci_bdf); i < FORGE_DESC_BDF_LEN; i++) {
        desc->pci_bdf[i] = '\0';
    }

    desc->driver_version[FORGE_DESC_VER_LEN - 1] = '\0';
    for (size_t i = strlen(desc->driver_version); i < FORGE_DESC_VER_LEN; i++) {
        desc->driver_version[i] = '\0';
    }

    desc->vbios_version[FORGE_DESC_VER_LEN - 1] = '\0';
    for (size_t i = strlen(desc->vbios_version); i < FORGE_DESC_VER_LEN; i++) {
        desc->vbios_version[i] = '\0';
    }

    desc->firmware_version[FORGE_DESC_VER_LEN - 1] = '\0';
    for (size_t i = strlen(desc->firmware_version); i < FORGE_DESC_VER_LEN; i++) {
        desc->firmware_version[i] = '\0';
    }

    return FORGE_PROBE_OK;
}

size_t forge_descriptor_serialize(const ForgeMachineDescriptor *desc, uint8_t *out_buf, size_t max_len) {
    if (!desc || !out_buf || max_len < FORGE_DESC_SERIALIZED_LEN) return 0;
    memset(out_buf, 0, FORGE_DESC_SERIALIZED_LEN);

    /*
     * Explicit little-endian canonical serialization (208 bytes):
     * 0x00: pci_vendor_id (2)
     * 0x02: pci_device_id (2)
     * 0x04: pci_subsystem_vendor_id (2)
     * 0x06: pci_subsystem_device_id (2)
     * 0x08: pci_bdf (16)
     * 0x18: gpu_uuid (16)
     * 0x28: rm_compute_class (4)
     * 0x2c: rm_sm_version (4)
     * 0x30: warp_size (4)
     * 0x34: max_warps_per_sm (4)
     * 0x38: gpu_core_count (4)
     * 0x3c: sm_per_tpc (4)
     * 0x40: driver_version (32)
     * 0x60: vbios_version (32)
     * 0x80: firmware_version (32)
     * 0xa0: total_system_memory_bytes (8)
     * 0xa8: dma_mask (8)
     * 0xb0: page_size (4)
     * 0xb4: max_threads_per_block (4)
     * 0xb8: shared_mem_per_block_bytes (4)
     * 0xbc: features (4)
     * 0xc0: observation_flags (4)
     * 0xc4..0xcf: zero reserved (12)
     */
    put_u16_le(out_buf + 0x00, desc->pci_vendor_id);
    put_u16_le(out_buf + 0x02, desc->pci_device_id);
    put_u16_le(out_buf + 0x04, desc->pci_subsystem_vendor_id);
    put_u16_le(out_buf + 0x06, desc->pci_subsystem_device_id);
    memcpy(out_buf + 0x08, desc->pci_bdf, FORGE_DESC_BDF_LEN);
    memcpy(out_buf + 0x18, desc->gpu_uuid, FORGE_DESC_UUID_LEN);
    put_u32_le(out_buf + 0x28, desc->rm_compute_class);
    put_u32_le(out_buf + 0x2c, desc->rm_sm_version);
    put_u32_le(out_buf + 0x30, desc->warp_size);
    put_u32_le(out_buf + 0x34, desc->max_warps_per_sm);
    put_u32_le(out_buf + 0x38, desc->gpu_core_count);
    put_u32_le(out_buf + 0x3c, desc->sm_per_tpc);
    memcpy(out_buf + 0x40, desc->driver_version, FORGE_DESC_VER_LEN);
    memcpy(out_buf + 0x60, desc->vbios_version, FORGE_DESC_VER_LEN);
    memcpy(out_buf + 0x80, desc->firmware_version, FORGE_DESC_VER_LEN);
    put_u64_le(out_buf + 0xa0, desc->total_system_memory_bytes);
    put_u64_le(out_buf + 0xa8, desc->dma_mask);
    put_u32_le(out_buf + 0xb0, desc->page_size);
    put_u32_le(out_buf + 0xb4, desc->max_threads_per_block);
    put_u32_le(out_buf + 0xb8, desc->shared_mem_per_block_bytes);
    put_u32_le(out_buf + 0xbc, desc->features);
    put_u32_le(out_buf + 0xc0, desc->observation_flags);

    return FORGE_DESC_SERIALIZED_LEN;
}

int forge_descriptor_compute_digest(const ForgeMachineDescriptor *desc, uint8_t out_digest[FORGE_DESC_DIGEST_LEN]) {
    if (!desc || !out_digest) return FORGE_PROBE_ERR_INVALID_ARG;

    ForgeMachineDescriptor norm = *desc;
    int rc = forge_descriptor_normalize(&norm);
    if (rc != FORGE_PROBE_OK) return rc;

    uint8_t serialized[FORGE_DESC_SERIALIZED_LEN];
    if (forge_descriptor_serialize(&norm, serialized, sizeof(serialized)) != FORGE_DESC_SERIALIZED_LEN) {
        return FORGE_PROBE_ERR_INVALID_ARG;
    }

    sha256_bytes(serialized, sizeof(serialized), out_digest);
    return FORGE_PROBE_OK;
}

int forge_descriptor_run_kat(void) {
    /* Frozen Known Answer Test (KAT) descriptor */
    ForgeMachineDescriptor kat_desc;
    memset(&kat_desc, 0, sizeof(kat_desc));

    kat_desc.pci_vendor_id = 0x10de;
    kat_desc.pci_device_id = 0x2e12;
    kat_desc.pci_subsystem_vendor_id = 0x10de;
    kat_desc.pci_subsystem_device_id = 0x0000;
    snprintf(kat_desc.pci_bdf, sizeof(kat_desc.pci_bdf), "000f:01:00.0");
    for (int i = 0; i < 16; i++) kat_desc.gpu_uuid[i] = (uint8_t)(i + 1);
    kat_desc.rm_compute_class = 0x0000cec0;
    kat_desc.rm_sm_version = 0x00000a04;
    kat_desc.warp_size = 32;
    kat_desc.max_warps_per_sm = 48;
    kat_desc.gpu_core_count = 6144;
    kat_desc.sm_per_tpc = 2;
    snprintf(kat_desc.driver_version, sizeof(kat_desc.driver_version), "580.173.02");
    snprintf(kat_desc.vbios_version, sizeof(kat_desc.vbios_version), "9a.0b.2d.00.00");
    snprintf(kat_desc.firmware_version, sizeof(kat_desc.firmware_version), "580.173.02");
    kat_desc.total_system_memory_bytes = 130660888576ULL;
    kat_desc.dma_mask = 0x1ffffffffffULL;
    kat_desc.page_size = 4096;
    kat_desc.max_threads_per_block = 1024;
    kat_desc.shared_mem_per_block_bytes = 49152;
    kat_desc.features = 0x1f;
    kat_desc.observation_flags = 0x1ffff;

    uint8_t serialized[FORGE_DESC_SERIALIZED_LEN];
    if (forge_descriptor_serialize(&kat_desc, serialized, sizeof(serialized)) != FORGE_DESC_SERIALIZED_LEN) {
        return FORGE_PROBE_ERR_KAT_FAILURE;
    }

    uint8_t digest[32];
    sha256_bytes(serialized, sizeof(serialized), digest);

    char hex[65];
    forge_descriptor_digest_hex(digest, hex);

    const char *expected_kat_digest = "10d63d05f888eaba4fe473c5f17febe22f462f543ae0c2b005a9a8e5417e09fd";
    if (strcmp(hex, expected_kat_digest) != 0) {
        fprintf(stderr, "ERROR: FORGE descriptor KAT mismatch: expected %s, got %s\n",
                expected_kat_digest, hex);
        return FORGE_PROBE_ERR_KAT_FAILURE;
    }
    return FORGE_PROBE_OK;
}

const char *forge_descriptor_derived_alias(const ForgeMachineDescriptor *desc) {
    if (!desc) return NULL;
    if (desc->rm_compute_class == 0xcec0 && desc->rm_sm_version == 0x0a04) {
        return "sm_121";
    }
    return NULL;
}

int forge_descriptor_verify_alias(const ForgeMachineDescriptor *desc, const char *alias) {
    if (!desc || !alias) return FORGE_PROBE_ERR_INVALID_ARG;
    const char *derived = forge_descriptor_derived_alias(desc);
    if (!derived) return FORGE_PROBE_ERR_UNBOUND_ALIAS;
    if (strcmp(derived, alias) != 0) return FORGE_PROBE_ERR_UNBOUND_ALIAS;
    return FORGE_PROBE_OK;
}

void forge_descriptor_digest_hex(const uint8_t digest[FORGE_DESC_DIGEST_LEN], char out_hex[65]) {
    if (!digest || !out_hex) return;
    for (int i = 0; i < FORGE_DESC_DIGEST_LEN; i++) {
        snprintf(&out_hex[i * 2], 3, "%02x", digest[i]);
    }
    out_hex[64] = '\0';
}
