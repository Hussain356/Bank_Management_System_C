/*
 * sha256.h - Minimal, dependency-free SHA-256 (FIPS 180-4).
 */
#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_SIZE 32

typedef struct {
    uint32_t state[8];
    uint8_t  buf[64];
    size_t   buflen;
    uint64_t total; /* total bytes processed */
} Sha256Ctx;

void sha256_init(Sha256Ctx *ctx);
void sha256_update(Sha256Ctx *ctx, const uint8_t *data, size_t len);
void sha256_final(Sha256Ctx *ctx, uint8_t out[SHA256_DIGEST_SIZE]);

/* One-shot convenience wrapper. */
void sha256(const uint8_t *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);

#endif /* SHA256_H */
