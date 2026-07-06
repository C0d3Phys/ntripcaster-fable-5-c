/*
 * conn.h — Tipos centrales de conexión del NtripCaster
 *
 * Una conexión puede ser:
 *   SOURCE: receptor GNSS que empuja datos RTCM3 a un mountpoint
 *   CLIENT: rover que consume datos de un mountpoint
 *
 * Estado de la máquina de estados:
 *
 *   NEW ──► HANDSHAKE ──► SOURCE_ACTIVE ──► CLOSING ──► CLOSED
 *                    └──► CLIENT_ACTIVE ──►
 *                    └──► NEAREST_WAIT  ──► CLIENT_ACTIVE
 *
 * NEAREST_WAIT: cliente envió GGA, esperamos resolver el mountpoint más cercano
 */
#ifndef NTRIPCASTER_CONN_H
#define NTRIPCASTER_CONN_H

#include <stdint.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <time.h>

/* ── Forward declarations ───────────────────────────────────────── */
struct mountpoint_t;

/* ── Tipo de conexión ───────────────────────────────────────────── */
typedef enum {
    CONN_TYPE_UNKNOWN = 0,
    CONN_TYPE_SOURCE,       /* Receptor GNSS → pushea RTCM3 */
    CONN_TYPE_CLIENT,       /* Rover → consume RTCM3 */
} conn_type_t;

/* ── Estado de la conexión ──────────────────────────────────────── */
typedef enum {
    CONN_STATE_NEW = 0,
    CONN_STATE_HANDSHAKE,       /* Leyendo/parseando request HTTP/NTRIP */
    CONN_STATE_NEAREST_WAIT,    /* Cliente NEAREST: esperando GGA NMEA */
    CONN_STATE_SOURCE_ACTIVE,   /* Source autenticado, empujando datos */
    CONN_STATE_CLIENT_ACTIVE,   /* Cliente suscrito, recibiendo datos */
    CONN_STATE_CLOSING,         /* Enviando cierre, vaciando buffer */
    CONN_STATE_CLOSED,          /* Cerrado, listo para free */
} conn_state_t;

/* ── Versión del protocolo NTRIP ────────────────────────────────── */
typedef enum {
    NTRIP_VERSION_UNKNOWN = 0,
    NTRIP_VERSION_1,
    NTRIP_VERSION_2,
} ntrip_version_t;

/* ── Buffer de lectura/escritura por conexión ───────────────────── */
#define CONN_READ_BUF_SIZE   (16 * 1024)   /* 16 KB — suficiente para handshake */
#define CONN_WRITE_BUF_SIZE  (64 * 1024)   /* 64 KB — cola de escritura al cliente */

typedef struct {
    uint8_t  data[CONN_WRITE_BUF_SIZE];
    uint32_t head;   /* próximo byte a enviar */
    uint32_t tail;   /* próximo byte libre */
} write_buf_t;

/* ── Conexión ───────────────────────────────────────────────────── */
typedef struct conn_t {
    /* Descriptor y estado */
    int              fd;
    conn_type_t      type;
    conn_state_t     state;
    ntrip_version_t  ntrip_version;

    /* Mountpoint al que pertenece (NULL hasta handshake completo) */
    struct mountpoint_t *mp;

    /* Para clientes: offset de lectura en el ring buffer del mountpoint.
     * Monotonically increasing byte counter.
     * Inicializado al write_pos del momento de suscripción. */
    uint64_t         read_offset;

    /* Buffers */
    uint8_t          read_buf[CONN_READ_BUF_SIZE];
    uint32_t         read_len;   /* bytes válidos en read_buf */
    write_buf_t      wbuf;       /* cola de escritura pendiente */

    /* Identificación */
    char             remote_addr[48];  /* "192.168.1.1:34521" */
    char             mountpoint[64];   /* nombre del mountpoint solicitado */
    char             user[64];

    /* Timestamps */
    time_t           connected_at;
    time_t           last_active;

    /* Estadísticas.
     *
     * IMP-01D: bytes_tx se incrementaba DOS VECES por el mismo byte --
     * una en broker_client_fill() al copiar del ring al write_buf
     * ("encolado para enviar"), y otra en flush_write_buf() al hacer el
     * write() real al socket ("transmitido de verdad"). Todo "tx=" que
     * se veía en los logs (broker_conn_free, io_engine) era ~2x el
     * tráfico real. Separados:
     *   bytes_queued -- bytes copiados del ring al write_buf del cliente
     *                   (cuánto se preparó para enviar; incluye lo que
     *                   todavía puede estar pendiente en el buffer).
     *   bytes_tx     -- bytes que write(2) confirmó haber escrito al fd
     *                   (el número real de "transmitido"). ÚNICA fuente
     *                   de verdad para reportar tráfico saliente. */
    uint64_t         bytes_rx;
    uint64_t         bytes_tx;
    uint64_t         bytes_queued;

    /* Para la lista de clientes del mountpoint (intrusive linked list) */
    struct conn_t   *next;
    struct conn_t   *prev;

    /* Lista global de conexiones del broker (sweep de timeouts) */
    struct conn_t   *gnext;
    struct conn_t   *gprev;

} conn_t;

/* ── Helpers inline ─────────────────────────────────────────────── */

static inline int conn_wbuf_avail(const conn_t *c)
{
    return (int)(CONN_WRITE_BUF_SIZE - (c->wbuf.tail - c->wbuf.head));
}

static inline int conn_wbuf_pending(const conn_t *c)
{
    return (int)(c->wbuf.tail - c->wbuf.head);
}

/* Retorna el nombre del estado para logging */
static inline const char *conn_state_name(conn_state_t s)
{
    static const char *names[] = {
        "new", "handshake", "nearest_wait",
        "source_active", "client_active", "closing", "closed"
    };
    return (s <= CONN_STATE_CLOSED) ? names[s] : "?";
}

static inline const char *conn_type_name(conn_type_t t)
{
    static const char *names[] = { "unknown", "source", "client" };
    return (t <= CONN_TYPE_CLIENT) ? names[t] : "?";
}

#endif /* NTRIPCASTER_CONN_H */
