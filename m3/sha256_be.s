// sha256_be.s -- canonical big-endian SHA-256 wrapper for PHYSICS M3.
//
// int sha256_be(const u8 *data /*x0*/, u64 len /*x1*/, u8 out[32] /*x2*/)
//
// Calls sha256_compute() from the repo-root sha256_clean.c (compiled, never
// copied) with its own 16-byte-aligned stack scratch[128] and a stack u32[8]
// state, then stores the digest to `out` as canonical big-endian bytes
// (REV of each native u32 word), i.e. the same bytes `sha256sum` prints.
//
// Fail closed: every precondition is checked before anything is written.
// On a nonzero return `out` is untouched and sha256_compute is never called.
//   0 SHA256_BE_OK
//   1 SHA256_BE_ERR_LEN        len > SHA256_MAX_LEN
//   2 SHA256_BE_ERR_DATA_ALIGN data not 4-byte aligned (sha256_transform does u32 loads)
//   3 SHA256_BE_ERR_OUT_ALIGN  out not 4-byte aligned (digest stored as u32 words)
//   4 SHA256_BE_ERR_WRAP       data + len wraps the address space
// General registers only (no FP/SIMD). AAPCS64; clobbers x0-x18 as allowed.

        .equ SHA256_MAX_LEN,            0x10000
        .equ SHA256_BE_OK,              0
        .equ SHA256_BE_ERR_LEN,         1
        .equ SHA256_BE_ERR_DATA_ALIGN,  2
        .equ SHA256_BE_ERR_OUT_ALIGN,   3
        .equ SHA256_BE_ERR_WRAP,        4

        // Stack frame (192 B, 16-aligned):
        //   [sp+0]   x29, x30
        //   [sp+16]  x19 (+8 pad)
        //   [sp+32]  u32 state[8]   (native words from sha256_compute)
        //   [sp+64]  u8 scratch[128]
        .equ FRAME,     192
        .equ OFF_STATE, 32
        .equ OFF_SCR,   64

        .global SHA256_MAX_LEN
        .section .text.sha256_be, "ax", %progbits
        .balign 4
        .global sha256_be
        .type   sha256_be, %function
sha256_be:
        mov     x9, #SHA256_MAX_LEN
        cmp     x1, x9
        b.hi    .Lerr_len
        tst     x0, #3
        b.ne    .Lerr_data
        tst     x2, #3
        b.ne    .Lerr_out
        adds    x9, x0, x1
        b.cs    .Lerr_wrap

        stp     x29, x30, [sp, #-FRAME]!
        mov     x29, sp
        str     x19, [sp, #16]
        mov     x19, x2                         // out
        add     x2, sp, #OFF_SCR                // scratch[128]
        add     x3, sp, #OFF_STATE              // u32 state[8]
        bl      sha256_compute

        add     x9, sp, #OFF_STATE
        mov     x10, #4                         // 4 pairs of words
1:      ldp     w11, w12, [x9], #8
        rev     w11, w11
        rev     w12, w12
        stp     w11, w12, [x19], #8
        subs    x10, x10, #1
        b.ne    1b

        mov     w0, #SHA256_BE_OK
        ldr     x19, [sp, #16]
        ldp     x29, x30, [sp], #FRAME
        ret

.Lerr_len:
        mov     w0, #SHA256_BE_ERR_LEN
        ret
.Lerr_data:
        mov     w0, #SHA256_BE_ERR_DATA_ALIGN
        ret
.Lerr_out:
        mov     w0, #SHA256_BE_ERR_OUT_ALIGN
        ret
.Lerr_wrap:
        mov     w0, #SHA256_BE_ERR_WRAP
        ret
        .size   sha256_be, . - sha256_be
