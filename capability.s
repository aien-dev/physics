/*
 * capability.s - Kernel-Private Root Capability Record Genesis & Validation
 * Milestone 2: PHYSICS_BOOT (Patched Specification)
 *
 * Scope: Kernel-private resource authority root (Zero Ambient Authority).
 * Typed Identifier: CapabilityId { slot: 0, generation: 1 }. Decoupled from memory address.
 * Storage: PHYSICS_CAPABILITY_TABLE @ 0x40206000
 */

    .global init_root_capability
    .global root_capability_synthesis
    .global validate_capability

    .extern print_string

    .section .text, "ax"
    .balign 4

/*
 * init_root_capability():
 * Synthesizes CAP_ROOT at 0x40206000:
 *   +0x00: slot (u32=0), generation (u32=1)
 *   +0x08: principal_id (u64=1, Physics)
 *   +0x10: resource_type (u32=0xFFFFFFFF, UNIVERSAL_ROOT)
 *   +0x14: allowed_ops (u32=0xFFFFFFFF, ALL_AUTHORITY)
 *   +0x18: bound_base (u64=0x40208000, FREE_FRAME_BASE)
 *   +0x20: bound_size (u64=0x07DF8000, DRAM_END - FREE_FRAME_BASE)
 *   +0x28: revocation_state (u32=1, ACTIVE)
 *   +0x2C: attenuation_depth (u32=0)
 *   +0x30: provenance_digest (32 bytes = atlas.sha256)
 */
init_root_capability:
root_capability_synthesis:
    stp x29, x30, [sp, #-16]!
    mov x29, sp

    ldr x0, =0x40206000         /* Target: PHYSICS_CAPABILITY_TABLE */

    /* +0x00: slot = 0, generation = 1 */
    mov w1, #0
    str w1, [x0, #0]
    mov w1, #1
    str w1, [x0, #4]

    /* +0x08: principal_id = 1 */
    mov x1, #1
    str x1, [x0, #8]

    /* +0x10: resource_type = 0xFFFFFFFF, allowed_ops = 0xFFFFFFFF */
    mov w1, #-1
    str w1, [x0, #16]
    str w1, [x0, #20]

    /* +0x18: bound_base = 0x40208000 (FREE_FRAME_BASE) */
    ldr x1, =0x40208000
    str x1, [x0, #24]

    /* +0x20: bound_size = 0x07DF8000 (~125.9 MiB free DRAM) */
    ldr x1, =0x07DF8000
    str x1, [x0, #32]

    /* +0x28: revocation_state = 1 (ACTIVE), attenuation_depth = 0 */
    mov w1, #1
    str w1, [x0, #40]
    mov w1, #0
    str w1, [x0, #44]

    /* +0x30: Pinned provenance digest from atlas.sha256 (32 bytes) */
    adr x1, pinned_atlas_sha256
    ldp x2, x3, [x1, #0]
    ldp x4, x5, [x1, #16]
    stp x2, x3, [x0, #48]
    stp x4, x5, [x0, #64]

    /* Emit telemetry: "PHYSICS: CAP_ROOT_GENESIS\n" */
    ldr x0, =msg_cap_genesis
    bl print_string

    ldp x29, x30, [sp], #16
    ret

/*
 * validate_capability(cap_ptr, requested_op, address, size)
 *   x0: pointer to capability record
 *   w1: requested operation bitmask
 *   x2: requested address
 *   x3: requested size
 * Returns:
 *   x0: 1 if valid, 0 if denied
 */
validate_capability:
    /* Check active state */
    ldr w4, [x0, #40]           /* revocation_state */
    cmp w4, #1
    b.ne .Lcap_denied

    /* Check allowed_ops: (allowed_ops & requested_op) == requested_op */
    ldr w4, [x0, #20]           /* allowed_ops */
    and w5, w4, w1
    cmp w5, w1
    b.ne .Lcap_denied

    /* Check bounds: address >= bound_base */
    ldr x4, [x0, #24]           /* bound_base */
    cmp x2, x4
    b.lo .Lcap_denied

    /* Check overflow: address + size */
    add x5, x2, x3
    cmp x5, x2
    b.lo .Lcap_denied           /* Wrapped around */

    /* Check upper bound: address + size <= bound_base + bound_size */
    ldr x6, [x0, #32]           /* bound_size */
    add x7, x4, x6
    cmp x5, x7
    b.hi .Lcap_denied

    mov x0, #1
    ret

.Lcap_denied:
    mov x0, #0
    ret

    .section .rodata
    .balign 8
msg_cap_genesis:
    .asciz "PHYSICS: CAP_ROOT_GENESIS\n"

pinned_atlas_sha256:
    /* f7802501 b410a0c1 9eff7b8f ca8865c9 ba9c96bf a4f8065c a8f508b6 7748b9a5 */
    .byte 0xf7, 0x80, 0x25, 0x01, 0xb4, 0x10, 0xa0, 0xc1
    .byte 0x9e, 0xff, 0x7b, 0x8f, 0xca, 0x88, 0x65, 0xc9
    .byte 0xba, 0x9c, 0x96, 0xbf, 0xa4, 0xf8, 0x06, 0x5c
    .byte 0xa8, 0xf5, 0x08, 0xb6, 0x77, 0x48, 0xb9, 0xa5
