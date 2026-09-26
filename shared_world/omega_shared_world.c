/*
 * omega_shared_world.c -- SPSC ring mechanics + CPU acquire/release for
 * OMEGA_SHARED_WORLD_V1. See omega_shared_world.h for the layering rationale.
 *
 * Ordering model (single-producer / single-consumer per ring):
 *   - head/tail are free-running 64-bit counters. slot = counter & mask.
 *     Because they never wrap in the machine's lifetime (2^64 exchanges), the
 *     per-slot sequence == the counter value at publication is globally unique;
 *     this is what defeats ABA on slot reuse and replay of an old entry.
 *   - PUBLISH: producer writes the entire descriptor (sequence := tail), then
 *     RELEASE-stores tail+1. A consumer that ACQUIRE-loads the new tail is
 *     guaranteed by release/acquire to observe the complete descriptor.
 *   - CONSUME: consumer ACQUIRE-loads tail; if head < tail, slot[head] is fully
 *     published. It validates the entry (magic/version/epoch/expected-sequence/
 *     msg-type/checksum) and then RELEASE-stores head+1 so the producer's
 *     flow-control (tail - head < capacity) sees the slot freed. Every
 *     non-empty poll advances head by exactly one, so a rejected (hostile)
 *     entry is consumed-and-rejected rather than re-read forever.
 *
 * On AArch64, atomic_load_explicit(...,acquire) lowers to LDAR and
 * atomic_store_explicit(...,release) to STLR -- the `ldar`/`stlr` the doctrine
 * specifies. Verified by disassembling the built object (see the M20 receipt).
 */
#include "omega_shared_world.h"

#include <stdatomic.h>
#include <string.h>

/* ---- 64-bit release/acquire over the shared (volatile) counters ------ *
 * The ABI declares head/tail as `volatile uint64_t` (endian-neutral, shared
 * with the GPU). We access them with C11 atomics via a checked cast; both
 * fields are 8-byte aligned by construction (offsets 0 and 64). */
static inline uint64_t load_acquire_u64(volatile uint64_t *p) {
    return atomic_load_explicit((_Atomic uint64_t *)p, memory_order_acquire);
}
static inline uint64_t load_relaxed_u64(volatile uint64_t *p) {
    return atomic_load_explicit((_Atomic uint64_t *)p, memory_order_relaxed);
}
static inline void store_release_u64(volatile uint64_t *p, uint64_t v) {
    atomic_store_explicit((_Atomic uint64_t *)p, v, memory_order_release);
}
static inline void store_release_u32(volatile uint32_t *p, uint32_t v) {
    atomic_store_explicit((_Atomic uint32_t *)p, v, memory_order_release);
}

/* ---- CRC32C (Castagnoli), lazy 256-entry table ---------------------- */
static uint32_t g_crc32c_table[256];
static int g_crc32c_ready = 0;
static void crc32c_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
        g_crc32c_table[i] = c;
    }
    g_crc32c_ready = 1;
}
static uint32_t crc32c(const uint8_t *data, size_t len) {
    if (!g_crc32c_ready) crc32c_init();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = g_crc32c_table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t omega_sw_desc_checksum(const OmegaSharedWorldDesc *desc) {
    /* control bytes [0x00..0x3C) then all 64 payload bytes */
    uint8_t buf[0x3C + 64];
    memcpy(buf, desc, 0x3C);
    memcpy(buf + 0x3C, desc->payload, 64);
    return crc32c(buf, sizeof(buf));
}

/* ---- Layout --------------------------------------------------------- */

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((uint64_t)(a) - 1))

/* Fixed layout offsets within the region (all cache-line aligned). */
static uint64_t off_header(void)  { return 0; }
static uint64_t off_c2g(void)     { return ALIGN_UP(sizeof(OmegaSharedWorldHeader), OMEGA_SW_CACHELINE); }
static uint64_t off_g2c(void)     { return ALIGN_UP(off_c2g() + sizeof(OmegaSharedWorldRing), OMEGA_SW_CACHELINE); }
static uint64_t off_fault(void)   { return ALIGN_UP(off_g2c() + sizeof(OmegaSharedWorldRing), OMEGA_SW_CACHELINE); }
static uint64_t off_objtbl(void)  { return ALIGN_UP(off_fault() + sizeof(OmegaSharedWorldFaultMailbox), OMEGA_SW_CACHELINE); }
static uint64_t off_prologue_end(void) { return ALIGN_UP(off_objtbl() + sizeof(OmegaSharedWorldObjectTable), OMEGA_SW_CACHELINE); }

uint64_t omega_sw_required_bytes(void) { return off_prologue_end(); }

static void ring_init(OmegaSharedWorldRing *r, uint32_t epoch, uint32_t generation) {
    memset(r, 0, sizeof(*r));
    r->tail = 0;
    r->head = 0;
    r->capacity = OMEGA_SW_RING_CAPACITY;
    r->mask = OMEGA_SW_RING_MASK;
    r->world_epoch = epoch;
    r->ring_generation = generation;
}

int omega_sw_format(OmegaSharedWorld *w, void *base, uint64_t region_bytes,
                    uint32_t world_epoch, uint32_t world_generation) {
    if (!w || !base) return OMEGA_SW_ERR_INVALID;
    if (region_bytes < omega_sw_required_bytes()) return OMEGA_SW_ERR_INVALID;

    memset(w, 0, sizeof(*w));
    w->base = (uint8_t *)base;
    w->region_bytes = region_bytes;
    w->world_epoch = world_epoch;

    w->hdr     = (OmegaSharedWorldHeader *)(w->base + off_header());
    w->c2g     = (OmegaSharedWorldRing *)(w->base + off_c2g());
    w->g2c     = (OmegaSharedWorldRing *)(w->base + off_g2c());
    w->fault   = (OmegaSharedWorldFaultMailbox *)(w->base + off_fault());
    w->objects = (OmegaSharedWorldObjectTable *)(w->base + off_objtbl());

    ring_init(w->c2g, world_epoch, world_generation);
    ring_init(w->g2c, world_epoch, world_generation);

    memset(w->fault, 0, sizeof(*w->fault));
    w->fault->world_epoch = world_epoch;

    memset(w->objects, 0, sizeof(*w->objects));
    w->objects->world_epoch = world_epoch;

    OmegaSharedWorldHeader *h = w->hdr;
    memset(h, 0, sizeof(*h));
    h->abi_version = OMEGA_SW_ABI_VERSION;
    h->header_bytes = (uint16_t)sizeof(OmegaSharedWorldHeader);
    h->world_epoch = world_epoch;
    h->world_generation = world_generation;
    h->region_bytes = region_bytes;
    h->ring_capacity = OMEGA_SW_RING_CAPACITY;
    h->desc_bytes = (uint32_t)sizeof(OmegaSharedWorldDesc);
    h->off_cpu_to_gpu_ring = off_c2g();
    h->off_gpu_to_cpu_ring = off_g2c();
    h->off_fault_mailbox = off_fault();
    h->off_object_table = off_objtbl();
    /* future rings remain 0 (absent) */

    /* Publish the header magic LAST with a release store so any attacher that
     * acquire-reads a valid magic sees the fully written world. */
    atomic_thread_fence(memory_order_release);
    store_release_u32(&h->magic, OMEGA_SW_MAGIC);
    return OMEGA_SW_OK;
}

int omega_sw_attach(OmegaSharedWorld *w, void *base, uint64_t region_bytes,
                    uint32_t expected_epoch) {
    if (!w || !base) return OMEGA_SW_ERR_INVALID;
    if (region_bytes < omega_sw_required_bytes()) return OMEGA_SW_ERR_INVALID;
    OmegaSharedWorldHeader *h = (OmegaSharedWorldHeader *)base;
    uint32_t magic = atomic_load_explicit((_Atomic uint32_t *)&h->magic, memory_order_acquire);
    if (magic != OMEGA_SW_MAGIC) return OMEGA_SW_ERR_INVALID;
    if (h->abi_version != OMEGA_SW_ABI_VERSION) return OMEGA_SW_ERR_INVALID;
    if (h->header_bytes != sizeof(OmegaSharedWorldHeader)) return OMEGA_SW_ERR_INVALID;
    if (h->desc_bytes != sizeof(OmegaSharedWorldDesc)) return OMEGA_SW_ERR_INVALID;
    if (h->world_epoch != expected_epoch) return OMEGA_SW_ERR_INVALID;
    if (h->off_cpu_to_gpu_ring != off_c2g() || h->off_gpu_to_cpu_ring != off_g2c() ||
        h->off_fault_mailbox != off_fault() || h->off_object_table != off_objtbl())
        return OMEGA_SW_ERR_INVALID;

    memset(w, 0, sizeof(*w));
    w->base = (uint8_t *)base;
    w->region_bytes = region_bytes;
    w->world_epoch = expected_epoch;
    w->hdr = h;
    w->c2g = (OmegaSharedWorldRing *)(w->base + off_c2g());
    w->g2c = (OmegaSharedWorldRing *)(w->base + off_g2c());
    w->fault = (OmegaSharedWorldFaultMailbox *)(w->base + off_fault());
    w->objects = (OmegaSharedWorldObjectTable *)(w->base + off_objtbl());
    return OMEGA_SW_OK;
}

/* ---- Object table --------------------------------------------------- */

int omega_sw_object_register(OmegaSharedWorld *w, uint64_t region_offset,
                             uint64_t size, uint32_t permissions,
                             uint32_t *out_object_id, uint32_t *out_generation) {
    if (!w || size == 0) return OMEGA_SW_ERR_INVALID;
    /* Objects live strictly in the payload area beyond the reserved prologue. */
    if (region_offset < off_prologue_end()) return OMEGA_SW_ERR_INVALID;
    if (region_offset > w->region_bytes) return OMEGA_SW_ERR_INVALID;
    if (size > w->region_bytes - region_offset) return OMEGA_SW_ERR_INVALID;
    if (permissions & ~(uint32_t)(OMEGA_SW_PERM_READ | OMEGA_SW_PERM_WRITE))
        return OMEGA_SW_ERR_INVALID;

    OmegaSharedWorldObjectTable *t = w->objects;
    for (uint32_t i = 0; i < OMEGA_SW_MAX_OBJECTS; i++) {
        OmegaSharedWorldObject *o = &t->objects[i];
        if (o->state == OMEGA_SW_OBJ_ACTIVE) continue;
        o->object_id = i;
        o->generation++;                    /* fresh generation on (re)use */
        if (o->generation == 0) o->generation = 1;
        o->state = OMEGA_SW_OBJ_ACTIVE;
        o->permissions = permissions;
        o->region_offset = region_offset;
        o->size_bytes = size;
        if (i + 1 > t->count) t->count = i + 1;
        if (out_object_id) *out_object_id = i;
        if (out_generation) *out_generation = o->generation;
        return OMEGA_SW_OK;
    }
    return OMEGA_SW_ERR_INVALID;
}

int omega_sw_object_revoke(OmegaSharedWorld *w, uint32_t object_id) {
    if (!w || object_id >= OMEGA_SW_MAX_OBJECTS) return OMEGA_SW_ERR_INVALID;
    OmegaSharedWorldObject *o = &w->objects->objects[object_id];
    if (o->state != OMEGA_SW_OBJ_ACTIVE) return OMEGA_SW_ERR_INVALID;
    o->state = OMEGA_SW_OBJ_REVOKED;
    o->generation++;
    if (o->generation == 0) o->generation = 1;
    return OMEGA_SW_OK;
}

int omega_sw_object_resolve(OmegaSharedWorld *w, uint32_t object_id,
                            uint32_t object_generation, uint32_t offset,
                            uint32_t length, uint32_t required_perms,
                            void **out_cpu) {
    if (!w || !out_cpu) return OMEGA_SW_ERR_INVALID;
    if (object_id >= OMEGA_SW_MAX_OBJECTS) return OMEGA_SW_ERR_REJECTED;
    OmegaSharedWorldObject *o = &w->objects->objects[object_id];
    if (o->state != OMEGA_SW_OBJ_ACTIVE) return OMEGA_SW_ERR_REJECTED;
    if (o->generation != object_generation) return OMEGA_SW_ERR_REJECTED; /* stale/ABA */
    if ((o->permissions & required_perms) != required_perms) return OMEGA_SW_ERR_REJECTED;
    if (length == 0) return OMEGA_SW_ERR_REJECTED;
    if ((uint64_t)offset > o->size_bytes) return OMEGA_SW_ERR_REJECTED;
    if ((uint64_t)length > o->size_bytes - offset) return OMEGA_SW_ERR_REJECTED;
    /* Final absolute bound against the region itself (defense in depth). */
    uint64_t abs_off = o->region_offset + offset;
    if (abs_off > w->region_bytes || (uint64_t)length > w->region_bytes - abs_off)
        return OMEGA_SW_ERR_REJECTED;
    *out_cpu = w->base + abs_off;
    return OMEGA_SW_OK;
}

/* ---- Fault mailbox -------------------------------------------------- */

void omega_sw_raise_fault(OmegaSharedWorld *w, uint32_t code, uint64_t sequence,
                          uint32_t detail_a, uint32_t detail_b) {
    if (!w || !w->fault) return;
    OmegaSharedWorldFaultMailbox *m = w->fault;
    m->world_epoch = w->world_epoch;
    m->fault_sequence = sequence;
    m->detail_a = detail_a;
    m->detail_b = detail_b;
    store_release_u32(&m->fault_code, code);
}

/* ---- Ring producer / consumer --------------------------------------- */

uint64_t omega_sw_ring_depth(const OmegaSharedWorldRing *ring) {
    uint64_t t = load_acquire_u64((volatile uint64_t *)&ring->tail);
    uint64_t h = load_acquire_u64((volatile uint64_t *)&ring->head);
    return t - h;
}

bool omega_sw_ring_full(const OmegaSharedWorldRing *ring) {
    return omega_sw_ring_depth(ring) >= ring->capacity;
}

int omega_sw_ring_publish(OmegaSharedWorld *w, OmegaSharedWorldRing *ring,
                          const OmegaSharedWorldDesc *desc) {
    if (!w || !ring || !desc) return OMEGA_SW_ERR_INVALID;

    uint64_t t = load_relaxed_u64(&ring->tail);      /* producer owns tail */
    uint64_t h = load_acquire_u64(&ring->head);      /* observe consumer   */
    if (t - h >= ring->capacity) return OMEGA_SW_ERR_FULL; /* never overwrite */

    OmegaSharedWorldDesc *slot = &ring->slots[t & ring->mask];

    /* Build the entry in a local, stamp the canonical fields, checksum, then
     * copy the body into the slot; the tail release-store publishes it. */
    OmegaSharedWorldDesc d = *desc;
    d.magic = OMEGA_SW_MAGIC;
    d.abi_version = OMEGA_SW_ABI_VERSION;
    d.sequence = t;                       /* slot sequence == counter (ABA/replay guard) */
    d.world_epoch = w->world_epoch;
    d.reserved0 = 0;
    /* CRC32C is optional defense-in-depth: the primary torn/stale/replay guard
     * is release/acquire ordering plus sequence==index. The CPU producer always
     * fills and flags it; the consumer verifies only when the flag is set (so a
     * GPU worker need not implement CRC32C in SASS). */
    d.flags |= OMEGA_SW_FLAG_CHECKSUM;
    d.checksum = 0;
    d.checksum = omega_sw_desc_checksum(&d);

    memcpy(slot, &d, sizeof(d));

    /* RELEASE: all descriptor stores above happen-before the tail becomes
     * visible to a consumer that acquire-loads it. */
    store_release_u64(&ring->tail, t + 1);
    return OMEGA_SW_OK;
}

int omega_sw_ring_consume(OmegaSharedWorld *w, OmegaSharedWorldRing *ring,
                          OmegaSharedWorldDesc *out_desc) {
    if (!w || !ring) return OMEGA_SW_ERR_INVALID;

    uint64_t h = load_relaxed_u64(&ring->head);      /* consumer owns head */
    uint64_t t = load_acquire_u64(&ring->tail);      /* observe producer   */
    if (h >= t) return OMEGA_SW_ERR_EMPTY;

    OmegaSharedWorldDesc d;
    memcpy(&d, &ring->slots[h & ring->mask], sizeof(d));

    /* Validate against hostile input. Any failure consumes-and-rejects slot h
     * (head advances) so the consumer always makes forward progress and never
     * re-reads a bad slot forever. */
    int fault = OMEGA_SW_FAULT_NONE;
    if (d.magic != OMEGA_SW_MAGIC)                       fault = OMEGA_SW_FAULT_BAD_MAGIC;
    else if (d.abi_version != OMEGA_SW_ABI_VERSION)      fault = OMEGA_SW_FAULT_BAD_VERSION;
    else if (d.world_epoch != w->world_epoch)            fault = OMEGA_SW_FAULT_STALE_EPOCH;
    else if (d.sequence != h)                            fault = OMEGA_SW_FAULT_BAD_SEQUENCE; /* stale/replay/torn */
    else if (d.msg_type == OMEGA_SW_MSG_NONE ||
             d.msg_type > OMEGA_SW_MSG_SHUTDOWN)         fault = OMEGA_SW_FAULT_BAD_MSGTYPE;
    else if (d.flags & OMEGA_SW_FLAG_CHECKSUM) {
        /* Optional defense-in-depth; verified only when the producer flagged
         * it. The essential guarantees (no stale/torn/dup/replay/reorder) hold
         * without it via release/acquire + sequence==head. */
        uint32_t want = d.checksum;
        OmegaSharedWorldDesc tmp = d;
        tmp.checksum = 0;
        if (omega_sw_desc_checksum(&tmp) != want)        fault = OMEGA_SW_FAULT_BAD_CHECKSUM;
    }

    /* RELEASE-advance head: frees slot h for the producer's flow control. */
    store_release_u64(&ring->head, h + 1);

    if (fault != OMEGA_SW_FAULT_NONE) {
        omega_sw_raise_fault(w, (uint32_t)fault, h, d.msg_type, (uint32_t)d.sequence);
        return OMEGA_SW_ERR_REJECTED;
    }
    if (out_desc) *out_desc = d;
    return OMEGA_SW_OK;
}
