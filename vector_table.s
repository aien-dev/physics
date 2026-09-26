/*
 * vector_table.s - 2048-Byte Aligned 16-Entry AArch64 Exception Vector Table
 * Milestone 2: PHYSICS_BOOT (Patched Specification)
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
    .global trap_record_base
    .extern print_string
    .extern print_hex64

    .section .text.vectors, "ax"
    .balign 2048

vector_table:
/* Slot 0 (0x000): Current EL with SP0 - Sync */
    mov x0, #0
    b common_trap_entry
    .balign 128

/* Slot 1 (0x080): Current EL with SP0 - IRQ */
    mov x0, #1
    b common_trap_entry
    .balign 128

/* Slot 2 (0x100): Current EL with SP0 - FIQ */
    mov x0, #2
    b common_trap_entry
    .balign 128

/* Slot 3 (0x180): Current EL with SP0 - SError */
    mov x0, #3
    b common_trap_entry
    .balign 128

/* Slot 4 (0x200): Current EL with SPx - Sync */
    mov x0, #4
    b common_trap_entry
    .balign 128

/* Slot 5 (0x280): Current EL with SPx - IRQ */
    mov x0, #5
    b common_trap_entry
    .balign 128

/* Slot 6 (0x300): Current EL with SPx - FIQ */
    mov x0, #6
    b common_trap_entry
    .balign 128

/* Slot 7 (0x380): Current EL with SPx - SError */
    mov x0, #7
    b common_trap_entry
    .balign 128

/* Slot 8 (0x400): Lower EL AArch64 - Sync */
    mov x0, #8
    b common_trap_entry
    .balign 128

/* Slot 9 (0x480): Lower EL AArch64 - IRQ */
    mov x0, #9
    b common_trap_entry
    .balign 128

/* Slot 10 (0x500): Lower EL AArch64 - FIQ */
    mov x0, #10
    b common_trap_entry
    .balign 128

/* Slot 11 (0x580): Lower EL AArch64 - SError */
    mov x0, #11
    b common_trap_entry
    .balign 128

/* Slot 12 (0x600): Lower EL AArch32 - Sync */
    mov x0, #12
    b common_trap_entry
    .balign 128

/* Slot 13 (0x680): Lower EL AArch32 - IRQ */
    mov x0, #13
    b common_trap_entry
    .balign 128

/* Slot 14 (0x700): Lower EL AArch32 - FIQ */
    mov x0, #14
    b common_trap_entry
    .balign 128

/* Slot 15 (0x780): Lower EL AArch32 - SError */
    mov x0, #15
    b common_trap_entry
    .balign 128

/*
 * Common Trap Collector (Outside the 2048-byte table)
 * Captures raw architectural state into PHYSICS_BOOT_STATE:
 *   +0x40: VECTOR_SLOT (u64)
 *   +0x48: CURRENT_EL  (u64)
 *   +0x50: ESR_ELx     (u64)
 *   +0x58: ELR_ELx     (u64)
 *   +0x60: SPSR_ELx    (u64)
 *   +0x68: FAR_ELx     (u64)
 *   +0x70: FAR_VALID   (u64: 1 if EC in {0x20, 0x21, 0x22, 0x24, 0x25}, else 0)
 */
    .section .text, "ax"
    .balign 4
common_trap_entry:
    /* Preserve scratch registers in dedicated trap record @ 0x40205840 */
    ldr x19, =0x40205840
    str x0, [x19, #0]           /* Store VECTOR_SLOT */

    mrs x2, CurrentEL
    and x2, x2, #0x0C
    str x2, [x19, #8]           /* Store CURRENT_EL */

    cmp x2, #0x08
    b.eq .Lread_el2

    /* EL1 Traps */
    mrs x3, esr_el1
    str x3, [x19, #16]          /* Store ESR_EL1 */
    mrs x4, elr_el1
    str x4, [x19, #24]          /* Store ELR_EL1 */
    mrs x5, spsr_el1
    str x5, [x19, #32]          /* Store SPSR_EL1 */

    /* Evaluate EC for FAR validity: EC is bits [31:26] */
    lsr w6, w3, #26
    cmp w6, #0x20               /* Instruction Abort lower EL */
    b.eq .Lfar_valid_el1
    cmp w6, #0x21               /* Instruction Abort same EL */
    b.eq .Lfar_valid_el1
    cmp w6, #0x22               /* PC alignment */
    b.eq .Lfar_valid_el1
    cmp w6, #0x24               /* Data Abort lower EL */
    b.eq .Lfar_valid_el1
    cmp w6, #0x25               /* Data Abort same EL */
    b.eq .Lfar_valid_el1

    /* FAR not meaningful for this exception class */
    str xzr, [x19, #40]         /* FAR = 0 */
    str xzr, [x19, #48]         /* FAR_VALID = 0 */
    b .Lemit_trap_telemetry

.Lfar_valid_el1:
    mrs x7, far_el1
    str x7, [x19, #40]          /* Store FAR_EL1 */
    mov x8, #1
    str x8, [x19, #48]          /* FAR_VALID = 1 */
    b .Lemit_trap_telemetry

.Lread_el2:
    mrs x3, esr_el2
    str x3, [x19, #16]
    mrs x4, elr_el2
    str x4, [x19, #24]
    mrs x5, spsr_el2
    str x5, [x19, #32]

    lsr w6, w3, #26
    cmp w6, #0x20
    b.eq .Lfar_valid_el2
    cmp w6, #0x21
    b.eq .Lfar_valid_el2
    cmp w6, #0x22
    b.eq .Lfar_valid_el2
    cmp w6, #0x24
    b.eq .Lfar_valid_el2
    cmp w6, #0x25
    b.eq .Lfar_valid_el2

    str xzr, [x19, #40]
    str xzr, [x19, #48]
    b .Lemit_trap_telemetry

.Lfar_valid_el2:
    mrs x7, far_el2
    str x7, [x19, #40]
    mov x8, #1
    str x8, [x19, #48]

.Lemit_trap_telemetry:
    /* Set UART base */
    ldr x20, =0x09000000

    ldr x0, =msg_fault_banner
    bl print_string

    /* Print SLOT */
    ldr x0, =msg_slot_label
    bl print_string
    ldr x0, [x19, #0]
    bl print_hex64
    ldr x0, =msg_newline
    bl print_string

    /* Print ESR */
    ldr x0, =msg_esr_label
    bl print_string
    ldr x0, [x19, #16]
    bl print_hex64
    ldr x0, =msg_newline
    bl print_string

    /* Print FAR */
    ldr x0, =msg_far_label
    bl print_string
    ldr x0, [x19, #40]
    bl print_hex64
    ldr x0, =msg_newline
    bl print_string

    /* Print ELR */
    ldr x0, =msg_elr_label
    bl print_string
    ldr x0, [x19, #24]
    bl print_hex64
    ldr x0, =msg_newline
    bl print_string

.Lfault_quiescent_halt:
    wfe
    b .Lfault_quiescent_halt

    .section .rodata
    .balign 8
msg_fault_banner:
    .asciz "\n[FAULT] EXCEPTION TRAPPED\n"
msg_slot_label:
    .asciz "  SLOT: "
msg_esr_label:
    .asciz "  ESR: "
msg_far_label:
    .asciz "  FAR: "
msg_elr_label:
    .asciz "  ELR: "
msg_newline:
    .asciz "\n"
