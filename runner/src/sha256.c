/* sha256.c — reference SHA-256 implementation (public domain).
 *
 * Based on the FIPS 180-4 spec. One-shot only — no streaming API.
 * Used by the launcher to verify ROM dumps against canonical hashes
 * for projects (like zelda3) that publish SHA-256 rather than CRC32.
 */
#include "sha256.h"
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static inline uint32_t rotr32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(uint32_t H[8], const uint8_t blk[64]) {
    uint32_t W[64];
    for (int i = 0; i < 16; i++) {
        W[i] = ((uint32_t)blk[i*4]     << 24) |
               ((uint32_t)blk[i*4 + 1] << 16) |
               ((uint32_t)blk[i*4 + 2] << 8)  |
               ((uint32_t)blk[i*4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(W[i-15], 7) ^ rotr32(W[i-15], 18) ^ (W[i-15] >> 3);
        uint32_t s1 = rotr32(W[i-2], 17) ^ rotr32(W[i-2], 19) ^ (W[i-2] >> 10);
        W[i] = W[i-16] + s0 + W[i-7] + s1;
    }
    uint32_t a=H[0], b=H[1], c=H[2], d=H[3], e=H[4], f=H[5], g=H[6], h=H[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K[i] + W[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    H[0]+=a; H[1]+=b; H[2]+=c; H[3]+=d;
    H[4]+=e; H[5]+=f; H[6]+=g; H[7]+=h;
}

void sha256_compute(const uint8_t *data, size_t len, uint8_t out[32]) {
    uint32_t H[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };

    /* Process full 64-byte blocks. */
    size_t off = 0;
    while (off + 64 <= len) {
        sha256_block(H, data + off);
        off += 64;
    }

    /* Final block(s): copy tail + 0x80 + pad zeros + 64-bit big-endian length. */
    uint8_t tail[128];
    size_t tail_len = len - off;
    memcpy(tail, data + off, tail_len);
    tail[tail_len] = 0x80;
    size_t pad_start = tail_len + 1;
    size_t total_tail = (tail_len < 56) ? 64 : 128;
    memset(tail + pad_start, 0, total_tail - pad_start - 8);
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) {
        tail[total_tail - 1 - i] = (uint8_t)(bits >> (i * 8));
    }
    sha256_block(H, tail);
    if (total_tail == 128) sha256_block(H, tail + 64);

    for (int i = 0; i < 8; i++) {
        out[i*4]     = (uint8_t)(H[i] >> 24);
        out[i*4 + 1] = (uint8_t)(H[i] >> 16);
        out[i*4 + 2] = (uint8_t)(H[i] >> 8);
        out[i*4 + 3] = (uint8_t)(H[i]);
    }
}
