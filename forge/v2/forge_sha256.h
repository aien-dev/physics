/* forge/v2/forge_sha256.h -- FORGE V2 private view of the pinned SHA-256.
 *
 * The implementation lives in ../../sha256_clean.c (audited and pinned into
 * the firmware builds). Do not modify it and do not add a second exported
 * SHA-256 (the symbols would clash at link time). This header only
 * forward-declares the one entry point FORGE V2 uses.
 *
 * sha256_compute returns the eight state words; FORGE V2 packs them
 * big-endian into the standard 32-byte digest (see forge_v2_sha256()).
 */
#ifndef FORGE_V2_SHA256_H
#define FORGE_V2_SHA256_H

#include <stdint.h>

void sha256_compute(const uint8_t *data, uint64_t len, uint8_t scratch_buf[128],
                    uint32_t out_digest[8]);

#endif
