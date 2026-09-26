/*
 * m3/effect_broker.s - Deterministic Effect Broker & Admission Pipeline
 * Milestone 3: PHYSICS_EFFECTS (Sovereign Machine)
 *
 * Single admission path: submit_effect(intent) -> EffectReceipt.
 * 10-stage fail-closed validation pipeline.
 * Physical execution ONLY occurs after all admission stages pass.
 * Every intent attempt produces an immutable cryptographic receipt.
 */

    .global submit_effect

    .extern sha256_be
    .extern lookup_capability
    .extern lookup_replay
    .extern record_replay
    .extern is_receipt_ledger_full
    .extern commit_receipt
    .extern get_receipt_count
    .extern allocate_frame_in_range
    .extern release_frame

    .section .text, "ax"
    .balign 4

    .equ UART_MMIO_BASE,        0x09000000
    .equ RECEIPT_ENTRIES_BASE,  0x4020A080
    .equ RECEIPT_RECORD_SIZE,   192

    .equ RES_UNIVERSAL_ROOT,    1
    .equ RES_MEMORY_FRAME,      2
    .equ RES_CONSOLE,           3
    .equ RES_MEASUREMENT,       4

    .equ OP_FRAME_GRANT,        1
    .equ OP_FRAME_RELEASE,      2
    .equ OP_CONSOLE_WRITE,      1
    .equ OP_MEASUREMENT_READ,   1

    .equ DEC_ADMITTED,                  1
    .equ DEC_REJECTED_STRUCTURAL,       2
    .equ DEC_REJECTED_UNKNOWN_CAP,      3
    .equ DEC_REJECTED_STALE_GENERATION, 4
    .equ DEC_REJECTED_REVOKED,          5
    .equ DEC_REJECTED_WRONG_PRINCIPAL,  6
    .equ DEC_REJECTED_OPERATION,        7
    .equ DEC_REJECTED_RESOURCE,         8
    .equ DEC_REJECTED_BOUNDS,           9
    .equ DEC_REJECTED_CONSTRAINT,       10
    .equ DEC_REJECTED_REPLAY_CONFLICT,  11
    .equ DEC_REJECTED_EXHAUSTED,        12
    .equ DEC_EXECUTION_FAILED,          13

    .equ REPLAY_MISS,                   0
    .equ REPLAY_HIT,                    1
    .equ REPLAY_CONFLICT,               2

/*
 * submit_effect(intent_ptr) -> x0: committed EffectReceipt pointer (or 0)
 *   x0: intent_ptr
 *
 * Stack Frame Layout (192 bytes):
 *   [sp+0]   x29, x30
 *   [sp+16]  x19, x20  (intent_ptr, cap_ptr)
 *   [sp+32]  x21, x22  (decision, rejection_reason)
 *   [sp+48]  x23, x24  (actual_effect, output)
 *   [sp+64]  ReceiptData (96 bytes: 0x00..0x5F)
 *     [sp+64]:  version(u16), length(u16), decision(u32)
 *     [sp+72]:  rejection_reason(u32), reserved0(u32)
 *     [sp+80]:  request_id(u64)
 *     [sp+88]:  intent_digest(32 bytes)
 *     [sp+120]: actual_effect(u64)
 *     [sp+128]: output(u64)
 *     [sp+136]: cap_slot(u32), cap_gen(u32)
 *     [sp+144]: machine_gen(u64)
 *     [sp+152]: measurement(u64)
 *   [sp+160] 32 bytes scratch
 */
    .equ FRAME_SIZE,    192
    .equ OFF_RECEIPT,   64

submit_effect:
    stp x29, x30, [sp, #-FRAME_SIZE]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]

    mov x19, x0                 /* x19 = intent_ptr */
    mov x20, #0                 /* x20 = cap_ptr (null initially) */
    mov w21, #DEC_ADMITTED      /* w21 = decision */
    mov w22, #0                 /* w22 = rejection_reason */
    mov x23, #0                 /* x23 = actual_effect */
    mov x24, #0                 /* x24 = output */

    /* Zero ReceiptData scratchpad (96 bytes at sp+64) */
    add x0, sp, #OFF_RECEIPT
    mov x1, #96
.Lzero_rcpt_scratch:
    cbz x1, .Lzero_rcpt_scratch_done
    stp xzr, xzr, [x0], #16
    sub x1, x1, #16
    b .Lzero_rcpt_scratch
.Lzero_rcpt_scratch_done:

    /* -------------------------------------------------------------
     * Stage 1: Structural Validation
     * ------------------------------------------------------------- */
    /* Check alignment of intent_ptr (must be 8-byte aligned) */
    tst x19, #7
    b.ne .Lfail_structural

    /* Check version == 1, length == 64, reserved0 == 0 */
    ldrh w1, [x19, #0]          /* version */
    cmp w1, #1
    b.ne .Lfail_structural

    ldrh w2, [x19, #2]          /* length */
    cmp w2, #64
    b.ne .Lfail_structural

    ldr w3, [x19, #4]           /* reserved0 */
    cbnz w3, .Lfail_structural
    b .Lstage2

.Lfail_structural:
    mov w21, #DEC_REJECTED_STRUCTURAL
    b .Lcommit_rejection

    /* -------------------------------------------------------------
     * Stage 2: Principal Validation
     * ------------------------------------------------------------- */
.Lstage2:
    ldr x7, [x19, #0x10]        /* principal_id */
    cbz x7, .Lfail_principal
    b .Lstage3

.Lfail_principal:
    mov w21, #DEC_REJECTED_WRONG_PRINCIPAL
    b .Lcommit_rejection

    /* -------------------------------------------------------------
     * Stage 3: Digest Intent & Check Replay Cache
     * ------------------------------------------------------------- */
.Lstage3:
    /* Compute SHA-256 of the 64-byte EffectIntent into ReceiptData.intent_digest (+88) */
    mov x0, x19                 /* data */
    mov x1, #64                 /* len */
    add x2, sp, #(OFF_RECEIPT + 24) /* out = sp + 88 */
    bl sha256_be
    cbnz w0, .Lfail_structural

    /* Check Replay Cache: lookup_replay(request_id, intent_digest) */
    ldr x0, [x19, #0x08]        /* request_id */
    add x1, sp, #(OFF_RECEIPT + 24) /* intent_digest */
    bl lookup_replay
    /* w0: REPLAY_MISS (0), REPLAY_HIT (1), REPLAY_CONFLICT (2) */
    cmp w0, #REPLAY_CONFLICT
    b.eq .Lfail_replay_conflict
    cmp w0, #REPLAY_HIT
    b.eq .Lhandle_replay_hit
    b .Lstage4

.Lfail_replay_conflict:
    mov w21, #DEC_REJECTED_REPLAY_CONFLICT
    b .Lcommit_rejection

.Lhandle_replay_hit:
    /* Idempotent replay: return existing committed receipt without re-executing */
    ldr x2, =RECEIPT_ENTRIES_BASE
    mov x3, #RECEIPT_RECORD_SIZE
    madd x0, x1, x3, x2         /* x0 = existing receipt ptr */
    b .Lexit

    /* -------------------------------------------------------------
     * Stage 4: Check Ledger Space (Fail-Closed Exhaustion)
     * ------------------------------------------------------------- */
.Lstage4:
    bl is_receipt_ledger_full
    cbnz w0, .Lfail_exhausted
    b .Lstage5

.Lfail_exhausted:
    mov w21, #DEC_REJECTED_EXHAUSTED
    /* On exhaustion, halt admission fail-closed without modifying memory */
    mov x0, #0
    b .Lexit

    /* -------------------------------------------------------------
     * Stage 5: Capability Lookup & Verification
     * ------------------------------------------------------------- */
.Lstage5:
    ldr w0, [x19, #0x18]        /* capability_slot */
    ldr w1, [x19, #0x1C]        /* capability_generation */
    ldr x2, [x19, #0x10]        /* principal_id */
    bl lookup_capability
    cbz x0, .Lfail_cap_lookup
    mov x20, x0                 /* x20 = validated CapabilityRecord ptr */
    b .Lstage6

.Lfail_cap_lookup:
    /* w1 carries exact rejection code from lookup_capability */
    mov w21, w1
    b .Lcommit_rejection

    /* -------------------------------------------------------------
     * Stage 6: Resource Type Check
     * ------------------------------------------------------------- */
.Lstage6:
    ldr w3, [x19, #0x20]        /* intent.resource_type */
    ldr w4, [x20, #0x10]        /* cap.resource_type */
    cmp w3, w4
    b.ne .Lfail_resource
    b .Lstage7

.Lfail_resource:
    mov w21, #DEC_REJECTED_RESOURCE
    b .Lcommit_rejection

    /* -------------------------------------------------------------
     * Stage 7: Operation Rights Check
     * ------------------------------------------------------------- */
.Lstage7:
    ldr w5, [x19, #0x24]        /* intent.operation */
    cbz w5, .Lfail_operation
    ldr w6, [x20, #0x14]        /* cap.allowed_ops */
    mvn w7, w6
    tst w5, w7
    b.ne .Lfail_operation
    b .Lstage8

.Lfail_operation:
    mov w21, #DEC_REJECTED_OPERATION
    b .Lcommit_rejection

    /* -------------------------------------------------------------
     * Stage 8: Spatial Bounds & Overflow Check
     * ------------------------------------------------------------- */
.Lstage8:
    ldr x8, [x19, #0x28]        /* target_base */
    ldr x9, [x19, #0x30]        /* target_size */
    ldr x10, [x20, #0x18]       /* cap.bound_base */
    ldr x11, [x20, #0x20]       /* cap.bound_size */

    /* target_base >= cap.bound_base */
    cmp x8, x10
    b.lo .Lfail_bounds

    /* target_base + target_size overflow */
    adds x12, x8, x9
    b.cs .Lfail_bounds

    /* cap.bound_base + cap.bound_size */
    adds x13, x10, x11
    b.cs .Lfail_bounds

    /* target_base + target_size <= cap.bound_base + cap.bound_size */
    cmp x12, x13
    b.hi .Lfail_bounds

    /* Concrete resource class rules */
    ldr w3, [x19, #0x20]        /* resource_type */
    cmp w3, #RES_MEMORY_FRAME
    b.eq .Lcheck_memory_bounds
    cmp w3, #RES_CONSOLE
    b.eq .Lcheck_console_bounds
    b .Lstage9_admitted

.Lcheck_memory_bounds:
    /* Frame target must be 4 KiB aligned and size must be 4 KiB */
    cmp x9, #4096
    b.ne .Lfail_bounds
    tst x8, #0xFFF
    b.ne .Lfail_bounds
    b .Lstage9_admitted

.Lcheck_console_bounds:
    /* Console MMIO target must be strictly UART_MMIO_BASE (0x09000000) */
    ldr x14, =UART_MMIO_BASE
    cmp x8, x14
    b.ne .Lfail_bounds
    b .Lstage9_admitted

.Lfail_bounds:
    mov w21, #DEC_REJECTED_BOUNDS
    b .Lcommit_rejection

    /* -------------------------------------------------------------
     * Stage 9: PHYSICAL EXECUTION (Admitted Operations Only)
     * ------------------------------------------------------------- */
.Lstage9_admitted:
    mov w21, #DEC_ADMITTED

    ldr w3, [x19, #0x20]        /* resource_type */
    ldr w5, [x19, #0x24]        /* operation */
    ldr x8, [x19, #0x28]        /* target_base */
    ldr x9, [x19, #0x30]        /* target_size */

    cmp w3, #RES_MEMORY_FRAME
    b.eq .Lexec_memory
    cmp w3, #RES_CONSOLE
    b.eq .Lexec_console
    cmp w3, #RES_MEASUREMENT
    b.eq .Lexec_measurement
    b .Lfail_exec

.Lexec_memory:
    cmp w5, #OP_FRAME_GRANT
    b.eq .Lexec_frame_grant
    cmp w5, #OP_FRAME_RELEASE
    b.eq .Lexec_frame_release
    b .Lfail_exec

.Lexec_frame_grant:
    mov x0, x8                  /* target_base */
    mov x1, x9                  /* target_size */
    bl allocate_frame_in_range
    cbz x0, .Lfail_exec
    mov x23, x0                 /* actual_effect = granted frame address */
    mov x24, #0                 /* output = 0 */
    b .Lcommit_admitted

.Lexec_frame_release:
    mov x0, x8                  /* target_base */
    bl release_frame
    cbz x0, .Lfail_exec
    mov x23, x0                 /* actual_effect = released frame address */
    mov x24, #0
    b .Lcommit_admitted

.Lexec_console:
    cmp w5, #OP_CONSOLE_WRITE
    b.ne .Lfail_exec
    /* Write single character from param0 (+0x38) */
    ldr x1, =UART_MMIO_BASE
    ldrb w2, [x19, #0x38]       /* param0 */
    strb w2, [x1]
    mov x23, x1                 /* actual_effect = UART base */
    mov x24, #1                 /* output = 1 byte written */
    b .Lcommit_admitted

.Lexec_measurement:
    cmp w5, #OP_MEASUREMENT_READ
    b.ne .Lfail_exec
    /* Return receipt count as measurement */
    bl get_receipt_count
    mov x23, #0
    mov x24, x0                 /* output = receipt_count */
    b .Lcommit_admitted

.Lfail_exec:
    mov w21, #DEC_EXECUTION_FAILED
    b .Lcommit_rejection

    /* -------------------------------------------------------------
     * Stage 10: Commit Receipt & Update Replay Cache
     * ------------------------------------------------------------- */
.Lcommit_admitted:
.Lcommit_rejection:
    /* Populate ReceiptData scratchpad at [sp + OFF_RECEIPT] */
    add x0, sp, #OFF_RECEIPT
    mov w1, #1
    strh w1, [x0, #0]           /* version = 1 */
    mov w1, #RECEIPT_RECORD_SIZE
    strh w1, [x0, #2]           /* length = 192 */
    str w21, [x0, #4]           /* decision */
    str w22, [x0, #8]           /* rejection_reason */

    ldr x1, [x19, #0x08]        /* request_id */
    str x1, [x0, #16]

    /* intent_digest already computed at +24 */
    str x23, [x0, #0x38]        /* actual_effect */
    str x24, [x0, #0x40]        /* output */

    /* Bind capability slot and generation */
    ldr w2, [x19, #0x18]
    str w2, [x0, #0x48]         /* capability_slot */
    ldr w3, [x19, #0x1C]
    str w3, [x0, #0x4C]         /* capability_generation */

    /* Commit receipt to ledger */
    add x0, sp, #OFF_RECEIPT
    bl commit_receipt
    cbz x0, .Lexit              /* Ledger was exhausted */

    mov x19, x0                 /* x19 = committed receipt ptr */

    /* Read assigned receipt_slot from committed receipt offset */
    ldr x1, =RECEIPT_ENTRIES_BASE
    sub x2, x19, x1
    mov x3, #RECEIPT_RECORD_SIZE
    udiv x2, x2, x3             /* x2 = receipt_slot index */

    /* Record in Replay Cache: record_replay(request_id, intent_digest, slot, decision, effect, output) */
    ldr x0, [x19, #0x10]        /* request_id */
    add x1, x19, #0x18          /* intent_digest */
    mov w2, w2                  /* receipt_slot */
    mov w3, w21                 /* decision */
    mov x4, x23                 /* actual_effect */
    mov x5, x24                 /* output */
    bl record_replay

    mov x0, x19                 /* Return committed receipt pointer */

.Lexit:
    ldp x23, x24, [sp, #48]
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #FRAME_SIZE
    ret
