#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compute SHA-256 of `len` bytes at `data`. Writes 32 bytes to `out`.
 * Self-contained reference implementation (public domain). */
void sha256_compute(const uint8_t *data, size_t len, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
