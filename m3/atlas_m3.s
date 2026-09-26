/*
 * m3/atlas_m3.s - Atlas Bootstrap Seed for Milestone 3 (PHYSICS_EFFECTS)
 * Target Platform: QEMU virt AArch64 / Sovereign Machine
 * Normative Base: 0x00000000
 *
 * Populates Machine Boot Descriptor @ 0x401FE000, verifies physics.bin
 * (18,432 bytes) at 0x40200000 with NIST SHA-256 against pinned digest,
 * sets up PHYSICS_ENTRY_ABI registers, and executes handoff jump.
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
.equ DESC_PHYSICS_SIZE, 18432
.endif
.ifndef ABI_X0
.equ ABI_X0, 0x401FE000
.endif
.ifndef ABI_X1
.equ ABI_X1, 18432
.endif
.ifndef ABI_X2
.equ ABI_X2, 0x5048595349435330
.endif

    .global _start
    .section .text
    .balign 64

_start:
    /* 1. Mask all interrupts (DAIF) */
    msr daifset, #0xf

    /* 2. Initialize Stack Pointer in scratchpad RAM (0x401FC000) */
    ldr x0, =0x401FC000
    mov sp, x0

    /* 3. Emit Checkpoint 1: AWAKEN */
    adr x21, msg_awaken
    bl print_string

    /* 4. Populate Machine Boot Descriptor @ 0x401FE000 */
    ldr x22, =0x401FE000
    ldr x0, =DESC_MAGIC
    str x0, [x22, #0]
    ldr w0, =DESC_VERSION
    str w0, [x22, #8]
    ldr w0, =DESC_LENGTH
    str w0, [x22, #12]
    ldr x0, =DESC_FLAGS
    str x0, [x22, #16]
    ldr x0, =DESC_RAM_BASE
    str x0, [x22, #24]
    ldr x0, =DESC_RAM_SIZE
    str x0, [x22, #32]
    ldr x0, =DESC_UART_BASE
    str x0, [x22, #40]
    ldr x0, =DESC_PHYSICS_BASE
    str x0, [x22, #48]
    ldr x0, =DESC_PHYSICS_SIZE
    str x0, [x22, #56]

    /* 5. Emit Checkpoint 2: VERIFY */
    adr x21, msg_verify
    bl print_string

    /* 6. Verify PHYSICS Integrity using SHA-256 */
    ldr x0, =0x40200000         /* Payload base */
    ldr x1, =18432              /* Payload size */
    ldr x2, =0x401FE100         /* 128-byte scratchpad */
    ldr x3, =0x401FE080         /* 32-byte digest output buffer */
    bl sha256_compute

    /* 7. Compare computed digest with pinned digest */
    ldr x0, =0x401FE080
    adr x1, pinned_physics_sha256
    mov x2, #4                  /* 4 pairs of 64-bit words */
.Lcmp_digest:
    cbz x2, .Lverify_pass
    ldr x3, [x0], #8
    ldr x4, [x1], #8
    cmp x3, x4
    b.ne .Lverify_fail
    sub x2, x2, #1
    b .Lcmp_digest

.Lverify_pass:
    /* 8. Emit Checkpoint 3: HANDOFF */
    adr x21, msg_handoff
    bl print_string

    /* 9. Set up PHYSICS_ENTRY_ABI registers */
    ldr x0, =ABI_X0
    ldr x1, =ABI_X1
    ldr x2, =ABI_X2
    ldr x19, =0x40200000
    br x19                      /* Transfer physical authority to Physics */

.Lverify_fail:
    adr x21, msg_panic_verify
    bl print_string
.Lpanic_halt:
    wfe
    b .Lpanic_halt

/* Diagnostic UART */
print_string:
    ldr x20, =0x09000000
.Lput_loop:
    ldrb w0, [x21], #1
    cbz w0, .Lput_done
1:  ldrb w1, [x20, #24]
    tst w1, #32
    b.ne 1b
    strb w0, [x20]
    b .Lput_loop
.Lput_done:
    ret

    .section .rodata
    .balign 8
msg_awaken:
    .asciz "ATLAS: AWAKEN\n"
msg_verify:
    .asciz "ATLAS: VERIFY\n"
msg_handoff:
    .asciz "ATLAS: HANDOFF\n"
msg_panic_verify:
    .asciz "PANIC: VERIFICATION_FAILED\n"

    .balign 8
pinned_physics_sha256:
    .include "m3_physics_pin.inc"
