/*
 * rtcm3_station.c — Decode mensajes de estación base RTCM3
 */
#include "rtcm3_station.h"
#include "rtcm3_bits.h"
#define _USE_MATH_DEFINES
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── Constantes WGS84 ───────────────────────────────────────────── */
#define WGS84_A    6378137.0
#define WGS84_B    6356752.31424518
#define WGS84_E2   (1.0 - (WGS84_B/WGS84_A)*(WGS84_B/WGS84_A))

/* ── ECEF → LLH (Bowring, 3 iteraciones) ───────────────────────── */
void rtcm3_ecef_to_llh(double x, double y, double z,
                        double *lat_deg, double *lon_deg, double *h_m)
{
    double p   = sqrt(x*x + y*y);
    double lon = atan2(y, x);

    /* Estimación inicial Bowring */
    double lat = atan2(z, p * (1.0 - WGS84_E2));
    for (int i = 0; i < 4; i++) {
        double sin_lat = sin(lat);
        double N = WGS84_A / sqrt(1.0 - WGS84_E2 * sin_lat * sin_lat);
        lat = atan2(z + WGS84_E2 * N * sin_lat, p);
    }

    double sin_lat = sin(lat);
    double N = WGS84_A / sqrt(1.0 - WGS84_E2 * sin_lat * sin_lat);
    double h = (fabs(lat) < 0.8) /* < ~45 deg */
             ? (p / cos(lat)) - N
             : (z / sin_lat) - N * (1.0 - WGS84_E2);

    *lat_deg = lat * (180.0 / M_PI);
    *lon_deg = lon * (180.0 / M_PI);
    if (h_m) *h_m = h;
}

/* ── Helper: leer string ASCII del bitstream ─────────────────────── */
static void read_string(rtcm3_bitcursor_t *c, char *out, int max_len)
{
    int n = (int)RGET_U(c, 8); /* longitud del string */
    if (n >= max_len) n = max_len - 1;
    for (int i = 0; i < n; i++)
        out[i] = (char)(uint8_t)RGET_U(c, 8);
    out[n] = '\0';
    /* Consumir bytes restantes si n fue truncado */
}

/* ── Decode 1005 — Coordenadas ECEF ─────────────────────────────── */

/*
 * Layout mensaje 1005 (148 bits total):
 *   [12] message number (1005)
 *   [12] station ID
 *   [6]  ITRF realization year
 *   [1]  GPS indicator
 *   [1]  GLONASS indicator
 *   [1]  Galileo indicator
 *   [1]  Reference-station indicator
 *   [38] ECEF-X (signed), escala 0.0001 m
 *   [1]  Single receiver oscillator indicator
 *   [1]  Reserved
 *   [38] ECEF-Y (signed), escala 0.0001 m
 *   [2]  Quarter cycle indicator
 *   [38] ECEF-Z (signed), escala 0.0001 m
 */
rtcm3_rc_t rtcm3_decode_1005(const uint8_t *body, uint32_t body_len,
                              rtcm3_coords_t *out)
{
    if (body_len < 19) return RTCM3_RC_NEED_DATA; /* 148 bits = 18.5 bytes */

    rtcm3_bitcursor_t c;
    rtcm3_cursor_init(&c, body, body_len * 8u);

    RSKIP(&c, 12);                          /* message number */
    out->station_id = (uint16_t)RGET_U(&c, 12);
    RSKIP(&c, 6 + 1 + 1 + 1 + 1);          /* ITRF year + indicators */
    out->ecef_x_m  = RGET_SL(&c, 38) * 0.0001;
    RSKIP(&c, 2);                           /* oscillator + reserved */
    out->ecef_y_m  = RGET_SL(&c, 38) * 0.0001;
    RSKIP(&c, 2);                           /* quarter cycle */
    out->ecef_z_m  = RGET_SL(&c, 38) * 0.0001;
    out->antenna_h_m  = 0.0;
    out->has_antenna_h = 0;

    rtcm3_ecef_to_llh(out->ecef_x_m, out->ecef_y_m, out->ecef_z_m,
                      &out->lat_deg, &out->lon_deg, NULL);

    return c.overflow ? RTCM3_RC_NEED_DATA : RTCM3_RC_OK;
}

/* ── Decode 1006 — Coordenadas ECEF + altura antena ─────────────── */

/*
 * Layout mensaje 1006 (164 bits):
 *   Igual que 1005 + [16] antenna height, escala 0.0001 m
 */
rtcm3_rc_t rtcm3_decode_1006(const uint8_t *body, uint32_t body_len,
                              rtcm3_coords_t *out)
{
    /* Reusar 1005 — tiene el mismo header */
    rtcm3_rc_t rc = rtcm3_decode_1005(body, body_len, out);
    if (rc != RTCM3_RC_OK) return rc;
    if (body_len < 21) return RTCM3_RC_NEED_DATA; /* 164 bits = 20.5 bytes */

    /* Offset para antenna height: bit 148 */
    out->antenna_h_m   = (double)rtcm3_getbitu(body, 148, 16) * 0.0001;
    out->has_antenna_h = 1;
    return RTCM3_RC_OK;
}

/* ── Decode 1007 — Descriptor de antena ─────────────────────────── */

rtcm3_rc_t rtcm3_decode_1007(const uint8_t *body, uint32_t body_len,
                              rtcm3_antenna_t *out)
{
    if (body_len < 3) return RTCM3_RC_NEED_DATA;

    rtcm3_bitcursor_t c;
    rtcm3_cursor_init(&c, body, body_len * 8u);

    RSKIP(&c, 12);
    out->station_id = (uint16_t)RGET_U(&c, 12);
    read_string(&c, out->descriptor, RTCM3_STATION_STR_MAX);
    out->setup_id   = (uint8_t)RGET_U(&c, 8);
    out->serial[0]  = '\0';
    out->has_serial = 0;

    return c.overflow ? RTCM3_RC_NEED_DATA : RTCM3_RC_OK;
}

/* ── Decode 1008 — Descriptor de antena + serial ────────────────── */

rtcm3_rc_t rtcm3_decode_1008(const uint8_t *body, uint32_t body_len,
                              rtcm3_antenna_t *out)
{
    rtcm3_rc_t rc = rtcm3_decode_1007(body, body_len, out);
    if (rc != RTCM3_RC_OK) return rc;

    /* Serial viene después del setup_id */
    rtcm3_bitcursor_t c;
    rtcm3_cursor_init(&c, body, body_len * 8u);
    RSKIP(&c, 12 + 12); /* msg_type + station_id */
    int n_desc = (int)RGET_U(&c, 8);
    RSKIP(&c, n_desc * 8 + 8); /* descriptor + setup_id */
    read_string(&c, out->serial, RTCM3_STATION_STR_MAX);
    out->has_serial = 1;

    return c.overflow ? RTCM3_RC_NEED_DATA : RTCM3_RC_OK;
}

/* ── Decode 1033 — Receiver + Antenna Descriptor ─────────────────── */

/*
 * Layout:
 *   [12] message number
 *   [12] station ID
 *   [8+N×8] antenna descriptor string
 *   [8]  setup_id
 *   [8+M×8] antenna serial number string
 *   [8+P×8] receiver type descriptor string
 *   [8+Q×8] receiver firmware version string
 *   [8+R×8] receiver serial number string
 */
rtcm3_rc_t rtcm3_decode_1033(const uint8_t *body, uint32_t body_len,
                              rtcm3_receiver_t *out)
{
    if (body_len < 4) return RTCM3_RC_NEED_DATA;

    rtcm3_bitcursor_t c;
    rtcm3_cursor_init(&c, body, body_len * 8u);

    RSKIP(&c, 12);
    out->station_id = (uint16_t)RGET_U(&c, 12);
    read_string(&c, out->antenna_desc,     RTCM3_STATION_STR_MAX);
    out->setup_id = (uint8_t)RGET_U(&c, 8);
    read_string(&c, out->antenna_serial,   RTCM3_STATION_STR_MAX);
    read_string(&c, out->receiver_desc,    RTCM3_STATION_STR_MAX);
    read_string(&c, out->receiver_firmware,RTCM3_STATION_STR_MAX);
    read_string(&c, out->receiver_serial,  RTCM3_STATION_STR_MAX);

    return c.overflow ? RTCM3_RC_NEED_DATA : RTCM3_RC_OK;
}
