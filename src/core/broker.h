/*
 * broker.h — Broker principal del NtripCaster
 *
 * El broker es el núcleo que conecta todos los componentes:
 *   - Mantiene el registro de mountpoints
 *   - Crea y destruye conexiones
 *   - Coordina el relay source → clientes
 *   - Expone datos para la sourcetable
 *   - Implementa NEAREST (cliente GGA → mountpoint más cercano)
 *
 * No hace I/O directamente — eso es trabajo del io_engine.
 * El broker es el "cerebro", el io_engine es los "músculos".
 *
 * Thread safety:
 *   Todas las funciones son thread-safe.
 *   El broker usa el locking interno de mountpoint_registry_t.
 */
#ifndef NTRIPCASTER_BROKER_H
#define NTRIPCASTER_BROKER_H

#include <stdint.h>
#include <stddef.h>
#include "conn.h"
#include "mountpoint.h"

/* ── Configuración del broker ────────────────────────────────────── */
typedef struct {
    int      max_clients;        /* default: 1024 — enforced en client_register */
    int      max_sources;        /* default: 128  — enforced en source_register */
    int      client_timeout_s;   /* inactivo por N segundos → kick */
    int      source_timeout_s;
    int      handshake_timeout_s;/* request incompleto por N segundos → kick */
    size_t   ring_size;          /* tamaño del ring buffer (ignorado si RING_SIZE está fijo) */
    char     admin_password[64];

    /* Identidad del caster — sourcetable (CAS/NET) + header Server.
     * Viene de la sección [caster] del conf (ver core/config.h). */
    int      port;               /* puerto de escucha (para la CAS line) */
    char     caster_name[64];
    char     caster_operator[64];
    char     caster_country[8];
} broker_config_t;

/* ── Estado global del broker ────────────────────────────────────── */
typedef struct broker_t {
    broker_config_t      config;
    mountpoint_registry_t mounts;

    /* Lista global de conexiones vivas (sweep de timeouts).
     * Protegida por conns_lock; alloc/free enlazan/desenlazan. */
    conn_t              *conns_head;
    pthread_mutex_t      conns_lock;

    /* Estadísticas globales */
    _Atomic uint64_t     total_connections;
    _Atomic uint64_t     total_bytes_relayed;
    _Atomic int          active_sources;
    _Atomic int          active_clients;

} broker_t;

/* ── Lifecycle ───────────────────────────────────────────────────── */

void broker_init(broker_t *b, const broker_config_t *cfg);
void broker_destroy(broker_t *b);

/* ── Gestión de conexiones ───────────────────────────────────────── */

/*
 * broker_conn_alloc — Crea una nueva conexión para fd.
 * Inicializa todos los campos, establece remote_addr.
 * Retorna NULL si OOM.
 */
conn_t *broker_conn_alloc(broker_t *b, int fd,
                           const char *remote_addr);

/*
 * broker_conn_free — Libera una conexión ya cerrada.
 * Desuscribe/detacha del mountpoint si aún está suscrita.
 * NO cierra el fd — eso lo hace el io_engine antes de llamar aquí.
 */
void broker_conn_free(broker_t *b, conn_t *conn);

/* ── Handshake completado — registrar conexión ───────────────────── */

/*
 * broker_source_register — Llamado por el protocol handler cuando un source
 * completó el handshake. Attach al mountpoint.
 * Retorna 0 si OK, -1 si el mountpoint ya tiene source.
 */
int broker_source_register(broker_t *b, conn_t *conn, const char *mountpoint);

/*
 * broker_client_register — Llamado por el protocol handler cuando un cliente
 * completó el handshake. Subscribe al mountpoint.
 * Retorna 0 si OK, -1 si mountpoint no existe / lleno.
 */
int broker_client_register(broker_t *b, conn_t *conn, const char *mountpoint);

/* ── Relay ───────────────────────────────────────────────────────── */

/*
 * broker_source_data — Llamado por io_engine cuando hay datos del source.
 * Escribe al ring + decode RTCM3 para coords/logging.
 * Retorna bytes procesados.
 */
size_t broker_source_data(broker_t *b, conn_t *source,
                           const uint8_t *data, size_t len);

/*
 * broker_client_fill — Llamado por io_engine cuando el socket del cliente
 * está listo para escritura (EPOLLOUT).
 * Lee del ring → escribe al write_buf del cliente.
 * Retorna bytes copiados al write_buf, 0 si al día, -1 si lag excesivo.
 */
ssize_t broker_client_fill(broker_t *b, conn_t *client);

/* ── NEAREST ─────────────────────────────────────────────────────── */

/*
 * broker_nearest — Dado lat/lon del rover (de su GGA NMEA),
 * retorna el nombre del mountpoint más cercano con source activo.
 * Usa Haversine. Retorna NULL si no hay ninguno activo.
 */
const char *broker_nearest(broker_t *b, double lat, double lon);

/* ── Sourcetable ─────────────────────────────────────────────────── */

/*
 * broker_sourcetable_fill — Llena entries[] con los mountpoints activos.
 * Retorna número de entradas escritas.
 * (Las entries las formatea sourcetable.c en text/plain o html)
 */
typedef struct {
    char   name[64];
    char   identifier[64];
    char   format[16];
    char   nav_system[32];
    double lat, lon;
    int    clients;
    int    active;          /* 1 = tiene source */
    int    nmea;
} sourcetable_entry_t;

int broker_sourcetable_fill(broker_t *b,
                             sourcetable_entry_t *entries, int max);

/* ── Haversine (distancia entre dos coords) ──────────────────────── */
double haversine_km(double lat1, double lon1, double lat2, double lon2);

#endif /* NTRIPCASTER_BROKER_H */
