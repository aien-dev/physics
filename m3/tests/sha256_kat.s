// sha256_kat.s -- bare-metal SHA-256 known-answer test for m3/sha256_be.s.
//
// Loaded raw at 0x40200000 (entered from tests/kat_stub.s in flash) on
// qemu-system-aarch64 -M virt -cpu cortex-a57, EL1, MMU off.
// Enables SCTLR_EL1.A, runs the vectors below, writes results to RES, prints
// "M3_KAT_DONE" (or "M3_KAT_FAULT" from any exception) on the PL011, then wfe.
// The host checks results by pmemsave state inspection, not by UART text.
//
// RES layout (0x40280000, 0x240 bytes, all little-endian):
//   +0x000 u64  magic "M3KATRES" (written last; absent => incomplete run)
//   +0x008 u64  SCTLR_EL1 read back after enabling A (bit 1)
//   +0x010 u64  CurrentEL
//   +0x020 u32  rc[9]   return code of each call below
//   +0x080 u8   digest[5][32]  vectors 0..4 (canonical big-endian bytes)
//   +0x140 u8   out buffers of refusal vectors 5..8, 0x30 B each, prefilled 0xA5
//   +0x200 u64  fault magic "M3FAULT!", ESR_EL1, ELR_EL1, FAR_EL1, vector offset
// Vectors:
//   0 ""                         1 "abc"
//   2 NIST 56-byte message       3 PAT[0..1000)
//   4 PAT[0..SHA256_MAX_LEN)     5 len = SHA256_MAX_LEN+1   -> rc 1
//   6 data = "abc"+1 (misalign)  -> rc 2    7 out misaligned -> rc 3
//   8 data+len wraps              -> rc 4
// PAT (0x40290000, 0x10000 bytes): byte i = i & 0xff.

        .equ RES,        0x40280000
        .equ PAT,        0x40290000
        .equ PAT_LEN,    0x10000
        .equ STACK_TOP,  0x40270000
        .equ UART_DR,    0x09000000
        // SHA256_MAX_LEN is the absolute symbol exported by m3/sha256_be.s;
        // kat.ld asserts it fits in PAT.

        .equ R_MAGIC,  0x000
        .equ R_SCTLR,  0x008
        .equ R_EL,     0x010
        .equ R_RC,     0x020
        .equ R_DIG,    0x080
        .equ R_REF,    0x140
        .equ R_REFSZ,  0x30
        .equ R_FAULT,  0x200
        .equ R_END,    0x240

        .section .text.boot, "ax", %progbits
        .global _start
_start:
        msr     daifset, #0xf
        ldr     x0, =STACK_TOP
        mov     sp, x0
        adr     x0, vectors
        msr     vbar_el1, x0
        isb
        mrs     x0, sctlr_el1
        orr     x0, x0, #(1 << 1)               // SCTLR_EL1.A: alignment checking
        msr     sctlr_el1, x0
        isb

        // Clear RES, then prefill the refusal out buffers with 0xA5.
        ldr     x20, =RES
        mov     x0, x20
        mov     x1, #(R_END / 8)
1:      str     xzr, [x0], #8
        subs    x1, x1, #1
        b.ne    1b
        add     x0, x20, #R_REF
        mov     x1, #((R_FAULT - R_REF) / 8)
        ldr     x2, =0xa5a5a5a5a5a5a5a5
2:      str     x2, [x0], #8
        subs    x1, x1, #1
        b.ne    2b

        mrs     x0, sctlr_el1
        str     x0, [x20, #R_SCTLR]
        mrs     x0, CurrentEL
        str     x0, [x20, #R_EL]

        // PAT[i] = i & 0xff
        ldr     x0, =PAT
        mov     x1, #0
3:      strb    w1, [x0, x1]
        add     x1, x1, #1
        cmp     x1, #PAT_LEN
        b.ne    3b

.ifdef KAT_FAULT_PROBE
        // Deliberate misaligned load: must trap to the vector table.
        ldr     x0, =PAT + 1
        ldr     w0, [x0]
.endif

        .macro  KAT idx, data, len, out
        ldr     x0, =\data
        ldr     x1, =\len
        ldr     x2, =\out
        bl      sha256_be
        str     w0, [x20, #(R_RC + 4 * \idx)]
        .endm

        KAT 0, msg_empty, 0,                  RES + R_DIG + 0 * 32
        KAT 1, msg_abc,   3,                  RES + R_DIG + 1 * 32
        KAT 2, msg_nist,  56,                 RES + R_DIG + 2 * 32
        KAT 3, PAT,       1000,               RES + R_DIG + 3 * 32
        KAT 4, PAT,       SHA256_MAX_LEN,   RES + R_DIG + 4 * 32
        KAT 5, PAT,       SHA256_MAX_LEN+1, RES + R_REF + 0 * R_REFSZ
        KAT 6, msg_abc+1, 3,                  RES + R_REF + 1 * R_REFSZ
        KAT 7, msg_abc,   3,                  RES + R_REF + 2 * R_REFSZ + 1
        KAT 8, 0xffffffffffffff00, 0x200,     RES + R_REF + 3 * R_REFSZ

        ldr     x0, =0x534552544b41334d         // "M3KATRES"
        str     x0, [x20, #R_MAGIC]
        dsb     sy
        adr     x0, str_done
        bl      puts
halt:   wfe
        b       halt

// puts(x0 = NUL-terminated string) -- byte writes to the PL011 data register.
puts:   ldr     x1, =UART_DR
1:      ldrb    w2, [x0], #1
        cbz     w2, 2f
        strb    w2, [x1]
        b       1b
2:      ret

fault_common:                                   // x9 = vector offset
        ldr     x20, =RES + R_FAULT
        ldr     x0, =0x21544c554146334d         // "M3FAULT!"
        str     x0, [x20, #0]
        mrs     x0, esr_el1
        str     x0, [x20, #8]
        mrs     x0, elr_el1
        str     x0, [x20, #16]
        mrs     x0, far_el1
        str     x0, [x20, #24]
        str     x9, [x20, #32]
        dsb     sy
        adr     x0, str_fault
        bl      puts
        b       halt

        .ltorg

        .balign 2048
vectors:
        .irp    n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        .balign 0x80
        mov     x9, #(\n * 0x80)
        b       fault_common
        .endr

        .section .rodata, "a", %progbits
        .balign 8
msg_empty:
        .balign 4
msg_abc:
        .ascii  "abc"
        .balign 4
msg_nist:
        .ascii  "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
        .balign 4
str_done:
        .asciz  "M3_KAT_DONE\n"
str_fault:
        .asciz  "M3_KAT_FAULT\n"
