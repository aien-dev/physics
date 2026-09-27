#include "forge_descriptor.h"
#include "nvrm.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Forward declaration of sha256_compute from sha256_clean.c */
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

static int find_nvidia_gpu_bdf(char *out_bdf, size_t max_len) {
    DIR *d = opendir("/sys/bus/pci/devices");
    if (!d) return -1;
    struct dirent *de;
    int found = 0;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", de->d_name);
        uint32_t vendor = 0;
        if (read_sysfs_u32(path, &vendor) == 0 && vendor == 0x10de) {
            snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/class", de->d_name);
            uint32_t pci_class = 0;
            if (read_sysfs_u32(path, &pci_class) == 0 && ((pci_class >> 16) == 0x03)) {
                size_t nlen = strlen(de->d_name);
                if (nlen < max_len) {
                    memcpy(out_bdf, de->d_name, nlen + 1);
                    found = 1;
                    break;
                }
            }
        }
    }
    closedir(d);
    return found ? 0 : -1;
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

static int read_proc_gpu_info(const char *bdf, char *out_vbios, size_t max_vbios) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/driver/nvidia/gpus/%s/information", bdf);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    int found_vbios = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Video BIOS:", 11) == 0) {
            char *p = line + 11;
            while (*p && isspace((unsigned char)*p)) p++;
            size_t i = 0;
            while (p[i] && !isspace((unsigned char)p[i]) && i < max_vbios - 1) {
                out_vbios[i] = p[i];
                i++;
            }
            out_vbios[i] = '\0';
            found_vbios = 1;
        }
    }
    fclose(f);
    return found_vbios ? 0 : -1;
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

    /* 1. Discover GPU BDF via sysfs */
    char bdf[FORGE_DESC_BDF_LEN] = {0};
    if (find_nvidia_gpu_bdf(bdf, sizeof(bdf)) != 0) {
        return FORGE_PROBE_ERR_SYSFS;
    }
    snprintf(out->pci_bdf, sizeof(out->pci_bdf), "%s", bdf);

    /* 2. Read PCI identities via sysfs */
    char path[512];
    uint32_t val = 0;

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", bdf);
    if (read_sysfs_u32(path, &val) != 0 || val != 0x10de) return FORGE_PROBE_ERR_MALFORMED_PCI;
    out->pci_vendor_id = (uint16_t)val;

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/device", bdf);
    if (read_sysfs_u32(path, &val) != 0 || val == 0) return FORGE_PROBE_ERR_MISSING_FIELD;
    out->pci_device_id = (uint16_t)val;

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/subsystem_vendor", bdf);
    if (read_sysfs_u32(path, &val) == 0) out->pci_subsystem_vendor_id = (uint16_t)val;

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/subsystem_device", bdf);
    if (read_sysfs_u32(path, &val) == 0) out->pci_subsystem_device_id = (uint16_t)val;

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/dma_mask_bits", bdf);
    if (read_sysfs_u32(path, &val) == 0 && val > 0) {
        out->dma_mask = (val >= 64) ? ~0ULL : ((1ULL << val) - 1ULL);
    } else {
        out->dma_mask = 0x1ffffffffffULL; /* 41-bit default */
    }

    /* 3. Driver version & VBIOS from procfs */
    if (read_proc_driver_version(out->driver_version, sizeof(out->driver_version)) != 0) {
        return FORGE_PROBE_ERR_PROCFS;
    }
    if (read_proc_gpu_info(bdf, out->vbios_version, sizeof(out->vbios_version)) != 0) {
        return FORGE_PROBE_ERR_PROCFS;
    }

    /* 4. RM observation via Nvrm */
    Nvrm rm;
    if (nvrm_open(&rm) != 0) {
        return FORGE_PROBE_ERR_NVRM;
    }
    out->rm_compute_class = rm.compute_class;
    out->rm_sm_version = rm.sm_version;
    memcpy(out->gpu_uuid, rm.gpu_uuid, 16);
    nvrm_close(&rm);

    /* Validate RM compute class: must be Blackwell Compute B (0xcec0) */
    if (out->rm_compute_class != 0xcec0) {
        return FORGE_PROBE_ERR_UNSUPPORTED_CC;
    }

    /* 5. Memory topology & architecture characteristics */
    if (read_total_memory(&out->total_system_memory_bytes) != 0) {
        return FORGE_PROBE_ERR_MISSING_FIELD;
    }
    out->page_size = (uint32_t)sysconf(_SC_PAGESIZE);
    if (out->page_size == 0) out->page_size = 4096;

    out->warp_size = 32;
    out->max_threads_per_block = 1024;
    out->shared_mem_per_block_bytes = 49152; /* 48 KiB standard reserved block smem */

    out->features = FORGE_FEATURE_UNIFIED_MEMORY |
                    FORGE_FEATURE_COMPUTE_B |
                    FORGE_FEATURE_GPFIFO_A |
                    FORGE_FEATURE_USERMODE_A |
                    FORGE_FEATURE_PAGE_FAULTING;

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

    memset(desc->reserved, 0, sizeof(desc->reserved));
    return FORGE_PROBE_OK;
}

int forge_descriptor_compute_digest(const ForgeMachineDescriptor *desc, uint8_t out_digest[FORGE_DESC_DIGEST_LEN]) {
    if (!desc || !out_digest) return FORGE_PROBE_ERR_INVALID_ARG;

    /* Copy and normalize temporary descriptor to ensure zero padding is pristine */
    ForgeMachineDescriptor norm = *desc;
    int rc = forge_descriptor_normalize(&norm);
    if (rc != FORGE_PROBE_OK) return rc;

    sha256_bytes((const uint8_t *)&norm, sizeof(norm), out_digest);
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
