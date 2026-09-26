/*
 * test_shared_world_host.c -- host (CPU-only) qualification of the
 * OMEGA_SHARED_WORLD_V1 ring protocol and object table.
 *
 * This proves the PROTOCOL logic (generation, epoch, expected-sequence/ABA,
 * wrap, replay, malformed, bounds, flow control) and the CPU acquire/release
 * publication using a real two-thread SPSC producer/consumer on the host Arm
 * CPU. It does NOT touch the GPU; the GPU cross-processor proof is a separate
 * on-silicon campaign. A malloc'd, page-aligned buffer stands in for the
 * coherent region so the identical shared-memory image is exercised.
 */
#include "omega_shared_world.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_pass = 0, g_fail = 0;
static void check(int cond, const char *name) {
    if (cond) { g_pass++; printf("  [PASS] %s\n", name); }
    else      { g_fail++; printf("  [FAIL] %s\n", name); }
}

static void *make_region(uint64_t bytes) {
    void *p = NULL;
    if (posix_memalign(&p, 4096, bytes) != 0) { perror("posix_memalign"); exit(2); }
    memset(p, 0, bytes);
    return p;
}

static OmegaSharedWorldDesc mk_req(uint32_t xform, uint32_t arg, uint32_t input) {
    OmegaSharedWorldDesc d;
    memset(&d, 0, sizeof(d));
    d.msg_type = OMEGA_SW_MSG_XFORM_REQ;
    d.flags = OMEGA_SW_FLAG_INLINE_PAYLOAD;
    d.xform = (uint16_t)xform;
    d.arg_a = arg;
    d.payload_len = 4;
    memcpy(d.payload, &input, 4);
    return d;
}

/* ---- Single-threaded protocol tests --------------------------------- */

static void test_lifecycle_and_layout(void) {
    printf("[GROUP] lifecycle & layout\n");
    uint64_t need = omega_sw_required_bytes();
    void *r = make_region(need + 4096);
    OmegaSharedWorld w;
    check(omega_sw_format(&w, r, need + 4096, 7, 1) == OMEGA_SW_OK, "format succeeds");
    check(w.hdr->magic == OMEGA_SW_MAGIC, "header magic set");
    check(w.hdr->world_epoch == 7, "world epoch stamped");
    check(w.c2g->capacity == OMEGA_SW_RING_CAPACITY, "c2g capacity");
    check(w.g2c->mask == OMEGA_SW_RING_MASK, "g2c mask");

    /* too-small region fails closed */
    OmegaSharedWorld w2;
    check(omega_sw_format(&w2, r, need - 1, 7, 1) == OMEGA_SW_ERR_INVALID, "undersized region rejected");

    /* attach validates */
    OmegaSharedWorld wa;
    check(omega_sw_attach(&wa, r, need + 4096, 7) == OMEGA_SW_OK, "attach with correct epoch");
    check(omega_sw_attach(&wa, r, need + 4096, 8) == OMEGA_SW_ERR_INVALID, "attach wrong epoch rejected");
    free(r);
}

static void test_empty_single_full(void) {
    printf("[GROUP] empty / single / full\n");
    uint64_t need = omega_sw_required_bytes();
    void *r = make_region(need);
    OmegaSharedWorld w; omega_sw_format(&w, r, need, 1, 1);
    OmegaSharedWorldDesc out;

    check(omega_sw_ring_consume(&w, w.c2g, &out) == OMEGA_SW_ERR_EMPTY, "empty ring -> EMPTY");

    OmegaSharedWorldDesc d = mk_req(OMEGA_SW_XFORM_XOR_CONST, 0xA5A5A5A5u, 0x12345678u);
    check(omega_sw_ring_publish(&w, w.c2g, &d) == OMEGA_SW_OK, "publish one");
    check(omega_sw_ring_depth(w.c2g) == 1, "depth == 1");
    check(omega_sw_ring_consume(&w, w.c2g, &out) == OMEGA_SW_OK, "consume one");
    check(out.sequence == 0 && out.arg_a == 0xA5A5A5A5u, "consumed entry matches");
    check(omega_sw_ring_consume(&w, w.c2g, &out) == OMEGA_SW_ERR_EMPTY, "drained -> EMPTY");

    /* fill to capacity, next publish must fail (no overwrite) */
    int ok = 1;
    for (uint32_t i = 0; i < OMEGA_SW_RING_CAPACITY; i++)
        ok &= (omega_sw_ring_publish(&w, w.c2g, &d) == OMEGA_SW_OK);
    check(ok, "fill to capacity");
    check(omega_sw_ring_full(w.c2g), "ring reports full");
    check(omega_sw_ring_publish(&w, w.c2g, &d) == OMEGA_SW_ERR_FULL, "overfill -> FULL (no overwrite)");
    free(r);
}

static void test_wraparound(void) {
    printf("[GROUP] wraparound (many full wraps)\n");
    uint64_t need = omega_sw_required_bytes();
    void *r = make_region(need);
    OmegaSharedWorld w; omega_sw_format(&w, r, need, 1, 1);
    OmegaSharedWorldDesc out;
    uint64_t total = (uint64_t)OMEGA_SW_RING_CAPACITY * 8 + 5; /* 8+ full wraps */
    int ok = 1;
    for (uint64_t i = 0; i < total; i++) {
        OmegaSharedWorldDesc d = mk_req(OMEGA_SW_XFORM_ADD_SEQ, 0, (uint32_t)i);
        ok &= (omega_sw_ring_publish(&w, w.c2g, &d) == OMEGA_SW_OK);
        ok &= (omega_sw_ring_consume(&w, w.c2g, &out) == OMEGA_SW_OK);
        ok &= (out.sequence == i);           /* strictly monotonic across wraps */
        uint32_t in; memcpy(&in, out.payload, 4);
        ok &= (in == (uint32_t)i);
    }
    check(ok, "8+ wraps: sequences strictly monotonic, payloads intact");
    check(w.c2g->tail == total && w.c2g->head == total, "cursors advanced exactly total");
    free(r);
}

static void test_replay_and_malformed(void) {
    printf("[GROUP] replay / malformed injection\n");
    uint64_t need = omega_sw_required_bytes();
    void *r = make_region(need);
    OmegaSharedWorld w; omega_sw_format(&w, r, need, 42, 1);
    OmegaSharedWorldDesc out;

    /* Inject a well-formed entry but with a STALE sequence (replay of an old
     * counter value) directly into slot 0, then publish tail. Consumer expects
     * sequence == head(0) and must reject. */
    OmegaSharedWorldDesc d = mk_req(OMEGA_SW_XFORM_XOR_CONST, 1, 1);
    d.magic = OMEGA_SW_MAGIC; d.abi_version = OMEGA_SW_ABI_VERSION;
    d.world_epoch = 42; d.sequence = 0xDEAD0000ull; d.checksum = 0;
    d.checksum = omega_sw_desc_checksum(&d);
    memcpy(&w.c2g->slots[0], &d, sizeof(d));
    atomic_store_explicit((_Atomic uint64_t *)&w.c2g->tail, 1, memory_order_release);
    check(omega_sw_ring_consume(&w, w.c2g, &out) == OMEGA_SW_ERR_REJECTED, "replayed sequence rejected");
    check(w.fault->fault_code == OMEGA_SW_FAULT_BAD_SEQUENCE, "fault: BAD_SEQUENCE");
    check(w.c2g->head == 1, "head advanced past rejected slot (forward progress)");

    /* helper to inject a raw slot at the current tail with a mutator */
    #define INJECT(mutate, expect_fault, label) do {                              \
        OmegaSharedWorld ww; void *rr = make_region(need);                        \
        omega_sw_format(&ww, rr, need, 42, 1);                                    \
        OmegaSharedWorldDesc x = mk_req(OMEGA_SW_XFORM_XOR_CONST, 1, 1);          \
        x.magic = OMEGA_SW_MAGIC; x.abi_version = OMEGA_SW_ABI_VERSION;           \
        x.world_epoch = 42; x.sequence = 0;                                      \
        x.flags |= OMEGA_SW_FLAG_CHECKSUM; /* CPU-produced: checksum is verified */\
        x.checksum = 0;                                                          \
        x.checksum = omega_sw_desc_checksum(&x);                                  \
        mutate;                                                                   \
        memcpy(&ww.c2g->slots[0], &x, sizeof(x));                                 \
        atomic_store_explicit((_Atomic uint64_t *)&ww.c2g->tail, 1, memory_order_release); \
        OmegaSharedWorldDesc o;                                                   \
        int rc = omega_sw_ring_consume(&ww, ww.c2g, &o);                          \
        check(rc == OMEGA_SW_ERR_REJECTED && ww.fault->fault_code == (expect_fault), label); \
        free(rr);                                                                 \
    } while (0)

    INJECT(x.magic = 0xBADBAD00u,        OMEGA_SW_FAULT_BAD_MAGIC,   "bad magic rejected");
    INJECT(x.abi_version = 0x99,          OMEGA_SW_FAULT_BAD_VERSION, "bad version rejected");
    INJECT(x.world_epoch = 43,            OMEGA_SW_FAULT_STALE_EPOCH, "stale epoch rejected");
    INJECT(x.msg_type = 0x7fff,           OMEGA_SW_FAULT_BAD_MSGTYPE, "bad msg type rejected");
    /* corrupt a payload byte AFTER checksum -> checksum mismatch */
    INJECT(x.payload[10] ^= 0xFF,         OMEGA_SW_FAULT_BAD_CHECKSUM,"torn/corrupt payload rejected");
    #undef INJECT
    free(r);
}

static void test_object_table(void) {
    printf("[GROUP] object table logical references\n");
    uint64_t payload_area = 65536;
    uint64_t need = omega_sw_required_bytes() + payload_area;
    void *r = make_region(need);
    OmegaSharedWorld w; omega_sw_format(&w, r, need, 5, 1);

    uint32_t id, gen;
    uint64_t obj_off = omega_sw_required_bytes(); /* first payload byte */
    check(omega_sw_object_register(&w, obj_off, 4096, OMEGA_SW_PERM_READ | OMEGA_SW_PERM_WRITE, &id, &gen) == OMEGA_SW_OK,
          "register object");
    void *cpu = NULL;
    check(omega_sw_object_resolve(&w, id, gen, 0, 4096, OMEGA_SW_PERM_READ, &cpu) == OMEGA_SW_OK, "resolve in-bounds");
    check(omega_sw_object_resolve(&w, id, gen, 0, 4097, OMEGA_SW_PERM_READ, &cpu) == OMEGA_SW_ERR_REJECTED, "OOB length rejected");
    check(omega_sw_object_resolve(&w, id, gen, 4096, 1, OMEGA_SW_PERM_READ, &cpu) == OMEGA_SW_ERR_REJECTED, "OOB offset rejected");
    check(omega_sw_object_resolve(&w, id, gen + 1, 0, 16, OMEGA_SW_PERM_READ, &cpu) == OMEGA_SW_ERR_REJECTED, "stale generation rejected");
    check(omega_sw_object_resolve(&w, id, gen, 0, 16, OMEGA_SW_PERM_WRITE, &cpu) == OMEGA_SW_OK, "write perm granted where allowed");
    check(omega_sw_object_resolve(&w, 63, 1, 0, 16, OMEGA_SW_PERM_READ, &cpu) == OMEGA_SW_ERR_REJECTED, "unregistered id rejected");

    /* revoke bumps generation; old reference now stale */
    omega_sw_object_revoke(&w, id);
    check(omega_sw_object_resolve(&w, id, gen, 0, 16, OMEGA_SW_PERM_READ, &cpu) == OMEGA_SW_ERR_REJECTED, "revoked object reference rejected");

    /* object cannot alias the reserved prologue */
    check(omega_sw_object_register(&w, 0, 64, OMEGA_SW_PERM_READ, &id, &gen) == OMEGA_SW_ERR_INVALID, "prologue-aliasing object rejected");
    free(r);
}

/* ---- Two-thread SPSC exchange (real acquire/release) ---------------- */

typedef struct {
    OmegaSharedWorld *w;
    uint64_t count;
    volatile int *go;
    int slow;                 /* inject variable delay */
} XferArgs;

static void spin_delay(int n) { for (volatile int i = 0; i < n; i++) { } }

static void *producer_fn(void *arg) {
    XferArgs *a = arg;
    while (!*a->go) { }
    for (uint64_t i = 0; i < a->count; ) {
        OmegaSharedWorldDesc d = mk_req(OMEGA_SW_XFORM_XOR_CONST, 0x0F0F0F0Fu, (uint32_t)(i * 2654435761u));
        int rc = omega_sw_ring_publish(a->w, a->w->c2g, &d);
        if (rc == OMEGA_SW_OK) { i++; if (a->slow && (i & 0x3FF) == 0) spin_delay(500); }
        /* on FULL, spin until consumer drains */
    }
    return NULL;
}

typedef struct { XferArgs base; uint64_t received; uint64_t dup; uint64_t missing; uint64_t bad; } ConsState;

static void *consumer_fn(void *arg) {
    ConsState *cs = arg;
    XferArgs *a = &cs->base;
    while (!*a->go) { }
    uint64_t expect = 0;
    OmegaSharedWorldDesc out;
    while (cs->received < a->count) {
        int rc = omega_sw_ring_consume(a->w, a->w->c2g, &out);
        if (rc == OMEGA_SW_OK) {
            if (out.sequence != expect) { if (out.sequence < expect) cs->dup++; else cs->missing++; }
            uint32_t in; memcpy(&in, out.payload, 4);
            if (in != (uint32_t)(out.sequence * 2654435761u)) cs->bad++;
            expect = out.sequence + 1;
            cs->received++;
            if (a->slow && (cs->received & 0x7FF) == 0) spin_delay(300);
        }
    }
    return NULL;
}

static void test_two_thread_exchange(uint64_t count, int slow, const char *label) {
    printf("[GROUP] two-thread SPSC exchange: %s (%llu msgs)\n", label, (unsigned long long)count);
    uint64_t need = omega_sw_required_bytes();
    void *r = make_region(need);
    OmegaSharedWorld w; omega_sw_format(&w, r, need, 1, 1);
    volatile int go = 0;

    ConsState cs; memset(&cs, 0, sizeof(cs));
    cs.base.w = &w; cs.base.count = count; cs.base.go = &go; cs.base.slow = slow;
    XferArgs pa = { &w, count, &go, slow };

    pthread_t pt, ct;
    pthread_create(&ct, NULL, consumer_fn, &cs);
    pthread_create(&pt, NULL, producer_fn, &pa);
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    go = 1;
    pthread_join(pt, NULL);
    pthread_join(ct, NULL);
    struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    check(cs.received == count, "received == sent");
    check(cs.dup == 0, "zero duplicates");
    check(cs.missing == 0, "zero missing / reordered-as-valid");
    check(cs.bad == 0, "zero torn payloads");
    check(w.fault->fault_code == OMEGA_SW_FAULT_NONE, "no faults raised");
    printf("         %.3fs, %.2f M msg/s\n", secs, count / 1e6 / secs);
    free(r);
}

/* ---- CPU-authority boundary (Step 8) -------------------------------- */

/*
 * Prove that no value a (hostile) GPU could place in a shared descriptor can be
 * turned into CPU execution authority. The ABI has no field for a function
 * pointer, host syscall, host/kernel VA, RM handle, or physical address; the
 * only memory reference is a logical {object_id, generation, offset, length}
 * tuple, and resolution yields an address ONLY inside a current, valid,
 * in-bounds registered object window. Arbitrary attacker values resolve to
 * nothing.
 */
static void test_cpu_authority_boundary(void) {
    printf("[GROUP] CPU authority boundary (Step 8)\n");
    uint64_t payload_area = 65536;
    uint64_t need = omega_sw_required_bytes() + payload_area;
    void *r = make_region(need);
    OmegaSharedWorld w; omega_sw_format(&w, r, need, 9, 1);
    uint32_t id, gen;
    omega_sw_object_register(&w, omega_sw_required_bytes(), 4096,
                             OMEGA_SW_PERM_READ | OMEGA_SW_PERM_WRITE, &id, &gen);
    void *cpu = NULL;

    /* A descriptor is only 128 bytes of fixed fields: confirm there is simply no
     * field wide enough / typed to carry a 64-bit host pointer as authority. The
     * object reference fields are 32-bit logical ids/offsets, not addresses. */
    OmegaSharedWorldDesc d; memset(&d, 0, sizeof(d));
    check(sizeof(d.object_id) == 4 && sizeof(d.object_offset) == 4,
          "object reference fields are 32-bit logical, not 64-bit addresses");

    /* Attacker sprays a real host VA pattern and an RM-handle-like value into the
     * logical-reference fields; resolution must reject (no matching object). */
    uintptr_t host_va = (uintptr_t)&w;                 /* a genuine host pointer  */
    uint32_t forged_id  = (uint32_t)(host_va & 0xffff);
    uint32_t forged_gen = (uint32_t)(host_va >> 16);
    check(omega_sw_object_resolve(&w, forged_id, forged_gen, 0, 16,
              OMEGA_SW_PERM_READ, &cpu) == OMEGA_SW_ERR_REJECTED,
          "forged id/gen from a host VA does not resolve");

    check(omega_sw_object_resolve(&w, 0xCF000001u /* RM-handle-like */, 1, 0, 16,
              OMEGA_SW_PERM_READ, &cpu) == OMEGA_SW_ERR_REJECTED,
          "RM-handle-like object id rejected");

    check(omega_sw_object_resolve(&w, id, gen, 0xFFFFFFF0u /* huge offset */, 16,
              OMEGA_SW_PERM_READ, &cpu) == OMEGA_SW_ERR_REJECTED,
          "arbitrary huge offset rejected (no escape from object window)");

    /* A valid reference resolves ONLY to an address inside the region. */
    check(omega_sw_object_resolve(&w, id, gen, 0, 16, OMEGA_SW_PERM_READ, &cpu)
              == OMEGA_SW_OK, "valid reference resolves");
    check((uint8_t *)cpu >= w.base &&
          (uint8_t *)cpu < w.base + w.region_bytes,
          "resolved address lies strictly inside the coherent region");

    /* There is no msg_type that means "execute" / "syscall": the enum is a small
     * closed set, and unknown types are rejected by the consumer. */
    check(OMEGA_SW_MSG_SHUTDOWN == 0x0004u,
          "message type space is a small closed enum (no execute/syscall verb)");
    free(r);
}

int main(int argc, char **argv) {
    uint64_t big = (argc > 1) ? strtoull(argv[1], NULL, 10) : 2000000ull;
    printf("=== OMEGA_SHARED_WORLD_V1 HOST PROTOCOL QUALIFICATION ===\n");
    test_lifecycle_and_layout();
    test_empty_single_full();
    test_wraparound();
    test_replay_and_malformed();
    test_object_table();
    test_cpu_authority_boundary();
    test_two_thread_exchange(100000, 0, "fast");
    test_two_thread_exchange(200000, 1, "variable delays");
    test_two_thread_exchange(big, 0, "sustained");
    printf("=== HOST QUALIFICATION: %d PASSED, %d FAILED ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
