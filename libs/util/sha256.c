/*
 *
 *      sha256.c
 *      SHA-256 (FIPS 180-4) cryptographic hash
 *
 *      2026/10/5 MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <libs/std/stddef.h>
#include <libs/std/string.h>
#include <libs/util/sha256.h>

/* Round constants of the compression function. */
static const uint32_t sha256_k[64] = {
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U, 0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U, 0x72BE5D74U,
    0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U, 0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU, 0x983E5152U, 0xA831C66DU,
    0xB00327C8U, 0xBF597FC7U, 0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U, 0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U, 0x650A7354U, 0x766A0ABBU, 0x81C2C92EU,
    0x92722C85U, 0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U, 0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U,
    0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U, 0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U, 0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U,
};

/* Initial hash value the eight working words start from. */
static const uint32_t sha256_iv[8] = {
    0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU, 0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U,
};

/* Rotate a word right; masking the shift keeps a count of zero defined. */
static uint32_t sha256_rotr(uint32_t value, unsigned int shift)
{
    return (value >> (shift & 31U)) | (value << ((32U - shift) & 31U));
}

/* Read a 32-bit word in big-endian order. */
static uint32_t sha256_get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Write a 32-bit word in big-endian order. */
static void sha256_put_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

/* Absorb one 64-byte block: extend the message schedule, then run 64 rounds. */
static void sha256_transform(sha256_ctx_t *ctx, const uint8_t *block)
{
    uint32_t w[64];
    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (size_t i = 0; i < 16; i++) w[i] = sha256_get_be32(block + (i * 4));
    for (size_t i = 16; i < 64; i++) {
        uint32_t s0 = sha256_rotr(w[i - 15], 7) ^ sha256_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = sha256_rotr(w[i - 2], 17) ^ sha256_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i]        = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (size_t i = 0; i < 64; i++) {
        uint32_t s1  = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
        uint32_t ch  = (e & f) ^ (~e & g);
        uint32_t t1  = h + s1 + ch + sha256_k[i] + w[i];
        uint32_t s0  = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2  = s0 + maj;

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

/* Write the message length, in bits, as a 64-bit big-endian field. */
static void sha256_put_length(uint8_t *p, uint64_t length)
{
    for (size_t i = 0; i < 8; i++) p[i] = (uint8_t)(length >> (56 - (i * 8)));
}

/* Append 0x80 and the zero fill, flushing a block when the length no longer fits. */
static void sha256_pad(sha256_ctx_t *ctx)
{
    size_t used = ctx->used;

    ctx->buffer[used++] = 0x80;
    if (used > 56) {
        memset(ctx->buffer + used, 0, SHA256_BLOCK_SIZE - used);
        sha256_transform(ctx, ctx->buffer);
        used = 0;
    }
    memset(ctx->buffer + used, 0, 56 - used);
}

/* Initialize a context to the initial hash value. */
void sha256_init(sha256_ctx_t *ctx)
{
    memcpy(ctx->state, sha256_iv, sizeof(sha256_iv));
    ctx->length = 0;
    ctx->used   = 0;
}

/* Absorb more message bytes, buffering whatever does not complete a block. */
void sha256_update(sha256_ctx_t *ctx, const void *data, size_t length)
{
    const uint8_t *bytes = data;

    /* Nothing to absorb, and it keeps a NULL buffer out of memcpy below. */
    if (!length) return;
    ctx->length += length;

    /* Top up a partially filled block first. */
    if (ctx->used) {
        size_t want = SHA256_BLOCK_SIZE - ctx->used;
        size_t take = (length < want) ? length : want;

        memcpy(ctx->buffer + ctx->used, bytes, take);
        ctx->used += take;
        bytes += take;
        length -= take;
        if (ctx->used < SHA256_BLOCK_SIZE) return;
        sha256_transform(ctx, ctx->buffer);
        ctx->used = 0;
    }

    /* Absorb whole blocks straight out of the caller's buffer. */
    while (length >= SHA256_BLOCK_SIZE) {
        sha256_transform(ctx, bytes);
        bytes += SHA256_BLOCK_SIZE;
        length -= SHA256_BLOCK_SIZE;
    }

    /* Keep the tail for the next call. */
    if (length) {
        memcpy(ctx->buffer, bytes, length);
        ctx->used = length;
    }
}

/* Produce the digest and leave the context ready for a new message. */
void sha256_final(sha256_ctx_t *ctx, uint8_t digest[SHA256_DIGEST_SIZE])
{
    uint64_t bits = ctx->length * 8;

    sha256_pad(ctx);
    sha256_put_length(ctx->buffer + 56, bits);
    sha256_transform(ctx, ctx->buffer);

    for (size_t i = 0; i < 8; i++) sha256_put_be32(digest + (i * 4), ctx->state[i]);
    sha256_init(ctx);
}

/* Hash @length bytes in a single call. */
void sha256(const void *data, size_t length, uint8_t digest[SHA256_DIGEST_SIZE])
{
    sha256_ctx_t ctx;

    sha256_init(&ctx);
    sha256_update(&ctx, data, length);
    sha256_final(&ctx, digest);
}
