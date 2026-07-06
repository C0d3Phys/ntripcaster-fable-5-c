/*
 * mountpoint.c — Gestión de mountpoints y relay de datos
 */
#include "mountpoint.h"
#include "logger.h"
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

    /* Reset de stats de decode: son POR SOURCE, no históricas del mount.
     * Sin esto, al cambiar la fuente (ej. Demo1 MSM → SCSC legacy) los
     * tipos de mensaje de ambas se mezclan y confunden el diagnóstico. */
    mp->msg_stats_n       = 0;
    mp->dec_len           = 0;
    mp->dec_frames_ok     = 0;
    mp->dec_bytes_skipped = 0;
    mp->rpt_frames        = 0;
    mp->rpt_skipped       = 0;

    pthread_rwlock_unlock(&mp->lock);

    log_info("mount:%s source attached fd=%d addr=%s",
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

    log_info("mount:%s source detached fd=%d  relayed=%llu bytes / %llu frames",
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

    log_info("mount:%s client subscribed fd=%d addr=%s  total=%d",
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

    log_info("mount:%s client unsubscribed fd=%d  remaining=%d",
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
/*
 * mp_count_msg_type — Cuenta un frame por tipo de mensaje (tabla chica,
 * orden de aparición).
 *
 * IMP-01D: esta tabla (msg_stats[]/msg_stats_n) la escribe el thread del
 * source (llamado desde mp_relay) y la LEE report_mount_stats() desde el
 * accept thread cada ~30s. A diferencia de los contadores simples de
 * arriba, "agregar un tipo nuevo" es leer N, comparar contra N entradas,
 * y tal vez escribir en la entrada N -- no se puede expresar como un
 * único load/store atómico. Se protege con el rwlock que el mountpoint
 * ya tiene para source/clients (wrlock acá, rdlock en el reporte).
 */
static void mp_count_msg_type(mountpoint_t *mp, uint16_t type)
{
    pthread_rwlock_wrlock(&mp->lock);
    for (int i = 0; i < mp->msg_stats_n; i++) {
        if (mp->msg_stats[i].type == type) {
            mp->msg_stats[i].count++;
            pthread_rwlock_unlock(&mp->lock);
            return;
        }
    }
    if (mp->msg_stats_n < MP_MSG_STATS_MAX) {
        mp->msg_stats[mp->msg_stats_n].type  = type;
        mp->msg_stats[mp->msg_stats_n].count = 1;
        mp->msg_stats_n++;
    }
    pthread_rwlock_unlock(&mp->lock);
}

size_t mp_relay(mountpoint_t *mp, const uint8_t *data, size_t len)
{
    if (len == 0) return 0;

    /* ── 1. Escribir al ring (zero-copy relay) ──
     * El relay NUNCA depende del decode: los bytes salen a los clientes
     * tal cual llegaron, aunque estén corruptos. El decode de abajo es
     * SOLO para visibilidad (coords + stats de integridad). */
    size_t written = rb_write(&mp->ring, data, len);
    mp->bytes_relayed += written;

    /* ── 2. Decode incremental con reensamblado ──
     * Acumular en dec_buf y parsear ahí: los frames partidos entre
     * read()s se completan en el próximo chunk en vez de perderse. */
    const uint8_t *p         = data;
    size_t         remaining = len;

    while (remaining > 0) {
        size_t space = MP_DEC_BUF_SIZE - mp->dec_len;
        size_t take  = remaining < space ? remaining : space;
        memcpy(mp->dec_buf + mp->dec_len, p, take);
        mp->dec_len += (uint32_t)take;
        p           += take;
        remaining   -= take;

        rtcm3_frame_t frames[RELAY_FRAME_MAX];
        int           frame_count = 0;
        size_t        used        = 0;

        rtcm3_parse_stream(mp->dec_buf, mp->dec_len, frames,
                           RELAY_FRAME_MAX, &frame_count, &used);

        mp->frames_relayed += (uint64_t)frame_count;
        mp->dec_frames_ok  += (uint64_t)frame_count;

        /* Bytes consumidos que NO son frames válidos = basura
         * (preamble falso, CRC malo, ruido serial) */
        size_t frame_bytes = 0;
        for (int i = 0; i < frame_count; i++) {
            frame_bytes += frames[i].frame_len;
            mp_count_msg_type(mp, frames[i].msg_type);

            rtcm3_message_t msg;
            if (rtcm3_decode(&frames[i], &msg) != RTCM3_RC_OK)
                continue;
            switch (msg.type) {
            case RTCM3_MSG_COORDS:   mp_update_coords(mp, &msg.coords);     break;
            case RTCM3_MSG_RECEIVER: mp_update_receiver(mp, &msg.receiver); break;
            default: break;
            }
        }
        if (used > frame_bytes)
            mp->dec_bytes_skipped += (uint64_t)(used - frame_bytes);

        /* Correr el resto (frame incompleto) al inicio del buffer */
        if (used > 0) {
            memmove(mp->dec_buf, mp->dec_buf + used, mp->dec_len - used);
            mp->dec_len -= (uint32_t)used;
        }

        /* Guardia anti-atasco: un 0xD3 falso con longitud que promete
         * más de lo que puede existir (frame max = 1029B) nunca se va
         * a completar — saltarlo a mano para no frenar el parser. */
        if (mp->dec_len > 1100 && used == 0) {
            mp->dec_bytes_skipped++;
            memmove(mp->dec_buf, mp->dec_buf + 1, mp->dec_len - 1);
            mp->dec_len--;
        }
    }

    return written;
}
