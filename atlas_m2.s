/*
 * ATLAS M2: Bootstrap Seed for Milestone 2 (PHYSICS_BOOT)
 *
 * The pinned SHA-256 of physics.bin lives in physics_pin.inc, which
 * build_m2.py regenerates from the physics.bin it just built. That file is
 * the single source for the pin; this file carries no digest copy.
 *
 * Descriptor fields and PHYSICS_ENTRY_ABI registers default to the
 * CONTRACT-QEMU-VIRT-AARCH64-M2 values. The Seam 2 hostile matrix overrides
 * one at a time with `--defsym NAME=value`; the canonical build overrides none.
 */

.ifndef DESC_MAGIC
.equ DESC_MAGIC, 0x4D424453435F3031
.endif
.ifndef DESC_VERSION
.equ DESC_VERSION, 1
.endif
.ifndef DESC_LENGTH
.equ DESC_LENGTH, 64
.endif
.ifndef DESC_FLAGS
.equ DESC_FLAGS, 0
.endif
.ifndef DESC_RAM_BASE
.equ DESC_RAM_BASE, 0x40000000
.endif
.ifndef DESC_RAM_SIZE
.equ DESC_RAM_SIZE, 0x08000000
.endif
.ifndef DESC_UART_BASE
.equ DESC_UART_BASE, 0x09000000
.endif
.ifndef DESC_PHYSICS_BASE
.equ DESC_PHYSICS_BASE, 0x40200000
.endif
.ifndef DESC_PHYSICS_SIZE
.equ DESC_PHYSICS_SIZE, 6144
.endif
.ifndef ABI_X0
.equ ABI_X0, 0x401FE000
.endif
.ifndef ABI_X1
.equ ABI_X1, 6144
.endif
.ifndef ABI_X2
.equ ABI_X2, 0x5048595349435330
.endif

.global _start
.section .text
.balign 64

_start:
    /* 1. Mask all interrupts (DAIF: D=1, A=1, I=1, F=1) */
    msr daifset, #0xf

    /* 2. Initialize Stack Pointer to Scratchpad RAM (Disjoint from Descriptor @ 0x401FE000) */
    ldr x0, =0x401FC000
    mov sp, x0

    /* 3. Initialize UART MMIO Base Pointer in x20 */
    ldr x20, =0x09000000

    /* 4. Emit Diagnostic Checkpoint 1: AWAKEN */
    adr x21, msg_awaken
    bl print_string

    /* 5. Populate Machine Boot Descriptor at 0x401FE000 (64 bytes) */
    ldr x22, =0x401FE000
    ldr x0, =DESC_MAGIC         /* +0x00: magic = 'MBDSC_01' */
    str x0, [x22, #0]
    ldr w0, =DESC_VERSION       /* +0x08: version (u32) */
    str w0, [x22, #8]
    ldr w0, =DESC_LENGTH        /* +0x0C: length (u32) */
    str w0, [x22, #12]
    ldr x0, =DESC_FLAGS         /* +0x10: flags_reserved (u64) */
    str x0, [x22, #16]
    ldr x0, =DESC_RAM_BASE      /* +0x18: RAM Base */
    str x0, [x22, #24]
    ldr x0, =DESC_RAM_SIZE      /* +0x20: RAM Size */
    str x0, [x22, #32]
    ldr x0, =DESC_UART_BASE     /* +0x28: UART MMIO Base */
    str x0, [x22, #40]
    ldr x0, =DESC_PHYSICS_BASE  /* +0x30: Physics Payload Base */
    str x0, [x22, #48]
    ldr x0, =DESC_PHYSICS_SIZE  /* +0x38: Physics Payload Size */
    str x0, [x22, #56]

    /* 6. Emit Diagnostic Checkpoint 2: VERIFY */
    adr x21, msg_verify
    bl print_string

    /* 7. Verify PHYSICS Integrity using Cryptographic SHA-256 */
    /* sha256_compute(payload, len, scratch_buf, out_digest) */
    ldr x0, =0x40200000         /* Arg 0: Payload base */
    ldr x1, =6144               /* Arg 1: Length = 6144 bytes */
    ldr x2, =0x401FE100         /* Arg 2: 128-byte scratchpad for padding block */
    ldr x3, =0x401FE080         /* Arg 3: 32-byte digest output buffer */
    bl sha256_compute

    /* Compare computed 32-byte SHA-256 against pinned expected digest */
    ldr x2, =0x401FE080
    adr x3, pinned_sha256_digest

    /* Compare 8 x 32-bit words (2 x 64-bit pair loads) */
    ldp x4, x5, [x2, #0]
    ldp x6, x7, [x3, #0]
    cmp x4, x6
    b.ne .Lrefuse_handoff
    cmp x5, x7
    b.ne .Lrefuse_handoff

    ldp x4, x5, [x2, #16]
    ldp x6, x7, [x3, #16]
    cmp x4, x6
    b.ne .Lrefuse_handoff
    cmp x5, x7
    b.ne .Lrefuse_handoff

    /* 8. Emit Diagnostic Checkpoint 3: HANDOFF */
    adr x21, msg_handoff
    bl print_string

    /* 9. Establish PHYSICS_ENTRY_ABI */
    ldr x0, =ABI_X0             /* x0 = pointer to Boot Descriptor (0x401FE000) */
    ldr x1, =ABI_X1             /* x1 = payload size (6144 bytes) */
    ldr x2, =ABI_X2             /* x2 = verification cookie ('PHYSICS0') */

    /* Static Target Proof: x19 is explicitly loaded from literal 0x40200000 immediately before branch */
    ldr x19, =0x40200000        /* Target: Contract-defined Physics Entry */

    /* 10. Transfer Control to Physics */
    br x19

    /* Terminal fallthrough trap */
    b .Lquiescent_halt

.Lrefuse_handoff:
    /* Emit Refusal Diagnostic */
    adr x21, msg_refuse
    bl print_string

.Lquiescent_halt:
    /* Fail-Closed Quiescence */
    wfe
    b .Lquiescent_halt

/* Helper: Polled UART String Output */
print_string:
.Lprint_char:
    ldrb w0, [x21], #1
    cbz w0, .Lprint_ret
    str w0, [x20]
    b .Lprint_char
.Lprint_ret:
    ret

.balign 8
msg_awaken:
    .asciz "ATLAS: AWAKEN\n"
msg_verify:
    .asciz "ATLAS: VERIFY\n"
msg_handoff:
    .asciz "ATLAS: HANDOFF\n"
msg_refuse:
    .asciz "ATLAS: REFUSE\n"

.balign 8
pinned_sha256_digest:
    .include "physics_pin.inc"

.ltorg
.balign 64
