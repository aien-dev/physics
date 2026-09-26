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
.equ CONTRACT_DRAM_END,  0x48000000
.equ BITMAP_OFFSET,      32
.equ STATIC_DATA_SIZE,   4096

/*
 * PHYSICS_STATIC_DATA layout (0x40207000, 4 KiB):
 *   +0x00: DRAM_END (u64)
 *   +0x08: total_frames (u64)
 *   +0x10: next_free_cursor (u64, frame index)
 *   +0x18: reserved (u64, zero)
 *   +0x20: frame bitmap, ceil(total_frames / 8) bytes, bit set = allocated
 * For CONTRACT-QEMU-VIRT-AARCH64-M2: 32,248 frames, 4,031 bitmap bytes,
 * bitmap spans [0x40207020, 0x40207FFF), last byte 0x40207FFE < 0x40208000.
 */

/*
 * init_frame_authority(ram_base, ram_size):
 * Initializes frame bitmap at 0x40207000 for frames [0x40208000, ram_base + ram_size).
 * Parameters:
 *   x0: RAM Base (0x40000000)
 *   x1: RAM Size (0x08000000)
 * Returns:
 *   x0: Total managed frame count, or 0 if the requested range is refused.
 *
 * Refuses (returns 0, writes nothing) unless ram_base + ram_size is exactly
 * CONTRACT_DRAM_END without overflow and the bitmap fits in PHYSICS_STATIC_DATA.
 * Ingress already enforces the exact machine profile; this is the allocator's
 * own guard so no caller can widen frame authority.
 */
init_frame_authority:
init_frame_allocator:
frame_allocator_init:
    stp x29, x30, [sp, #-16]!
    mov x29, sp

    /* Calculate DRAM_END with overflow check */
    adds x2, x0, x1             /* x2 = DRAM_END */
    b.cs .Lno_frames
    ldr x4, =CONTRACT_DRAM_END
    cmp x2, x4
    b.ne .Lno_frames

    ldr x4, =FREE_FRAME_BASE
    cmp x2, x4
    b.ls .Lno_frames

    sub x5, x2, x4              /* x5 = managed bytes */
    lsr x5, x5, #FRAME_SHIFT    /* x5 = total frame count */

    /* Bitmap bytes = (total_frames + 7) / 8; must fit after the header */
    add x6, x5, #7
    lsr x6, x6, #3              /* x6 = bitmap bytes */
    add x7, x6, #BITMAP_OFFSET
    cmp x7, #STATIC_DATA_SIZE
    b.hi .Lno_frames

    ldr x3, =FRAME_BITMAP_BASE
    str x2, [x3, #0]            /* +0: DRAM_END */
    str x5, [x3, #8]            /* +8: total_frames */
    str xzr, [x3, #16]          /* +16: next_free_cursor = 0 */
    str xzr, [x3, #24]          /* +24: reserved */
    add x7, x3, #BITMAP_OFFSET  /* x7 = bitmap start */

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
    mov x0, #0
    ldp x29, x30, [sp], #16
    ret

/*
 * allocate_frame():
 * Allocates a single 4 KiB frame from [FREE_FRAME_BASE, DRAM_END).
 * Returns:
 *   x0: Physical frame base address, or 0 (FRAME_EXHAUSTED) on exhaustion.
 *       0 is never a valid frame: every frame is >= FREE_FRAME_BASE.
 */
allocate_frame:
    ldr x3, =FRAME_BITMAP_BASE
    ldr x2, [x3, #0]            /* DRAM_END */
    ldr x5, [x3, #8]            /* total_frames */
    cbz x5, .Lalloc_fail

    ldr x8, [x3, #16]           /* Frame index cursor */
    add x7, x3, #32             /* Bitmap pointer */

.Lfind_bit_loop:
    cmp x8, x5
    b.hs .Lalloc_fail           /* Exhausted */

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
    /* Compute physical address: FREE_FRAME_BASE + (frame_index << 12) */
    ldr x4, =FREE_FRAME_BASE
    lsl x13, x8, #FRAME_SHIFT
    add x0, x4, x13

    /* Assert address < DRAM_END before granting anything */
    cmp x0, x2
    b.hs .Lalloc_fail

    /* Mark bit as allocated and advance next_free_cursor */
    orr w11, w11, w12
    strb w11, [x7, x9]
    add x14, x8, #1
    str x14, [x3, #16]
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
    b.hs .Lreserved             /* At or above DRAM_END is reserved/unmapped */

    mov x0, #0
    ret

.Lreserved:
    mov x0, #1
    ret

    .section .rodata
    .balign 8
msg_frame_auth_bound:
    .asciz "PHYSICS: FRAME_AUTH_BOUND\n"
