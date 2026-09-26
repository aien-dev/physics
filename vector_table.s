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
    msr tpidr_el1, x0
    mov x0, #0
    b common_trap_entry
    .balign 128

/* Slot 1 (0x080): Current EL with SP0 - IRQ */
    msr tpidr_el1, x0
    mov x0, #1
    b common_trap_entry
    .balign 128

/* Slot 2 (0x100): Current EL with SP0 - FIQ */
    msr tpidr_el1, x0
    mov x0, #2
    b common_trap_entry
    .balign 128

/* Slot 3 (0x180): Current EL with SP0 - SError */
    msr tpidr_el1, x0
    mov x0, #3
    b common_trap_entry
    .balign 128

/* Slot 4 (0x200): Current EL with SPx - Sync */
    msr tpidr_el1, x0
    mov x0, #4
    b common_trap_entry
    .balign 128

/* Slot 5 (0x280): Current EL with SPx - IRQ */
    msr tpidr_el1, x0
    mov x0, #5
    b common_trap_entry
    .balign 128

/* Slot 6 (0x300): Current EL with SPx - FIQ */
    msr tpidr_el1, x0
    mov x0, #6
    b common_trap_entry
    .balign 128

/* Slot 7 (0x380): Current EL with SPx - SError */
    msr tpidr_el1, x0
    mov x0, #7
    b common_trap_entry
    .balign 128

/* Slot 8 (0x400): Lower EL AArch64 - Sync */
    msr tpidr_el1, x0
    mov x0, #8
    b common_trap_entry
    .balign 128

/* Slot 9 (0x480): Lower EL AArch64 - IRQ */
    msr tpidr_el1, x0
    mov x0, #9
    b common_trap_entry
    .balign 128

/* Slot 10 (0x500): Lower EL AArch64 - FIQ */
    msr tpidr_el1, x0
    mov x0, #10
    b common_trap_entry
    .balign 128

/* Slot 11 (0x580): Lower EL AArch64 - SError */
    msr tpidr_el1, x0
    mov x0, #11
    b common_trap_entry
    .balign 128

/* Slot 12 (0x600): Lower EL AArch32 - Sync */
    msr tpidr_el1, x0
    mov x0, #12
    b common_trap_entry
    .balign 128

/* Slot 13 (0x680): Lower EL AArch32 - IRQ */
    msr tpidr_el1, x0
    mov x0, #13
    b common_trap_entry
    .balign 128

/* Slot 14 (0x700): Lower EL AArch32 - FIQ */
    msr tpidr_el1, x0
    mov x0, #14
    b common_trap_entry
    .balign 128

/* Slot 15 (0x780): Lower EL AArch32 - SError */
    msr tpidr_el1, x0
    mov x0, #15
    b common_trap_entry
    .balign 128

/*
 * Common Trap Collector (outside the 2048-byte table)
 *
 * Entry state: x0 = vector slot, original x0 stashed in TPIDR_EL1, every
 * other GPR and SP exactly as interrupted. The collector saves the complete
 * interrupted GPR file before reusing any register and never touches the
 * stack until the frame is captured, so capture works even with a bad SP.
 *
 * PHYSICS_TRAP_FRAME @ 0x40205880 (PHYSICS_BOOT_STATE + 0x80), 320 bytes:
 *   +0x000..+0x0F0: x0..x30 (31 x u64)
 *   +0x0F8: SP (interrupted SP_EL1)
 *   +0x100: VECTOR_SLOT
 *   +0x108: CURRENT_EL (CurrentEL & 0xC)
 *   +0x110: ESR_EL1
 *   +0x118: ELR_EL1
 *   +0x120: SPSR_EL1
 *   +0x128: FAR_EL1 (0 when FAR_VALID == 0)
 *   +0x130: FAR_VALID (1 iff EC in {0x20,0x21,0x24,0x25} with ISS.FnV == 0,
 *           or EC in {0x22,0x34,0x35})
 *   +0x138: TRAP_FRAME_MAGIC 0x314D524650415254 ('TRAPFRM1'), written last
 *
 * The machine contract admits EL1 entry only and PHYSICS installs VBAR_EL1
 * only, so every exception taken through this table is taken to EL1.
 */
    .equ TRAP_FRAME_BASE, 0x40205880
    .equ TRAP_STACK_TOP,  0x40205800

    .section .text, "ax"
    .balign 4
common_trap_entry:
    msr tpidr_el0, x1                   /* stash original x1 */
    movz x1, #0x4020, lsl #16
    movk x1, #0x5880                    /* x1 = TRAP_FRAME_BASE */

    stp x2, x3, [x1, #0x010]
    stp x4, x5, [x1, #0x020]
    stp x6, x7, [x1, #0x030]
    stp x8, x9, [x1, #0x040]
    stp x10, x11, [x1, #0x050]
    stp x12, x13, [x1, #0x060]
    stp x14, x15, [x1, #0x070]
    stp x16, x17, [x1, #0x080]
    stp x18, x19, [x1, #0x090]
    stp x20, x21, [x1, #0x0A0]
    stp x22, x23, [x1, #0x0B0]
    stp x24, x25, [x1, #0x0C0]
    stp x26, x27, [x1, #0x0D0]
    stp x28, x29, [x1, #0x0E0]
    str x30, [x1, #0x0F0]

    mrs x2, tpidr_el1                   /* original x0 */
    mrs x3, tpidr_el0                   /* original x1 */
    stp x2, x3, [x1, #0x000]
    mov x2, sp
    str x2, [x1, #0x0F8]                /* interrupted SP */

    str x0, [x1, #0x100]                /* VECTOR_SLOT */
    mrs x2, CurrentEL
    and x2, x2, #0x0C
    str x2, [x1, #0x108]                /* CURRENT_EL */

    mrs x3, esr_el1
    str x3, [x1, #0x110]
    mrs x4, elr_el1
    str x4, [x1, #0x118]
    mrs x5, spsr_el1
    str x5, [x1, #0x120]

    /* FAR validity from EC = ESR[31:26] and FnV = ESR[10] */
    ubfx x6, x3, #26, #6
    cmp x6, #0x22                       /* PC alignment fault */
    b.eq .Lfar_valid
    cmp x6, #0x34                       /* Watchpoint, lower EL */
    b.eq .Lfar_valid
    cmp x6, #0x35                       /* Watchpoint, same EL */
    b.eq .Lfar_valid
    cmp x6, #0x20                       /* Instruction Abort, lower EL */
    b.eq .Lfar_check_fnv
    cmp x6, #0x21                       /* Instruction Abort, same EL */
    b.eq .Lfar_check_fnv
    cmp x6, #0x24                       /* Data Abort, lower EL */
    b.eq .Lfar_check_fnv
    cmp x6, #0x25                       /* Data Abort, same EL */
    b.eq .Lfar_check_fnv
    b .Lfar_invalid

.Lfar_check_fnv:
    tbnz x3, #10, .Lfar_invalid         /* FnV: FAR not valid */
.Lfar_valid:
    mrs x7, far_el1
    mov x8, #1
    b .Lfar_store
.Lfar_invalid:
    mov x7, #0
    mov x8, #0
.Lfar_store:
    stp x7, x8, [x1, #0x128]            /* FAR, FAR_VALID */

    ldr x9, =0x314D524650415254         /* 'TRAPFRM1' */
    str x9, [x1, #0x138]
    dsb sy

    /* Frame captured. Move to a known stack before any call. */
    mov x19, x1
    ldr x2, =TRAP_STACK_TOP
    mov sp, x2

    ldr x0, =msg_fault_banner
    bl print_string
    ldr x0, =msg_slot_label
    ldr x1, [x19, #0x100]
    bl print_field
    ldr x0, =msg_el_label
    ldr x1, [x19, #0x108]
    bl print_field
    ldr x0, =msg_esr_label
    ldr x1, [x19, #0x110]
    bl print_field
    ldr x0, =msg_elr_label
    ldr x1, [x19, #0x118]
    bl print_field
    ldr x0, =msg_spsr_label
    ldr x1, [x19, #0x120]
    bl print_field
    ldr x0, =msg_far_label
    ldr x1, [x19, #0x128]
    bl print_field
    ldr x0, =msg_far_valid_label
    ldr x1, [x19, #0x130]
    bl print_field
    ldr x0, =msg_trap_captured
    bl print_string

.Lfault_quiescent_halt:
    wfe
    b .Lfault_quiescent_halt

/* print_field(label=x0, value=x1): "<label><0x16hex>\n" */
print_field:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    str x20, [sp, #16]
    mov x20, x1
    bl print_string
    mov x0, x20
    bl print_hex64
    ldr x0, =msg_newline
    bl print_string
    ldr x20, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

    .section .rodata
    .balign 8
msg_fault_banner:
    .asciz "\n[FAULT] EXCEPTION TRAPPED\n"
msg_slot_label:
    .asciz "  SLOT: "
msg_el_label:
    .asciz "  CURRENT_EL: "
msg_esr_label:
    .asciz "  ESR: "
msg_elr_label:
    .asciz "  ELR: "
msg_spsr_label:
    .asciz "  SPSR: "
msg_far_label:
    .asciz "  FAR: "
msg_far_valid_label:
    .asciz "  FAR_VALID: "
msg_trap_captured:
    .asciz "PHYSICS: TRAP_FRAME_CAPTURED\n"
msg_newline:
    .asciz "\n"
