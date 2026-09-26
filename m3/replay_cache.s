/*
 * m3/replay_cache.s - Bounded Replay Cache & Idempotency Engine
 * Milestone 3: PHYSICS_EFFECTS (Sovereign Machine)
 *
 * Fixed-capacity 32-entry replay cache.
 * Identical replay -> idempotent original result with zero re-execution.
 * Conflicting intent with same request_id -> REJECTED_REPLAY_CONFLICT.
 * Storage: PHYSICS_REPLAY_CACHE @ 0x4020C000 (4 KiB)
 */

    .global init_replay_cache
    .global lookup_replay
    .global record_replay

    .section .text, "ax"
    .balign 4

    .equ REPLAY_CACHE_BASE,     0x4020C000
    .equ REPLAY_CACHE_CAPACITY, 32
    .equ REPLAY_ENTRY_SIZE,     64
    .equ REPLAY_ENTRY_SHIFT,    6

    .equ REPLAY_MISS,           0
    .equ REPLAY_HIT,            1
    .equ REPLAY_CONFLICT,       2

/*
 * init_replay_cache():
 * Zeros the 4 KiB replay cache area.
 */
init_replay_cache:
    ldr x0, =REPLAY_CACHE_BASE
    mov x1, #4096
.Lzero_replay_loop:
    cbz x1, .Lzero_replay_done
    stp xzr, xzr, [x0], #16
    sub x1, x1, #16
    b .Lzero_replay_loop
.Lzero_replay_done:
    ret

/*
 * lookup_replay(request_id, intent_digest_ptr)
 *   x0: request_id, x1: intent_digest_ptr
 * Returns:
 *   w0: REPLAY_MISS (0), REPLAY_HIT (1), or REPLAY_CONFLICT (2)
 *   x1: receipt_slot
 *   w2: decision
 *   x3: actual_effect
 */
lookup_replay:
    stp x29, x30, [sp, #-16]!
    mov x29, sp

    ldr x2, =REPLAY_CACHE_BASE
    mov w3, #0                  /* index 0..31 */

.Lsearch_replay:
    cmp w3, #REPLAY_CACHE_CAPACITY
    b.ge .Lreplay_not_found

    ubfiz x4, x3, #REPLAY_ENTRY_SHIFT, #5
    add x5, x2, x4              /* x5 = entry ptr */

    /* Check if entry valid */
    ldr x6, [x5, #0x40]         /* entry.valid */
    cbz x6, .Lreplay_next

    /* Check request_id match */
    ldr x7, [x5, #0x00]         /* entry.request_id */
    cmp x0, x7
    b.ne .Lreplay_next

    /* request_id matches! Compare 32-byte intent_digest */
    add x8, x5, #0x08           /* entry.intent_digest */
    ldp x9, x10, [x8, #0]
    ldp x11, x12, [x1, #0]
    cmp x9, x11
    b.ne .Lreplay_mismatch
    cmp x10, x12
    b.ne .Lreplay_mismatch

    ldp x9, x10, [x8, #16]
    ldp x11, x12, [x1, #16]
    cmp x9, x11
    b.ne .Lreplay_mismatch
    cmp x10, x12
    b.ne .Lreplay_mismatch

    /* Exact match! REPLAY_HIT */
    ldr w1, [x5, #0x28]         /* receipt_slot */
    ldr w2, [x5, #0x2C]         /* decision */
    ldr x3, [x5, #0x30]         /* actual_effect */
    mov w0, #REPLAY_HIT
    ldp x29, x30, [sp], #16
    ret

.Lreplay_mismatch:
    /* Same request_id, different intent digest -> REPLAY_CONFLICT */
    mov w0, #REPLAY_CONFLICT
    ldp x29, x30, [sp], #16
    ret

.Lreplay_next:
    add w3, w3, #1
    b .Lsearch_replay

.Lreplay_not_found:
    mov w0, #REPLAY_MISS
    ldp x29, x30, [sp], #16
    ret

/*
 * record_replay(request_id, intent_digest_ptr, receipt_slot, decision, actual_effect, output)
 *   x0: request_id, x1: intent_digest_ptr, w2: receipt_slot, w3: decision, x4: actual_effect, x5: output
 */
record_replay:
    ldr x6, =REPLAY_CACHE_BASE
    mov w7, #0

.Lfind_replay_slot:
    cmp w7, #REPLAY_CACHE_CAPACITY
    b.ge .Lrecord_done          /* Cache full */

    ubfiz x8, x7, #REPLAY_ENTRY_SHIFT, #5
    add x9, x6, x8              /* x9 = entry ptr */

    ldr x10, [x9, #0x40]        /* valid */
    cbz x10, .Lfound_free_replay

    /* Check if already matches this request_id */
    ldr x11, [x9, #0x00]
    cmp x0, x11
    b.eq .Lfound_free_replay

    add w7, w7, #1
    b .Lfind_replay_slot

.Lfound_free_replay:
    str x0, [x9, #0x00]         /* request_id */

    /* Copy 32-byte digest */
    ldp x12, x13, [x1, #0]
    ldp x14, x15, [x1, #16]
    stp x12, x13, [x9, #0x08]
    stp x14, x15, [x9, #0x18]

    str w2, [x9, #0x28]         /* receipt_slot */
    str w3, [x9, #0x2C]         /* decision */
    str x4, [x9, #0x30]         /* actual_effect */
    str x5, [x9, #0x38]         /* output */

    mov x12, #1
    str x12, [x9, #0x40]        /* valid = 1 */

.Lrecord_done:
    ret
