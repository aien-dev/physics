/*
 * m3/physics.s - Bare-Metal Authority Nucleus for Milestone 3 (PHYSICS_EFFECTS)
 * Target Platform: QEMU virt AArch64 / Sovereign Machine
 * Entry Point: _physics_entry @ 0x40200000
 * ABI: PHYSICS_ENTRY_ABI (x0 = 0x401FE000, x1 = 18432, x2 = 'PHYSICS0')
 */

    .global _physics_entry
    .global _start
    .global print_string
    .global print_char

    .extern vector_table
    .extern init_frame_authority
    .extern init_capability_table
    .extern init_receipt_ledger
    .extern init_replay_cache
    .extern run_m3_unit_tests

    .section .text.entry, "ax"
    .balign 64

_physics_entry:
_start:
    /* 0. Initialize Physics Kernel Stack @ 0x40208800 (growing downward) */
    ldr x3, =0x40208800
    mov sp, x3

    /* 1. Validate Verification Cookie in x2: 'PHYSICS0' (0x5048595349435330) */
    ldr x3, =0x5048595349435330
    cmp x2, x3
    b.ne .Lpanic_bad_cookie

    /* 1b. Validate payload size in x1 (18,432 bytes) */
    ldr x3, =18432
    cmp x1, x3
    b.ne .Lpanic_bad_payload_size

    /* 2. Validate Boot Descriptor Pointer in x0: 0x401FE000 */
    ldr x3, =0x401FE000
    cmp x0, x3
    b.ne .Lpanic_bad_descriptor

    /* 3. Validate Descriptor Structural Header */
    ldr x4, [x0, #0]
    ldr x5, =0x4D424453435F3031          /* 'MBDSC_01' */
    cmp x4, x5
    b.ne .Lpanic_bad_desc_magic

    ldr w4, [x0, #8]                     /* version == 1 */
    cmp w4, #1
    b.ne .Lpanic_bad_desc_version

    ldr w4, [x0, #12]                    /* length == 64 */
    cmp w4, #64
    b.ne .Lpanic_bad_desc_length

    ldr x4, [x0, #16]                    /* flags_reserved == 0 */
    cbnz x4, .Lpanic_bad_desc_reserved

    /* 4. Validate Machine Profile */
    ldr x19, [x0, #24]                   /* ram_base == 0x40000000 */
    ldr x5, =0x40000000
    cmp x19, x5
    b.ne .Lpanic_ingress_corrupt

    ldr x20, [x0, #32]                   /* ram_size == 0x08000000 (128 MiB) */
    ldr x5, =0x08000000
    cmp x20, x5
    b.ne .Lpanic_ingress_corrupt

    ldr x21, [x0, #40]                   /* uart_base == 0x09000000 */
    ldr x5, =0x09000000
    cmp x21, x5
    b.ne .Lpanic_ingress_corrupt

    ldr x22, [x0, #48]                   /* physics_base == 0x40200000 */
    ldr x5, =0x40200000
    cmp x22, x5
    b.ne .Lpanic_ingress_corrupt

    ldr x23, [x0, #56]                   /* physics_size == 18432 */
    ldr x5, =18432
    cmp x23, x5
    b.ne .Lpanic_ingress_corrupt

    /* 5. Copy Validated Ingress Data to PHYSICS_BOOT_STATE @ 0x40208800 */
    ldr x8, =0x40208800
    ldp x4, x5, [x0, #0]
    stp x4, x5, [x8, #0]
    ldp x4, x5, [x0, #16]
    stp x4, x5, [x8, #16]
    ldp x4, x5, [x0, #32]
    stp x4, x5, [x8, #32]
    ldp x4, x5, [x0, #48]
    stp x4, x5, [x8, #48]

    /* 6. Verify CurrentEL == EL1 (0x04) */
    mrs x9, CurrentEL
    and x9, x9, #0x0C
    cmp x9, #0x04
    b.ne .Lpanic_uncontracted_el
    str x9, [x8, #0x40]                  /* Record verified CurrentEL */

    /* 7. Switch to Dedicated Kernel Stack: SP = 0x40208800 */
    ldr x10, =0x40208800
    mov sp, x10
    mov x0, #0                           /* Drop Atlas scratchpad reference */

    /* 8. Diagnostic Checkpoints */
    adr x0, msg_awaken
    bl print_string

    adr x0, msg_ingress_valid
    bl print_string

    adr x0, msg_entry_el1_verified
    bl print_string

    /* 9. Install Exception Vector Table */
    ldr x1, =vector_table
    msr vbar_el1, x1
    isb

    adr x0, msg_vbar_installed
    bl print_string

    /* 10. Initialize Physical Frame Authority */
    ldr x8, =0x40208800
    ldr x0, [x8, #24]                   /* ram_base */
    ldr x1, [x8, #32]                   /* ram_size */
    bl init_frame_authority
    cbz x0, .Lpanic_frame_authority

    adr x0, msg_frame_auth_bound
    bl print_string

    /* 11. Initialize Capability Table (CAP_ROOT Genesis) */
    bl init_capability_table
    adr x0, msg_cap_root_genesis
    bl print_string

    /* 12. Initialize Receipt Ledger */
    bl init_receipt_ledger
    adr x0, msg_receipt_ledger_init
    bl print_string

    /* 13. Initialize Replay Cache */
    bl init_replay_cache
    adr x0, msg_replay_cache_init
    bl print_string

    /* 14. Milestone 3 Ready */
    adr x0, msg_quiescent_ready
    bl print_string

    /* 15. Execute Milestone 3 Unit Tests */
    bl run_m3_unit_tests

.Lquiescent_halt:
    wfe
    b .Lquiescent_halt

/* Panic handlers */
.Lpanic_bad_cookie:
    adr x0, msg_panic_cookie
    bl print_string
    b .Lquiescent_halt

.Lpanic_bad_payload_size:
    adr x0, msg_panic_payload_size
    bl print_string
    b .Lquiescent_halt

.Lpanic_bad_descriptor:
    adr x0, msg_panic_desc
    bl print_string
    b .Lquiescent_halt

.Lpanic_bad_desc_magic:
    adr x0, msg_panic_desc_magic
    bl print_string
    b .Lquiescent_halt

.Lpanic_bad_desc_version:
    adr x0, msg_panic_desc_version
    bl print_string
    b .Lquiescent_halt

.Lpanic_bad_desc_length:
    adr x0, msg_panic_desc_length
    bl print_string
    b .Lquiescent_halt

.Lpanic_bad_desc_reserved:
    adr x0, msg_panic_desc_reserved
    bl print_string
    b .Lquiescent_halt

.Lpanic_ingress_corrupt:
    adr x0, msg_panic_ingress
    bl print_string
    b .Lquiescent_halt

.Lpanic_uncontracted_el:
    adr x0, msg_panic_el
    bl print_string
    b .Lquiescent_halt

.Lpanic_frame_authority:
    adr x0, msg_panic_frame_auth
    bl print_string
    b .Lquiescent_halt

/* UART Polled Telemetry */
print_string:
    ldr x1, =0x09000000                  /* PL011 UART */
.Lputs_loop:
    ldrb w2, [x0], #1
    cbz w2, .Lputs_done
1:  ldrb w3, [x1, #24]                   /* UARTFR */
    tst w3, #32                          /* TXFF */
    b.ne 1b
    strb w2, [x1]
    b .Lputs_loop
.Lputs_done:
    ret

print_char:
    ldr x1, =0x09000000
1:  ldrb w2, [x1, #24]
    tst w2, #32
    b.ne 1b
    strb w0, [x1]
    ret

    .section .rodata
    .balign 8
msg_awaken:
    .asciz "PHYSICS: AWAKEN\n"
msg_ingress_valid:
    .asciz "PHYSICS: INGRESS_VALID\n"
msg_entry_el1_verified:
    .asciz "PHYSICS: ENTRY_EL1_VERIFIED\n"
msg_vbar_installed:
    .asciz "PHYSICS: VBAR_INSTALLED\n"
msg_frame_auth_bound:
    .asciz "PHYSICS: FRAME_AUTH_BOUND\n"
msg_cap_root_genesis:
    .asciz "PHYSICS: CAP_ROOT_GENESIS\n"
msg_receipt_ledger_init:
    .asciz "PHYSICS: RECEIPT_LEDGER_INIT\n"
msg_replay_cache_init:
    .asciz "PHYSICS: REPLAY_CACHE_INIT\n"
msg_quiescent_ready:
    .asciz "PHYSICS: QUIESCENT_READY\n"

msg_panic_cookie:
    .asciz "PANIC: BAD VERIFICATION COOKIE\n"
msg_panic_payload_size:
    .asciz "PANIC: BAD PAYLOAD SIZE\n"
msg_panic_desc:
    .asciz "PANIC: BAD BOOT DESCRIPTOR POINTER\n"
msg_panic_desc_magic:
    .asciz "PANIC: BAD DESCRIPTOR MAGIC\n"
msg_panic_desc_version:
    .asciz "PANIC: UNSUPPORTED DESCRIPTOR VERSION\n"
msg_panic_desc_length:
    .asciz "PANIC: TRUNCATED/OVERSIZED DESCRIPTOR\n"
msg_panic_desc_reserved:
    .asciz "PANIC: NON-ZERO RESERVED FLAGS\n"
msg_panic_ingress:
    .asciz "PANIC: INGRESS MACHINE PROFILE VIOLATION\n"
msg_panic_el:
    .asciz "PANIC: UNCONTRACTED ENTRY EXCEPTION LEVEL\n"
msg_panic_frame_auth:
    .asciz "PANIC: FRAME AUTHORITY REJECTED\n"
