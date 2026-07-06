/*
 * sha256.h — SHA-256 (FIPS 180-4), implementación de dominio público.
 *
 * Vendoreado siguiendo el mismo patrón que vendor/inih y vendor/uthash:
 * un solo .c/.h, sin dependencias, sin gestión externa del paquete.
 *
 * API mínima de streaming (init/update/final) + wrapper de una sola
 * llamada. Usado por src/protocol/pwhash.c para construir HMAC-SHA256
 * y PBKDF2-HMAC-SHA256 (ver FEATURE_registry_sqlite_dashboard.md §5 y
 * FEATURE_improvements_FASE_A_20260704.md §D.1 — decisión de hashing).
 */
#ifndef VENDOR_SHA256_H
#define VENDOR_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_BLOCK_SIZE  64
#define SHA256_DIGEST_SIZE 32

typedef struct {
    uint8_t  data[SHA256_BLOCK_SIZE];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t hash[SHA256_DIGEST_SIZE]);

/* Wrapper de una sola llamada: sha256(data, len) -> hash[32] */
void sha256(const uint8_t *data, size_t len, uint8_t hash[SHA256_DIGEST_SIZE]);

#endif /* VENDOR_SHA256_H */
