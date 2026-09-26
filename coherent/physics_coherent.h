/*
 * physics_coherent.h -- PHYSICS coherent-region abstraction (OMEGA-facing).
 *
 * A PhysicsCoherentRegion is the long-term architectural interface between
 * OMEGA and the physical coherent memory it interprets. It represents one
 * PHYSICS-owned, CPU+GPU-coherent allocation with a stable logical identity,
 * a generation, a size, and explicitly declared coherent-visibility properties.
 *
 *   OMEGA DEFINES the shared-world semantics; PHYSICS REALIZES the coherent
 *   memory, addresses, mappings, and synchronization mechanics.
 *
 * This header deliberately exposes NOTHING about the physical realization:
 * no NvrmMem, no RM object handles, no UVM ioctl structures, no NVIDIA
 * allocation classes. Those remain PHYSICS implementation details behind an
 * opaque backing pointer. OMEGA-facing code compiles against this header
 * without ever including nvrm.h.
 *
 * The abstraction supports: allocate, internal CPU-view resolution, internal
 * GPU-VA resolution, bounds checks, generation validation, revoke/free, and
 * explicit coherent-visibility properties.
 *
 * Raw CPU pointers and GPU virtual addresses returned by the resolve* calls
 * are for the TRUSTED host runtime only (to seed a QMD or read a result). They
 * are NEVER serialized into the shared-memory ABI: the shared world references
 * memory only by logical {object_id, generation, offset, length}.
 */
#ifndef PHYSICS_COHERENT_H
#define PHYSICS_COHERENT_H

#include <stddef.h>
#include <stdint.h>

/* Result codes (fail-closed; negative == rejected). */
#define PHYS_COHERENT_OK              0
#define PHYS_COHERENT_ERR_INVALID    -1
#define PHYS_COHERENT_ERR_NOMEM      -2
#define PHYS_COHERENT_ERR_HARDWARE   -3
#define PHYS_COHERENT_ERR_STALE_GEN  -4
#define PHYS_COHERENT_ERR_BOUNDS     -5
#define PHYS_COHERENT_ERR_REVOKED    -6

/*
 * Coherent-visibility properties. Declared explicitly so callers state, and
 * PHYSICS records, exactly what coherence contract a region carries. On GB10
 * (no discrete VRAM) every region is coherent system memory visible to both
 * the Arm CPU and the Blackwell GPU with hardware snoop and GPU-cacheable
 * attributes; the enum leaves room to distinguish weaker contracts later.
 */
typedef enum {
    /* CPU+GPU coherent system memory, hardware snoop, GPU-cacheable.
     * Cross-processor ordering still requires explicit acquire/release. */
    PHYS_COHERENT_VIS_CPU_GPU_SNOOP = 1
} PhysicsCoherentVisibility;

/* Opaque PHYSICS context: owns exactly one RM client / device / VA space, and
 * (for Stage 1) the GPFIFO channel used to bootstrap the resident worker. */
typedef struct PhysicsCoherentContext PhysicsCoherentContext;

/*
 * A coherent region handle. The first fields are the OMEGA-facing logical
 * identity; `_backing` is an opaque pointer into PHYSICS-private state and must
 * never be dereferenced or interpreted outside physics_coherent.c.
 */
typedef struct {
    uint32_t region_id;      /* stable logical identity                 */
    uint32_t generation;     /* bumped on revoke/reuse (ABA guard)      */
    uint64_t size_bytes;     /* usable region size                      */
    PhysicsCoherentVisibility visibility;
    void    *_backing;       /* PHYSICS-private realization (opaque)    */
} PhysicsCoherentRegion;

/* ---- Context lifecycle ---------------------------------------------- */

/* Open a PHYSICS coherent context (one RM client + device + VA space). */
int  physics_coherent_open(PhysicsCoherentContext **out_ctx);

/* Tear down every region, mapping, channel, and RM object owned here. */
void physics_coherent_close(PhysicsCoherentContext *ctx);

/* ---- Region lifecycle ----------------------------------------------- */

/*
 * Allocate one coherent region of at least `size` bytes with the requested
 * visibility. On success `out` carries a fresh region_id and generation, and
 * the whole region is zero-initialized.
 */
int  physics_coherent_region_alloc(PhysicsCoherentContext *ctx,
                                    uint64_t size,
                                    PhysicsCoherentVisibility vis,
                                    PhysicsCoherentRegion *out);

/*
 * Revoke a region: bump its generation (so any outstanding logical reference
 * with the old generation is now rejected) and free the physical backing.
 */
int  physics_coherent_region_revoke(PhysicsCoherentContext *ctx,
                                     PhysicsCoherentRegion *region);

/* ---- Internal address resolution (trusted host runtime only) -------- */

/*
 * Resolve a bounds- and generation-checked CPU view of [offset, offset+len).
 * `gen` must equal the region's current generation. Returns a raw CPU pointer
 * for trusted host use only; never place it in shared memory.
 */
int  physics_coherent_resolve_cpu(const PhysicsCoherentRegion *region,
                                   uint32_t gen,
                                   uint64_t offset,
                                   uint64_t len,
                                   void **out_cpu);

/*
 * Resolve a bounds- and generation-checked GPU virtual address for
 * [offset, offset+len). For trusted host use only (e.g. to program a QMD
 * constant-bank argument); never placed in shared memory.
 */
int  physics_coherent_resolve_gpu(const PhysicsCoherentRegion *region,
                                   uint32_t gen,
                                   uint64_t offset,
                                   uint64_t len,
                                   uint64_t *out_gpu_va);

/* Region base GPU VA (offset 0), generation-checked. Trusted host only. */
int  physics_coherent_region_base_gpu(const PhysicsCoherentRegion *region,
                                       uint32_t gen,
                                       uint64_t *out_gpu_va);

/* Region base CPU pointer (offset 0), generation-checked. Trusted host only. */
int  physics_coherent_region_base_cpu(const PhysicsCoherentRegion *region,
                                       uint32_t gen,
                                       void **out_cpu);

/* ---- Accessors ------------------------------------------------------- */

uint32_t physics_coherent_region_generation(const PhysicsCoherentRegion *region);
uint64_t physics_coherent_region_size(const PhysicsCoherentRegion *region);
int      physics_coherent_region_is_active(const PhysicsCoherentRegion *region);

/* ---- Queue capability (trusted host runtime only) -------------------- */

/*
 * Submit an already-assembled GPFIFO method stream to the resident channel and
 * ring the doorbell. The method words are pure data; no RM handle or NVRM
 * structure crosses this boundary, so OMEGA-side codegen can build a launch
 * stream and hand it to PHYSICS to submit without depending on NVRM internals.
 */
int physics_coherent_submit_methods(PhysicsCoherentContext *ctx,
                                    const uint32_t *methods, size_t count);

#endif /* PHYSICS_COHERENT_H */
