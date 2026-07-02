/*
 * mountpoint.c — Gestión de mountpoints y relay de datos
 */
#include "mountpoint.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ── Buffer temporal para decode RTCM3 durante relay ─────────────── */
#define RELAY_FRAME_MAX 256

/* ── Registro ─────────────────────────────────────────────────────── */

void mp_registry_init(mountpoint_registry_t *reg)
{
    memset(reg, 0, sizeof(*reg));
    pthread_mutex_init(&reg->lock, NULL);
}

void mp_registry_destroy(mountpoint_registry_t *reg)
{
    pthread_mutex_lock(&reg->lock);
    for (int i = 0; i < reg->count; i++) {
        pthread_rwlock_destroy(&reg->mounts[i].lock);
    }
    pthread_mutex_unlock(&reg->lock);
    pthread_mutex_destroy(&reg->lock);
}

mountpoint_t *mp_find(mountpoint_registry_t *reg, const char *name)
{
    /* Lock-free read — count solo crece, nunca decrece en runtime */
    int n = reg->count;
    for (int i = 0; i < n; i++) {
        if (strncmp(reg->mounts[i].name, name, MOUNTPOINT_NAME_MAX - 1) == 0)
            return &reg->mounts[i];
    }
    return NULL;
}

mountpoint_t *mp_get_or_create(mountpoint_registry_t *reg, const char *name)
{
    /* Primero buscar sin lock */
    mountpoint_t *mp = mp_find(reg, name);
    if (mp) return mp;

    /* Crear con lock */
    pthread_mutex_lock(&reg->lock);

    /* Double-check: puede que otro thread lo haya creado */
    mp = mp_find(reg, name);
    if (mp) {
        pthread_mutex_unlock(&reg->lock);
        return mp;
    }

    if (reg->count >= MOUNTPOINT_MAX) {
        pthread_mutex_unlock(&reg->lock);
        return NULL;
    }

    mp = &reg->mounts[reg->count];
    memset(mp, 0, sizeof(*mp));
    strncpy(mp->name, name, MOUNTPOINT_NAME_MAX - 1);
    rb_init(&mp->ring);
    pthread_rwlock_init(&mp->lock, NULL);

    reg->count++;   /* publicar — visible a mp_find sin lock */

    pthread_mutex_unlock(&reg->lock);
    return mp;
}

/* ── Source ───────────────────────────────────────────────────────── */

int mp_source_attach(mountpoint_t *mp, conn_t *source)
{
    pthread_rwlock_wrlock(&mp->lock);

    if (mp->source != NULL) {
        /* Ya hay un source activo */
        pthread_rwlock_unlock(&mp->lock);
        return -1;
    }

    mp->source             = source;
    mp->active             = 1;
    mp->source_connected_at = time(NULL);
    source->mp             = mp;

    pthread_rwlock_unlock(&mp->lock);

    printf("[mount:%s] source attached fd=%d addr=%s\n",
           mp->name, source->fd, source->remote_addr);
    return 0;
}

void mp_source_detach(mountpoint_t *mp, conn_t *source)
{
    pthread_rwlock_wrlock(&mp->lock);

    if (mp->source == source) {
        mp->source = NULL;
        mp->active = 0;
    }

    pthread_rwlock_unlock(&mp->lock);

    printf("[mount:%s] source detached fd=%d  relayed=%llu bytes / %llu frames\n",
           mp->name, source->fd,
           (unsigned long long)mp->bytes_relayed,
           (unsigned long long)mp->frames_relayed);
}

/* ── Clientes ─────────────────────────────────────────────────────── */

int mp_client_subscribe(mountpoint_t *mp, conn_t *client)
{
    pthread_rwlock_wrlock(&mp->lock);

    if (mp->client_count >= CLIENT_MAX_PER_MOUNT) {
        pthread_rwlock_unlock(&mp->lock);
        return -1;
    }

    /* Inicializar offset al write_pos actual — el cliente no recibe
     * datos históricos, solo los nuevos a partir de este momento */
    client->read_offset = rb_write_pos(&mp->ring);
    client->mp          = mp;

    /* Insertar al inicio de la lista */
    client->prev = NULL;
    client->next = mp->clients_head;
    if (mp->clients_head)
        mp->clients_head->prev = client;
    mp->clients_head = client;
    mp->client_count++;

    pthread_rwlock_unlock(&mp->lock);

    printf("[mount:%s] client subscribed fd=%d addr=%s  total=%d\n",
           mp->name, client->fd, client->remote_addr, mp->client_count);
    return 0;
}

void mp_client_unsubscribe(mountpoint_t *mp, conn_t *client)
{
    pthread_rwlock_wrlock(&mp->lock);

    /* Eliminar de la lista doblemente enlazada */
    if (client->prev) client->prev->next = client->next;
    else              mp->clients_head   = client->next;
    if (client->next) client->next->prev = client->prev;

    client->next = NULL;
    client->prev = NULL;
    client->mp   = NULL;
    mp->client_count--;

    pthread_rwlock_unlock(&mp->lock);

    printf("[mount:%s] client unsubscribed fd=%d  remaining=%d\n",
           mp->name, client->fd, mp->client_count);
}

/* ── Relay ────────────────────────────────────────────────────────── */

void mp_update_coords(mountpoint_t *mp, const rtcm3_coords_t *coords)
{
    /* Sin lock extra — solo escribe el source, no hay race */
    mp->coords     = *coords;
    mp->has_coords = 1;
}

void mp_update_receiver(mountpoint_t *mp, const rtcm3_receiver_t *rx)
{
    mp->receiver     = *rx;
    mp->has_receiver = 1;
}

/*
 * mp_relay — Hot path: source data → ring buffer + decode para logging.
 *
 * Esta función corre en el thread del source (io_engine worker).
 * No copia datos a los clientes — ellos leen del ring directamente
 * en sus propios threads vía rb_read().
 *
 * El decoder RTCM3 extrae coords/logging sin bloquear el relay.
 */
size_t mp_relay(mountpoint_t *mp, const uint8_t *data, size_t len)
{
    if (len == 0) return 0;

    /* ── 1. Escribir al ring (zero-copy relay) ── */
    size_t written = rb_write(&mp->ring, data, len);
    mp->bytes_relayed += written;

    /* ── 2. Decode RTCM3 para logging/coords (best-effort) ── */
    rtcm3_frame_t frames[RELAY_FRAME_MAX];
    int           frame_count = 0;
    size_t        used        = 0;

    rtcm3_parse_stream(data, len, frames, RELAY_FRAME_MAX,
                       &frame_count, &used);

    mp->frames_relayed += (uint64_t)frame_count;

    for (int i = 0; i < frame_count; i++) {
        rtcm3_message_t msg;
        if (rtcm3_decode(&frames[i], &msg) != RTCM3_RC_OK)
            continue;

        switch (msg.type) {
        case RTCM3_MSG_COORDS:
            mp_update_coords(mp, &msg.coords);
            break;

        case RTCM3_MSG_RECEIVER:
            mp_update_receiver(mp, &msg.receiver);
            break;

        case RTCM3_MSG_MSM:
            /* Logging futuro: epoch, satélites por constelación */
            break;

        default:
            break;
        }
    }

    return written;
}
