/*
 * ring_buffer.h — Buffer circular de broadcast para relay RTCM3
 *
 * Diseño: SPMC (Single Producer, Multiple Consumer)
 *   - UN source escribe (write_pos atómico)
 *   - N clientes leen, cada uno con su propio read_offset
 *   - Zero-copy: clientes leen directo del buffer del mountpoint
 *   - Sin locks en el path de lectura
 *
 * Si un cliente queda demasiado atrás (lag > RING_SIZE) se desconecta
 * automáticamente — el caster no bloquea al source por un rover lento.
 *
 * Tamaño: potencia de 2 para que (pos & RING_MASK) == (pos % RING_SIZE)
 *   256 KB / ~14 KB/s (MSM7 típico a 115 kbps) ≈ 18 segundos de buffer.
 */
#ifndef NTRIPCASTER_RING_BUFFER_H
#define NTRIPCASTER_RING_BUFFER_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>
#include <sys/types.h>   /* ssize_t */

/* ── Configuración ───────────────────────────────────────────────── */
#define RING_SIZE  (256u * 1024u)   /* 256 KB, DEBE ser potencia de 2 */
#define RING_MASK  (RING_SIZE - 1u)

_Static_assert((RING_SIZE & RING_MASK) == 0,
               "RING_SIZE must be a power of 2");

/* ── Estructura ──────────────────────────────────────────────────── */
typedef struct {
    uint8_t              buf[RING_SIZE];
    _Atomic uint64_t     write_pos;   /* bytes totales escritos (monotónico) */
    _Atomic uint32_t     num_clients; /* clientes suscritos ahora */
} ring_buffer_t;

/* ── API ─────────────────────────────────────────────────────────── */

/*
 * rb_init — Inicializa el ring buffer a cero.
 */
void rb_init(ring_buffer_t *rb);

/*
 * rb_write — Escribe `len` bytes al ring buffer.
 * Solo debe ser llamado por UN thread (el handler del source).
 * Retorna bytes escritos (siempre == len si len <= RING_SIZE).
 */
size_t rb_write(ring_buffer_t *rb, const uint8_t *data, size_t len);

/*
 * rb_read — Lee datos para un cliente desde su read_offset.
 *
 * @rb          Ring buffer del mountpoint
 * @client_pos  [in/out] Posición de lectura del cliente (se avanza)
 * @out         Buffer destino
 * @max_len     Máximo a leer en esta llamada
 *
 * Retorna:
 *   > 0  bytes copiados a out; *client_pos avanzado
 *     0  no hay datos nuevos (cliente al día)
 *    -1  cliente demasiado lento — lag > RING_SIZE → desconectar
 */
ssize_t rb_read(const ring_buffer_t *rb, uint64_t *client_pos,
                uint8_t *out, size_t max_len);

/*
 * rb_bytes_available — Cuántos bytes hay disponibles para este cliente.
 * Retorna < 0 si el cliente está demasiado atrás.
 */
static inline int64_t rb_bytes_available(const ring_buffer_t *rb,
                                          uint64_t client_pos)
{
    uint64_t wp = atomic_load_explicit(&rb->write_pos, memory_order_acquire);
    int64_t  avail = (int64_t)(wp - client_pos);
    if (avail > (int64_t)RING_SIZE) return -(int64_t)RING_SIZE; /* lag */
    return avail;
}

/*
 * rb_write_pos — Retorna la posición de escritura actual.
 * Usar al suscribir un cliente nuevo para inicializar su read_offset.
 */
static inline uint64_t rb_write_pos(const ring_buffer_t *rb)
{
    return atomic_load_explicit(&rb->write_pos, memory_order_acquire);
}

#endif /* NTRIPCASTER_RING_BUFFER_H */
