/*
 * m3/capability.s - Capability Ledger, Genesis Root, Attenuation & Revocation
 * Milestone 3: PHYSICS_EFFECTS (Sovereign Machine)
 *
 * Fixed-capacity (32 records) kernel-private capability ledger.
 * No heap, no dynamic memory allocation. Checked arithmetic.
 * Storage: PHYSICS_CAPABILITY_TABLE @ 0x40209000 (4 KiB)
 */

    .global init_capability_table
    .global lookup_capability
    .global derive_capability
    .global revoke_capability

    .extern sha256_be
    .extern print_string

    .section .text, "ax"
    .balign 4

    .equ CAP_TABLE_BASE,        0x40209000
    .equ CAP_TABLE_CAPACITY,    32
    .equ CAP_RECORD_SIZE,       128
    .equ CAP_RECORD_SHIFT,      7

    .equ RES_UNIVERSAL_ROOT,    1
    .equ RES_MEMORY_FRAME,      2
    .equ RES_CONSOLE,           3
    .equ RES_MEASUREMENT,       4

    .equ OP_ALL_AUTHORITY,      0xFFFFFFFF
    .equ OP_FRAME_GRANT,        1
    .equ OP_FRAME_RELEASE,      2
    .equ OP_CONSOLE_WRITE,      1
    .equ OP_MEASUREMENT_READ,   1

    .equ STATE_ACTIVE,          1
    .equ STATE_REVOKED,         2

    .equ DEC_ADMITTED,                  1
    .equ DEC_REJECTED_STRUCTURAL,       2
    .equ DEC_REJECTED_UNKNOWN_CAP,      3
    .equ DEC_REJECTED_STALE_GENERATION, 4
    .equ DEC_REJECTED_REVOKED,          5
    .equ DEC_REJECTED_WRONG_PRINCIPAL,  6
    .equ DEC_REJECTED_OPERATION,        7
    .equ DEC_REJECTED_RESOURCE,         8
    .equ DEC_REJECTED_BOUNDS,           9
    .equ DEC_REJECTED_CONSTRAINT,       10
    .equ DEC_REJECTED_REPLAY_CONFLICT,  11
    .equ DEC_REJECTED_EXHAUSTED,        12
    .equ DEC_EXECUTION_FAILED,          13

/*
 * init_capability_table():
 * Zeros the 4 KiB capability table, then synthesizes CAP_ROOT at slot 0:
 *   +0x00: slot = 0, generation = 1
 *   +0x08: principal_id = 1 (Physics)
 *   +0x10: resource_type = 1 (RES_UNIVERSAL_ROOT), allowed_ops = 0xFFFFFFFF
 *   +0x18: bound_base = 0x40210000 (FREE_FRAME_BASE)
 *   +0x20: bound_size = 0x07DF0000 (DRAM_END - FREE_FRAME_BASE = 32,240 frames)
 *   +0x28: revocation_state = 1 (ACTIVE), attenuation_depth = 0
 *   +0x30: provenance_digest (32 bytes = atlas.sha256)
 *   +0x50: parent_slot = 0xFFFFFFFF, parent_generation = 0xFFFFFFFF
 *   +0x58: lifetime = 0 (unbounded)
 *   +0x60: reserved (32 zero bytes)
 */
init_capability_table:
    stp x29, x30, [sp, #-16]!
    mov x29, sp

    /* Zero all 4096 bytes of the capability table */
    ldr x0, =CAP_TABLE_BASE
    mov x1, #4096
.Lzero_cap_loop:
    cbz x1, .Lzero_cap_done
    stp xzr, xzr, [x0], #16
    sub x1, x1, #16
    b .Lzero_cap_loop

.Lzero_cap_done:
    ldr x0, =CAP_TABLE_BASE     /* Target: Slot 0 */

    /* +0x00: slot = 0, generation = 1 */
    mov w1, #0
    str w1, [x0, #0]
    mov w1, #1
    str w1, [x0, #4]

    /* +0x08: principal_id = 1 (Physics) */
    mov x1, #1
    str x1, [x0, #8]

    /* +0x10: resource_type = 1 (RES_UNIVERSAL_ROOT), allowed_ops = 0xFFFFFFFF */
    mov w1, #RES_UNIVERSAL_ROOT
    str w1, [x0, #16]
    mov w1, #-1
    str w1, [x0, #20]

    /* +0x18: bound_base = 0x40210000 (FREE_FRAME_BASE) */
    ldr x1, =0x40210000
    str x1, [x0, #24]

    /* +0x20: bound_size = 0x07DF0000 */
    ldr x1, =0x07DF0000
    str x1, [x0, #32]

    /* +0x28: revocation_state = 1 (ACTIVE), attenuation_depth = 0 */
    mov w1, #STATE_ACTIVE
    str w1, [x0, #40]
    mov w1, #0
    str w1, [x0, #44]

    /* +0x30: Pinned provenance digest from atlas.sha256 (32 bytes) */
    adr x1, pinned_atlas_sha256
    ldp x2, x3, [x1, #0]
    ldp x4, x5, [x1, #16]
    stp x2, x3, [x0, #48]
    stp x4, x5, [x0, #64]

    /* +0x50: parent_slot = -1, parent_generation = -1 */
    mov w1, #-1
    str w1, [x0, #0x50]
    str w1, [x0, #0x54]

    /* +0x58: lifetime = 0 (unbounded) */
    str xzr, [x0, #0x58]

    ldp x29, x30, [sp], #16
    ret

/*
 * lookup_capability(slot, generation, principal_id)
 *   w0: slot, w1: generation, x2: principal_id
 * Returns:
 *   x0: Pointer to CapabilityRecord (or 0 if invalid/denied)
 *   w1: Decision / rejection code (DEC_ADMITTED or DEC_REJECTED_*)
 */
lookup_capability:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    stp x19, x20, [sp, #16]

    /* 1. Slot range check */
    cmp w0, #CAP_TABLE_CAPACITY
    b.hs .Llookup_unknown

    /* 2. Compute record address: CAP_TABLE_BASE + (slot << 7) */
    ldr x19, =CAP_TABLE_BASE
    ubfiz x3, x0, #CAP_RECORD_SHIFT, #5
    add x19, x19, x3            /* x19 = record ptr */

    /* 3. Check record allocated (generation > 0) */
    ldr w4, [x19, #4]           /* record.generation */
    cbz w4, .Llookup_unknown

    /* 4. Generation check: presented == record */
    cmp w1, w4
    b.ne .Llookup_stale

    /* 5. Immediate revocation check */
    ldr w5, [x19, #40]          /* record.revocation_state */
    cmp w5, #STATE_ACTIVE
    b.ne .Llookup_revoked

    /* 6. Principal check */
    ldr x6, [x19, #8]           /* record.principal_id */
    cmp x2, x6
    b.ne .Llookup_wrong_principal

    /* 7. Ancestor-chain validation on use */
    ldr w7, [x19, #0]           /* slot */
    cbz w7, .Llookup_success    /* CAP_ROOT has no parent, admitted */

    mov x20, x19                /* x20 = current node in traversal */
    mov w8, #8                  /* loop guard (max depth 8) */

.Lancestor_loop:
    cbz w8, .Llookup_revoked    /* depth exceeded fail-closed */
    sub w8, w8, #1

    ldr w9, [x20, #0x50]        /* parent_slot */
    ldr w10, [x20, #0x54]       /* parent_generation */

    /* If parent_slot is CAP_ROOT (0), check slot 0 */
    cmp w9, #CAP_TABLE_CAPACITY
    b.hs .Llookup_revoked

    ldr x11, =CAP_TABLE_BASE
    ubfiz x12, x9, #CAP_RECORD_SHIFT, #5
    add x11, x11, x12           /* x11 = parent record ptr */

    /* Parent generation check */
    ldr w13, [x11, #4]
    cmp w10, w13
    b.ne .Llookup_stale

    /* Parent revocation state check */
    ldr w14, [x11, #40]
    cmp w14, #STATE_ACTIVE
    b.ne .Llookup_revoked

    /* If parent is slot 0 (CAP_ROOT), ancestor chain fully verified! */
    cbz w9, .Llookup_success

    /* Advance to parent */
    mov x20, x11
    b .Lancestor_loop

.Llookup_success:
    mov x0, x19
    mov w1, #DEC_ADMITTED
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

.Llookup_unknown:
    mov x0, #0
    mov w1, #DEC_REJECTED_UNKNOWN_CAP
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

.Llookup_stale:
    mov x0, #0
    mov w1, #DEC_REJECTED_STALE_GENERATION
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

.Llookup_revoked:
    mov x0, #0
    mov w1, #DEC_REJECTED_REVOKED
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

.Llookup_wrong_principal:
    mov x0, #0
    mov w1, #DEC_REJECTED_WRONG_PRINCIPAL
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

/*
 * derive_capability(
 *   w0: parent_slot,
 *   w1: parent_generation,
 *   x2: caller_principal,
 *   x3: child_principal,
 *   w4: requested_resource_type,
 *   w5: requested_ops,
 *   x6: requested_base,
 *   x7: requested_size,
 *   x8: requested_lifetime [passed in x8]
 * )
 * Returns:
 *   x0: (new_generation << 32) | child_slot, or 0 on error.
 */
derive_capability:
    stp x29, x30, [sp, #-192]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]

    /* Save arguments into callee-saved registers */
    mov x22, x3                 /* child_principal */
    mov w23, w4                 /* rtype */
    mov w24, w5                 /* ops */
    mov x25, x6                 /* base */
    mov x26, x7                 /* size */
    mov x27, x8                 /* lifetime */

    /* 1. Lookup and validate parent (w0, w1, x2 already set) */
    bl lookup_capability
    cbz x0, .Lderive_fail
    mov x19, x0                 /* x19 = validated parent record ptr */

    /* 2. Child principal cannot be 0 */
    cbz x22, .Lderive_fail

    /* 3. Resource type containment */
    ldr w4, [x19, #16]          /* parent.resource_type */
    cmp w4, #RES_UNIVERSAL_ROOT
    b.eq .Lcheck_root_narrowing
    /* Specific resource: child must match exactly */
    cmp w23, w4
    b.ne .Lderive_fail
    b .Lcheck_ops

.Lcheck_root_narrowing:
    /* Narrowing from root: child can be MEMORY_FRAME, CONSOLE, or MEASUREMENT */
    cmp w23, #RES_MEMORY_FRAME
    b.eq .Lcheck_ops
    cmp w23, #RES_CONSOLE
    b.eq .Lcheck_ops
    cmp w23, #RES_MEASUREMENT
    b.eq .Lcheck_ops
    b .Lderive_fail             /* Cannot derive ROOT or unknown type */

.Lcheck_ops:
    /* 4. Rights narrowing: (child_ops & ~parent_ops) == 0 */
    cbz w24, .Lderive_fail      /* Cannot have 0 ops */
    ldr w5, [x19, #20]          /* parent.allowed_ops */
    mvn w6, w5
    tst w24, w6
    b.ne .Lderive_fail          /* Child has forbidden rights */

    /* 5. Bounds containment with checked arithmetic */
    ldr w4, [x19, #16]          /* parent.resource_type */
    cmp w4, #RES_UNIVERSAL_ROOT
    b.ne .Lcheck_standard_bounds

    /* Parent is CAP_ROOT: check bounds according to requested concrete resource */
    cmp w23, #RES_CONSOLE
    b.eq .Lcheck_root_console_bounds
    cmp w23, #RES_MEASUREMENT
    b.eq .Lcheck_root_measurement_bounds

    /* Otherwise MEMORY_FRAME: fall through to check standard bounds within DRAM */

.Lcheck_standard_bounds:
    ldr x5, [x19, #24]          /* parent.bound_base */
    ldr x6, [x19, #32]          /* parent.bound_size */

    /* Check child base >= parent base */
    cmp x25, x5
    b.lo .Lderive_fail

    /* Check child base + size overflow */
    adds x7, x25, x26           /* x7 = child end */
    b.cs .Lderive_fail

    /* Check parent base + size overflow */
    adds x8, x5, x6             /* x8 = parent end */
    b.cs .Lderive_fail

    /* Check child end <= parent end */
    cmp x7, x8
    b.hi .Lderive_fail

    /* For memory frame: size must be > 0 and 4 KiB aligned */
    cmp w23, #RES_MEMORY_FRAME
    b.ne .Lcheck_lifetime
    cbz x26, .Lderive_fail
    tst x25, #0xFFF
    b.ne .Lderive_fail
    tst x26, #0xFFF
    b.ne .Lderive_fail
    b .Lcheck_lifetime

.Lcheck_root_console_bounds:
    ldr x5, =0x09000000         /* PL011 UART MMIO base */
    cmp x25, x5
    b.ne .Lderive_fail
    cmp x26, #0x1000
    b.hi .Lderive_fail
    cbz x26, .Lderive_fail
    b .Lcheck_lifetime

.Lcheck_root_measurement_bounds:
    cbnz x25, .Lderive_fail
    cbnz x26, .Lderive_fail
    b .Lcheck_lifetime

.Lcheck_lifetime:
    /* 6. Lifetime narrowing: if parent.lifetime != 0, child <= parent and child != 0 */
    ldr x9, [x19, #0x58]        /* parent.lifetime */
    cbz x9, .Lcheck_depth
    cbz x27, .Lderive_fail      /* Cannot widen to unbounded */
    cmp x27, x9
    b.hi .Lderive_fail

.Lcheck_depth:
    /* 7. Attenuation depth: parent.depth + 1 <= 8 */
    ldr w10, [x19, #44]         /* parent.attenuation_depth */
    cmp w10, #8
    b.ge .Lderive_fail
    add w20, w10, #1            /* w20 = child depth */

    /* 8. Find a free slot in [1..31] */
    ldr x11, =CAP_TABLE_BASE
    mov w21, #1                 /* slot index 1..31 in callee-saved w21 */
.Lsearch_slot:
    cmp w21, #CAP_TABLE_CAPACITY
    b.ge .Lderive_fail          /* Table full */

    ubfiz x13, x21, #CAP_RECORD_SHIFT, #5
    add x28, x11, x13           /* x28 = candidate slot ptr (callee-saved) */

    ldr w15, [x28, #40]         /* slot.revocation_state */
    cmp w15, #STATE_ACTIVE
    b.ne .Lslot_found

    add w21, w21, #1
    b .Lsearch_slot

.Lslot_found:
    /* Generation bumping */
    ldr w16, [x28, #4]          /* old generation */
    add w16, w16, #1
    cbnz w16, 1f
    /* Wrapped to 0? Generation wrap fails closed */
    add w21, w21, #1
    b .Lsearch_slot
1:
    str w16, [sp, #184]         /* Save new_generation on stack */

    /* 9. Compute Provenance Digest: SHA-256 of parent_prov (32B) + child fields (48B) = 80B */
    /* Construct hash buffer on stack at [sp + 96] */
    add x0, sp, #96
    /* Copy 32 bytes parent provenance */
    add x1, x19, #48            /* parent.provenance */
    ldp x2, x3, [x1, #0]
    ldp x4, x5, [x1, #16]
    stp x2, x3, [x0, #0]
    stp x4, x5, [x0, #16]

    /* Copy child fields: child_principal (8B), rtype (4B), ops (4B), base (8B), size (8B), depth (4B), slot (4B), lifetime (8B) */
    str x22, [x0, #32]          /* child_principal */
    str w23, [x0, #40]          /* rtype */
    str w24, [x0, #44]          /* ops */
    str x25, [x0, #48]          /* base */
    str x26, [x0, #56]          /* size */
    str w20, [x0, #64]          /* child_depth */
    str w21, [x0, #68]          /* child_slot (binds identity) */
    str x27, [x0, #72]          /* lifetime */

    /* Call sha256_be(data, len=80, out) */
    /* Target child provenance location: x28 + 48 */
    mov x1, #80
    add x2, x28, #48            /* child.provenance */
    bl sha256_be
    cbnz w0, .Lderive_fail

    /* 10. Write child record fields into slot (x28 is preserved!) */
    ldr w16, [sp, #184]         /* reload new generation */
    str w21, [x28, #0]          /* slot */
    str w16, [x28, #4]          /* generation */
    str x22, [x28, #8]          /* principal_id */
    str w23, [x28, #16]         /* resource_type */
    str w24, [x28, #20]         /* allowed_ops */
    str x25, [x28, #24]         /* bound_base */
    str x26, [x28, #32]         /* bound_size */
    mov w1, #STATE_ACTIVE
    str w1, [x28, #40]          /* revocation_state */
    str w20, [x28, #44]         /* attenuation_depth */
    ldr w2, [x19, #0]           /* parent.slot */
    str w2, [x28, #0x50]        /* parent_slot */
    ldr w3, [x19, #4]           /* parent.generation */
    str w3, [x28, #0x54]        /* parent_generation */
    str x27, [x28, #0x58]       /* lifetime */

    /* Zero reserved bytes at +0x60..+0x7F */
    stp xzr, xzr, [x28, #0x60]
    stp xzr, xzr, [x28, #0x70]

    /* Return (generation << 32) | slot */
    ubfiz x0, x16, #32, #32
    orr x0, x0, x21

    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp x29, x30, [sp], #192
    ret

.Lderive_fail:
    mov x0, #0
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp x29, x30, [sp], #192
    ret

/*
 * revoke_capability(slot, generation, caller_principal)
 *   w0: slot, w1: generation, x2: caller_principal
 * Returns:
 *   x0: 1 on success, 0 on failure.
 */
revoke_capability:
    stp x29, x30, [sp, #-16]!
    mov x29, sp

    /* Cannot revoke CAP_ROOT (slot 0) */
    cbz w0, .Lrevoke_fail
    cmp w0, #CAP_TABLE_CAPACITY
    b.ge .Lrevoke_fail

    ldr x3, =CAP_TABLE_BASE
    ubfiz x4, x0, #CAP_RECORD_SHIFT, #5
    add x3, x3, x4              /* record ptr */

    /* Check generation matches */
    ldr w5, [x3, #4]
    cmp w1, w5
    b.ne .Lrevoke_fail

    /* Check caller is owner OR Physics (principal 1) */
    ldr x6, [x3, #8]
    cmp x2, x6
    b.eq 1f
    cmp x2, #1
    b.ne .Lrevoke_fail
1:
    /* Mark REVOKED */
    mov w7, #STATE_REVOKED
    str w7, [x3, #40]

    mov x0, #1
    ldp x29, x30, [sp], #16
    ret

.Lrevoke_fail:
    mov x0, #0
    ldp x29, x30, [sp], #16
    ret

    .section .rodata
    .balign 8
pinned_atlas_sha256:
    .byte 0xf7, 0x80, 0x25, 0x01, 0xb4, 0x10, 0xa0, 0xc1
    .byte 0x9e, 0xff, 0x7b, 0x8f, 0xca, 0x88, 0x65, 0xc9
    .byte 0xba, 0x9c, 0x96, 0xbf, 0xa4, 0xf8, 0x06, 0x5c
    .byte 0xa8, 0xf5, 0x08, 0xb6, 0x77, 0x48, 0xb9, 0xa5
