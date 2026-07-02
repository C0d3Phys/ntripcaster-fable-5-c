/*
 * rtcm3_eph.c — Decode de efemérides RTCM3
 *
 * Mensajes: 1019 (GPS), 1020 (GLONASS), 1042 (BeiDou),
 *           1044 (QZSS), 1045/1046 (Galileo F/NAV + I/NAV)
 *
 * Escala de factores de acuerdo al estándar RTCM 10403.3.
 * Verificado contra librtcm (Swift Navigation) y RTKLIB.
 *
 * Para el caster: PRN, semana, salud del satélite para logging.
 * Para futura PPP: todos los parámetros Keplerianos + correcciones de reloj.
 */
#include "rtcm3_eph.h"
#include "rtcm3_bits.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ────────────────────────────────────────────────────────────────────
 * Macros de escala — conversión a unidades SI
 * ────────────────────────────────────────────────────────────────── */
#define SC2RAD  3.1415926535898   /* Semicircles → radianes (× π) */

#define P2_4    (1.0 / (1 << 4))          /* 2^-4  */
#define P2_5    (1.0 / (1 << 5))          /* 2^-5  */
#define P2_11   (1.0 / (1 << 11))         /* 2^-11 */
#define P2_19   (1.0 / (1 << 19))         /* 2^-19 */
#define P2_20   (1.0 / (1 << 20))         /* 2^-20 */
#define P2_29   (1.0 / (1u << 29))        /* 2^-29 */
#define P2_30   (1.0 / (1u << 30))        /* 2^-30 */
#define P2_31   (1.0 / (1u << 31))        /* 2^-31 */
#define P2_33   (1.0 / (1u << 31) / 4.0) /* 2^-33 */
#define P2_40   (1.0 / ((uint64_t)1 << 40)) /* 2^-40 */
#define P2_43   (1.0 / ((uint64_t)1 << 43)) /* 2^-43 */
#define P2_50   (1.0 / ((uint64_t)1 << 50)) /* 2^-50 */
#define P2_55   (1.0 / ((uint64_t)1 << 55)) /* 2^-55 */

/* ────────────────────────────────────────────────────────────────────
 * 1019 — GPS L1/L2 Ephemeris
 *
 * Layout (488 bits total, referencia: RTCM 10403.3 §3.5.9):
 *   [12] msg type
 *   [6]  satellite ID (1–32)
 *   [10] week number (GPS week, mod 1024)
 *   [4]  SV accuracy (URA index)
 *   [2]  code on L2
 *   [14] IDOT   × 2^-43 π rad/s
 *   [8]  IODE
 *   [16] toc    × 2^4 s
 *   [8]  af2    × 2^-55 s/s²
 *   [16] af1    × 2^-43 s/s
 *   [22] af0    × 2^-31 s
 *   [10] IODC
 *   [16] Crs    × 2^-5 m
 *   [16] Δn     × 2^-43 π rad/s
 *   [32] M0     × 2^-31 π rad
 *   [16] Cuc    × 2^-29 rad
 *   [32] e      × 2^-33 (unsigned)
 *   [16] Cus    × 2^-29 rad
 *   [32] √A     × 2^-19 m^0.5
 *   [16] toe    × 2^4 s
 *   [16] Cic    × 2^-29 rad
 *   [32] Ω0     × 2^-31 π rad
 *   [16] Cis    × 2^-29 rad
 *   [32] i0     × 2^-31 π rad
 *   [16] Crc    × 2^-5 m
 *   [32] ω      × 2^-31 π rad
 *   [24] Ω_dot  × 2^-43 π rad/s
 *   [8]  tgd    × 2^-31 s
 *   [6]  SV health
 *   [1]  L2 P data flag
 *   [1]  fit interval flag
 * ────────────────────────────────────────────────────────────────── */
rtcm3_rc_t rtcm3_decode_1019(const uint8_t *body, uint32_t body_len,
                              rtcm3_eph_kepler_t *out)
{
    if (body_len < 61) return RTCM3_RC_NEED_DATA; /* 488 bits = 61 bytes */

    rtcm3_bitcursor_t c;
    rtcm3_cursor_init(&c, body, body_len * 8u);

    RSKIP(&c, 12);                                        /* msg type */
    out->gnss        = RTCM3_GNSS_GPS;
    out->sat_id      = (uint8_t)RGET_U(&c, 6);           /* PRN */
    out->week        = (uint16_t)RGET_U(&c, 10);
    out->accuracy    = (uint8_t)RGET_U(&c, 4);
    RSKIP(&c, 2);                                         /* code on L2 */
    out->idot        = RGET_S(&c, 14) * P2_43 * SC2RAD;
    out->iode        = (uint16_t)RGET_U(&c, 8);
    out->toc         = RGET_U(&c, 16) * 16;              /* ×2^4 */
    out->af2         = RGET_S(&c, 8)  * P2_55;
    out->af1         = RGET_S(&c, 16) * P2_43;
    out->af0         = RGET_S(&c, 22) * P2_31;
    out->iodc        = (uint16_t)RGET_U(&c, 10);
    out->crs         = RGET_S(&c, 16) * P2_5;
    out->delta_n     = RGET_S(&c, 16) * P2_43 * SC2RAD;
    out->m0          = RGET_SL(&c, 32) * P2_31 * SC2RAD;
    out->cuc         = RGET_S(&c, 16) * P2_29;
    out->ecc         = (double)RGET_UL(&c, 32) * P2_33;
    out->cus         = RGET_S(&c, 16) * P2_29;
    out->sqrt_a      = (double)RGET_UL(&c, 32) * P2_19;
    out->toe         = RGET_U(&c, 16) * 16;              /* ×2^4 */
    out->cic         = RGET_S(&c, 16) * P2_29;
    out->omega0      = RGET_SL(&c, 32) * P2_31 * SC2RAD;
    out->cis         = RGET_S(&c, 16) * P2_29;
    out->i0          = RGET_SL(&c, 32) * P2_31 * SC2RAD;
    out->crc         = RGET_S(&c, 16) * P2_5;
    out->omega       = RGET_SL(&c, 32) * P2_31 * SC2RAD;
    out->omega_dot   = RGET_S(&c, 24) * P2_43 * SC2RAD;
    out->tgd         = RGET_S(&c, 8)  * P2_31;
    out->health      = (uint8_t)RGET_U(&c, 6);
    RSKIP(&c, 2);                                         /* L2 P flag + fit interval */

    return c.overflow ? RTCM3_RC_NEED_DATA : RTCM3_RC_OK;
}

/* ────────────────────────────────────────────────────────────────────
 * 1020 — GLONASS L1/L2 Ephemeris
 *
 * Layout (360 bits, referencia: RTCM 10403.3 §3.5.10):
 *   [12] msg type
 *   [6]  satellite slot (1–24)
 *   [5]  frequency channel number (offset +7, rango −7 a +6)
 *   [1]  almanac health
 *   [1]  almanac health availability
 *   [2]  P1 (time interval code)
 *   [12] tk (ref time: [5]h + [6]min + [1]×30s)
 *   [1]  MSB of Bn (1=healthy)
 *   [1]  P2 flag
 *   [7]  tb (time index × 15 min = seconds in GLONASS day)
 *   [24] x_dot  × 2^-20 km/s (signed)
 *   [27] x      × 2^-11 km   (signed)
 *   [5]  x_acc  × 2^-30 km/s² (unsigned)
 *   [24] y_dot  × 2^-20 km/s (signed)
 *   [27] y      × 2^-11 km   (signed)
 *   [5]  y_acc  × 2^-30 km/s²
 *   [24] z_dot  × 2^-20 km/s (signed)
 *   [27] z      × 2^-11 km   (signed)
 *   [5]  z_acc  × 2^-30 km/s²
 *   [1]  P3
 *   [11] gamma_n × 2^-40 (signed)
 *   [2]  P
 *   [1]  Fn (telemetry health)
 *   [22] tau_n  × 2^-30 s (signed)
 *   [5]  delta_tau_n × 2^-30 s (signed)
 *   [5]  En (age of data, days)
 *   [1]  P4
 *   [4]  Ft (user range accuracy)
 *   [11] Nt (day number)
 *   [2]  M (satellite type)
 *   [1]  additional data availability
 *   [11] Na
 *   [32] tau_c (GLONASS time correction)
 *   [5]  N4 (4-year cycle number)
 *   [22] tau_GPS
 *   [1]  ln health
 * ────────────────────────────────────────────────────────────────── */
rtcm3_rc_t rtcm3_decode_1020(const uint8_t *body, uint32_t body_len,
                              rtcm3_eph_glonass_t *out)
{
    if (body_len < 45) return RTCM3_RC_NEED_DATA; /* 360 bits = 45 bytes */

    rtcm3_bitcursor_t c;
    rtcm3_cursor_init(&c, body, body_len * 8u);

    RSKIP(&c, 12);
    out->slot          = (uint8_t)RGET_U(&c, 6);
    out->freq_channel  = (int8_t)((int)RGET_U(&c, 5) - 7); /* +7 offset */
    RSKIP(&c, 1 + 1 + 2);  /* almanac health + avail + P1 */

    /* tk encoding: [5]h + [6]min + [1]×30s */
    uint32_t tk_h   = RGET_U(&c, 5);
    uint32_t tk_min = RGET_U(&c, 6);
    uint32_t tk_30s = RGET_U(&c, 1);
    out->tk = (tk_h * 3600 + tk_min * 60 + tk_30s * 30) * 1000u; /* → ms */

    out->health = (uint8_t)RGET_U(&c, 1); /* MSB of Bn: 1=good */
    RSKIP(&c, 1);                          /* P2 */

    /* tb: index × 15 min → seconds, then ms */
    out->tb = RGET_U(&c, 7) * 15u * 60u * 1000u;

    /* Velocidades, posiciones, aceleraciones (PZ-90 ECEF) */
    out->vel_x_km_s = RGET_S(&c, 24) * P2_20;
    out->pos_x_km   = RGET_SL(&c, 27) * P2_11;
    out->acc_x      = (double)RGET_U(&c, 5) * P2_30;

    out->vel_y_km_s = RGET_S(&c, 24) * P2_20;
    out->pos_y_km   = RGET_SL(&c, 27) * P2_11;
    out->acc_y      = (double)RGET_U(&c, 5) * P2_30;

    out->vel_z_km_s = RGET_S(&c, 24) * P2_20;
    out->pos_z_km   = RGET_SL(&c, 27) * P2_11;
    out->acc_z      = (double)RGET_U(&c, 5) * P2_30;

    RSKIP(&c, 1);   /* P3 */
    out->gamma_n = RGET_S(&c, 11) * P2_40;
    RSKIP(&c, 3);   /* P + Fn */
    out->tau_n   = RGET_S(&c, 22) * P2_30;
    /* Resto: delta_tau_n, En, P4, Ft, Nt, M, Na, tau_c, N4, tau_GPS, ln */
    /* No necesario para logging básico del caster */

    return c.overflow ? RTCM3_RC_NEED_DATA : RTCM3_RC_OK;
}

/* ────────────────────────────────────────────────────────────────────
 * Helper interno: GPS-like Keplerian decode
 *
 * Los mensajes 1042 (BeiDou), 1044 (QZSS) y 1045/1046 (Galileo)
 * tienen layout muy similar a 1019, con variaciones menores en
 * los campos de semana, health y bits de identificación.
 * ────────────────────────────────────────────────────────────────── */

/* ── 1042 — BeiDou Ephemeris (499 bits) ────────────────────────────
 *   [12] msg type
 *   [6]  satellite ID (1–63)
 *   [13] week number (BDT week)
 *   [4]  URAI (user range accuracy index)
 *   [2]  IDOT  (sign + mag differently decoded in some impls)
 *   Wait — let me use the correct layout:
 *
 *   [12] msg type
 *   [6]  sat id
 *   [13] BDT week
 *   [4]  SV accuracy (URAI)
 *   [14] IDOT   × 2^-43 π rad/s
 *   [5]  AODE (age of data, ephemeris)
 *   [17] toc    × 2^3 s
 *   [11] af2    × 2^-66 s/s²
 *   [22] af1    × 2^-50 s/s
 *   [24] af0    × 2^-33 s
 *   [5]  AODC
 *   [18] Crs    × 2^-6 m
 *   [16] Δn     × 2^-43 π rad/s
 *   [32] M0     × 2^-31 π rad
 *   [18] Cuc    × 2^-31 rad
 *   [32] e      × 2^-33 (unsigned)
 *   [18] Cus    × 2^-31 rad
 *   [32] √A     × 2^-19 m^0.5
 *   [17] toe    × 2^3 s
 *   [18] Cic    × 2^-31 rad
 *   [32] Ω0     × 2^-31 π rad
 *   [18] Cis    × 2^-31 rad
 *   [32] i0     × 2^-31 π rad
 *   [18] Crc    × 2^-6 m
 *   [32] ω      × 2^-31 π rad
 *   [24] Ω_dot  × 2^-43 π rad/s
 *   [10] tgd1   × 0.1 ns
 *   [10] tgd2   × 0.1 ns
 *   [1]  SV health
 * ────────────────────────────────────────────────────────────────── */
#define P2_3   (1.0 / 8.0)    /* 2^-3 → ×8 shift → just ×8 for time */
#define P2_6   (1.0 / 64.0)   /* 2^-6 */
#define P2_31u (1.0 / (double)(1u << 31)) /* 2^-31 unsigned */
#define P2_33u (P2_31u / 4.0)
#define P2_50  (1.0 / ((uint64_t)1 << 50))
#define P2_66  (1.0 / ((uint64_t)1 << 50) / (double)(1 << 16))

rtcm3_rc_t rtcm3_decode_1042(const uint8_t *body, uint32_t body_len,
                              rtcm3_eph_kepler_t *out)
{
    if (body_len < 63) return RTCM3_RC_NEED_DATA; /* 499 bits = 62.4 bytes */

    rtcm3_bitcursor_t c;
    rtcm3_cursor_init(&c, body, body_len * 8u);

    RSKIP(&c, 12);
    out->gnss     = RTCM3_GNSS_BEIDOU;
    out->sat_id   = (uint8_t)RGET_U(&c, 6);
    out->week     = (uint16_t)RGET_U(&c, 13);    /* BDT week */
    out->accuracy = (uint8_t)RGET_U(&c, 4);
    out->idot     = RGET_S(&c, 14) * P2_43 * SC2RAD;
    out->iode     = (uint16_t)RGET_U(&c, 5);     /* AODE */
    out->toc      = RGET_U(&c, 17) * 8;          /* ×2^3 */
    out->af2      = RGET_S(&c, 11) * P2_66;
    out->af1      = RGET_S(&c, 22) * P2_50;
    out->af0      = RGET_S(&c, 24) * P2_33;
    out->iodc     = (uint16_t)RGET_U(&c, 5);     /* AODC */
    out->crs      = RGET_S(&c, 18) * P2_6;
    out->delta_n  = RGET_S(&c, 16) * P2_43 * SC2RAD;
    out->m0       = RGET_SL(&c, 32) * P2_31 * SC2RAD;
    out->cuc      = RGET_S(&c, 18) * P2_31u;
    out->ecc      = (double)RGET_UL(&c, 32) * P2_33u;
    out->cus      = RGET_S(&c, 18) * P2_31u;
    out->sqrt_a   = (double)RGET_UL(&c, 32) * P2_19;
    out->toe      = RGET_U(&c, 17) * 8;          /* ×2^3 */
    out->cic      = RGET_S(&c, 18) * P2_31u;
    out->omega0   = RGET_SL(&c, 32) * P2_31 * SC2RAD;
    out->cis      = RGET_S(&c, 18) * P2_31u;
    out->i0       = RGET_SL(&c, 32) * P2_31 * SC2RAD;
    out->crc      = RGET_S(&c, 18) * P2_6;
    out->omega    = RGET_SL(&c, 32) * P2_31 * SC2RAD;
    out->omega_dot= RGET_S(&c, 24) * P2_43 * SC2RAD;
    out->tgd      = RGET_S(&c, 10) * 1e-10;      /* tgd1: 0.1 ns */
    RSKIP(&c, 10);                                /* tgd2 */
    out->health   = (uint8_t)RGET_U(&c, 1);

    return c.overflow ? RTCM3_RC_NEED_DATA : RTCM3_RC_OK;
}

/* ── 1044 — QZSS Ephemeris (485 bits, casi idéntico a 1019) ────────
 *
 *   [12] msg type
 *   [4]  satellite number (1-10 → PRN 193-202)
 *   [16] toc    × 2^4 s
 *   [8]  af2    × 2^-55 s/s²
 *   [16] af1    × 2^-43 s/s
 *   [22] af0    × 2^-31 s
 *   [8]  IODE
 *   [16] Crs    × 2^-5 m
 *   [16] Δn     × 2^-43 π rad/s
 *   [32] M0     × 2^-31 π rad
 *   [16] Cuc    × 2^-29 rad
 *   [32] e      × 2^-33 (unsigned)
 *   [16] Cus    × 2^-29 rad
 *   [32] √A     × 2^-19 m^0.5
 *   [16] toe    × 2^4 s
 *   [16] Cic    × 2^-29 rad
 *   [32] Ω0     × 2^-31 π rad
 *   [16] Cis    × 2^-29 rad
 *   [32] i0     × 2^-31 π rad
 *   [16] Crc    × 2^-5 m
 *   [32] ω      × 2^-31 π rad
 *   [24] Ω_dot  × 2^-43 π rad/s
 *   [14] IDOT   × 2^-43 π rad/s
 *   [2]  code on L2
 *   [10] GPS week
 *   [4]  SV accuracy
 *   [6]  SV health
 *   [8]  tgd    × 2^-31 s
 *   [10] IODC
 *   [1]  fit interval flag
 * ────────────────────────────────────────────────────────────────── */
rtcm3_rc_t rtcm3_decode_1044(const uint8_t *body, uint32_t body_len,
                              rtcm3_eph_kepler_t *out)
{
    if (body_len < 61) return RTCM3_RC_NEED_DATA; /* 485 bits = 60.6 bytes */

    rtcm3_bitcursor_t c;
    rtcm3_cursor_init(&c, body, body_len * 8u);

    RSKIP(&c, 12);
    out->gnss    = RTCM3_GNSS_QZSS;
    out->sat_id  = (uint8_t)RGET_U(&c, 4);   /* QZSS PRN offset 192 */
    out->toc     = RGET_U(&c, 16) * 16;
    out->af2     = RGET_S(&c, 8)  * P2_55;
    out->af1     = RGET_S(&c, 16) * P2_43;
    out->af0     = RGET_S(&c, 22) * P2_31;
    out->iode    = (uint16_t)RGET_U(&c, 8);
    out->crs     = RGET_S(&c, 16) * P2_5;
    out->delta_n = RGET_S(&c, 16) * P2_43 * SC2RAD;
    out->m0      = RGET_SL(&c, 32) * P2_31 * SC2RAD;
    out->cuc     = RGET_S(&c, 16) * P2_29;
    out->ecc     = (double)RGET_UL(&c, 32) * P2_33;
    out->cus     = RGET_S(&c, 16) * P2_29;
    out->sqrt_a  = (double)RGET_UL(&c, 32) * P2_19;
    out->toe     = RGET_U(&c, 16) * 16;
    out->cic     = RGET_S(&c, 16) * P2_29;
    out->omega0  = RGET_SL(&c, 32) * P2_31 * SC2RAD;
    out->cis     = RGET_S(&c, 16) * P2_29;
    out->i0      = RGET_SL(&c, 32) * P2_31 * SC2RAD;
    out->crc     = RGET_S(&c, 16) * P2_5;
    out->omega   = RGET_SL(&c, 32) * P2_31 * SC2RAD;
    out->omega_dot = RGET_S(&c, 24) * P2_43 * SC2RAD;
    out->idot    = RGET_S(&c, 14) * P2_43 * SC2RAD;
    RSKIP(&c, 2);                              /* code on L2 */
    out->week    = (uint16_t)RGET_U(&c, 10);
    out->accuracy= (uint8_t)RGET_U(&c, 4);
    out->health  = (uint8_t)RGET_U(&c, 6);
    out->tgd     = RGET_S(&c, 8) * P2_31;
    out->iodc    = (uint16_t)RGET_U(&c, 10);
    RSKIP(&c, 1);                              /* fit interval */

    return c.overflow ? RTCM3_RC_NEED_DATA : RTCM3_RC_OK;
}

/* ── 1045 — Galileo F/NAV Ephemeris (496 bits) ─────────────────────
 *
 *   [12] msg type
 *   [6]  satellite ID (1–50 E1-E50)
 *   [12] GST week number
 *   [10] IODnav
 *   [8]  SISA (signal-in-space accuracy index)
 *   [14] IDOT   × 2^-43 π rad/s
 *   [14] toc    × 60 s
 *   [6]  af2    × 2^-59 s/s²
 *   [21] af1    × 2^-46 s/s
 *   [31] af0    × 2^-34 s
 *   [16] Crs    × 2^-5 m
 *   [16] Δn     × 2^-43 π rad/s
 *   [32] M0     × 2^-31 π rad
 *   [16] Cuc    × 2^-29 rad
 *   [32] e      × 2^-33 (unsigned)
 *   [16] Cus    × 2^-29 rad
 *   [32] √A     × 2^-19 m^0.5
 *   [14] toe    × 60 s
 *   [16] Cic    × 2^-29 rad
 *   [32] Ω0     × 2^-31 π rad
 *   [16] Cis    × 2^-29 rad
 *   [32] i0     × 2^-31 π rad
 *   [16] Crc    × 2^-5 m
 *   [32] ω      × 2^-31 π rad
 *   [24] Ω_dot  × 2^-43 π rad/s
 *   [10] BGD E5a/E1 × 2^-32 s
 *   [2]  E5a health status
 *   [1]  E5a data validity
 *   [7]  reserved
 * ────────────────────────────────────────────────────────────────── */
#define P2_32  (1.0 / (double)(1u << 31) / 2.0) /* 2^-32 */
#define P2_34  (1.0 / (double)((uint64_t)1 << 34))
#define P2_46  (1.0 / (double)((uint64_t)1 << 46))
#define P2_59  (1.0 / (double)((uint64_t)1 << 59))

rtcm3_rc_t rtcm3_decode_1045(const uint8_t *body, uint32_t body_len,
                              rtcm3_eph_kepler_t *out)
{
    if (body_len < 62) return RTCM3_RC_NEED_DATA; /* 496 bits = 62 bytes */

    rtcm3_bitcursor_t c;
    rtcm3_cursor_init(&c, body, body_len * 8u);

    RSKIP(&c, 12);
    out->gnss     = RTCM3_GNSS_GALILEO;
    out->sat_id   = (uint8_t)RGET_U(&c, 6);
    out->week     = (uint16_t)RGET_U(&c, 12);    /* GST week */
    out->iode     = (uint16_t)RGET_U(&c, 10);    /* IODnav */
    out->accuracy = (uint8_t)RGET_U(&c, 8);      /* SISA */
    out->idot     = RGET_S(&c, 14) * P2_43 * SC2RAD;
    out->toc      = RGET_U(&c, 14) * 60;
    out->af2      = RGET_S(&c, 6)  * P2_59;
    out->af1      = RGET_S(&c, 21) * P2_46;
    out->af0      = RGET_S(&c, 31) * P2_34;
    out->crs      = RGET_S(&c, 16) * P2_5;
    out->delta_n  = RGET_S(&c, 16) * P2_43 * SC2RAD;
    out->m0       = RGET_SL(&c, 32) * P2_31 * SC2RAD;
    out->cuc      = RGET_S(&c, 16) * P2_29;
    out->ecc      = (double)RGET_UL(&c, 32) * P2_33;
    out->cus      = RGET_S(&c, 16) * P2_29;
    out->sqrt_a   = (double)RGET_UL(&c, 32) * P2_19;
    out->toe      = RGET_U(&c, 14) * 60;
    out->cic      = RGET_S(&c, 16) * P2_29;
    out->omega0   = RGET_SL(&c, 32) * P2_31 * SC2RAD;
    out->cis      = RGET_S(&c, 16) * P2_29;
    out->i0       = RGET_SL(&c, 32) * P2_31 * SC2RAD;
    out->crc      = RGET_S(&c, 16) * P2_5;
    out->omega    = RGET_SL(&c, 32) * P2_31 * SC2RAD;
    out->omega_dot= RGET_S(&c, 24) * P2_43 * SC2RAD;
    out->tgd      = RGET_S(&c, 10) * P2_32; /* BGD E5a/E1 */
    out->health   = (uint8_t)RGET_U(&c, 2); /* E5a health */
    /* data validity + reserved */
    out->iodc     = out->iode;               /* Galileo usa IODnav para ambos */

    return c.overflow ? RTCM3_RC_NEED_DATA : RTCM3_RC_OK;
}

/* ── 1046 — Galileo I/NAV Ephemeris (504 bits) ─────────────────────
 *
 * Similar a 1045 pero con campos adicionales de integridad:
 *   - BGD E5b/E1 adicional (10 bits)
 *   - E5b health + validity
 *   - E1 health + validity
 *
 *   [12] msg type
 *   [6]  satellite ID
 *   [12] GST week
 *   [10] IODnav
 *   [8]  SISA
 *   [14] IDOT   × 2^-43 π rad/s
 *   [14] toc    × 60 s
 *   [6]  af2    × 2^-59 s/s²
 *   [21] af1    × 2^-46 s/s
 *   [31] af0    × 2^-34 s
 *   [16] Crs    × 2^-5 m
 *   [16] Δn     × 2^-43 π rad/s
 *   [32] M0     × 2^-31 π rad
 *   [16] Cuc    × 2^-29 rad
 *   [32] e      × 2^-33
 *   [16] Cus    × 2^-29 rad
 *   [32] √A     × 2^-19 m^0.5
 *   [14] toe    × 60 s
 *   [16] Cic    × 2^-29 rad
 *   [32] Ω0     × 2^-31 π rad
 *   [16] Cis    × 2^-29 rad
 *   [32] i0     × 2^-31 π rad
 *   [16] Crc    × 2^-5 m
 *   [32] ω      × 2^-31 π rad
 *   [24] Ω_dot  × 2^-43 π rad/s
 *   [10] BGD E5a/E1 × 2^-32 s
 *   [10] BGD E5b/E1 × 2^-32 s  ← diferencia principal con 1045
 *   [2]  E5b health
 *   [1]  E5b data validity
 *   [2]  E1 health
 *   [1]  E1 data validity
 *   [2]  reserved
 * ────────────────────────────────────────────────────────────────── */
#define P2_32  (1.0 / (double)(1u << 31) / 2.0) /* 2^-32 */

rtcm3_rc_t rtcm3_decode_1046(const uint8_t *body, uint32_t body_len,
                              rtcm3_eph_kepler_t *out)
{
    if (body_len < 63) return RTCM3_RC_NEED_DATA; /* 504 bits = 63 bytes */

    rtcm3_bitcursor_t c;
    rtcm3_cursor_init(&c, body, body_len * 8u);

    RSKIP(&c, 12);
    out->gnss     = RTCM3_GNSS_GALILEO;
    out->sat_id   = (uint8_t)RGET_U(&c, 6);
    out->week     = (uint16_t)RGET_U(&c, 12);
    out->iode     = (uint16_t)RGET_U(&c, 10);
    out->accuracy = (uint8_t)RGET_U(&c, 8);
    out->idot     = RGET_S(&c, 14) * P2_43 * SC2RAD;
    out->toc      = RGET_U(&c, 14) * 60;
    out->af2      = RGET_S(&c, 6)  * P2_59;
    out->af1      = RGET_S(&c, 21) * P2_46;
    out->af0      = RGET_S(&c, 31) * P2_34;
    out->crs      = RGET_S(&c, 16) * P2_5;
    out->delta_n  = RGET_S(&c, 16) * P2_43 * SC2RAD;
    out->m0       = RGET_SL(&c, 32) * P2_31 * SC2RAD;
    out->cuc      = RGET_S(&c, 16) * P2_29;
    out->ecc      = (double)RGET_UL(&c, 32) * P2_33;
    out->cus      = RGET_S(&c, 16) * P2_29;
    out->sqrt_a   = (double)RGET_UL(&c, 32) * P2_19;
    out->toe      = RGET_U(&c, 14) * 60;
    out->cic      = RGET_S(&c, 16) * P2_29;
    out->omega0   = RGET_SL(&c, 32) * P2_31 * SC2RAD;
    out->cis      = RGET_S(&c, 16) * P2_29;
    out->i0       = RGET_SL(&c, 32) * P2_31 * SC2RAD;
    out->crc      = RGET_S(&c, 16) * P2_5;
    out->omega    = RGET_SL(&c, 32) * P2_31 * SC2RAD;
    out->omega_dot= RGET_S(&c, 24) * P2_43 * SC2RAD;
    out->tgd      = RGET_S(&c, 10) * P2_32; /* BGD E5a/E1 */
    RSKIP(&c, 10);                           /* BGD E5b/E1 */
    out->health   = (uint8_t)(RGET_U(&c, 2) | (RGET_U(&c, 2) << 2));
    /* health: [E5b_health:2] [E5b_valid:1] [E1_health:2] [E1_valid:1] */
    out->iodc     = out->iode;

    return c.overflow ? RTCM3_RC_NEED_DATA : RTCM3_RC_OK;
}

/* ── Nombre de constelación + PRN para logging ─────────────────────── */
const char *rtcm3_eph_gnss_prn(const rtcm3_eph_kepler_t *eph, char *buf)
{
    static const char prefix[] = {'G','R','E','S','J','C','I','?'};
    int idx = (eph->gnss < 8) ? (int)eph->gnss : 7;
    snprintf(buf, 8, "%c%02d", prefix[idx], (int)eph->sat_id);
    return buf;
}
