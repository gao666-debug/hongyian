/*
 * hy_sha256.c —— 最小 SHA-256 / HMAC-SHA256（FIPS 180-4）
 */
#include "hy_sha256.h"

#include <string.h>

#define HY_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define HY_SHR(x, n)  ((x) >> (n))

static const uint32_t g_sha256_k[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
    0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
    0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
    0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
    0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
    0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

typedef struct {
    uint32_t state[8];
    uint64_t total_len;
    uint8_t  buf[64];
    uint32_t buf_len;
} hy_sha256_ctx_t;

static void hy_sha256_block(hy_sha256_ctx_t *ctx, const uint8_t *p)
{
    uint32_t w[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    uint32_t i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = HY_ROTR(w[i - 15], 7) ^ HY_ROTR(w[i - 15], 18) ^ HY_SHR(w[i - 15], 3);
        uint32_t s1 = HY_ROTR(w[i - 2], 17) ^ HY_ROTR(w[i - 2], 19) ^ HY_SHR(w[i - 2], 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        uint32_t s1 = HY_ROTR(e, 6) ^ HY_ROTR(e, 11) ^ HY_ROTR(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + ch + g_sha256_k[i] + w[i];
        uint32_t s0 = HY_ROTR(a, 2) ^ HY_ROTR(a, 13) ^ HY_ROTR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void hy_sha256_init(hy_sha256_ctx_t *ctx)
{
    ctx->state[0] = 0x6a09e667UL;
    ctx->state[1] = 0xbb67ae85UL;
    ctx->state[2] = 0x3c6ef372UL;
    ctx->state[3] = 0xa54ff53aUL;
    ctx->state[4] = 0x510e527fUL;
    ctx->state[5] = 0x9b05688cUL;
    ctx->state[6] = 0x1f83d9abUL;
    ctx->state[7] = 0x5be0cd19UL;
    ctx->total_len = 0;
    ctx->buf_len = 0;
}

static void hy_sha256_update(hy_sha256_ctx_t *ctx, const uint8_t *data, uint32_t len)
{
    uint32_t i;

    ctx->total_len += len;

    if (ctx->buf_len != 0) {
        uint32_t need = 64 - ctx->buf_len;
        uint32_t take = (len < need) ? len : need;
        memcpy(&ctx->buf[ctx->buf_len], data, take);
        ctx->buf_len += take;
        data += take;
        len -= take;
        if (ctx->buf_len == 64) {
            hy_sha256_block(ctx, ctx->buf);
            ctx->buf_len = 0;
        }
    }

    for (i = 0; i + 64 <= len; i += 64) {
        hy_sha256_block(ctx, &data[i]);
    }

    if (i < len) {
        ctx->buf_len = len - i;
        memcpy(ctx->buf, &data[i], ctx->buf_len);
    }
}

static void hy_sha256_final(hy_sha256_ctx_t *ctx, uint8_t out[32])
{
    uint64_t bits = ctx->total_len * 8U;
    uint8_t  pad[72];
    uint32_t pad_len;
    uint32_t i;

    pad_len = (ctx->buf_len < 56) ? (56 - ctx->buf_len) : (120 - ctx->buf_len);
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    for (i = 0; i < 8; i++) {
        pad[pad_len + i] = (uint8_t)(bits >> (56 - i * 8));
    }
    hy_sha256_update(ctx, pad, pad_len + 8);

    for (i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

void hy_sha256(const uint8_t *data, uint32_t len, uint8_t out[32])
{
    hy_sha256_ctx_t ctx;

    hy_sha256_init(&ctx);
    if (len != 0) {
        hy_sha256_update(&ctx, data, len);
    }
    hy_sha256_final(&ctx, out);
}

void hy_hmac_sha256(const uint8_t *key, uint32_t key_len,
                    const uint8_t *msg, uint32_t msg_len,
                    uint8_t out[32])
{
    uint8_t k[64];
    uint8_t ipad[64];
    uint8_t opad[64];
    uint8_t inner[32];
    hy_sha256_ctx_t ctx;
    uint32_t i;

    memset(k, 0, sizeof(k));
    if (key_len > 64) {
        hy_sha256(key, key_len, k);
    } else if (key_len != 0) {
        memcpy(k, key, key_len);
    }

    for (i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36);
        opad[i] = (uint8_t)(k[i] ^ 0x5c);
    }

    hy_sha256_init(&ctx);
    hy_sha256_update(&ctx, ipad, 64);
    if (msg_len != 0) {
        hy_sha256_update(&ctx, msg, msg_len);
    }
    hy_sha256_final(&ctx, inner);

    hy_sha256_init(&ctx);
    hy_sha256_update(&ctx, opad, 64);
    hy_sha256_update(&ctx, inner, 32);
    hy_sha256_final(&ctx, out);
}

void hy_hex_encode(const uint8_t *in, uint32_t len, char *out)
{
    static const char hex[] = "0123456789abcdef";
    uint32_t i;

    for (i = 0; i < len; i++) {
        out[i * 2]     = hex[(in[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[in[i] & 0x0F];
    }
    out[len * 2] = '\0';
}
