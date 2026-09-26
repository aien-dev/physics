/*
 * memory_alloc.s - Physical Frame Authority
 * Milestone 2: PHYSICS_BOOT (Patched Specification)
 *
 * Scope: Physical Frame Authority over [FREE_FRAME_BASE, DRAM_END).
 * Excludes all reserved Physics pages: [0x40000000, 0x40208000) permanently reserved.
 * Bitmap Anchor: PHYSICS_STATIC_DATA @ 0x40207000
 */

    .global init_frame_authority
    .global init_frame_allocator
    .global frame_allocator_init
    .global allocate_frame
    .global is_frame_reserved

    .extern print_string

    .section .text, "ax"
    .balign 4

.equ FREE_FRAME_BASE,    0x40208000
.equ FRAME_BITMAP_BASE,  0x40207000
.equ FRAME_SIZE,         4096
.equ FRAME_SHIFT,        12

/*
 * init_frame_authority(ram_base, ram_size):
 * Initializes frame bitmap at 0x40207000 for frames [0x40208000, ram_base + ram_size).
 * Parameters:
 *   x0: RAM Base (0x40000000)
 *   x1: RAM Size (0x08000000)
 * Returns:
 *   x0: Total managed frame count
 */
init_frame_authority:
init_frame_allocator:
frame_allocator_init:
    stp x29, x30, [sp, #-16]!
    mov x29, sp

    /* Calculate DRAM_END */
    add x2, x0, x1              /* x2 = DRAM_END */

    /* Store DRAM_END in state header @ 0x40207000 */
    ldr x3, =FRAME_BITMAP_BASE
    str x2, [x3, #0]            /* +0: DRAM_END */

    /* Managed span = DRAM_END - FREE_FRAME_BASE */
    ldr x4, =FREE_FRAME_BASE
    cmp x2, x4
    b.ls .Lno_frames

    sub x5, x2, x4              /* x5 = managed bytes */
    lsr x5, x5, #FRAME_SHIFT    /* x5 = total frame count */
    str x5, [x3, #8]            /* +8: total_frames */

    /* Zero the bitmap: (total_frames + 7) / 8 bytes starting at 0x40207010 */
    add x6, x5, #7
    lsr x6, x6, #3              /* x6 = bitmap bytes */
    add x7, x3, #16             /* x7 = bitmap start (+16) */

.Lzero_loop:
    cbz x6, .Lzero_done
    strb wzr, [x7], #1
    sub x6, x6, #1
    b .Lzero_loop

.Lzero_done:
    /* Emit telemetry: "PHYSICS: FRAME_AUTH_BOUND\n" */
    ldr x0, =msg_frame_auth_bound
    bl print_string

    mov x0, x5
    ldp x29, x30, [sp], #16
    ret

.Lno_frames:
    str xzr, [x3, #8]
    mov x0, #0
    ldp x29, x30, [sp], #16
    ret

/*
 * allocate_frame():
 * Allocates a single 4 KiB frame from [FREE_FRAME_BASE, DRAM_END).
 * Returns:
 *   x0: Physical frame base address, or 0 on exhaustion.
 */
allocate_frame:
    ldr x3, =FRAME_BITMAP_BASE
    ldr x2, [x3, #0]            /* DRAM_END */
    ldr x5, [x3, #8]            /* total_frames */
    cbz x5, .Lalloc_fail

    add x7, x3, #16             /* Bitmap pointer */
    mov x8, #0                  /* Frame index */

.Lfind_bit_loop:
    cmp x8, x5
    b.ge .Lalloc_fail           /* Exhausted */

    lsr x9, x8, #3              /* Byte index = x8 / 8 */
    and x10, x8, #7             /* Bit index = x8 % 8 */

    ldrb w11, [x7, x9]
    mov w12, #1
    lsl w12, w12, w10           /* Mask = 1 << bit_index */

    tst w11, w12
    b.eq .Lfound_free_frame

    add x8, x8, #1
    b .Lfind_bit_loop

.Lfound_free_frame:
    /* Mark bit as allocated */
    orr w11, w11, w12
    strb w11, [x7, x9]

    /* Compute physical address: FREE_FRAME_BASE + (frame_index << 12) */
    ldr x4, =FREE_FRAME_BASE
    lsl x13, x8, #FRAME_SHIFT
    add x0, x4, x13

    /* Assert address < DRAM_END */
    cmp x0, x2
    b.ge .Lalloc_fail

    ret

.Lalloc_fail:
    mov x0, #0
    ret

/*
 * is_frame_reserved(addr):
 * Parameters:
 *   x0: physical address
 * Returns:
 *   x0: 1 if reserved, 0 if within free-frame authority
 */
is_frame_reserved:
    ldr x1, =FREE_FRAME_BASE
    cmp x0, x1
    b.lo .Lreserved             /* Any address below 0x40208000 is permanently reserved */

    ldr x3, =FRAME_BITMAP_BASE
    ldr x2, [x3, #0]            /* DRAM_END */
    cmp x0, x2
    b.ge .Lreserved             /* At or above DRAM_END is reserved/unmapped */

    mov x0, #0
    ret

.Lreserved:
    mov x0, #1
    ret

    .section .rodata
    .balign 8
msg_frame_auth_bound:
    .asciz "PHYSICS: FRAME_AUTH_BOUND\n"
