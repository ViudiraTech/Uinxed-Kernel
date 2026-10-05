/*
 *
 *      sha256.h
 *      SHA-256 (FIPS 180-4) cryptographic hash
 *
 *      2026/10/5 MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SHA256_H_
#define INCLUDE_SHA256_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

#define SHA256_DIGEST_SIZE 32
#define SHA256_BLOCK_SIZE  64

/* Streaming SHA-256 state; the caller owns the storage, no heap is used. */
typedef struct {
        uint32_t state[8];
        uint64_t length; // total message length in bytes
        uint8_t  buffer[SHA256_BLOCK_SIZE];
        size_t   used; // bytes currently buffered
} sha256_ctx_t;

/* Initialize a context to the FIPS 180-4 initial hash value. */
void sha256_init(sha256_ctx_t *ctx);

/* Absorb `length` bytes; call any number of times with arbitrary chunk sizes.  A zero length is a no-op. */
void sha256_update(sha256_ctx_t *ctx, const void *data, size_t length);

/* Produce the digest and leave the context ready for a new message. */
void sha256_final(sha256_ctx_t *ctx, uint8_t digest[SHA256_DIGEST_SIZE]);

/* Hash @length bytes in a single call. */
void sha256(const void *data, size_t length, uint8_t digest[SHA256_DIGEST_SIZE]);

#endif // INCLUDE_SHA256_H_
