/*
 * pwhash.c — HMAC-SHA256 + PBKDF2-HMAC-SHA256 (dkLen == 32 == hLen, así
 * que el cálculo es siempre de UN solo bloque F(P,S,c,1) -- ver RFC 8018
 * §5.2). Construido sobre vendor/sha256, sin más dependencias.
 *
 * Ver pwhash.h para el contrato público y el motivo de cada decisión.
 */
#include "pwhash.h"
#include "sha256.h"   /* vendor/sha256 -- CMake ya agrega su include dir */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define HASH_SIZE  SHA256_DIGEST_SIZE   /* 32 */
#define SALT_BYTES 16
#define PREFIX     "pbkdf2-sha256$"

/* ── HMAC-SHA256 (RFC 2104) ──────────────────────────────────────────── */

static void hmac_sha256(const uint8_t *key, size_t key_len,
                         const uint8_t *msg, size_t msg_len,
                         uint8_t out[HASH_SIZE])
{
    uint8_t key_block[SHA256_BLOCK_SIZE];
    memset(key_block, 0, sizeof(key_block));

    if (key_len > SHA256_BLOCK_SIZE) {
        /* Llave más larga que un bloque: se reemplaza por su hash (el
         * resto del bloque queda en 0, ya seteado arriba). */
        sha256(key, key_len, key_block);
    } else {
        memcpy(key_block, key, key_len);
    }

    uint8_t ipad[SHA256_BLOCK_SIZE], opad[SHA256_BLOCK_SIZE];
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
        ipad[i] = (uint8_t)(key_block[i] ^ 0x36);
        opad[i] = (uint8_t)(key_block[i] ^ 0x5c);
    }

    sha256_ctx_t ctx;
    uint8_t inner[HASH_SIZE];
    sha256_init(&ctx);
    sha256_update(&ctx, ipad, sizeof(ipad));
    sha256_update(&ctx, msg, msg_len);
    sha256_final(&ctx, inner);

    sha256_init(&ctx);
    sha256_update(&ctx, opad, sizeof(opad));
    sha256_update(&ctx, inner, sizeof(inner));
    sha256_final(&ctx, out);
}

/* ── PBKDF2-HMAC-SHA256, un solo bloque (dkLen == HASH_SIZE) ─────────── */

static void pbkdf2_sha256_block1(const char *password, size_t password_len,
                                  const uint8_t *salt, size_t salt_len,
                                  unsigned int iterations,
                                  uint8_t out[HASH_SIZE])
{
    /* U1 = HMAC(password, salt || INT_32_BE(1)).
     * salt_len está acotado por parse_stored (<= sizeof(p->salt) == 64),
     * así que 64 + 4 entra sobrado en un buffer de 128. */
    uint8_t buf[128];
    memcpy(buf, salt, salt_len);
    buf[salt_len + 0] = 0x00;
    buf[salt_len + 1] = 0x00;
    buf[salt_len + 2] = 0x00;
    buf[salt_len + 3] = 0x01;

    uint8_t u[HASH_SIZE], t[HASH_SIZE];
    hmac_sha256((const uint8_t *)password, password_len, buf, salt_len + 4, u);
    memcpy(t, u, HASH_SIZE);

    /* T = U1 xor U2 xor ... xor U_iterations. Ya calculamos U1 arriba,
     * así que el loop genera U2..U_iterations (iterations-1 vueltas). */
    for (unsigned int i = 1; i < iterations; i++) {
        uint8_t u_next[HASH_SIZE];
        hmac_sha256((const uint8_t *)password, password_len, u, HASH_SIZE, u_next);
        memcpy(u, u_next, HASH_SIZE);
        for (int j = 0; j < HASH_SIZE; j++) t[j] = (uint8_t)(t[j] ^ u[j]);
    }

    memcpy(out, t, HASH_SIZE);
}

/* ── hex helpers ──────────────────────────────────────────────────────── */

static void to_hex(const uint8_t *data, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = digits[(data[i] >> 4) & 0xf];
        out[i * 2 + 1] = digits[data[i] & 0xf];
    }
    out[len * 2] = '\0';
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int from_hex(const char *hex, size_t hex_len, uint8_t *out, size_t out_max)
{
    if (hex_len == 0 || (hex_len % 2) != 0 || (hex_len / 2) > out_max) return -1;
    for (size_t i = 0; i < hex_len / 2; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(hex_len / 2);
}

/* ── parseo del string serializado ───────────────────────────────────── */

typedef struct {
    unsigned int iterations;
    uint8_t      salt[64];
    size_t       salt_len;
    uint8_t      hash[HASH_SIZE];
    size_t       hash_len;
} parsed_hash_t;

static int parse_stored(const char *stored, parsed_hash_t *p)
{
    size_t prefix_len = sizeof(PREFIX) - 1;
    if (strncmp(stored, PREFIX, prefix_len) != 0) return -1;
    const char *rest = stored + prefix_len;

    char *end = NULL;
    long iter = strtol(rest, &end, 10);
    if (end == rest || *end != '$' || iter <= 0) return -1;
    p->iterations = (unsigned int)iter;

    const char *salt_start = end + 1;
    const char *dollar2 = strchr(salt_start, '$');
    if (!dollar2) return -1;
    size_t salt_hex_len = (size_t)(dollar2 - salt_start);
    int salt_len = from_hex(salt_start, salt_hex_len, p->salt, sizeof(p->salt));
    if (salt_len <= 0) return -1;
    p->salt_len = (size_t)salt_len;

    const char *hash_start = dollar2 + 1;
    size_t hash_hex_len = strlen(hash_start);
    int hash_len = from_hex(hash_start, hash_hex_len, p->hash, sizeof(p->hash));
    if (hash_len != HASH_SIZE) return -1;
    p->hash_len = (size_t)hash_len;

    return 0;
}

int pwhash_looks_valid(const char *stored)
{
    if (!stored) return 0;
    parsed_hash_t p;
    return parse_stored(stored, &p) == 0 ? 1 : 0;
}

int pwhash_create(const char *password, unsigned int iterations,
                   char *out, size_t out_max)
{
    if (!password || !out) return -1;
    if (iterations == 0) iterations = PWHASH_DEFAULT_ITERATIONS;

    uint8_t salt[SALT_BYTES];
    FILE *urandom = fopen("/dev/urandom", "rb");
    if (!urandom) return -1;
    size_t got = fread(salt, 1, sizeof(salt), urandom);
    fclose(urandom);
    if (got != sizeof(salt)) return -1;

    uint8_t hash[HASH_SIZE];
    pbkdf2_sha256_block1(password, strlen(password), salt, sizeof(salt),
                          iterations, hash);

    char salt_hex[SALT_BYTES * 2 + 1];
    char hash_hex[HASH_SIZE * 2 + 1];
    to_hex(salt, sizeof(salt), salt_hex);
    to_hex(hash, sizeof(hash), hash_hex);

    int n = snprintf(out, out_max, "%s%u$%s$%s",
                      PREFIX, iterations, salt_hex, hash_hex);
    if (n < 0 || (size_t)n >= out_max) return -1;
    return 0;
}

/* Comparación en tiempo constante: XOR acumulado sobre longitud FIJA
 * (HASH_SIZE), sin strcmp/memcmp de corte anticipado. `diff` es volatile
 * para que el compilador no la optimice a un memcmp de corte corto. */
static int constant_time_eq(const uint8_t *a, const uint8_t *b, size_t len)
{
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) diff = (uint8_t)(diff | (a[i] ^ b[i]));
    return diff == 0;
}

int pwhash_verify(const char *password, const char *stored)
{
    if (!password || !stored) return -1;

    parsed_hash_t p;
    if (parse_stored(stored, &p) != 0) return -1;

    uint8_t computed[HASH_SIZE];
    pbkdf2_sha256_block1(password, strlen(password), p.salt, p.salt_len,
                          p.iterations, computed);

    return constant_time_eq(computed, p.hash, HASH_SIZE) ? 0 : -1;
}
