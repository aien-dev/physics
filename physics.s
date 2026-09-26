/*
 * physics.s - Bare-Metal Authority Nucleus for Milestone 2 (PHYSICS_BOOT)
 * Target Platform: QEMU virt AArch64 / Sovereign Machine
 * Entry Point: _physics_entry @ 0x40200000
 * ABI: PHYSICS_ENTRY_ABI
 */

    .global _physics_entry
    .global _start
    .global print_string
    .global print_hex64
    .global print_char

    .extern vector_table
    .extern init_frame_authority
    .extern init_root_capability

    .section .text.entry, "ax"
    .balign 64

_physics_entry:
_start:
    /*
     * 1. Validate Verification Cookie in x2:
     *    Must match strictly 0x5048595349435330 (ASCII 'PHYSICS0').
     */
    ldr x3, =0x5048595349435330
    cmp x2, x3
    b.ne .Lpanic_bad_cookie

    /*
     * 1b. Validate payload size in x1 against PHYSICS_ENTRY_ABI (6144 bytes).
     */
    ldr x3, =6144
    cmp x1, x3
    b.ne .Lpanic_bad_payload_size

    /*
     * 2. Validate Boot Descriptor Pointer in x0:
     *    Must match strictly 0x401FE000.
     */
    ldr x3, =0x401FE000
    cmp x0, x3
    b.ne .Lpanic_bad_descriptor

    /*
     * 3. Validate Descriptor Structural Header:
     *    +0x00: magic (u64 == 0x4D424453435F3031, 'MBDSC_01')
     *    +0x08: version (u32 == 1)
     *    +0x0C: length (u32 == 64)
     *    +0x10: flags_reserved (u64 == 0)
     */
    ldr x4, [x0, #0]
    ldr x5, =0x4D424453435F3031
    cmp x4, x5
    b.ne .Lpanic_bad_desc_magic

    ldr w4, [x0, #8]
    cmp w4, #1
    b.ne .Lpanic_bad_desc_version

    ldr w4, [x0, #12]
    cmp w4, #64
    b.ne .Lpanic_bad_desc_length

    ldr x4, [x0, #16]
    cbnz x4, .Lpanic_bad_desc_reserved

    /*
     * 4. Validate Machine Profile (Ingress Authority Anti-Expansion Boundary):
     *    For CONTRACT-QEMU-VIRT-AARCH64-M2, require EXACT machine profile matches:
     *    +0x18: ram_base     == 0x40000000
     *    +0x20: ram_size     == 0x08000000 (128 MiB, DRAM_END strictly 0x48000000)
     *    +0x28: uart_base    == 0x09000000 (PL011 MMIO)
     *    +0x30: physics_base == 0x40200000
     *    +0x38: physics_size == 6144
     */
    ldr x19, [x0, #24]          /* x19 = ram_base */
    ldr x5, =0x40000000
    cmp x19, x5
    b.ne .Lpanic_ingress_corrupt

    ldr x20, [x0, #32]          /* x20 = ram_size */
    ldr x5, =0x08000000
    cmp x20, x5
    b.ne .Lpanic_ingress_corrupt

    ldr x21, [x0, #40]          /* x21 = uart_base */
    ldr x5, =0x09000000
    cmp x21, x5
    b.ne .Lpanic_ingress_corrupt

    ldr x22, [x0, #48]          /* x22 = physics_base */
    ldr x5, =0x40200000
    cmp x22, x5
    b.ne .Lpanic_ingress_corrupt

    ldr x23, [x0, #56]          /* x23 = physics_size */
    ldr x5, =6144
    cmp x23, x5
    b.ne .Lpanic_ingress_corrupt

    /*
     * 5. Copy Validated Ingress Data to PHYSICS_BOOT_STATE @ 0x40205800:
     *    Copy all 64 bytes (8 x 64-bit words) from [x0] to [0x40205800].
     */
    ldr x8, =0x40205800
    ldp x4, x5, [x0, #0]
    stp x4, x5, [x8, #0]
    ldp x4, x5, [x0, #16]
    stp x4, x5, [x8, #16]
    ldp x4, x5, [x0, #32]
    stp x4, x5, [x8, #32]
    ldp x4, x5, [x0, #48]
    stp x4, x5, [x8, #48]

    /*
     * 6. Contract-Driven Exception Level Verification (Item 6):
     *    For CONTRACT-QEMU-VIRT-AARCH64-M2, entry level MUST be strictly EL1 (0x04).
     */
    mrs x9, CurrentEL
    and x9, x9, #0x0C
    cmp x9, #0x04
    b.ne .Lpanic_uncontracted_el

    /*
     * Record the verified CurrentEL in the PHYSICS_BOOT_STATE entry record
     * (+0x40), outside the 64-byte descriptor copy at +0x00..+0x3F.
     */
    str x9, [x8, #0x40]

    /*
     * 7. Switch to Dedicated Kernel Stack:
     *    SP = 0x40205800 (growing downward toward 0x40201800, 16 KiB span).
     *    Zero x0 to abandon Atlas scratchpad pointer.
     */
    ldr x10, =0x40205800
    mov sp, x10
    mov x0, #0

    /*
     * 8. Emit Diagnostic Checkpoints:
     */
    ldr x0, =msg_awaken
    bl print_string

    ldr x0, =msg_ingress_valid
    bl print_string

    ldr x0, =msg_entry_el1_verified
    bl print_string

    /*
     * 9. Install Exception Vector Table:
     */
    ldr x1, =vector_table
    msr vbar_el1, x1
    isb

    ldr x0, =msg_vbar_installed
    bl print_string

    /*
     * 10. Initialize Physical Frame Authority:
     *     init_frame_authority(ram_base=0x40000000, ram_size=0x08000000)
     */
    ldr x8, =0x40205800
    ldr x0, [x8, #24]           /* ram_base from PHYSICS-owned copy */
    ldr x1, [x8, #32]           /* ram_size from PHYSICS-owned copy */
    bl init_frame_authority
    cbz x0, .Lpanic_frame_authority

    /*
     * 11. Initialize Root Capability (CAP_ROOT):
     *     init_root_capability()
     */
    bl init_root_capability

    /*
     * 12. Normal Boot Milestone Complete:
     *     Emit "PHYSICS: QUIESCENT_READY\n" and enter wfe loop.
     */
    ldr x0, =msg_quiescent_ready
    bl print_string

.Lquiescent_halt:
    wfe
    b .Lquiescent_halt

/*
 * Fail-Closed Panic Traps
 */
.Lpanic_bad_cookie:
    ldr x0, =msg_panic_cookie
    bl print_string
    b .Lquiescent_halt

.Lpanic_bad_descriptor:
    ldr x0, =msg_panic_desc
    bl print_string
    b .Lquiescent_halt

.Lpanic_bad_desc_magic:
    ldr x0, =msg_panic_desc_magic
    bl print_string
    b .Lquiescent_halt

.Lpanic_bad_desc_version:
    ldr x0, =msg_panic_desc_version
    bl print_string
    b .Lquiescent_halt

.Lpanic_bad_desc_length:
    ldr x0, =msg_panic_desc_length
    bl print_string
    b .Lquiescent_halt

.Lpanic_bad_desc_reserved:
    ldr x0, =msg_panic_desc_reserved
    bl print_string
    b .Lquiescent_halt

.Lpanic_ingress_corrupt:
    ldr x0, =msg_panic_ingress
    bl print_string
    b .Lquiescent_halt

.Lpanic_bad_payload_size:
    ldr x0, =msg_panic_payload_size
    bl print_string
    b .Lquiescent_halt

.Lpanic_frame_authority:
    ldr x0, =msg_panic_frame_auth
    bl print_string
    b .Lquiescent_halt

.Lpanic_uncontracted_el:
    ldr x0, =msg_panic_el
    bl print_string
    b .Lquiescent_halt

/*
 * Polled UART String Routines
 * Uses MMIO base 0x09000000 (PL011 UART).
 * Clobbers only x0, x2 and x16 (print_string also x1). Callee-saved
 * registers x19-x28 are never touched, so callers may keep state there.
 */
print_string:
    ldr x16, =0x09000000
.Lprint_str_loop:
    ldrb w1, [x0], #1
    cbz w1, .Lprint_str_ret
.Lpoll_txff:
    ldr w2, [x16, #24]          /* UART Flag Register (FR) */
    tbnz w2, #5, .Lpoll_txff    /* Wait if TXFF (bit 5) is set */
    str w1, [x16, #0]           /* UART Data Register (DR) */
    b .Lprint_str_loop
.Lprint_str_ret:
    ret

print_char:
    ldr x16, =0x09000000
.Lpoll_char:
    ldr w2, [x16, #24]
    tbnz w2, #5, .Lpoll_char
    str w0, [x16, #0]
    ret

print_hex64:
    /* Prints 64-bit value in x0 as 16 hex digits with '0x' prefix */
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    stp x19, x20, [sp, #16]

    mov x19, x0

    /* Print '0x' */
    mov w0, #'0'
    bl print_char
    mov w0, #'x'
    bl print_char

    /* Print 16 nibbles from MSB to LSB */
    mov w20, #60
.Lhex_loop:
    lsr x2, x19, x20
    and w2, w2, #0x0F
    cmp w2, #10
    b.lt .Lhex_digit
    add w0, w2, #('a' - 10)
    b .Lhex_out
.Lhex_digit:
    add w0, w2, #'0'
.Lhex_out:
    bl print_char
    subs w20, w20, #4
    b.ge .Lhex_loop

    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #32
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
msg_quiescent_ready:
    .asciz "PHYSICS: QUIESCENT_READY\n"

msg_panic_cookie:
    .asciz "PHYSICS: PANIC_BAD_COOKIE\n"
msg_panic_desc:
    .asciz "PHYSICS: PANIC_BAD_DESCRIPTOR_PTR\n"
msg_panic_desc_magic:
    .asciz "PHYSICS: PANIC_BAD_DESC_MAGIC\n"
msg_panic_desc_version:
    .asciz "PHYSICS: PANIC_BAD_DESC_VERSION\n"
msg_panic_desc_length:
    .asciz "PHYSICS: PANIC_BAD_DESC_LENGTH\n"
msg_panic_desc_reserved:
    .asciz "PHYSICS: PANIC_BAD_DESC_RESERVED\n"
msg_panic_ingress:
    .asciz "PHYSICS: PANIC_INGRESS_CORRUPT\n"
msg_panic_payload_size:
    .asciz "PHYSICS: PANIC_BAD_PAYLOAD_SIZE\n"
msg_panic_frame_auth:
    .asciz "PHYSICS: PANIC_FRAME_AUTHORITY\n"
msg_panic_el:
    .asciz "PHYSICS: PANIC_UNCONTRACTED_EL\n"
