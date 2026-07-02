/*
 * rtcm3.h — API pública de la librería RTCM3 del NtripCaster
 *
 * Include único para usar el decoder. Incluye todos los sub-headers.
 *
 * Uso mínimo en el caster (byte relay):
 *
 *   rtcm3_frame_t frames[32];
 *   int count;
 *   size_t used;
 *
 *   rtcm3_rc_t rc = rtcm3_parse_stream(buf, len, frames, 32, &count, &used);
 *   for (int i = 0; i < count; i++) {
 *       rtcm3_message_t msg;
 *       rtcm3_decode(&frames[i], &msg);
 *       if (msg.type == RTCM3_MSG_COORDS)
 *           update_mountpoint_coords(msg.station_id, msg.coords.lat_deg, msg.coords.lon_deg);
 *       // forward frames[i].data / frames[i].frame_len al cliente
 *   }
 *
 * Uso completo (logging + ephemeris):
 *
 *   rtcm3_message_t msg;
 *   rtcm3_decode(&frame, &msg);
 *   switch (msg.type) {
 *       case RTCM3_MSG_MSM:    log_msm(&msg.msm);    break;
 *       case RTCM3_MSG_EPH:    log_eph(&msg.eph);    break;
 *       case RTCM3_MSG_COORDS: log_coords(&msg.coords); break;
 *       default: break;
 *   }
 */
#ifndef NTRIPCASTER_RTCM3_H
#define NTRIPCASTER_RTCM3_H

#include "rtcm3_bits.h"
#include "rtcm3_frame.h"
#include "rtcm3_station.h"
#include "rtcm3_msm.h"
#include "rtcm3_eph.h"

/* ── Categorías de mensajes ──────────────────────────────────────── */
typedef enum {
    RTCM3_MSG_UNKNOWN  = 0,  /* No decodificado / no soportado */
    RTCM3_MSG_COORDS   = 1,  /* 1005, 1006 — coords ECEF */
    RTCM3_MSG_ANTENNA  = 2,  /* 1007, 1008 — descriptor antena */
    RTCM3_MSG_RECEIVER = 3,  /* 1033 — receptor + antena */
    RTCM3_MSG_MSM      = 4,  /* 1071-1137 — observaciones GNSS */
    RTCM3_MSG_EPH_GPS  = 5,  /* 1019 — efemérides GPS */
    RTCM3_MSG_EPH_GLO  = 6,  /* 1020 — efemérides GLONASS */
    RTCM3_MSG_EPH_BDS  = 7,  /* 1042 — efemérides BeiDou */
    RTCM3_MSG_EPH_QZSS = 8,  /* 1044 — efemérides QZSS */
    RTCM3_MSG_EPH_GAL  = 9,  /* 1045/1046 — efemérides Galileo */
    RTCM3_MSG_LEGACY   = 10, /* 1001-1012 — legacy GPS/GLONASS obs */
} rtcm3_msg_category_t;

/*
 * rtcm3_message_t — Contenedor del mensaje decodificado.
 *
 * El campo `type` determina qué union member es válido.
 * Los punteros dentro de los sub-structs son cero-copy: apuntan
 * al buffer original. No liberar por separado.
 */
typedef struct {
    rtcm3_msg_category_t type;
    uint16_t             msg_type;   /* Número de mensaje RTCM3 (ej. 1074) */
    uint16_t             station_id; /* Siempre presente si el msg tiene ID */

    union {
        rtcm3_coords_t       coords;   /* MSG_COORDS */
        rtcm3_antenna_t      antenna;  /* MSG_ANTENNA */
        rtcm3_receiver_t     receiver; /* MSG_RECEIVER */
        rtcm3_msm_header_t   msm;      /* MSG_MSM */
        rtcm3_eph_kepler_t   eph;      /* MSG_EPH_GPS/BDS/QZSS/GAL */
        rtcm3_eph_glonass_t  eph_glo;  /* MSG_EPH_GLO */
    };
} rtcm3_message_t;

/* ── Función principal de dispatch ──────────────────────────────── */

/*
 * rtcm3_decode — Decodifica un frame RTCM3 y llena rtcm3_message_t.
 *
 * Si el tipo de mensaje no está soportado, retorna RTCM3_RC_OK con
 * msg->type == RTCM3_MSG_UNKNOWN. Nunca retorna error por msg_type
 * desconocido — eso es normal en un caster (miles de tipos).
 *
 * @frame  Frame ya validado por rtcm3_parse_stream (CRC OK)
 * @msg    Output. Cero-copy — apunta al buffer del frame.
 * Returns RTCM3_RC_OK si se decodificó algo útil,
 *         RTCM3_RC_NEED_DATA si el body es demasiado corto (bug upstream).
 */
rtcm3_rc_t rtcm3_decode(const rtcm3_frame_t *frame, rtcm3_message_t *msg);

/*
 * rtcm3_msg_category_name — Nombre de la categoría para logging.
 */
const char *rtcm3_msg_category_name(rtcm3_msg_category_t cat);

#endif /* NTRIPCASTER_RTCM3_H */
