/*
 * pwhash.h — PBKDF2-HMAC-SHA256 sobre vendor/sha256, formato serializado
 * "pbkdf2-sha256$<iteraciones>$<salt_hex>$<hash_hex>".
 *
 * Ver docs/FEATURE_registry_sqlite_dashboard.md §5 (decisión de diseño:
 * por qué PBKDF2 y no Argon2/bcrypt, por qué 100k+ iteraciones, por qué
 * comparación en tiempo constante) y docs/FEATURE_improvements_FASE_A_20260704.md
 * §D.1 (IMP-02).
 *
 * auth.c usa SOLO pwhash_verify() en el hot path del handshake. main.c usa
 * pwhash_create() en el modo `--hash-password` (CLI de un solo uso, nunca
 * en el hot path).
 */
#ifndef NTRIPCASTER_PWHASH_H
#define NTRIPCASTER_PWHASH_H

#include <stddef.h>

/* Tamaño máximo del string serializado. Cálculo real con salt=16B/hash=32B:
 * "pbkdf2-sha256$" (14) + iter (hasta 7 dígitos) + "$" + salt_hex (32) +
 * "$" + hash_hex (64) = 119. Dejamos margen para iteraciones de más dígitos. */
#define PWHASH_STRING_MAX 160

/* Iteraciones por defecto para hashes nuevos (--hash-password / migración
 * del conf real). Medido "suficiente" para credenciales de rovers según
 * el doc de diseño; no es un límite duro -- el string es autodescriptivo,
 * así que subir esto a futuro no rompe hashes ya generados. */
#define PWHASH_DEFAULT_ITERATIONS 100000u

/*
 * pwhash_create — Genera un hash NUEVO para `password`, con salt aleatorio
 * de 16 bytes leído de /dev/urandom. Si `iterations` es 0, usa
 * PWHASH_DEFAULT_ITERATIONS. Escribe el string serializado en `out`
 * (recomendado: buffer de al menos PWHASH_STRING_MAX bytes).
 *
 * Retorna 0 si OK, -1 si error (password/out nulos, no se pudo leer
 * /dev/urandom, o `out` demasiado chico).
 */
int pwhash_create(const char *password, unsigned int iterations,
                   char *out, size_t out_max);

/*
 * pwhash_verify — Verifica `password` en texto plano contra `stored`
 * (el string "pbkdf2-sha256$..." tal como viene del conf). Reconstruye
 * el hash con la misma sal/iteraciones leídas de `stored` y compara en
 * tiempo constante (XOR acumulado sobre longitud fija, nunca strcmp).
 *
 * Retorna 0 si coincide, -1 si NO coincide, o si `stored` no tiene el
 * formato esperado (fail closed: un conf sin migrar rechaza todo en vez
 * de comparar en texto plano por accidente).
 */
int pwhash_verify(const char *password, const char *stored);

/*
 * pwhash_looks_valid — 1 si `stored` tiene la forma
 * "pbkdf2-sha256$<iter>$<salt_hex>$<hash_hex>" con longitudes correctas,
 * 0 si no. Pensado para que auth.c loguee un error CLARO ("este mount
 * tiene una password sin hashear, corré --hash-password") en vez de un
 * rechazo silencioso indistinguible de una password incorrecta.
 */
int pwhash_looks_valid(const char *stored);

#endif /* NTRIPCASTER_PWHASH_H */
