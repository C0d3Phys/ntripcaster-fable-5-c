/*
 * rtcm3_eph.h — Decode básico de mensajes de efemérides RTCM3
 *
 * Para el caster: PRN, semana, salud del satélite.
 * Para futura integración PPP: estructura base para extender.
 *
 * Mensajes soportados:
 *   1019 — GPS Ephemeris
 *   1020 — GLONASS Ephemeris
 *   1042 — BeiDou Ephemeris
 *   1044 — QZSS Ephemeris
 *   1045 — Galileo F/NAV Ephemeris
 *   1046 — Galileo I/NAV Ephemeris
 *   1041 — NavIC/IRNSS Ephemeris
 */
#ifndef NTRIPCASTER_RTCM3_EPH_H
#define NTRIPCASTER_RTCM3_EPH_H

#include <stdint.h>
#include "rtcm3_frame.h"
#include "rtcm3_msm.h"

/* ── Parámetros orbitales Keplerianos (comunes a GPS/Galileo/BeiDou/QZSS) ── */
typedef struct {
    /* Identificación */
    rtcm3_gnss_t gnss;
    uint8_t      sat_id;        /* PRN / slot number */
    uint16_t     week;          /* Semana GPS/GST/BDT */
    uint8_t      health;        /* Indicador de salud */
    uint8_t      accuracy;      /* User range accuracy */

    /* Tiempos de referencia */
    uint32_t     toc;           /* Tiempo de referencia del reloj (s) */
    uint32_t     toe;           /* Tiempo de referencia de la efeméride (s) */

    /* Correcciones de reloj */
    double af0;  /* Corrección de reloj (s) */
    double af1;  /* Drift de reloj (s/s) */
    double af2;  /* Deriva de drift (s/s²) */
    double tgd;  /* Group delay (s) */

    /* Parámetros Keplerianos */
    double sqrt_a;     /* Raíz cuadrada del semieje mayor (m^0.5) */
    double ecc;        /* Excentricidad */
    double m0;         /* Anomalía media en toe (rad) */
    double omega0;     /* Longitud del nodo ascendente (rad) */
    double i0;         /* Inclinación en toe (rad) */
    double omega;      /* Argumento del perigeo (rad) */
    double omega_dot;  /* Tasa de cambio del nodo ascendente (rad/s) */
    double idot;       /* Tasa de cambio de inclinación (rad/s) */
    double delta_n;    /* Corrección de movimiento medio (rad/s) */

    /* Correcciones armónicas */
    double crc, crs;  /* Correcciones seno/coseno del radio orbital */
    double cuc, cus;  /* Correcciones seno/coseno del argumento de latitud */
    double cic, cis;  /* Correcciones seno/coseno de inclinación */

    /* Issue of data */
    uint16_t iode;
    uint16_t iodc;
} rtcm3_eph_kepler_t;

/* ── Efemérides GLONASS (PVT en ECEF) ─────────────────────────── */
typedef struct {
    uint8_t  slot;           /* Slot GLONASS (1-24) */
    int8_t   freq_channel;   /* Canal de frecuencia (-7 a +6) */
    uint8_t  health;
    uint8_t  age;

    /* Estado del satélite en ECEF (sistema PZ-90) */
    double   pos_x_km;    /* Posición X (km) */
    double   pos_y_km;
    double   pos_z_km;
    double   vel_x_km_s;  /* Velocidad X (km/s) */
    double   vel_y_km_s;
    double   vel_z_km_s;
    double   acc_x;       /* Aceleración (km/s²) — solo perturbaciones lunares */
    double   acc_y;
    double   acc_z;

    /* Correcciones de reloj */
    double   tau_n;       /* Corrección de reloj (s) */
    double   gamma_n;     /* Frecuencia relativa de desviación */
    uint32_t tk;          /* Tiempo de referencia (ms desde medianoche Moscú) */
    uint32_t tb;          /* Tiempo de referencia para nav params */
} rtcm3_eph_glonass_t;

/* ── API ────────────────────────────────────────────────────────── */

rtcm3_rc_t rtcm3_decode_1019(const uint8_t *body, uint32_t body_len,
                              rtcm3_eph_kepler_t *out);

rtcm3_rc_t rtcm3_decode_1020(const uint8_t *body, uint32_t body_len,
                              rtcm3_eph_glonass_t *out);

rtcm3_rc_t rtcm3_decode_1042(const uint8_t *body, uint32_t body_len,
                              rtcm3_eph_kepler_t *out);  /* BeiDou */

rtcm3_rc_t rtcm3_decode_1044(const uint8_t *body, uint32_t body_len,
                              rtcm3_eph_kepler_t *out);  /* QZSS */

rtcm3_rc_t rtcm3_decode_1045(const uint8_t *body, uint32_t body_len,
                              rtcm3_eph_kepler_t *out);  /* Galileo F/NAV */

rtcm3_rc_t rtcm3_decode_1046(const uint8_t *body, uint32_t body_len,
                              rtcm3_eph_kepler_t *out);  /* Galileo I/NAV */

/* Nombres para logging */
const char *rtcm3_eph_gnss_prn(const rtcm3_eph_kepler_t *eph, char *buf);

#endif /* NTRIPCASTER_RTCM3_EPH_H */
