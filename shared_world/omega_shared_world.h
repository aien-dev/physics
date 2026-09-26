/*
 * omega_shared_world.h -- PHYSICS realization of the OMEGA_SHARED_WORLD_V1
 * ring mechanics and CPU-side acquire/release publication.
 *
 * OMEGA DEFINES the shared-world semantics (the ABI in
 * shared_world/omega_shared_world_abi.h and the doctrine). PHYSICS REALIZES
 * the coherent memory (coherent/physics_coherent.*) and the synchronization
 * mechanics: the release-publication and acquire-consumption ordering that
 * make a producer/consumer handshake correct across a coherent boundary.
 *
 * This translation unit is PURE with respect to hardware: it operates on a
 * base pointer into an already-mapped coherent region and uses C11 atomics
 * with explicit memory orders. On AArch64 an acquire load lowers to LDAR and a
 * release store to STLR (the `ldar`/`stlr` the doctrine names); this is
 * verified by disassembling the built object. It therefore host-tests the full
 * protocol logic (generation, epoch, ABA, wrap, replay, malformed, bounds)
 * with a CPU producer and CPU consumer, independently of the GPU worker.
 *
 * The GPU consumer/producer realizes the SAME ordering mechanically with
 * Blackwell STRONG.SYS loads/stores and MEMBAR fences (see the OMEGA Blackwell
 * codegen and the resident worker). The shared memory image is identical.
 */
#ifndef OMEGA_SHARED_WORLD_H
#define OMEGA_SHARED_WORLD_H

#include "omega_shared_world_abi.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Result codes (fail-closed). */
#define OMEGA_SW_OK               0
#define OMEGA_SW_ERR_INVALID     -1
#define OMEGA_SW_ERR_FULL        -2   /* producer: ring full, would overwrite   */
#define OMEGA_SW_ERR_EMPTY       -3   /* consumer: nothing published yet        */
#define OMEGA_SW_ERR_REJECTED    -4   /* consumer: hostile/stale entry rejected */

/*
 * A host-side handle onto a shared world laid out inside a coherent region.
 * Holds only a base CPU pointer plus cached, validated substructure pointers.
 * Carries no ownership of the region itself.
 */
typedef struct {
    uint8_t                       *base;         /* region base CPU mapping   */
    uint64_t                       region_bytes;
    OmegaSharedWorldHeader        *hdr;
    OmegaSharedWorldRing          *c2g;          /* CPU -> GPU ring           */
    OmegaSharedWorldRing          *g2c;          /* GPU -> CPU ring           */
    OmegaSharedWorldFaultMailbox  *fault;
    OmegaSharedWorldObjectTable   *objects;
    uint32_t                       world_epoch;
} OmegaSharedWorld;

/* ---- Layout & initialization ---------------------------------------- */

/*
 * Compute the minimum coherent region size needed to hold a full shared world
 * (header + both rings + fault mailbox + object table), all cache-line aligned.
 */
uint64_t omega_sw_required_bytes(void);

/*
 * Format a freshly allocated, zeroed coherent region as a shared world:
 * write the header, both ring control blocks, the fault mailbox, and the empty
 * object table, then a store fence. `base` must point at the region's CPU
 * mapping and `region_bytes` be its size. `world_epoch`/`world_generation`
 * stamp the world. Fails closed if the region is too small.
 */
int omega_sw_format(OmegaSharedWorld *w, void *base, uint64_t region_bytes,
                    uint32_t world_epoch, uint32_t world_generation);

/*
 * Attach to an already-formatted region (e.g. from a second mapping), fully
 * validating magic, version, header size, epoch and offsets before use.
 */
int omega_sw_attach(OmegaSharedWorld *w, void *base, uint64_t region_bytes,
                    uint32_t expected_epoch);

/* ---- Object table (logical references) ------------------------------ */

/*
 * Register a bounded [region_offset, +size) window as a logical shared object.
 * Returns the object_id and its live generation. The window must lie wholly
 * within the region and must not be the header/ring/mailbox reserved prologue.
 */
int omega_sw_object_register(OmegaSharedWorld *w, uint64_t region_offset,
                             uint64_t size, uint32_t permissions,
                             uint32_t *out_object_id, uint32_t *out_generation);

/* Revoke an object (bump generation, mark revoked). */
int omega_sw_object_revoke(OmegaSharedWorld *w, uint32_t object_id);

/*
 * Resolve+validate a logical reference to a CPU pointer, fail-closed:
 * epoch, object generation, active state, permission, and [offset,len) bounds
 * are all checked. Returns a pointer into the region or an error. Never
 * fabricates an address from attacker-supplied fields.
 */
int omega_sw_object_resolve(OmegaSharedWorld *w, uint32_t object_id,
                            uint32_t object_generation, uint32_t offset,
                            uint32_t length, uint32_t required_perms,
                            void **out_cpu);

/* ---- CPU producer / consumer (acquire/release) ---------------------- */

/*
 * Publish `desc` into a ring as the sole producer. Fills magic/version/epoch/
 * sequence/checksum, writes the descriptor body, then RELEASE-publishes the
 * tail so a consumer that ACQUIRE-observes the new tail is guaranteed to see
 * the fully written descriptor. Returns OMEGA_SW_ERR_FULL without overwriting
 * an unconsumed entry.
 */
int omega_sw_ring_publish(OmegaSharedWorld *w, OmegaSharedWorldRing *ring,
                          const OmegaSharedWorldDesc *desc);

/*
 * Consume the next entry from a ring as the sole consumer. ACQUIRE-observes the
 * tail; if an entry is available, copies it out, validates it against all
 * hostile-input rules (magic, version, epoch, expected sequence, msg type,
 * checksum), and only then RELEASE-advances head. On rejection it raises the
 * fault mailbox, does NOT advance head past a malformed slot's expected
 * sequence unless it is safe, and returns OMEGA_SW_ERR_REJECTED so the caller
 * can continue after a rejected entry.
 */
int omega_sw_ring_consume(OmegaSharedWorld *w, OmegaSharedWorldRing *ring,
                          OmegaSharedWorldDesc *out_desc);

/* Number of published-but-unconsumed entries (producer tail - consumer head). */
uint64_t omega_sw_ring_depth(const OmegaSharedWorldRing *ring);

/* True if the ring is full (cannot publish without overwriting). */
bool omega_sw_ring_full(const OmegaSharedWorldRing *ring);

/* ---- Integrity ------------------------------------------------------- */

/* CRC32C over control[0..0x3C) followed by all 64 payload bytes. */
uint32_t omega_sw_desc_checksum(const OmegaSharedWorldDesc *desc);

/* Raise the fault mailbox (release-published). */
void omega_sw_raise_fault(OmegaSharedWorld *w, uint32_t code, uint64_t sequence,
                          uint32_t detail_a, uint32_t detail_b);

#endif /* OMEGA_SHARED_WORLD_H */
