/*
 * broker.c — Implementación del broker principal
 */
#include "broker.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* ── Haversine ────────────────────────────────────────────────────── */
#define DEG2RAD(d) ((d) * 3.14159265358979323846 / 180.0)
#define EARTH_R_KM 6371.0

double haversine_km(double lat1, double lon1, double lat2, double lon2)
{
    double dlat = DEG2RAD(lat2 - lat1);
    double dlon = DEG2RAD(lon2 - lon1);
    double a    = sin(dlat / 2) * sin(dlat / 2)
                + cos(DEG2RAD(lat1)) * cos(DEG2RAD(lat2))
                * sin(dlon / 2) * sin(dlon / 2);
    return EARTH_R_KM * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

void broker_init(broker_t *b, const broker_config_t *cfg)
{
    memset(b, 0, sizeof(*b));
    if (cfg) b->config = *cfg;
    else {
        /* Defaults */
        b->config.max_clients      = 1024;
        b->config.max_sources      = 128;
        b->config.client_timeout_s = 60;
        b->config.source_timeout_s = 30;
    }
    if (b->config.handshake_timeout_s <= 0)
        b->config.handshake_timeout_s = 10;

    mp_registry_init(&b->mounts);
    pthread_mutex_init(&b->conns_lock, NULL);
    b->conns_head = NULL;

    printf("[broker] initialized  max_clients=%d max_sources=%d\n",
           b->config.max_clients, b->config.max_sources);
}

void broker_destroy(broker_t *b)
{
    mp_registry_destroy(&b->mounts);
    pthread_mutex_destroy(&b->conns_lock);
    printf("[broker] destroyed\n");
}

/* ── Conexiones ───────────────────────────────────────────────────── */

conn_t *broker_conn_alloc(broker_t *b, int fd, const char *remote_addr)
{
    conn_t *c = calloc(1, sizeof(conn_t));
    if (!c) return NULL;

    c->fd           = fd;
    c->type         = CONN_TYPE_UNKNOWN;
    c->state        = CONN_STATE_NEW;
    c->connected_at = time(NULL);
    c->last_active  = c->connected_at;

    if (remote_addr)
        strncpy(c->remote_addr, remote_addr, sizeof(c->remote_addr) - 1);

    /* Enlazar en la lista global (sweep de timeouts) */
    pthread_mutex_lock(&b->conns_lock);
    c->gprev = NULL;
    c->gnext = b->conns_head;
    if (b->conns_head) b->conns_head->gprev = c;
    b->conns_head = c;
    pthread_mutex_unlock(&b->conns_lock);

    atomic_fetch_add(&b->total_connections, 1);

    printf("[broker] conn_alloc fd=%d addr=%s\n", fd, c->remote_addr);
    return c;
}

void broker_conn_free(broker_t *b, conn_t *conn)
{
    if (!conn) return;

    /* Desenlazar de la lista global ANTES de liberar — el sweep de
     * timeouts recorre esta lista bajo conns_lock */
    pthread_mutex_lock(&b->conns_lock);
    if (conn->gprev) conn->gprev->gnext = conn->gnext;
    else             b->conns_head      = conn->gnext;
    if (conn->gnext) conn->gnext->gprev = conn->gprev;
    conn->gnext = conn->gprev = NULL;
    pthread_mutex_unlock(&b->conns_lock);

    /* Desuscribir/detachar si aún está en un mountpoint */
    if (conn->mp) {
        if (conn->type == CONN_TYPE_SOURCE) {
            mp_source_detach(conn->mp, conn);
            atomic_fetch_sub(&b->active_sources, 1);
        } else if (conn->type == CONN_TYPE_CLIENT) {
            mp_client_unsubscribe(conn->mp, conn);
            atomic_fetch_sub(&b->active_clients, 1);
        }
    }

    printf("[broker] conn_free fd=%d type=%s  rx=%llu tx=%llu bytes\n",
           conn->fd, conn_type_name(conn->type),
           (unsigned long long)conn->bytes_rx,
           (unsigned long long)conn->bytes_tx);

    free(conn);
}

/* ── Registro después de handshake ────────────────────────────────── */

int broker_source_register(broker_t *b, conn_t *conn, const char *mountpoint_name)
{
    /* Enforce real de max_sources (antes era solo decorativo en el log —
     * FEATURE_relay_capacity_reload §1.2 #1) */
    if (atomic_load(&b->active_sources) >= b->config.max_sources) {
        fprintf(stderr, "[broker] max_sources (%d) alcanzado, rechazando\n",
                b->config.max_sources);
        return -1;
    }

    mountpoint_t *mp = mp_get_or_create(&b->mounts, mountpoint_name);
    if (!mp) {
        fprintf(stderr, "[broker] mount limit reached, rejecting source\n");
        return -1;
    }

    if (mp_source_attach(mp, conn) != 0) {
        fprintf(stderr, "[broker] mount '%s' already has a source\n",
                mountpoint_name);
        return -1;
    }

    conn->type  = CONN_TYPE_SOURCE;
    conn->state = CONN_STATE_SOURCE_ACTIVE;
    strncpy(conn->mountpoint, mountpoint_name, sizeof(conn->mountpoint) - 1);

    atomic_fetch_add(&b->active_sources, 1);
    return 0;
}

int broker_client_register(broker_t *b, conn_t *conn, const char *mountpoint_name)
{
    /* Enforce real de max_clients (FEATURE_relay_capacity_reload §1.2 #1) */
    if (atomic_load(&b->active_clients) >= b->config.max_clients) {
        fprintf(stderr, "[broker] max_clients (%d) alcanzado, rechazando\n",
                b->config.max_clients);
        return -1;
    }

    mountpoint_t *mp = mp_find(&b->mounts, mountpoint_name);
    if (!mp || !mp->active) {
        fprintf(stderr, "[broker] mount '%s' not found or no source\n",
                mountpoint_name);
        return -1;
    }

    if (mp_client_subscribe(mp, conn) != 0) {
        fprintf(stderr, "[broker] mount '%s' client limit reached\n",
                mountpoint_name);
        return -1;
    }

    conn->type  = CONN_TYPE_CLIENT;
    conn->state = CONN_STATE_CLIENT_ACTIVE;
    strncpy(conn->mountpoint, mountpoint_name, sizeof(conn->mountpoint) - 1);

    atomic_fetch_add(&b->active_clients, 1);
    return 0;
}

/* ── Relay ────────────────────────────────────────────────────────── */

size_t broker_source_data(broker_t *b, conn_t *source,
                           const uint8_t *data, size_t len)
{
    (void)b;
    if (!source->mp || !data || len == 0) return 0;

    source->bytes_rx   += len;
    source->last_active = time(NULL);

    size_t written = mp_relay(source->mp, data, len);
    atomic_fetch_add(&b->total_bytes_relayed, written);
    return written;
}

/*
 * broker_client_fill — Lee del ring buffer del mountpoint y llena el
 * write_buf del cliente. El io_engine llama esto cuando hay datos nuevos
 * (lo detecta porque write_pos del ring cambió, o en EPOLLOUT).
 *
 * Retorna:
 *   > 0  bytes copiados al write_buf
 *     0  sin datos nuevos
 *    -1  cliente demasiado lento (lag > RING_SIZE) → desconectar
 */
ssize_t broker_client_fill(broker_t *b, conn_t *client)
{
    (void)b;
    if (!client->mp) return 0;

    /* Espacio disponible en el write_buf del cliente */
    int space = conn_wbuf_avail(client);
    if (space <= 0) return 0;   /* write_buf lleno, esperar EPOLLOUT */

    /* Usar el write_buf como destino directo (evitar copia extra) */
    uint32_t tail  = client->wbuf.tail & (CONN_WRITE_BUF_SIZE - 1);
    size_t   chunk = (size_t)space;

    /* Si el write_buf circularmente cruza el final, limitar al tramo hasta el fin */
    if (tail + chunk > CONN_WRITE_BUF_SIZE)
        chunk = CONN_WRITE_BUF_SIZE - tail;

    ssize_t n = rb_read(&client->mp->ring, &client->read_offset,
                        client->wbuf.data + tail, chunk);

    if (n > 0) {
        client->wbuf.tail += (uint32_t)n;
        client->bytes_tx  += (uint64_t)n;
        client->last_active = time(NULL);
    }

    return n;
}

/* ── NEAREST ──────────────────────────────────────────────────────── */

const char *broker_nearest(broker_t *b, double lat, double lon)
{
    double        best_dist = 1e18;
    mountpoint_t *best      = NULL;
    int           n         = b->mounts.count;

    for (int i = 0; i < n; i++) {
        mountpoint_t *mp = &b->mounts.mounts[i];
        if (!mp->active || !mp->has_coords) continue;

        double d = haversine_km(lat, lon,
                                mp->coords.lat_deg,
                                mp->coords.lon_deg);
        if (d < best_dist) {
            best_dist = d;
            best      = mp;
        }
    }

    if (best) {
        printf("[broker] nearest to (%.4f,%.4f) → %s (%.1f km)\n",
               lat, lon, best->name, best_dist);
        return best->name;
    }
    return NULL;
}

/* ── Sourcetable ──────────────────────────────────────────────────── */

int broker_sourcetable_fill(broker_t *b,
                             sourcetable_entry_t *entries, int max)
{
    int  n = 0;
    int  total = b->mounts.count;

    for (int i = 0; i < total && n < max; i++) {
        mountpoint_t *mp = &b->mounts.mounts[i];
        sourcetable_entry_t *e = &entries[n];

        strncpy(e->name,       mp->name,       sizeof(e->name) - 1);
        strncpy(e->identifier, mp->identifier, sizeof(e->identifier) - 1);
        strncpy(e->format,     mp->format,     sizeof(e->format) - 1);
        strncpy(e->nav_system, mp->nav_system, sizeof(e->nav_system) - 1);

        e->lat     = mp->has_coords ? mp->coords.lat_deg : 0.0;
        e->lon     = mp->has_coords ? mp->coords.lon_deg : 0.0;
        e->clients = mp->client_count;
        e->active  = mp->active;
        e->nmea    = mp->nmea;

        n++;
    }
    return n;
}
