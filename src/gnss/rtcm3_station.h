/*
 * rtcm3_station.h — Decode mensajes de estación base
 *
 * Mensajes soportados:
 *   1005 — Coordenadas ECEF (sin altura antena)
 *   1006 — Coordenadas ECEF + altura de antena
 *   1007 — Descriptor de antena
 *   1008 — Descriptor de antena + número de serie
 *   1033 — Receiver and Antenna Descriptor (completo)
 *
 * Uso principal en el caster: extraer lat/lon para NEAREST routing.
 * Validado vs librtcm: coords idénticas para Demo1.bin (Auckland NZ).
 */
#ifndef NTRIPCASTER_RTCM3_STATION_H
#define NTRIPCASTER_RTCM3_STATION_H

#include <stdint.h>
#include "rtcm3_frame.h"

/* Longitud máxima de strings en mensajes de estación */
#define RTCM3_STATION_STR_MAX 32

/* ── Tipos ──────────────────────────────────────────────────────── */

/*
 * rtcm3_coords_t — Coordenadas ECEF + lat/lon derivadas.
 * Fuente: mensajes 1005 / 1006.
 * Verificado: -5105681.0957 / 461564.0519 / -3782181.6888 (Demo1.bin)
 */
typedef struct {
    uint16_t station_id;       /* 12 bits */
    double   ecef_x_m;         /* 38 bits signed × 0.0001 m */
    double   ecef_y_m;
    double   ecef_z_m;
    double   antenna_h_m;      /* 16 bits × 0.0001 m (solo en msg 1006) */
    double   lat_deg;          /* Derivado de ECEF — para NEAREST */
    double   lon_deg;          /* Derivado de ECEF — para NEAREST */
    uint8_t  has_antenna_h;    /* 1 = datos de msg 1006 */
} rtcm3_coords_t;

/*
 * rtcm3_antenna_t — Descripción de antena.
 * Fuente: mensajes 1007 / 1008.
 */
typedef struct {
    uint16_t station_id;
    char     descriptor[RTCM3_STATION_STR_MAX]; /* Modelo antena */
    char     serial[RTCM3_STATION_STR_MAX];      /* Número de serie (1008) */
    uint8_t  setup_id;
    uint8_t  has_serial;
} rtcm3_antenna_t;

/*
 * rtcm3_receiver_t — Descripción completa de receptor + antena.
 * Fuente: mensaje 1033.
 */
typedef struct {
    uint16_t station_id;
    char     antenna_desc[RTCM3_STATION_STR_MAX];
    char     antenna_serial[RTCM3_STATION_STR_MAX];
    char     receiver_desc[RTCM3_STATION_STR_MAX];
    char     receiver_firmware[RTCM3_STATION_STR_MAX];
    char     receiver_serial[RTCM3_STATION_STR_MAX];
    uint8_t  setup_id;
} rtcm3_receiver_t;

/* ── API ────────────────────────────────────────────────────────── */

/*
 * rtcm3_decode_1005 / rtcm3_decode_1006
 * @body     Puntero al inicio del body del frame (después del preamble+header)
 * @body_len Longitud del body en bytes
 * @out      Resultado decodificado
 * Retorna RTCM3_RC_OK si OK.
 */
rtcm3_rc_t rtcm3_decode_1005(const uint8_t *body, uint32_t body_len,
                              rtcm3_coords_t *out);

rtcm3_rc_t rtcm3_decode_1006(const uint8_t *body, uint32_t body_len,
                              rtcm3_coords_t *out);

rtcm3_rc_t rtcm3_decode_1007(const uint8_t *body, uint32_t body_len,
                              rtcm3_antenna_t *out);

rtcm3_rc_t rtcm3_decode_1008(const uint8_t *body, uint32_t body_len,
                              rtcm3_antenna_t *out);

rtcm3_rc_t rtcm3_decode_1033(const uint8_t *body, uint32_t body_len,
                              rtcm3_receiver_t *out);

/*
 * rtcm3_ecef_to_llh — Conversión ECEF → Lat/Lon/Height (WGS84)
 * Algoritmo de Bowring (3 iteraciones, error < 0.1 mm).
 */
void rtcm3_ecef_to_llh(double x, double y, double z,
                        double *lat_deg, double *lon_deg, double *h_m);

#endif /* NTRIPCASTER_RTCM3_STATION_H */
