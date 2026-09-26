/*
 * m3/vector_table.s - 2,048-Byte Aligned AArch64 Exception Vector Table
 * Milestone 3: PHYSICS_EFFECTS (Sovereign Machine)
 *
 * Vector Table Geometry:
 *   Base Alignment: 2,048 bytes (0x800)
 *   Total Span: exactly 2,048 bytes (0x000..0x7FF)
 *   16 architectural vector slots spaced exactly 128 bytes (0x80) apart:
 *     0x000: Current EL with SP0 (Sync)
 *     0x080: Current EL with SP0 (IRQ)
 *     0x100: Current EL with SP0 (FIQ)
 *     0x180: Current EL with SP0 (SError)
 *     0x200: Current EL with SPx (Sync -> Main Trap)
 *     0x280: Current EL with SPx (IRQ)
 *     0x300: Current EL with SPx (FIQ)
 *     0x380: Current EL with SPx (SError)
 *     0x400: Lower EL AArch64 (Sync)
 *     0x480: Lower EL AArch64 (IRQ)
 *     0x500: Lower EL AArch64 (FIQ)
 *     0x580: Lower EL AArch64 (SError)
 *     0x600: Lower EL AArch32 (Sync)
 *     0x680: Lower EL AArch32 (IRQ)
 *     0x700: Lower EL AArch32 (FIQ)
 *     0x780: Lower EL AArch32 (SError)
 */

    .global vector_table

    .section .text.vectors, "ax"
    .balign 2048

vector_table:
/* Slot 0 (0x000): Current EL with SP0 - Sync */
    mov x0, #0
    b common_trap
    .balign 128

/* Slot 1 (0x080): Current EL with SP0 - IRQ */
    mov x0, #1
    b common_trap
    .balign 128

/* Slot 2 (0x100): Current EL with SP0 - FIQ */
    mov x0, #2
    b common_trap
    .balign 128

/* Slot 3 (0x180): Current EL with SP0 - SError */
    mov x0, #3
    b common_trap
    .balign 128

/* Slot 4 (0x200): Current EL with SPx - Sync */
    mov x0, #4
    b common_trap
    .balign 128

/* Slot 5 (0x280): Current EL with SPx - IRQ */
    mov x0, #5
    b common_trap
    .balign 128

/* Slot 6 (0x300): Current EL with SPx - FIQ */
    mov x0, #6
    b common_trap
    .balign 128

/* Slot 7 (0x380): Current EL with SPx - SError */
    mov x0, #7
    b common_trap
    .balign 128

/* Slot 8 (0x400): Lower EL AArch64 - Sync */
    mov x0, #8
    b common_trap
    .balign 128

/* Slot 9 (0x480): Lower EL AArch64 - IRQ */
    mov x0, #9
    b common_trap
    .balign 128

/* Slot 10 (0x500): Lower EL AArch64 - FIQ */
    mov x0, #10
    b common_trap
    .balign 128

/* Slot 11 (0x580): Lower EL AArch64 - SError */
    mov x0, #11
    b common_trap
    .balign 128

/* Slot 12 (0x600): Lower EL AArch32 - Sync */
    mov x0, #12
    b common_trap
    .balign 128

/* Slot 13 (0x680): Lower EL AArch32 - IRQ */
    mov x0, #13
    b common_trap
    .balign 128

/* Slot 14 (0x700): Lower EL AArch32 - FIQ */
    mov x0, #14
    b common_trap
    .balign 128

/* Slot 15 (0x780): Lower EL AArch32 - SError */
    mov x0, #15
    b common_trap
    .balign 128

/* Common Trap Handler in general .text section */
    .section .text, "ax"
    .balign 4
common_trap:
    /* Capture trap state to PHYSICS_BOOT_STATE + 0x80 = 0x40208880 */
    ldr x1, =0x40208880

    /* Save x2..x30 first */
    stp x2, x3, [x1, #16]
    stp x4, x5, [x1, #32]
    stp x6, x7, [x1, #48]
    stp x8, x9, [x1, #64]
    stp x10, x11, [x1, #80]
    stp x12, x13, [x1, #96]
    stp x14, x15, [x1, #112]
    stp x16, x17, [x1, #128]
    stp x18, x19, [x1, #144]
    stp x20, x21, [x1, #160]
    stp x22, x23, [x1, #176]
    stp x24, x25, [x1, #192]
    stp x26, x27, [x1, #208]
    stp x28, x29, [x1, #224]
    str x30, [x1, #240]

    /* Save slot index to +0x100 */
    str x0, [x1, #0x100]

    /* Save SP to +0x0F8 */
    mov x2, sp
    str x2, [x1, #0x0F8]

    /* Save CurrentEL */
    mrs x3, CurrentEL
    str x3, [x1, #0x108]

    /* Save ESR, ELR, SPSR */
    mrs x4, esr_el1
    str x4, [x1, #0x110]
    mrs x5, elr_el1
    str x5, [x1, #0x118]
    mrs x6, spsr_el1
    str x6, [x1, #0x120]

    /* Evaluate ESR.EC to check FAR validity */
    lsr x7, x4, #26             /* x7 = EC */
    cmp x7, #0x20               /* Instruction abort lower EL */
    b.eq .Lfar_valid
    cmp x7, #0x21               /* Instruction abort same EL */
    b.eq .Lfar_valid
    cmp x7, #0x22               /* PC alignment */
    b.eq .Lfar_valid
    cmp x7, #0x24               /* Data abort lower EL */
    b.eq .Lfar_valid
    cmp x7, #0x25               /* Data abort same EL */
    b.eq .Lfar_valid
    cmp x7, #0x26               /* SP alignment */
    b.eq .Lfar_valid

    /* FAR not valid */
    str xzr, [x1, #0x128]
    str xzr, [x1, #0x130]
    b .Lwrite_magic

.Lfar_valid:
    mrs x8, far_el1
    str x8, [x1, #0x128]
    mov x9, #1
    str x9, [x1, #0x130]

.Lwrite_magic:
    /* Write magic TRAPFRM1 (0x314D524650415254) last */
    ldr x10, =0x314D524650415254
    str x10, [x1, #0x138]

    /* Emit diagnostic trap notification to UART */
    ldr x1, =0x09000000
    adr x2, .Lmsg_trap
.Lput_trap_loop:
    ldrb w3, [x2], #1
    cbz w3, .Lput_trap_done
1:  ldrb w4, [x1, #24]
    tst w4, #32
    b.ne 1b
    strb w3, [x1]
    b .Lput_trap_loop
.Lput_trap_done:

    /* Quiesce into terminal loop */
2:  wfe
    b 2b

.Lmsg_trap:
    .asciz "PHYSICS: TRAP_EXCEPTION\n"
