/*
 * rtcm3_msm.h — Decode MSM (Multiple Signal Messages) RTCM3
 *
 * MSM cubre todos los sistemas GNSS modernos con estructura unificada.
 * Niveles MSM1-MSM7 difieren en qué campos de observación incluyen.
 *
 * Para el caster: decodificamos el HEADER (epoch, satélites, señales).
 * El cuerpo de observaciones se retransmite intacto al rover.
 *
 * FIX crítico vs Python decoder:
 *   GLONASS usa epoch de 27 bits (TOD desde medianoche Moscú UTC+3)
 *   GPS/Galileo/BeiDou/QZSS/NavIC usan 30 bits (TOW en ms mod 1 semana)
 *
 * Verificado:
 *   GPS  epoch 153138000 ms ✓ (vs librtcm)
 *   GLON epoch  77520000 ms ✓ (vs librtcm, Python tenía bug con 30 bits)
 */
#ifndef NTRIPCASTER_RTCM3_MSM_H
#define NTRIPCASTER_RTCM3_MSM_H

#include <stdint.h>
#include "rtcm3_frame.h"

/* Sistemas GNSS identificados por rango de msg_type */
typedef enum {
    RTCM3_GNSS_GPS     = 0,
    RTCM3_GNSS_GLONASS = 1,
    RTCM3_GNSS_GALILEO = 2,
    RTCM3_GNSS_SBAS    = 3,
    RTCM3_GNSS_QZSS    = 4,
    RTCM3_GNSS_BEIDOU  = 5,
    RTCM3_GNSS_NAVIC   = 6,
    RTCM3_GNSS_UNKNOWN = 7,
} rtcm3_gnss_t;

/* Nivel MSM (1-7) según complejidad de observaciones */
typedef enum {
    RTCM3_MSM_LEVEL_1 = 1, /* Pseudorange rough */
    RTCM3_MSM_LEVEL_2 = 2, /* Pseudorange fine */
    RTCM3_MSM_LEVEL_3 = 3, /* Pseudorange + phase rough */
    RTCM3_MSM_LEVEL_4 = 4, /* Pseudorange + phase + SNR */
    RTCM3_MSM_LEVEL_5 = 5, /* + Doppler */
    RTCM3_MSM_LEVEL_6 = 6, /* Pseudorange + phase alta resolución */
    RTCM3_MSM_LEVEL_7 = 7, /* Máxima resolución + Doppler */
} rtcm3_msm_level_t;

/*
 * rtcm3_msm_header_t — Header decodificado de un mensaje MSM.
 *
 * Para GPS/Galileo/BeiDou/QZSS/NavIC:
 *   epoch_ms = TOW (Time of Week) en milisegundos, módulo 1 semana
 *   glonass_day = 0 (no usado)
 *
 * Para GLONASS:
 *   epoch_ms = milisegundos desde medianoche Moscú (UTC+3)
 *   glonass_day = día de la semana (0=domingo, 1=lunes, ..., 6=sábado)
 */
typedef struct {
    uint16_t        station_id;
    rtcm3_gnss_t    gnss;
    rtcm3_msm_level_t level;       /* 1-7 */
    uint32_t        epoch_ms;      /* Ver descripción arriba */
    uint8_t         glonass_day;   /* Solo para GLONASS */
    uint8_t         multiple_msg;  /* 1 = hay más mensajes del mismo epoch */
    uint8_t         smoothing;     /* Divergence-free smoothing */
    uint8_t         smooth_int;    /* Smoothing interval */

    /* Satélites presentes (extraídos del satellite_mask de 64 bits) */
    uint8_t         num_satellites;
    uint8_t         satellite_ids[64]; /* PRN de cada satélite presente */

    /* Señales presentes (extraídas del signal_mask de 32 bits) */
    uint8_t         num_signals;
    uint8_t         signal_ids[32];    /* ID de señal de cada tipo presente */

    /* Cell mask: qué (satélite, señal) tienen datos */
    uint8_t         num_cells;         /* = num_satellites × num_signals (con cell mask) */
} rtcm3_msm_header_t;

/* ── API ────────────────────────────────────────────────────────── */

/*
 * rtcm3_msm_gnss — Retorna el sistema GNSS para un msg_type dado.
 * Ejemplos: 1074 → RTCM3_GNSS_GPS, 1087 → RTCM3_GNSS_GLONASS
 */
rtcm3_gnss_t rtcm3_msm_gnss(uint16_t msg_type);

/* Nombre del sistema GNSS */
const char *rtcm3_gnss_name(rtcm3_gnss_t gnss);

/*
 * rtcm3_decode_msm_header — Decode el header de cualquier mensaje MSM.
 * Funciona para MSM1 hasta MSM7 de cualquier constelación.
 *
 * @body     Body del frame RTCM3 (desde el inicio, incluyendo msg_type)
 * @body_len Longitud del body en bytes
 * @out      Header decodificado
 */
rtcm3_rc_t rtcm3_decode_msm_header(const uint8_t *body, uint32_t body_len,
                                   rtcm3_msm_header_t *out);

/*
 * rtcm3_is_msm — Retorna 1 si msg_type es un mensaje MSM (1071-1137)
 */
static inline int rtcm3_is_msm(uint16_t msg_type)
{
    return (msg_type >= 1071 && msg_type <= 1077)
        || (msg_type >= 1081 && msg_type <= 1087)
        || (msg_type >= 1091 && msg_type <= 1097)
        || (msg_type >= 1101 && msg_type <= 1107)
        || (msg_type >= 1111 && msg_type <= 1117)
        || (msg_type >= 1121 && msg_type <= 1127)
        || (msg_type >= 1131 && msg_type <= 1137);
}

#endif /* NTRIPCASTER_RTCM3_MSM_H */
