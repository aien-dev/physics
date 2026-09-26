/*
 * physics_coherent.c -- PHYSICS coherent-region realization over NVRM/UVM.
 *
 * Realizes the OMEGA-facing PhysicsCoherentRegion abstraction on the M16
 * native, libcuda-free NVRM/UVM substrate. All NVIDIA-specific detail
 * (NvrmMem, RM handles, UVM ioctls, allocation classes) is confined to this
 * translation unit. The public header exposes only logical identity,
 * generation, size, and coherent-visibility properties.
 */
#include "physics_coherent.h"
#include "m16_native.h"   /* PHYSICS-internal: NVRM/UVM substrate */

#include <stdlib.h>
#include <string.h>

#define PHYS_COHERENT_MAX_REGIONS 128u

/* PHYSICS-private per-region realization. Never visible to OMEGA. */
typedef struct {
    int      in_use;
    uint32_t region_id;
    uint32_t generation;
    NvrmMem  mem;            /* coherent unified allocation (CPU==GPU VA) */
    PhysicsCoherentVisibility visibility;
} PhysCoherentSlot;

struct PhysicsCoherentContext {
    M16NativeContext m16;    /* one RM client + device + VAS + channel */
    int              channel_ready;
    PhysCoherentSlot slots[PHYS_COHERENT_MAX_REGIONS];
    uint32_t         next_region_id;
};

/* Recover the private slot from a public handle, validating identity. */
static PhysCoherentSlot *slot_of(const PhysicsCoherentRegion *r) {
    if (!r || !r->_backing) return NULL;
    PhysCoherentSlot *s = (PhysCoherentSlot *)r->_backing;
    if (!s->in_use) return NULL;
    if (s->region_id != r->region_id) return NULL;
    return s;
}

int physics_coherent_open(PhysicsCoherentContext **out_ctx) {
    if (!out_ctx) return PHYS_COHERENT_ERR_INVALID;
    PhysicsCoherentContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return PHYS_COHERENT_ERR_NOMEM;

    if (m16_native_open(&ctx->m16) != 0) {
        free(ctx);
        return PHYS_COHERENT_ERR_HARDWARE;
    }
    if (m16_native_create_channel(&ctx->m16) != 0) {
        m16_native_close(&ctx->m16);
        free(ctx);
        return PHYS_COHERENT_ERR_HARDWARE;
    }
    ctx->channel_ready = 1;
    ctx->next_region_id = 1;
    *out_ctx = ctx;
    return PHYS_COHERENT_OK;
}

void physics_coherent_close(PhysicsCoherentContext *ctx) {
    if (!ctx) return;
    /* The RM client teardown in m16_native_close frees every RM object,
     * mapping, and channel in one shot; individual region frees are not
     * required, but we invalidate handles defensively. */
    for (uint32_t i = 0; i < PHYS_COHERENT_MAX_REGIONS; i++) {
        ctx->slots[i].in_use = 0;
        ctx->slots[i].generation++;
    }
    m16_native_close(&ctx->m16);
    free(ctx);
}

int physics_coherent_region_alloc(PhysicsCoherentContext *ctx,
                                   uint64_t size,
                                   PhysicsCoherentVisibility vis,
                                   PhysicsCoherentRegion *out) {
    if (!ctx || !out || size == 0) return PHYS_COHERENT_ERR_INVALID;
    if (vis != PHYS_COHERENT_VIS_CPU_GPU_SNOOP) return PHYS_COHERENT_ERR_INVALID;

    /* Find a free slot. */
    PhysCoherentSlot *s = NULL;
    for (uint32_t i = 0; i < PHYS_COHERENT_MAX_REGIONS; i++) {
        if (!ctx->slots[i].in_use) { s = &ctx->slots[i]; break; }
    }
    if (!s) return PHYS_COHERENT_ERR_NOMEM;

    void *cpu = NULL;
    uint64_t gpu_va = 0;
    if (m16_native_alloc_memory(&ctx->m16, (size_t)size, &cpu, &gpu_va) != 0) {
        return PHYS_COHERENT_ERR_HARDWARE;
    }
    /* nvrm_alloc rounds up to a page and zero-initializes the whole span. */
    s->in_use = 1;
    s->region_id = ctx->next_region_id++;
    s->generation++;                 /* first live generation is >= 1 */
    if (s->generation == 0) s->generation = 1;
    s->mem.cpu = cpu;
    s->mem.va = gpu_va;
    /* nvrm_alloc rounds the request up to a 4 KiB page; mirror that rounding
     * so bounds checks match the physically mapped span exactly. */
    s->mem.size = (size + 0xFFFull) & ~0xFFFull;
    s->visibility = vis;

    out->region_id = s->region_id;
    out->generation = s->generation;
    out->size_bytes = s->mem.size;
    out->visibility = vis;
    out->_backing = s;
    return PHYS_COHERENT_OK;
}

int physics_coherent_region_revoke(PhysicsCoherentContext *ctx,
                                    PhysicsCoherentRegion *region) {
    if (!ctx || !region) return PHYS_COHERENT_ERR_INVALID;
    PhysCoherentSlot *s = slot_of(region);
    if (!s) return PHYS_COHERENT_ERR_STALE_GEN;
    /* Bump generation so any outstanding logical reference is now rejected.
     * Physical backing is reclaimed at context close (single RM teardown);
     * revoking a live region simply retires its logical identity. */
    s->generation++;
    if (s->generation == 0) s->generation = 1;
    s->in_use = 0;
    region->generation = s->generation;
    region->_backing = NULL;
    return PHYS_COHERENT_OK;
}

static int check_window(const PhysCoherentSlot *s, uint32_t gen,
                        uint64_t offset, uint64_t len) {
    if (!s) return PHYS_COHERENT_ERR_REVOKED;
    if (s->generation != gen) return PHYS_COHERENT_ERR_STALE_GEN;
    if (len == 0) return PHYS_COHERENT_ERR_INVALID;
    if (offset > s->mem.size) return PHYS_COHERENT_ERR_BOUNDS;
    if (len > s->mem.size - offset) return PHYS_COHERENT_ERR_BOUNDS;
    return PHYS_COHERENT_OK;
}

int physics_coherent_resolve_cpu(const PhysicsCoherentRegion *region,
                                  uint32_t gen, uint64_t offset, uint64_t len,
                                  void **out_cpu) {
    if (!out_cpu) return PHYS_COHERENT_ERR_INVALID;
    PhysCoherentSlot *s = slot_of(region);
    int rc = check_window(s, gen, offset, len);
    if (rc != PHYS_COHERENT_OK) return rc;
    *out_cpu = (uint8_t *)s->mem.cpu + offset;
    return PHYS_COHERENT_OK;
}

int physics_coherent_resolve_gpu(const PhysicsCoherentRegion *region,
                                  uint32_t gen, uint64_t offset, uint64_t len,
                                  uint64_t *out_gpu_va) {
    if (!out_gpu_va) return PHYS_COHERENT_ERR_INVALID;
    PhysCoherentSlot *s = slot_of(region);
    int rc = check_window(s, gen, offset, len);
    if (rc != PHYS_COHERENT_OK) return rc;
    *out_gpu_va = s->mem.va + offset;
    return PHYS_COHERENT_OK;
}

int physics_coherent_region_base_gpu(const PhysicsCoherentRegion *region,
                                      uint32_t gen, uint64_t *out_gpu_va) {
    return physics_coherent_resolve_gpu(region, gen, 0, 1, out_gpu_va);
}

int physics_coherent_region_base_cpu(const PhysicsCoherentRegion *region,
                                      uint32_t gen, void **out_cpu) {
    return physics_coherent_resolve_cpu(region, gen, 0, 1, out_cpu);
}

uint32_t physics_coherent_region_generation(const PhysicsCoherentRegion *region) {
    PhysCoherentSlot *s = slot_of(region);
    return s ? s->generation : 0;
}

uint64_t physics_coherent_region_size(const PhysicsCoherentRegion *region) {
    PhysCoherentSlot *s = slot_of(region);
    return s ? s->mem.size : 0;
}

int physics_coherent_region_is_active(const PhysicsCoherentRegion *region) {
    return slot_of(region) != NULL;
}

/* ---- Queue capability (trusted host runtime only) -------------------- *
 * Submission of an already-assembled method stream to the resident channel.
 * The method words are pure data (no NVRM internals cross this boundary), so
 * OMEGA-side codegen can build a launch stream and hand it to PHYSICS to
 * submit without ever seeing an RM handle. Declared here and prototyped in a
 * PHYSICS-internal companion so OMEGA links against it explicitly. */
int physics_coherent_submit_methods(PhysicsCoherentContext *ctx,
                                     const uint32_t *methods, size_t count) {
    if (!ctx || !ctx->channel_ready || !methods || count == 0)
        return PHYS_COHERENT_ERR_INVALID;
    return m16_native_submit_methods(&ctx->m16, methods, count) == 0
               ? PHYS_COHERENT_OK : PHYS_COHERENT_ERR_HARDWARE;
}
