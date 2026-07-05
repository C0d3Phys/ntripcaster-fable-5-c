/*
 * ring_buffer.c — Implementación del buffer circular de broadcast
 */
#include "ring_buffer.h"
#include <string.h>

void rb_init(ring_buffer_t *rb)
{
    memset(rb->buf, 0, RING_SIZE);
    atomic_store(&rb->write_pos,   0);
    atomic_store(&rb->num_clients, 0);
}

/*
 * rb_write — Escribe datos al ring circular.
 *
 * Si len > RING_SIZE, solo escribe el último RING_SIZE bytes
 * (el source no puede escribir más de un buffer completo de una vez).
 *
 * El write es lineal dentro del buffer usando máscara:
 *   slot = write_pos & RING_MASK
 *
 * Si el dato cruza el final del buffer, se divide en dos memcpy.
 */
size_t rb_write(ring_buffer_t *rb, const uint8_t *data, size_t len)
{
    if (len == 0) return 0;

    /* Truncar si el dato es más grande que el buffer completo */
    if (len > RING_SIZE) {
        data += (len - RING_SIZE);
        len   = RING_SIZE;
    }

    /* Posición actual de escritura */
    uint64_t wp   = atomic_load_explicit(&rb->write_pos, memory_order_relaxed);
    uint32_t slot = (uint32_t)(wp & RING_MASK);
    uint32_t tail = RING_SIZE - slot;   /* bytes hasta el final del buffer */

    if (len <= tail) {
        /* Cabe en un solo memcpy */
        memcpy(rb->buf + slot, data, len);
    } else {
        /* Dividir en dos: hasta el final + desde el inicio */
        memcpy(rb->buf + slot, data,        tail);
        memcpy(rb->buf,        data + tail, len - tail);
    }

    /* Publicar la nueva posición — acquire/release para que los lectores
     * vean los datos escritos antes de ver el nuevo write_pos */
    atomic_store_explicit(&rb->write_pos, wp + len, memory_order_release);

    return len;
}

/*
 * rb_read — Lee datos disponibles para un cliente.
 *
 * No usa locks. El cliente tiene su propio `client_pos` no compartido.
 * El único riesgo es que el source sobrescriba datos que el cliente
 * no ha leído aún (lag > RING_SIZE) → retorna -1 → desconectar cliente.
 *
 * IMP-01B (fix torn-read): la versión anterior leía write_pos UNA sola
 * vez y copiaba en base a eso. Si el productor daba la vuelta completa
 * al ring DURANTE el memcpy (posible con RING_SIZE chico o un cliente
 * pausado que se despierta tarde), el lector podía copiar una mezcla
 * de bytes viejos y nuevos sin darse cuenta — corrupción silenciosa,
 * no un lag detectado.
 *
 * Fix: doble validación con reintento acotado. Se copia de forma
 * optimista y, después, se revisa si write_pos siguió perteneciendo a
 * la misma "vista" del ring (gap contra cp todavía <= RING_SIZE). Si
 * el productor pisó lo que se estaba copiando, se descarta el intento
 * y se reintenta una vez; si persiste, se declara lag (mismo desenlace
 * que un cliente que ya estaba al borde — nunca se entrega una mezcla).
 * El productor (rb_write) nunca espera: sigue siendo lock-free.
 */
ssize_t rb_read(const ring_buffer_t *rb, uint64_t *client_pos,
                uint8_t *out, size_t max_len)
{
    uint64_t cp = *client_pos;

    for (int intento = 0; intento < 2; intento++) {
        uint64_t wp1   = atomic_load_explicit(&rb->write_pos, memory_order_acquire);
        int64_t  avail = (int64_t)(wp1 - cp);

        if (avail <= 0)
            return 0;   /* Sin datos nuevos */

        if (avail > (int64_t)RING_SIZE)
            return -1;  /* Cliente demasiado atrás — desconectar */

        /* Cuánto leer en esta llamada */
        size_t to_read = (size_t)avail;
        if (to_read > max_len)
            to_read = max_len;

        uint32_t slot = (uint32_t)(cp & RING_MASK);
        uint32_t tail = RING_SIZE - slot;

        /* Copia optimista — puede quedar "torn" si el productor da la
         * vuelta mientras copiamos; se valida después. */
        if (to_read <= tail) {
            memcpy(out, rb->buf + slot, to_read);
        } else {
            memcpy(out,          rb->buf + slot, tail);
            memcpy(out + tail,   rb->buf,        to_read - tail);
        }

        uint64_t wp2 = atomic_load_explicit(&rb->write_pos, memory_order_acquire);
        if ((int64_t)(wp2 - cp) <= (int64_t)RING_SIZE) {
            /* Nadie pisó lo copiado: pertenece a una sola vista válida
             * del ring. Publicar avance y retornar. */
            *client_pos = cp + (uint64_t)to_read;
            return (ssize_t)to_read;
        }

        /* El productor dio la vuelta DURANTE la copia: lo copiado puede
         * estar mezclado. Descartar y reintentar (o declarar lag). */
    }

    return -1;
}
