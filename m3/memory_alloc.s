/*
 * m3/memory_alloc.s - Physical Frame Authority for Milestone 3
 * Target Platform: QEMU virt AArch64 / Sovereign Machine
 *
 * Scope: Physical Frame Authority over [FREE_FRAME_BASE, DRAM_END).
 * Excludes all reserved Physics pages: [0x40000000, 0x40210000) permanently reserved.
 * Bitmap Anchor: PHYSICS_STATIC_DATA @ 0x4020D000 (12 KiB)
 */

    .global init_frame_authority
    .global allocate_frame
    .global allocate_frame_in_range
    .global release_frame
    .global is_frame_reserved

    .section .text, "ax"
    .balign 4

    .equ FREE_FRAME_BASE,    0x40210000
    .equ FRAME_BITMAP_BASE,  0x4020D000
    .equ FRAME_SIZE,         4096
    .equ FRAME_SHIFT,        12
    .equ CONTRACT_DRAM_END,  0x48000000
    .equ BITMAP_OFFSET,      32
    .equ STATIC_DATA_SIZE,   12288

/*
 * init_frame_authority(ram_base, ram_size)
 *   x0: ram_base, x1: ram_size
 * Returns total managed frames in x0, or 0 on error.
 */
init_frame_authority:
    stp x29, x30, [sp, #-16]!
    mov x29, sp

    adds x2, x0, x1             /* x2 = DRAM_END */
    b.cs .Lno_frames
    ldr x4, =CONTRACT_DRAM_END
    cmp x2, x4
    b.ne .Lno_frames

    ldr x4, =FREE_FRAME_BASE
    cmp x2, x4
    b.ls .Lno_frames

    sub x5, x2, x4              /* x5 = managed bytes */
    lsr x5, x5, #FRAME_SHIFT    /* x5 = total frames */

    add x6, x5, #7
    lsr x6, x6, #3              /* x6 = bitmap bytes */
    add x7, x6, #BITMAP_OFFSET
    ldr x8, =STATIC_DATA_SIZE
    cmp x7, x8
    b.hi .Lno_frames

    ldr x3, =FRAME_BITMAP_BASE
    str x2, [x3, #0]            /* +0: DRAM_END */
    str x5, [x3, #8]            /* +8: total_frames */
    str xzr, [x3, #16]          /* +16: next_free_cursor = 0 */
    str xzr, [x3, #24]          /* +24: reserved */
    add x7, x3, #BITMAP_OFFSET  /* x7 = bitmap start */

.Lzero_bitmap_loop:
    cbz x6, .Lzero_bitmap_done
    strb wzr, [x7], #1
    sub x6, x6, #1
    b .Lzero_bitmap_loop

.Lzero_bitmap_done:
    mov x0, x5
    ldp x29, x30, [sp], #16
    ret

.Lno_frames:
    mov x0, #0
    ldp x29, x30, [sp], #16
    ret

/*
 * is_frame_reserved(frame_addr) -> x0: 1 if reserved, 0 if free DRAM
 *   x0: frame_addr
 */
is_frame_reserved:
    ldr x1, =FREE_FRAME_BASE
    cmp x0, x1
    b.lo .Lframe_is_res

    ldr x2, =CONTRACT_DRAM_END
    cmp x0, x2
    b.hs .Lframe_is_res

    mov x0, #0
    ret
.Lframe_is_res:
    mov x0, #1
    ret

/*
 * allocate_frame() -> x0: physical frame address, or 0 if exhausted
 */
allocate_frame:
    ldr x1, =FRAME_BITMAP_BASE
    ldr x2, [x1, #8]            /* total_frames */
    ldr x3, [x1, #16]           /* cursor */

    cmp x3, x2
    b.ge .Lalloc_exhausted

    /* Mark bit in bitmap */
    lsr x4, x3, #3              /* byte offset */
    and x5, x3, #7              /* bit index */
    add x6, x1, #BITMAP_OFFSET
    add x6, x6, x4
    ldrb w7, [x6]
    mov w8, #1
    lsl w8, w8, w5
    orr w7, w7, w8
    strb w7, [x6]

    /* Compute frame address: FREE_FRAME_BASE + (cursor << 12) */
    ldr x9, =FREE_FRAME_BASE
    lsl x10, x3, #FRAME_SHIFT
    add x0, x9, x10

    /* Advance cursor */
    add x3, x3, #1
    str x3, [x1, #16]
    ret

.Lalloc_exhausted:
    mov x0, #0
    ret

/*
 * allocate_frame_in_range(target_base, target_size)
 *   x0: target_base, x1: target_size
 * Allocates 4 KiB frame at target_base.
 * Returns target_base on success, or 0 on failure.
 */
allocate_frame_in_range:
    /* Check 4 KiB alignment */
    tst x0, #0xFFF
    b.ne .Lalloc_range_fail

    /* Check bounds: target_base >= FREE_FRAME_BASE, target_base < CONTRACT_DRAM_END */
    ldr x2, =FREE_FRAME_BASE
    cmp x0, x2
    b.lo .Lalloc_range_fail

    ldr x3, =CONTRACT_DRAM_END
    cmp x0, x3
    b.hs .Lalloc_range_fail

    /* Compute frame index */
    sub x4, x0, x2
    lsr x4, x4, #FRAME_SHIFT    /* x4 = frame index */

    ldr x5, =FRAME_BITMAP_BASE
    ldr x6, [x5, #8]            /* total_frames */
    cmp x4, x6
    b.ge .Lalloc_range_fail

    /* Check if already allocated */
    lsr x7, x4, #3              /* byte offset */
    and x8, x4, #7              /* bit index */
    add x9, x5, #BITMAP_OFFSET
    add x9, x9, x7
    ldrb w10, [x9]
    mov w11, #1
    lsl w11, w11, w8
    tst w10, w11
    b.ne .Lalloc_range_fail      /* Already allocated */

    /* Mark allocated */
    orr w10, w10, w11
    strb w10, [x9]

    /* Success: return target_base (in x0) */
    ret

.Lalloc_range_fail:
    mov x0, #0
    ret

/*
 * release_frame(frame_addr) -> x0: frame_addr on success, 0 on failure
 *   x0: frame_addr
 */
release_frame:
    ldr x2, =FREE_FRAME_BASE
    cmp x0, x2
    b.lo .Lrel_fail
    ldr x3, =CONTRACT_DRAM_END
    cmp x0, x3
    b.hs .Lrel_fail
    tst x0, #0xFFF
    b.ne .Lrel_fail

    sub x4, x0, x2
    lsr x4, x4, #FRAME_SHIFT
    ldr x5, =FRAME_BITMAP_BASE
    ldr x6, [x5, #8]
    cmp x4, x6
    b.ge .Lrel_fail

    lsr x7, x4, #3
    and x8, x4, #7
    add x9, x5, #BITMAP_OFFSET
    add x9, x9, x7
    ldrb w10, [x9]
    mov w11, #1
    lsl w11, w11, w8
    tst w10, w11
    b.eq .Lrel_fail             /* Was not allocated */

    bic w10, w10, w11
    strb w10, [x9]
    ret

.Lrel_fail:
    mov x0, #0
    ret
