#ifndef TINY_SHA256_H
#define TINY_SHA256_H

#include <stdint.h>
#include <stddef.h>

#define SHA256_DIGEST_LENGTH 32

typedef struct {
    uint32_t state[8];
    uint64_t bitcount;      // 累積 bit 數
    size_t buffer_len;      // 累積 buffer bytes (for padding)
    uint8_t buffer[64];     // block buffer
} tiny_sha256_ctx;

void tiny_sha256_init(tiny_sha256_ctx *ctx);
void tiny_sha256_update(tiny_sha256_ctx *ctx, const uint8_t *data, size_t len);
void tiny_sha256_final(tiny_sha256_ctx *ctx, uint8_t hash[SHA256_DIGEST_LENGTH]);

#endif // TINY_SHA256_H
