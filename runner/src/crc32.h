#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* IEEE 802.3 / zlib CRC32. */
uint32_t crc32_compute(const uint8_t *data, size_t len);

/*
 * Incremental form, for digesting a byte stream that is never materialised
 * as one buffer (the netplay state digests walk the *_saveload serializers
 * callback by callback). Seed with CRC32_INIT, fold with crc32_update, and
 * close with crc32_final; the result equals crc32_compute over the same
 * bytes concatenated.
 */
#define CRC32_INIT 0xFFFFFFFFu
uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len);

static inline uint32_t crc32_final(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }

#ifdef __cplusplus
}
#endif
