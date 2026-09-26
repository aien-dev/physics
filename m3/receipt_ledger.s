/*
 * m3/receipt_ledger.s - Cryptographic Receipt Chain & Bounded Evidence Ledger
 * Milestone 3: PHYSICS_EFFECTS (Sovereign Machine)
 *
 * Append-only bounded receipt ledger. 32 receipts maximum.
 * Each receipt binds the previous receipt's SHA-256 digest.
 * Full ledger halts admission (fail-closed, never silently overwrite evidence).
 * Storage: PHYSICS_RECEIPT_LEDGER @ 0x4020A000 (8 KiB)
 */

    .global init_receipt_ledger
    .global get_receipt_count
    .global is_receipt_ledger_full
    .global commit_receipt

    .extern sha256_be

    .section .text, "ax"
    .balign 4

    .equ RECEIPT_LEDGER_BASE,       0x4020A000
    .equ RECEIPT_LEDGER_CAPACITY,   32
    .equ RECEIPT_HEADER_SIZE,       128
    .equ RECEIPT_RECORD_SIZE,       192
    .equ RECEIPT_ENTRIES_BASE,      0x4020A080
    .equ RECEIPT_MAGIC,             0x524350544C444752  /* 'RCPTLDGR' */

/*
 * init_receipt_ledger():
 * Zeros the 8 KiB ledger area, then writes the ledger header:
 *   +0x00: magic (0x524350544C444752)
 *   +0x08: capacity (u32 = 32), count (u32 = 0)
 *   +0x10: machine_generation (u64 = 1)
 *   +0x18: reserved (8 zero bytes)
 *   +0x20: latest_receipt_digest (32 zero bytes)
 */
init_receipt_ledger:
    stp x29, x30, [sp, #-16]!
    mov x29, sp

    /* Zero 8192 bytes */
    ldr x0, =RECEIPT_LEDGER_BASE
    ldr x1, =8192
.Lzero_rcpt_loop:
    cbz x1, .Lzero_rcpt_done
    stp xzr, xzr, [x0], #16
    sub x1, x1, #16
    b .Lzero_rcpt_loop

.Lzero_rcpt_done:
    ldr x0, =RECEIPT_LEDGER_BASE
    ldr x1, =RECEIPT_MAGIC
    str x1, [x0, #0]            /* +0x00: magic */

    mov w1, #RECEIPT_LEDGER_CAPACITY
    str w1, [x0, #8]            /* +0x08: capacity = 32 */
    str wzr, [x0, #12]          /* +0x0C: count = 0 */

    mov x1, #1
    str x1, [x0, #16]           /* +0x10: machine_generation = 1 */

    ldp x29, x30, [sp], #16
    ret

/*
 * get_receipt_count() -> w0
 */
get_receipt_count:
    ldr x1, =RECEIPT_LEDGER_BASE
    ldr w0, [x1, #12]
    ret

/*
 * is_receipt_ledger_full() -> w0 (1 if full, 0 if space available)
 */
is_receipt_ledger_full:
    ldr x1, =RECEIPT_LEDGER_BASE
    ldr w0, [x1, #12]           /* count */
    cmp w0, #RECEIPT_LEDGER_CAPACITY
    cset w0, ge
    ret

/*
 * commit_receipt(receipt_data_ptr)
 *   x0: Pointer to a populated ReceiptData block on stack/memory (first 96 bytes: 0x00..0x5F)
 * Returns:
 *   x0: Pointer to committed receipt in PHYSICS_RECEIPT_LEDGER, or 0 if ledger full.
 */
commit_receipt:
    stp x29, x30, [sp, #-48]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]

    mov x19, x0                 /* x19 = src receipt data */

    /* 1. Check ledger full */
    ldr x20, =RECEIPT_LEDGER_BASE
    ldr w21, [x20, #12]         /* w21 = current count */
    cmp w21, #RECEIPT_LEDGER_CAPACITY
    b.ge .Lledger_exhausted

    /* 2. Compute destination entry address: RECEIPT_ENTRIES_BASE + count * 192 */
    ldr x22, =RECEIPT_ENTRIES_BASE
    mov x1, #RECEIPT_RECORD_SIZE
    madd x22, x21, x1, x22      /* x22 = dest receipt ptr */

    /* 3. Copy first 96 bytes (0x00..0x5F) from src to dst */
    /* 12 x 8-byte words */
    ldp x2, x3, [x19, #0]
    stp x2, x3, [x22, #0]
    ldp x4, x5, [x19, #16]
    stp x4, x5, [x22, #16]
    ldp x6, x7, [x19, #32]
    stp x6, x7, [x22, #32]
    ldp x8, x9, [x19, #48]
    stp x8, x9, [x22, #48]
    ldp x10, x11, [x19, #64]
    stp x10, x11, [x22, #64]
    ldp x12, x13, [x19, #80]
    stp x12, x13, [x22, #80]

    /* Ensure version = 1 and length = 192 */
    mov w1, #1
    strh w1, [x22, #0]          /* version = 1 */
    mov w1, #RECEIPT_RECORD_SIZE
    strh w1, [x22, #2]          /* length = 192 */

    /* Bind current machine_generation at +0x50 */
    ldr x14, [x20, #16]         /* machine_generation */
    str x14, [x22, #0x50]

    /* 4. Copy previous_receipt_digest (32 bytes) from header.latest_receipt_digest (+0x20) to dst (+0x60) */
    ldp x2, x3, [x20, #0x20]
    ldp x4, x5, [x20, #0x30]
    stp x2, x3, [x22, #0x60]
    stp x4, x5, [x22, #0x70]

    /* 5. Compute SHA-256 seal over bytes 0x00..0x7F (128 bytes) of destination receipt */
    /* Store result into dst + 0x80 (receipt_digest) */
    mov x0, x22                 /* data ptr */
    mov x1, #128                /* len = 128 bytes */
    add x2, x22, #0x80          /* out = dst + 0x80 */
    bl sha256_be
    cbnz w0, .Lledger_exhausted

    /* Zero trailing padding +0xA0..+0xBF (32 bytes) */
    stp xzr, xzr, [x22, #0xA0]
    stp xzr, xzr, [x22, #0xB0]

    /* 6. Update header.latest_receipt_digest with new digest */
    ldp x2, x3, [x22, #0x80]
    ldp x4, x5, [x22, #0x90]
    stp x2, x3, [x20, #0x20]
    stp x4, x5, [x20, #0x30]

    /* 7. Increment count and machine_generation */
    add w21, w21, #1
    str w21, [x20, #12]         /* count */

    ldr x14, [x20, #16]
    add x14, x14, #1
    str x14, [x20, #16]         /* machine_generation */

    mov x0, x22                 /* Return committed receipt pointer */
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x29, x30, [sp], #48
    ret

.Lledger_exhausted:
    mov x0, #0
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x29, x30, [sp], #48
    ret
