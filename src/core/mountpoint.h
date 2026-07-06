/*
 * mountpoint.h — Registro y gestión de mountpoints del caster
 *
 * Un mountpoint es un stream GNSS identificado por nombre (ej: "Demo1").
 * Puede tener:
 *   - 0 o 1 source activo (el receptor GNSS que empuja datos)
 *   - 0..N clientes suscritos (rovers recibiendo datos)
 *
 * El mountpoint es la unidad de relay:
 *   source → ring_buffer → [cliente1, cliente2, ..., clienteN]
 *
 * Concurrencia:
 *   - `lock` (rwlock) protege source y la lista de clientes
 *   - El ring_buffer usa atomics — no necesita lock para leer
 *   - Solo la modificación de la lista (sub/unsub) necesita write lock
 */
#ifndef NTRIPCASTER_MOUNTPOINT_H
#define NTRIPCASTER_MOUNTPOINT_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include "ring_buffer.h"
#include "conn.h"
#include "../gnss/rtcm3.h"

/* ── Configuración ───────────────────────────────────────────────── */
#define MOUNTPOINT_NAME_MAX  64
#define MOUNTPOINT_MAX       128   /* máximo de mountpoints por caster */
#define CLIENT_MAX_PER_MOUNT 256

/* ── Mountpoint ──────────────────────────────────────────────────── */
typedef struct mountpoint_t {
    char     name[MOUNTPOINT_NAME_MAX];
    int      active;    /* 1 = tiene source conectado */

    /* Source actual (solo uno) */
    conn_t  *source;

    /* Lista doblemente enlazada de clientes.
     * client_count es atómico (IMP-01D): se escribe bajo wrlock en
     * mp_client_subscribe/unsubscribe, pero se LEE sin lock desde
     * report_mount_stats() y broker_sourcetable_fill() en otro thread. */
    conn_t  *clients_head;
    _Atomic int client_count;

    /* Ring buffer de broadcast — source escribe, clientes leen */
    ring_buffer_t ring;

    /* Metadatos GNSS extraídos del stream (de 1005/1006) */
    rtcm3_coords_t   coords;
    rtcm3_receiver_t receiver;
    int              has_coords;
    int              has_receiver;

    /* Sourcetable info (de config + del stream) */
    char     identifier[64];
    char     format[16];        /* "RTCM 3.3" */
    char     format_details[32];/* "1004(1),1012(1),1074(1)..." */
    char     nav_system[32];    /* "GPS+GLO+GAL+BDS" */
    int      carrier;           /* 0/1/2 */
    int      nmea;              /* 1 = acepta GGA para NEAREST */

    /* Estadísticas (IMP-01D: atómicas -- se escriben desde el thread
     * worker del source en mp_relay() y se LEEN desde el accept thread
     * en report_mount_stats() cada ~30s, sin ningún lock en el medio.
     * Antes eran uint64_t planos: carrera de datos real bajo el
     * standard de C, aunque en la práctica x86-64 rara vez se viera un
     * torn read de 64 bits. _Atomic con memory_order_relaxed alcanza --
     * no hay ordenamiento entre estos contadores y otra cosa, solo
     * necesitamos que cada load/store individual sea atómico). */
    _Atomic uint64_t bytes_relayed;
    _Atomic uint64_t frames_relayed;
    time_t   source_connected_at;

    /* ── Decode incremental del stream (integridad, punto debug) ──
     * Los frames RTCM3 llegan partidos entre read()s del socket; sin
     * reensamblar, el decode per-chunk pierde frames y reporta basura
     * que no existe. dec_buf acumula el tail incompleto entre chunks.
     * dec_buf/dec_len NO son atómicos a propósito: solo los toca el
     * thread del source dentro de mp_relay(), nunca el reporte. */
#define MP_DEC_BUF_SIZE   8192
#define MP_MSG_STATS_MAX  24
    uint8_t  dec_buf[MP_DEC_BUF_SIZE];
    uint32_t dec_len;
    _Atomic uint64_t dec_frames_ok;      /* frames con CRC válido */
    _Atomic uint64_t dec_bytes_skipped;  /* bytes descartados: preamble falso/CRC malo */

    /* msg_stats[]/msg_stats_n: tabla chica de tamaño variable (conteo
     * por tipo de mensaje RTCM3) -- no se presta bien a atomics por
     * campo (agregar un tipo nuevo es "leer N, comparar, quizás
     * escribir en N+1", no una sola palabra). Se protege con `lock`
     * (el mismo rwlock de source/clients): mp_count_msg_type() toma
     * wrlock, report_mount_stats() toma rdlock para leerla. El costo
     * es un lock/unlock por FRAME decodeado -- barato frente a la
     * frecuencia real de mensajes RTCM3 (unos pocos Hz por tipo). */
    struct { uint16_t type; uint32_t count; } msg_stats[MP_MSG_STATS_MAX];
    int      msg_stats_n;

    /* snapshot para el reporte periódico (deltas cada ~30s) -- SOLO
     * los toca report_mount_stats(), siempre el mismo thread (accept
     * thread), así que no necesitan ser atómicos. */
    uint64_t rpt_bytes;
    uint64_t rpt_frames;
    uint64_t rpt_skipped;

    /* Lock — protege source, clients_head y msg_stats/msg_stats_n */
    pthread_rwlock_t lock;

} mountpoint_t;

/* ── Registro global de mountpoints ─────────────────────────────── */
typedef struct {
    mountpoint_t mounts[MOUNTPOINT_MAX];
    int          count;
    pthread_mutex_t lock;   /* protege count y creación de nuevos mounts */
} mountpoint_registry_t;

/* ── API ─────────────────────────────────────────────────────────── */

/* Inicializar/destruir el registro */
void mp_registry_init(mountpoint_registry_t *reg);
void mp_registry_destroy(mountpoint_registry_t *reg);

/* Buscar un mountpoint por nombre (retorna NULL si no existe) */
mountpoint_t *mp_find(mountpoint_registry_t *reg, const char *name);

/* Obtener o crear un mountpoint por nombre */
mountpoint_t *mp_get_or_create(mountpoint_registry_t *reg, const char *name);

/*
 * mp_source_attach — Registra un source en el mountpoint.
 * Retorna 0 si OK, -1 si ya hay un source activo.
 */
int mp_source_attach(mountpoint_t *mp, conn_t *source);

/*
 * mp_source_detach — Desconecta el source del mountpoint.
 * No cierra el fd — eso lo hace el io_engine.
 */
void mp_source_detach(mountpoint_t *mp, conn_t *source);

/*
 * mp_client_subscribe — Suscribe un cliente al mountpoint.
 * Inicializa conn->read_offset al write_pos actual del ring.
 * Retorna 0 si OK, -1 si mountpoint lleno.
 */
int mp_client_subscribe(mountpoint_t *mp, conn_t *client);

/*
 * mp_client_unsubscribe — Elimina un cliente de la lista.
 */
void mp_client_unsubscribe(mountpoint_t *mp, conn_t *client);

/*
 * mp_relay — Escribe datos del source al ring y prepara cola de escritura
 * para todos los clientes activos.
 *
 * Llama al decoder RTCM3 en cada frame para extraer coords/logging.
 * Retorna bytes escritos al ring.
 */
size_t mp_relay(mountpoint_t *mp, const uint8_t *data, size_t len);

/*
 * mp_update_coords — Actualiza coords del mountpoint desde un frame 1005/1006.
 * Llamado por el decoder RTCM3 durante mp_relay.
 */
void mp_update_coords(mountpoint_t *mp, const rtcm3_coords_t *coords);

/*
 * mp_update_receiver — Actualiza info del receptor desde un frame 1033.
 */
void mp_update_receiver(mountpoint_t *mp, const rtcm3_receiver_t *rx);

#endif /* NTRIPCASTER_MOUNTPOINT_H */
