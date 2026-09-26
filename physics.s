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
     *    Must match 0x5048595349435330 (ASCII 'PHYSICS0').
     */
    ldr x3, =0x5048595349435330
    cmp x2, x3
    b.ne .Lpanic_bad_cookie

    /*
     * 2. Validate Boot Descriptor Pointer in x0:
     *    Must match 0x401FE000.
     */
    ldr x3, =0x401FE000
    cmp x0, x3
    b.ne .Lpanic_bad_descriptor

    /*
     * 3. Validate Descriptor Contents (Ingress Validation Boundary):
     *    [x0, #0]  : RAM Base
     *    [x0, #8]  : RAM Size
     *    [x0, #16] : UART MMIO Base
     */
    ldr x19, [x0, #0]           /* x19 = RAM Base (e.g. 0x40000000) */
    ldr x20, [x0, #8]           /* x20 = RAM Size (e.g. 0x08000000) */
    ldr x21, [x0, #16]          /* x21 = UART MMIO Base (0x09000000) */

    /* Check RAM Size non-zero */
    cbz x20, .Lpanic_ingress_corrupt

    /* Check arithmetic overflow: RAM Base + RAM Size */
    add x4, x19, x20
    cmp x4, x19
    b.ls .Lpanic_ingress_corrupt /* Overflowed */

    /* Check Physics Image Containment: 0x40200000 >= RAM Base && 0x40208000 <= RAM End */
    ldr x5, =0x40200000
    cmp x5, x19
    b.lo .Lpanic_ingress_corrupt
    ldr x6, =0x40208000
    cmp x6, x4
    b.hi .Lpanic_ingress_corrupt

    /* Check UART MMIO Base: strictly 0x09000000 on QEMU virt */
    ldr x7, =0x09000000
    cmp x21, x7
    b.ne .Lpanic_ingress_corrupt

    /*
     * 4. Copy Validated Ingress Data to PHYSICS_BOOT_STATE @ 0x40205800:
     */
    ldr x8, =0x40205800
    str x19, [x8, #0]           /* +0: RAM Base */
    str x20, [x8, #8]           /* +8: RAM Size */
    str x21, [x8, #16]          /* +16: UART MMIO Base */

    mrs x9, CurrentEL
    and x9, x9, #0x0C
    str x9, [x8, #24]           /* +24: Ingress CurrentEL */

    /*
     * 5. Switch to Dedicated Kernel Stack:
     *    SP = 0x40205800 (growing downward toward 0x40201800, 16 KiB span).
     *    Zero x0 to abandon Atlas scratchpad pointer.
     */
    ldr x10, =0x40205800
    mov sp, x10
    mov x0, #0

    /*
     * 6. Emit Initial Diagnostics:
     */
    ldr x0, =msg_awaken
    bl print_string

    ldr x0, =msg_ingress_valid
    bl print_string

    /*
     * 7. Contract-Driven Exception Level & VBAR Setup:
     *    If CurrentEL == 0x04 (EL1): msr vbar_el1, x1
     *    If CurrentEL == 0x08 (EL2): msr vbar_el2, x1
     *    Else: panic uncontracted EL
     */
    ldr x1, =vector_table
    cmp x9, #0x04
    b.eq .Linstall_vbar_el1
    cmp x9, #0x08
    b.eq .Linstall_vbar_el2
    b .Lpanic_uncontracted_el

.Linstall_vbar_el1:
    msr vbar_el1, x1
    isb
    b .Lvbar_done

.Linstall_vbar_el2:
    msr vbar_el2, x1
    isb

.Lvbar_done:
    ldr x0, =msg_vbar_installed
    bl print_string

    /*
     * 8. Initialize Physical Frame Authority:
     *    init_frame_authority(ram_base=x19, ram_size=x20)
     */
    mov x0, x19
    mov x1, x20
    bl init_frame_authority

    /*
     * 9. Initialize Root Capability (CAP_ROOT):
     *    init_root_capability()
     */
    bl init_root_capability

    /*
     * 10. Normal Boot Milestone Complete:
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
    ldr x20, =0x09000000
    ldr x0, =msg_panic_cookie
    bl print_string
    b .Lquiescent_halt

.Lpanic_bad_descriptor:
    ldr x20, =0x09000000
    ldr x0, =msg_panic_desc
    bl print_string
    b .Lquiescent_halt

.Lpanic_ingress_corrupt:
    ldr x20, =0x09000000
    ldr x0, =msg_panic_ingress
    bl print_string
    b .Lquiescent_halt

.Lpanic_uncontracted_el:
    ldr x20, =0x09000000
    ldr x0, =msg_panic_el
    bl print_string
    b .Lquiescent_halt

/*
 * Polled UART String Routines
 * Uses MMIO base 0x09000000 (PL011 UART)
 */
print_string:
    ldr x20, =0x09000000
.Lprint_str_loop:
    ldrb w1, [x0], #1
    cbz w1, .Lprint_str_ret
.Lpoll_txff:
    ldr w2, [x20, #24]          /* UART Flag Register (FR) */
    tbnz w2, #5, .Lpoll_txff    /* Wait if TXFF (bit 5) is set */
    str w1, [x20, #0]           /* UART Data Register (DR) */
    b .Lprint_str_loop
.Lprint_str_ret:
    ret

print_char:
    ldr x20, =0x09000000
.Lpoll_char:
    ldr w2, [x20, #24]
    tbnz w2, #5, .Lpoll_char
    str w0, [x20, #0]
    ret

print_hex64:
    /* Prints 64-bit value in x0 as 16 hex digits with '0x' prefix */
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    str x19, [sp, #16]

    mov x19, x0

    /* Print '0x' */
    mov w0, #'0'
    bl print_char
    mov w0, #'x'
    bl print_char

    /* Print 16 nibbles from MSB to LSB */
    mov w1, #60
.Lhex_loop:
    lsr x2, x19, x1
    and w2, w2, #0x0F
    cmp w2, #10
    blt .Lhex_digit
    add w0, w2, #('a' - 10)
    b .Lhex_out
.Lhex_digit:
    add w0, w2, #'0'
.Lhex_out:
    bl print_char
    subs w1, w1, #4
    bge .Lhex_loop

    ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

    .section .rodata
    .balign 8
msg_awaken:
    .asciz "PHYSICS: AWAKEN\n"
msg_ingress_valid:
    .asciz "PHYSICS: INGRESS_VALID\n"
msg_vbar_installed:
    .asciz "PHYSICS: VBAR_INSTALLED\n"
msg_quiescent_ready:
    .asciz "PHYSICS: QUIESCENT_READY\n"

msg_panic_cookie:
    .asciz "PHYSICS: PANIC_BAD_COOKIE\n"
msg_panic_desc:
    .asciz "PHYSICS: PANIC_BAD_DESCRIPTOR_PTR\n"
msg_panic_ingress:
    .asciz "PHYSICS: PANIC_INGRESS_CORRUPT\n"
msg_panic_el:
    .asciz "PHYSICS: PANIC_UNCONTRACTED_EL\n"
