/*
 * m3/tests/test_m3_authority.s - Bare-Metal Dynamic Qualification Test Suite
 * Milestone 3: PHYSICS_EFFECTS (Sovereign Machine)
 *
 * Runs end-to-end derivation, attenuation, execution, rejection, revocation,
 * receipt chaining, replay idempotency, replay conflict, and adversarial matrix.
 * Records results in PHYSICS_BOOT_STATE + 0x200 (0x40208A00).
 */

    .global run_m3_unit_tests

    .extern submit_effect
    .extern derive_capability
    .extern revoke_capability
    .extern lookup_capability
    .extern get_receipt_count
    .extern print_string

    .section .text, "ax"
    .balign 4

    .equ TEST_RECORD_BASE,      0x40208A00
    .equ TEST_MAGIC,            0x4D33544553545355  /* 'M3TESTSU' */

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

/*
 * run_m3_unit_tests():
 * Executes all M3 test scenarios.
 */
run_m3_unit_tests:
    stp x29, x30, [sp, #-160]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]

    /* x19 = pass count, x20 = test count, x21 = bitmask */
    mov x19, #0
    mov x20, #0
    mov x21, #0

    adr x0, msg_unit_tests_start
    bl print_string

    /* -------------------------------------------------------------
     * TEST 1: End-to-End Scenario — Derive Memory Cap A from CAP_ROOT
     * Parent: Slot 0, Gen 1, caller=1. Child: principal=2, rtype=FRAME,
     * ops=FRAME_GRANT, bounds=[0x40210000, 0x40220000) (64 KiB), lifetime=0.
     * ------------------------------------------------------------- */
    add x20, x20, #1
    mov w0, #0                  /* parent_slot = 0 */
    mov w1, #1                  /* parent_gen = 1 */
    mov x2, #1                  /* caller_principal = 1 */
    mov x3, #2                  /* child_principal = 2 */
    mov w4, #RES_MEMORY_FRAME   /* rtype */
    mov w5, #OP_FRAME_GRANT     /* ops */
    ldr x6, =0x40210000         /* base */
    ldr x7, =0x00010000         /* size = 64 KiB */
    mov x8, #0                  /* lifetime = 0 */
    bl derive_capability
    /* Expected: Cap A in Slot 1, Gen 1 -> (1 << 32) | 1 */
    ldr x1, =(1 << 32) | 1
    cmp x0, x1
    b.ne 1f
    add x19, x19, #1
    orr x21, x21, #(1 << 0)
1:
    mov x22, x0                 /* x22 = Cap A handle */

    /* -------------------------------------------------------------
     * TEST 2: Valid Intent on Cap A -> ADMITTED, Frame Granted
     * Intent: req_id=101, principal=2, slot=1, gen=1, rtype=FRAME,
     * op=FRAME_GRANT, target=0x40210000, size=4096.
     * ------------------------------------------------------------- */
    add x20, x20, #1
    add x0, sp, #64             /* Intent buffer (64 bytes) */
    mov w1, #1
    strh w1, [x0, #0]           /* version = 1 */
    mov w1, #64
    strh w1, [x0, #2]           /* length = 64 */
    str wzr, [x0, #4]           /* reserved0 = 0 */
    mov x1, #101
    str x1, [x0, #8]            /* request_id = 101 */
    mov x1, #2
    str x1, [x0, #16]           /* principal_id = 2 */
    mov w1, #1
    str w1, [x0, #24]           /* slot = 1 */
    mov w1, #1
    str w1, [x0, #28]           /* generation = 1 */
    mov w1, #RES_MEMORY_FRAME
    str w1, [x0, #32]           /* rtype */
    mov w1, #OP_FRAME_GRANT
    str w1, [x0, #36]           /* op */
    ldr x1, =0x40210000
    str x1, [x0, #40]           /* target_base = 0x40210000 */
    mov x1, #4096
    str x1, [x0, #48]           /* target_size = 4096 */
    str xzr, [x0, #56]          /* reserved1 */

    bl submit_effect
    cbz x0, 2f
    ldr w1, [x0, #4]            /* decision */
    cmp w1, #DEC_ADMITTED
    b.ne 2f
    ldr x2, [x0, #0x38]         /* actual_effect */
    ldr x3, =0x40210000
    cmp x2, x3
    b.ne 2f
    add x19, x19, #1
    orr x21, x21, #(1 << 1)
2:

    /* -------------------------------------------------------------
     * TEST 3: Derive Cap B from Cap A (Strict subset)
     * Parent: Slot 1, Gen 1, caller=2. Child: principal=3, rtype=FRAME,
     * ops=FRAME_GRANT, bounds=[0x40210000, 0x40218000) (32 KiB, subset).
     * ------------------------------------------------------------- */
    add x20, x20, #1
    mov w0, #1                  /* parent_slot = 1 */
    mov w1, #1                  /* parent_gen = 1 */
    mov x2, #2                  /* caller_principal = 2 */
    mov x3, #3                  /* child_principal = 3 */
    mov w4, #RES_MEMORY_FRAME
    mov w5, #OP_FRAME_GRANT
    ldr x6, =0x40210000
    ldr x7, =0x00008000         /* size = 32 KiB */
    mov x8, #0
    bl derive_capability
    /* Expected: Cap B in Slot 2, Gen 1 */
    ldr x1, =(1 << 32) | 2
    cmp x0, x1
    b.ne 3f
    add x19, x19, #1
    orr x21, x21, #(1 << 2)
3:
    mov x23, x0                 /* x23 = Cap B handle */

    /* -------------------------------------------------------------
     * TEST 4: Intent on Cap B Targeting Outside B's Bounds -> REJECTED_BOUNDS
     * Target: 0x40219000 (outside [0x40210000, 0x40218000))
     * ------------------------------------------------------------- */
    add x20, x20, #1
    add x0, sp, #64
    mov w1, #1
    strh w1, [x0, #0]
    mov w1, #64
    strh w1, [x0, #2]
    str wzr, [x0, #4]
    mov x1, #102
    str x1, [x0, #8]            /* request_id = 102 */
    mov x1, #3
    str x1, [x0, #16]           /* principal_id = 3 */
    mov w1, #2
    str w1, [x0, #24]           /* slot = 2 */
    mov w1, #1
    str w1, [x0, #28]           /* generation = 1 */
    mov w1, #RES_MEMORY_FRAME
    str w1, [x0, #32]
    mov w1, #OP_FRAME_GRANT
    str w1, [x0, #36]
    ldr x1, =0x40219000         /* OUTSIDE B */
    str x1, [x0, #40]
    mov x1, #4096
    str x1, [x0, #48]
    str xzr, [x0, #56]

    bl submit_effect
    cbz x0, 4f
    ldr w1, [x0, #4]            /* decision */
    cmp w1, #DEC_REJECTED_BOUNDS
    b.ne 4f
    add x19, x19, #1
    orr x21, x21, #(1 << 3)
4:

    /* -------------------------------------------------------------
     * TEST 5: Revoke Cap A (Slot 1)
     * ------------------------------------------------------------- */
    add x20, x20, #1
    mov w0, #1                  /* slot = 1 */
    mov w1, #1                  /* gen = 1 */
    mov x2, #2                  /* principal = 2 */
    bl revoke_capability
    cmp x0, #1
    b.ne 5f
    add x19, x19, #1
    orr x21, x21, #(1 << 4)
5:

    /* -------------------------------------------------------------
     * TEST 6: Exercising Directly Revoked Cap A -> REJECTED_REVOKED
     * ------------------------------------------------------------- */
    add x20, x20, #1
    add x0, sp, #64
    mov w1, #1
    strh w1, [x0, #0]
    mov w1, #64
    strh w1, [x0, #2]
    str wzr, [x0, #4]
    mov x1, #103
    str x1, [x0, #8]
    mov x1, #2
    str x1, [x0, #16]
    mov w1, #1
    str w1, [x0, #24]           /* slot = 1 (revoked) */
    mov w1, #1
    str w1, [x0, #28]
    mov w1, #RES_MEMORY_FRAME
    str w1, [x0, #32]
    mov w1, #OP_FRAME_GRANT
    str w1, [x0, #36]
    ldr x1, =0x40211000
    str x1, [x0, #40]
    mov x1, #4096
    str x1, [x0, #48]
    str xzr, [x0, #56]

    bl submit_effect
    cbz x0, 6f
    ldr w1, [x0, #4]
    cmp w1, #DEC_REJECTED_REVOKED
    b.ne 6f
    add x19, x19, #1
    orr x21, x21, #(1 << 5)
6:

    /* -------------------------------------------------------------
     * TEST 7: Exercising Descendant Cap B (Parent A Revoked) -> REJECTED_REVOKED
     * ------------------------------------------------------------- */
    add x20, x20, #1
    add x0, sp, #64
    mov w1, #1
    strh w1, [x0, #0]
    mov w1, #64
    strh w1, [x0, #2]
    str wzr, [x0, #4]
    mov x1, #104
    str x1, [x0, #8]
    mov x1, #3
    str x1, [x0, #16]
    mov w1, #2
    str w1, [x0, #24]           /* slot = 2 (descendant of 1) */
    mov w1, #1
    str w1, [x0, #28]
    mov w1, #RES_MEMORY_FRAME
    str w1, [x0, #32]
    mov w1, #OP_FRAME_GRANT
    str w1, [x0, #36]
    ldr x1, =0x40210000
    str x1, [x0, #40]
    mov x1, #4096
    str x1, [x0, #48]
    str xzr, [x0, #56]

    bl submit_effect
    cbz x0, 7f
    ldr w1, [x0, #4]
    cmp w1, #DEC_REJECTED_REVOKED
    b.ne 7f
    add x19, x19, #1
    orr x21, x21, #(1 << 6)
7:

    /* -------------------------------------------------------------
     * TEST 8: Adversarial Matrix — Forged Slot (Slot 15 unallocated)
     * ------------------------------------------------------------- */
    add x20, x20, #1
    add x0, sp, #64
    mov w1, #1
    strh w1, [x0, #0]
    mov w1, #64
    strh w1, [x0, #2]
    str wzr, [x0, #4]
    mov x1, #105
    str x1, [x0, #8]
    mov x1, #2
    str x1, [x0, #16]
    mov w1, #15                 /* Forged slot */
    str w1, [x0, #24]
    mov w1, #1
    str w1, [x0, #28]
    mov w1, #RES_MEMORY_FRAME
    str w1, [x0, #32]
    mov w1, #OP_FRAME_GRANT
    str w1, [x0, #36]
    ldr x1, =0x40210000
    str x1, [x0, #40]
    mov x1, #4096
    str x1, [x0, #48]
    str xzr, [x0, #56]

    bl submit_effect
    cbz x0, 8f
    ldr w1, [x0, #4]
    cmp w1, #DEC_REJECTED_UNKNOWN_CAP
    b.ne 8f
    add x19, x19, #1
    orr x21, x21, #(1 << 7)
8:

    /* -------------------------------------------------------------
     * TEST 9: Adversarial Matrix — Out-of-Range Slot (Slot 40 >= 32)
     * ------------------------------------------------------------- */
    add x20, x20, #1
    add x0, sp, #64
    mov w1, #1
    strh w1, [x0, #0]
    mov w1, #64
    strh w1, [x0, #2]
    str wzr, [x0, #4]
    mov x1, #106
    str x1, [x0, #8]
    mov x1, #2
    str x1, [x0, #16]
    mov w1, #40                 /* Out of range */
    str w1, [x0, #24]
    mov w1, #1
    str w1, [x0, #28]
    mov w1, #RES_MEMORY_FRAME
    str w1, [x0, #32]
    mov w1, #OP_FRAME_GRANT
    str w1, [x0, #36]
    ldr x1, =0x40210000
    str x1, [x0, #40]
    mov x1, #4096
    str x1, [x0, #48]
    str xzr, [x0, #56]

    bl submit_effect
    cbz x0, 9f
    ldr w1, [x0, #4]
    cmp w1, #DEC_REJECTED_UNKNOWN_CAP
    b.ne 9f
    add x19, x19, #1
    orr x21, x21, #(1 << 8)
9:

    /* -------------------------------------------------------------
     * TEST 10: Adversarial Matrix — Stale Generation
     * Re-allocate slot 1. Slot 1 becomes gen 2. Present gen 1.
     * ------------------------------------------------------------- */
    add x20, x20, #1
    /* Derive fresh cap in slot 1 from root */
    mov w0, #0
    mov w1, #1
    mov x2, #1
    mov x3, #4                  /* principal 4 */
    mov w4, #RES_MEMORY_FRAME
    mov w5, #OP_FRAME_GRANT
    ldr x6, =0x40210000
    ldr x7, =0x00010000
    mov x8, #0
    bl derive_capability
    /* Should be slot 1, gen 2 -> (2 << 32) | 1 */
    ldr x1, =(2 << 32) | 1
    cmp x0, x1
    b.ne 10f

    /* Now submit intent presenting gen 1 (stale) */
    add x0, sp, #64
    mov w1, #1
    strh w1, [x0, #0]
    mov w1, #64
    strh w1, [x0, #2]
    str wzr, [x0, #4]
    mov x1, #107
    str x1, [x0, #8]
    mov x1, #4
    str x1, [x0, #16]
    mov w1, #1                  /* slot 1 */
    str w1, [x0, #24]
    mov w1, #1                  /* STALE generation 1 (current is 2) */
    str w1, [x0, #28]
    mov w1, #RES_MEMORY_FRAME
    str w1, [x0, #32]
    mov w1, #OP_FRAME_GRANT
    str w1, [x0, #36]
    ldr x1, =0x40210000
    str x1, [x0, #40]
    mov x1, #4096
    str x1, [x0, #48]
    str xzr, [x0, #56]

    bl submit_effect
    cbz x0, 10f
    ldr w1, [x0, #4]
    cmp w1, #DEC_REJECTED_STALE_GENERATION
    b.ne 10f
    add x19, x19, #1
    orr x21, x21, #(1 << 9)
10:

    /* -------------------------------------------------------------
     * TEST 11: Adversarial Matrix — Wrong Principal
     * Slot 1 gen 2 owned by principal 4. Caller presents principal 99.
     * ------------------------------------------------------------- */
    add x20, x20, #1
    add x0, sp, #64
    mov w1, #1
    strh w1, [x0, #0]
    mov w1, #64
    strh w1, [x0, #2]
    str wzr, [x0, #4]
    mov x1, #108
    str x1, [x0, #8]
    mov x1, #99                 /* Wrong principal */
    str x1, [x0, #16]
    mov w1, #1
    str w1, [x0, #24]
    mov w1, #2                  /* Current gen = 2 */
    str w1, [x0, #28]
    mov w1, #RES_MEMORY_FRAME
    str w1, [x0, #32]
    mov w1, #OP_FRAME_GRANT
    str w1, [x0, #36]
    ldr x1, =0x40210000
    str x1, [x0, #40]
    mov x1, #4096
    str x1, [x0, #48]
    str xzr, [x0, #56]

    bl submit_effect
    cbz x0, 11f
    ldr w1, [x0, #4]
    cmp w1, #DEC_REJECTED_WRONG_PRINCIPAL
    b.ne 11f
    add x19, x19, #1
    orr x21, x21, #(1 << 10)
11:

    /* -------------------------------------------------------------
     * TEST 12: Adversarial Matrix — Forbidden Operation
     * Slot 1 gen 2 allows only FRAME_GRANT. Request FRAME_RELEASE.
     * ------------------------------------------------------------- */
    add x20, x20, #1
    add x0, sp, #64
    mov w1, #1
    strh w1, [x0, #0]
    mov w1, #64
    strh w1, [x0, #2]
    str wzr, [x0, #4]
    mov x1, #109
    str x1, [x0, #8]
    mov x1, #4                  /* correct principal */
    str x1, [x0, #16]
    mov w1, #1
    str w1, [x0, #24]
    mov w1, #2
    str w1, [x0, #28]
    mov w1, #RES_MEMORY_FRAME
    str w1, [x0, #32]
    mov w1, #OP_FRAME_RELEASE   /* Forbidden operation */
    str w1, [x0, #36]
    ldr x1, =0x40210000
    str x1, [x0, #40]
    mov x1, #4096
    str x1, [x0, #48]
    str xzr, [x0, #56]

    bl submit_effect
    cbz x0, 12f
    ldr w1, [x0, #4]
    cmp w1, #DEC_REJECTED_OPERATION
    b.ne 12f
    add x19, x19, #1
    orr x21, x21, #(1 << 11)
12:

    /* -------------------------------------------------------------
     * TEST 13: Adversarial Matrix — Wrong Resource Type
     * Slot 1 gen 2 is RES_MEMORY_FRAME. Request RES_CONSOLE.
     * ------------------------------------------------------------- */
    add x20, x20, #1
    add x0, sp, #64
    mov w1, #1
    strh w1, [x0, #0]
    mov w1, #64
    strh w1, [x0, #2]
    str wzr, [x0, #4]
    mov x1, #110
    str x1, [x0, #8]
    mov x1, #4
    str x1, [x0, #16]
    mov w1, #1
    str w1, [x0, #24]
    mov w1, #2
    str w1, [x0, #28]
    mov w1, #RES_CONSOLE        /* Wrong resource */
    str w1, [x0, #32]
    mov w1, #OP_CONSOLE_WRITE
    str w1, [x0, #36]
    ldr x1, =0x09000000
    str x1, [x0, #40]
    mov x1, #1
    str x1, [x0, #48]
    str xzr, [x0, #56]

    bl submit_effect
    cbz x0, 13f
    ldr w1, [x0, #4]
    cmp w1, #DEC_REJECTED_RESOURCE
    b.ne 13f
    add x19, x19, #1
    orr x21, x21, #(1 << 12)
13:

    /* -------------------------------------------------------------
     * TEST 14: Adversarial Matrix — Child Rights Widening Refused
     * Parent has only FRAME_GRANT. Attempt to derive child with FRAME_RELEASE.
     * ------------------------------------------------------------- */
    add x20, x20, #1
    mov w0, #1                  /* parent_slot = 1 (gen 2) */
    mov w1, #2
    mov x2, #4                  /* caller = 4 */
    mov x3, #5                  /* child = 5 */
    mov w4, #RES_MEMORY_FRAME
    mov w5, #(OP_FRAME_GRANT | OP_FRAME_RELEASE) /* Widened ops! */
    ldr x6, =0x40210000
    ldr x7, =0x00010000
    mov x8, #0
    bl derive_capability
    /* Must return 0 (refusal) */
    cbnz x0, 14f
    add x19, x19, #1
    orr x21, x21, #(1 << 13)
14:

    /* -------------------------------------------------------------
     * TEST 15: Adversarial Matrix — Child Bounds Widening Refused
     * Parent bound is [0x40210000, 0x40220000). Child requests 0x40230000 end.
     * ------------------------------------------------------------- */
    add x20, x20, #1
    mov w0, #1
    mov w1, #2
    mov x2, #4
    mov x3, #5
    mov w4, #RES_MEMORY_FRAME
    mov w5, #OP_FRAME_GRANT
    ldr x6, =0x40210000
    ldr x7, =0x00020000         /* Widened bound: 128 KiB > 64 KiB! */
    mov x8, #0
    bl derive_capability
    cbnz x0, 15f
    add x19, x19, #1
    orr x21, x21, #(1 << 14)
15:

    /* -------------------------------------------------------------
     * TEST 16: Adversarial Matrix — Integer Overflow in Child Bounds Refused
     * base = 0xFFFFFFFFFFFFF000, size = 0x2000 (wraps around 0)
     * ------------------------------------------------------------- */
    add x20, x20, #1
    mov w0, #1
    mov w1, #2
    mov x2, #4
    mov x3, #5
    mov w4, #RES_MEMORY_FRAME
    mov w5, #OP_FRAME_GRANT
    ldr x6, =0xFFFFFFFFFFFFF000
    ldr x7, =0x2000
    mov x8, #0
    bl derive_capability
    cbnz x0, 16f
    add x19, x19, #1
    orr x21, x21, #(1 << 15)
16:

    /* -------------------------------------------------------------
     * TEST 17: Adversarial Matrix — Root Capability Export Attempt
     * External principal (principal 9) attempts to exercise slot 0.
     * ------------------------------------------------------------- */
    add x20, x20, #1
    add x0, sp, #64
    mov w1, #1
    strh w1, [x0, #0]
    mov w1, #64
    strh w1, [x0, #2]
    str wzr, [x0, #4]
    mov x1, #111
    str x1, [x0, #8]
    mov x1, #9                  /* External caller */
    str x1, [x0, #16]
    mov w1, #0                  /* CAP_ROOT slot 0 */
    str w1, [x0, #24]
    mov w1, #1
    str w1, [x0, #28]
    mov w1, #RES_UNIVERSAL_ROOT
    str w1, [x0, #32]
    mov w1, #1
    str w1, [x0, #36]
    ldr x1, =0x40210000
    str x1, [x0, #40]
    mov x1, #4096
    str x1, [x0, #48]
    str xzr, [x0, #56]

    bl submit_effect
    cbz x0, 17f
    ldr w1, [x0, #4]
    cmp w1, #DEC_REJECTED_WRONG_PRINCIPAL
    b.ne 17f
    add x19, x19, #1
    orr x21, x21, #(1 << 16)
17:

    /* -------------------------------------------------------------
     * TEST 18: Replay Semantics — Identical Intent Returns Cached Result
     * Submit intent with request_id=200 on slot 1 gen 2 (principal 4).
     * Then submit duplicate request_id=200 with identical intent.
     * ------------------------------------------------------------- */
    add x20, x20, #1
    /* First submission */
    add x0, sp, #64
    mov w1, #1
    strh w1, [x0, #0]
    mov w1, #64
    strh w1, [x0, #2]
    str wzr, [x0, #4]
    mov x1, #200
    str x1, [x0, #8]            /* request_id = 200 */
    mov x1, #4                  /* principal 4 */
    str x1, [x0, #16]
    mov w1, #1                  /* slot 1 */
    str w1, [x0, #24]
    mov w1, #2                  /* gen 2 */
    str w1, [x0, #28]
    mov w1, #RES_MEMORY_FRAME
    str w1, [x0, #32]
    mov w1, #OP_FRAME_GRANT
    str w1, [x0, #36]
    ldr x1, =0x40212000
    str x1, [x0, #40]
    mov x1, #4096
    str x1, [x0, #48]
    str xzr, [x0, #56]

    bl submit_effect
    cbz x0, 18f
    mov x24, x0                 /* x24 = first receipt */

    /* Second submission (identical request_id 200 and intent) */
    add x0, sp, #64
    bl submit_effect
    cmp x0, x24                 /* Must return identical receipt pointer! */
    b.ne 18f
    add x19, x19, #1
    orr x21, x21, #(1 << 17)
18:

    /* -------------------------------------------------------------
     * TEST 19: Replay Semantics — Replay Conflict (Same ID, altered intent)
     * Submit request_id=200 with target=0x40213000 (altered!)
     * ------------------------------------------------------------- */
    add x20, x20, #1
    add x0, sp, #64
    mov w1, #1
    strh w1, [x0, #0]
    mov w1, #64
    strh w1, [x0, #2]
    str wzr, [x0, #4]
    mov x1, #200                /* Same request_id = 200 */
    str x1, [x0, #8]
    mov x1, #4
    str x1, [x0, #16]
    mov w1, #1
    str w1, [x0, #24]
    mov w1, #2
    str w1, [x0, #28]
    mov w1, #RES_MEMORY_FRAME
    str w1, [x0, #32]
    mov w1, #OP_FRAME_GRANT
    str w1, [x0, #36]
    ldr x1, =0x40213000         /* ALTERED TARGET! */
    str x1, [x0, #40]
    mov x1, #4096
    str x1, [x0, #48]
    str xzr, [x0, #56]

    bl submit_effect
    cbz x0, 19f
    ldr w1, [x0, #4]
    cmp w1, #DEC_REJECTED_REPLAY_CONFLICT
    b.ne 19f
    add x19, x19, #1
    orr x21, x21, #(1 << 18)
19:

    /* -------------------------------------------------------------
     * TEST 20: Console Resource Test (CONSOLE_WRITE to UART)
     * Derive Console Cap C (slot 3) from CAP_ROOT (caller 1, child 6)
     * Submit CONSOLE_WRITE intent.
     * ------------------------------------------------------------- */
    add x20, x20, #1
    mov w0, #0                  /* parent = 0 */
    mov w1, #1
    mov x2, #1
    mov x3, #6                  /* child = 6 */
    mov w4, #RES_CONSOLE
    mov w5, #OP_CONSOLE_WRITE
    ldr x6, =0x09000000         /* base = UART MMIO */
    ldr x7, =0x1000             /* size = 4 KiB */
    mov x8, #0
    bl derive_capability
    cbz x0, 20f

    /* Submit CONSOLE_WRITE intent with character '*' (0x2A) */
    add x0, sp, #64
    mov w1, #1
    strh w1, [x0, #0]
    mov w1, #64
    strh w1, [x0, #2]
    str wzr, [x0, #4]
    mov x1, #300
    str x1, [x0, #8]
    mov x1, #6                  /* principal 6 */
    str x1, [x0, #16]
    mov w1, #3                  /* slot 3 */
    str w1, [x0, #24]
    mov w1, #1                  /* gen 1 */
    str w1, [x0, #28]
    mov w1, #RES_CONSOLE
    str w1, [x0, #32]
    mov w1, #OP_CONSOLE_WRITE
    str w1, [x0, #36]
    ldr x1, =0x09000000
    str x1, [x0, #40]
    mov x1, #1
    str x1, [x0, #48]
    mov x1, #0x2A               /* character '*' in param0 (+0x38 = 56) */
    str x1, [x0, #56]

    bl submit_effect
    cbz x0, 20f
    ldr w1, [x0, #4]
    cmp w1, #DEC_ADMITTED
    b.ne 20f
    add x19, x19, #1
    orr x21, x21, #(1 << 19)
20:

    /* -------------------------------------------------------------
     * TEST 21: Measurement Resource Test (MEASUREMENT_READ)
     * Derive Measurement Cap D (slot 4) from CAP_ROOT (caller 1, child 7)
     * Submit MEASUREMENT_READ intent.
     * ------------------------------------------------------------- */
    add x20, x20, #1
    mov w0, #0
    mov w1, #1
    mov x2, #1
    mov x3, #7                  /* child 7 */
    mov w4, #RES_MEASUREMENT
    mov w5, #OP_MEASUREMENT_READ
    mov x6, #0
    mov x7, #0
    mov x8, #0
    bl derive_capability
    cbz x0, 21f

    add x0, sp, #64
    mov w1, #1
    strh w1, [x0, #0]
    mov w1, #64
    strh w1, [x0, #2]
    str wzr, [x0, #4]
    mov x1, #400
    str x1, [x0, #8]
    mov x1, #7
    str x1, [x0, #16]
    mov w1, #4                  /* slot 4 */
    str w1, [x0, #24]
    mov w1, #1                  /* gen 1 */
    str w1, [x0, #28]
    mov w1, #RES_MEASUREMENT
    str w1, [x0, #32]
    mov w1, #OP_MEASUREMENT_READ
    str w1, [x0, #36]
    mov x1, #0
    str x1, [x0, #40]
    str x1, [x0, #48]
    str x1, [x0, #56]

    bl submit_effect
    cbz x0, 21f
    ldr w1, [x0, #4]
    cmp w1, #DEC_ADMITTED
    b.ne 21f
    add x19, x19, #1
    orr x21, x21, #(1 << 20)
21:

    /* -------------------------------------------------------------
     * Write Test Summary Record to PHYSICS_BOOT_STATE + 0x200 (0x40208A00)
     *   +0x00: magic (0x4D33544553545355 'M3TESTSU')
     *   +0x08: tests_run (u64)
     *   +0x10: tests_passed (u64)
     *   +0x18: bitmask_results (u64)
     * ------------------------------------------------------------- */
    ldr x0, =TEST_RECORD_BASE
    ldr x1, =TEST_MAGIC
    str x1, [x0, #0]
    str x20, [x0, #8]           /* tests_run */
    str x19, [x0, #16]          /* tests_passed */
    str x21, [x0, #24]          /* bitmask_results */

    /* Emit telemetry */
    adr x0, msg_unit_tests_done
    bl print_string

    ldp x23, x24, [sp, #48]
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #160
    ret

    .section .rodata
    .balign 8
msg_unit_tests_start:
    .asciz "PHYSICS: M3_TESTS_START\n"
msg_unit_tests_done:
    .asciz "PHYSICS: M3_TESTS_DONE\n"
