#include "forge_descriptor.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_total = 0, g_passed = 0, g_failed = 0;

static void report(const char *name, int ok) {
    g_total++;
    if (ok) {
        g_passed++;
        printf("[PASS] %s\n", name);
    } else {
        g_failed++;
        printf("[FAIL] %s\n", name);
    }
}

int main(void) {
    printf("============================================================\n");
    printf("           GATE 4: FORGE-HWID QUALIFICATION SUITE           \n");
    printf("============================================================\n\n");

    ForgeMachineDescriptor desc;
    memset(&desc, 0, sizeof(desc));

    /* 1. RAW_HARDWARE_PROBE_CAPTURED */
    int probe_rc = forge_probe_hardware(&desc);
    printf("[*] forge_probe_hardware() result: %d\n", probe_rc);
    if (probe_rc == 0) {
        printf("    PCI BDF:           %s\n", desc.pci_bdf);
        printf("    PCI Vendor:Device: %04x:%04x\n", desc.pci_vendor_id, desc.pci_device_id);
        printf("    PCI Subsystem:     %04x:%04x\n", desc.pci_subsystem_vendor_id, desc.pci_subsystem_device_id);
        printf("    RM Compute Class:  0x%x\n", desc.rm_compute_class);
        printf("    RM SM Version:     0x%x\n", desc.rm_sm_version);
        printf("    Driver Version:    %s\n", desc.driver_version);
        printf("    VBIOS Version:     %s\n", desc.vbios_version);
        printf("    Total Memory:      %" PRIu64 " bytes (%.2f GiB)\n",
               desc.total_system_memory_bytes, (double)desc.total_system_memory_bytes / (1024.0 * 1024.0 * 1024.0));
        printf("    DMA Mask:          0x%" PRIx64 "\n", desc.dma_mask);
        printf("    Features:          0x%x\n", desc.features);
    }
    int raw_probe_ok = (probe_rc == 0) &&
                       (desc.pci_vendor_id == 0x10de) &&
                       (desc.pci_device_id != 0) &&
                       (desc.pci_bdf[0] != '\0') &&
                       (desc.rm_compute_class == 0xcec0) &&
                       (desc.rm_sm_version == 0x0a04) &&
                       (desc.driver_version[0] != '\0') &&
                       (desc.total_system_memory_bytes > 0);
    report("RAW_HARDWARE_PROBE_CAPTURED", raw_probe_ok);

    /* 2. FORGE_DESCRIPTOR_NORMALIZED */
    int norm_rc = forge_descriptor_normalize(&desc);
    report("FORGE_DESCRIPTOR_NORMALIZED", norm_rc == 0);

    /* 3. DESCRIPTOR_DIGEST_BOUND */
    uint8_t digest1[FORGE_DESC_DIGEST_LEN];
    uint8_t digest2[FORGE_DESC_DIGEST_LEN];
    int d1_rc = forge_descriptor_compute_digest(&desc, digest1);
    int d2_rc = forge_descriptor_compute_digest(&desc, digest2);
    char hex_digest[65];
    forge_descriptor_digest_hex(digest1, hex_digest);
    printf("[*] Observed Descriptor Digest: %s\n", hex_digest);
    int digest_bound_ok = (d1_rc == 0) && (d2_rc == 0) &&
                          (memcmp(digest1, digest2, FORGE_DESC_DIGEST_LEN) == 0);
    report("DESCRIPTOR_DIGEST_BOUND", digest_bound_ok);

    /* 4. RECEIPT_HARDWARE_DERIVED (Derived alias sm_121 from observed facts) */
    const char *alias = forge_descriptor_derived_alias(&desc);
    printf("[*] Derived Display Alias: %s\n", alias ? alias : "(none)");
    int alias_ok = (alias != NULL) && (strcmp(alias, "sm_121") == 0) &&
                   (forge_descriptor_verify_alias(&desc, "sm_121") == 0);
    report("RECEIPT_HARDWARE_DERIVED", alias_ok);

    /* --- NEGATIVE TESTS --- */
    printf("\n[*] Running Gate 4 Negative Tests...\n");

    /* Neg 1: Missing required field (zeroed BDF) */
    ForgeMachineDescriptor missing_field = desc;
    memset(missing_field.pci_bdf, 0, sizeof(missing_field.pci_bdf));
    int neg_missing_rc = forge_descriptor_normalize(&missing_field);
    report("NEG_MISSING_REQUIRED_FIELD_REJECTED", neg_missing_rc != 0);

    /* Neg 2: Malformed PCI identity */
    ForgeMachineDescriptor malformed_pci = desc;
    malformed_pci.pci_vendor_id = 0x8086; /* Non-NVIDIA */
    int neg_pci_rc = forge_descriptor_normalize(&malformed_pci);
    report("NEG_MALFORMED_PCI_REJECTED", neg_pci_rc != 0);

    /* Neg 3: Unsupported compute class */
    ForgeMachineDescriptor unsupported_cc = desc;
    unsupported_cc.rm_compute_class = 0xc7c0; /* Hopper or unrecognized */
    const char *unsupported_alias = forge_descriptor_derived_alias(&unsupported_cc);
    report("NEG_UNSUPPORTED_COMPUTE_CLASS_REJECTED", unsupported_alias == NULL);

    /* Neg 4: Descriptor mutation causing digest mismatch */
    ForgeMachineDescriptor mutated = desc;
    mutated.dma_mask ^= 0x1000;
    uint8_t mutated_digest[FORGE_DESC_DIGEST_LEN];
    forge_descriptor_compute_digest(&mutated, mutated_digest);
    int mutation_mismatch = (memcmp(digest1, mutated_digest, FORGE_DESC_DIGEST_LEN) != 0);
    report("NEG_DESCRIPTOR_MUTATION_CAUSES_DIGEST_MISMATCH", mutation_mismatch);

    /* Neg 5: Hard-coded / unbound alias rejection */
    int unbound_rc = forge_descriptor_verify_alias(&desc, "sm_90");
    report("NEG_UNBOUND_ALIAS_REJECTED", unbound_rc != 0);

    printf("\nGate 4 Results: TOTAL=%d PASSED=%d FAILED=%d\n", g_total, g_passed, g_failed);
    return (g_failed == 0) ? 0 : 1;
}
