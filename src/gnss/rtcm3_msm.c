/*
 * rtcm3_msm.c — Decode MSM header RTCM3
 *
 * Layout común del header MSM (antes del satellite_mask):
 *
 * GPS / Galileo / BeiDou / QZSS / NavIC:
 *   [12] message number
 *   [12] station ID
 *   [30] epoch (TOW en ms, módulo 1 semana)
 *   [1]  multiple message
 *   [3]  IODS (issue of data station)
 *   [7]  reserved
 *   [2]  clock steering indicator
 *   [2]  external clock indicator
 *   [1]  GNSS divergence-free smoothing indicator
 *   [3]  GNSS smoothing interval
 *   [64] satellite mask
 *   [32] signal mask
 *   [N_sat × N_sig] cell mask
 *
 * GLONASS (diferente solo en epoch):
 *   [12] message number
 *   [12] station ID
 *   [3]  day of week (GLONASS)
 *   [27] milliseconds from start of day (UTC+3 Moscow time)  ← FIX vs Python
 *   [1]  multiple message
 *   ... (resto igual)
 */
#include "rtcm3_msm.h"
#include "rtcm3_bits.h"

/* ── Identificación de constelación ─────────────────────────────── */

rtcm3_gnss_t rtcm3_msm_gnss(uint16_t msg_type)
{
    if (msg_type >= 1071 && msg_type <= 1077) return RTCM3_GNSS_GPS;
    if (msg_type >= 1081 && msg_type <= 1087) return RTCM3_GNSS_GLONASS;
    if (msg_type >= 1091 && msg_type <= 1097) return RTCM3_GNSS_GALILEO;
    if (msg_type >= 1101 && msg_type <= 1107) return RTCM3_GNSS_SBAS;
    if (msg_type >= 1111 && msg_type <= 1117) return RTCM3_GNSS_QZSS;
    if (msg_type >= 1121 && msg_type <= 1127) return RTCM3_GNSS_BEIDOU;
    if (msg_type >= 1131 && msg_type <= 1137) return RTCM3_GNSS_NAVIC;
    return RTCM3_GNSS_UNKNOWN;
}

const char *rtcm3_gnss_name(rtcm3_gnss_t gnss)
{
    static const char *names[] = {
        "GPS", "GLONASS", "Galileo", "SBAS", "QZSS", "BeiDou", "NavIC", "?"
    };
    return names[(gnss < 8) ? gnss : 7];
}

/* ── Decode MSM header ───────────────────────────────────────────── */

rtcm3_rc_t rtcm3_decode_msm_header(const uint8_t *body, uint32_t body_len,
                                   rtcm3_msm_header_t *out)
{
    /* Mínimo: 12+12+30+1+3+7+2+2+1+3+64+32 = 169 bits = ~22 bytes */
    if (body_len < 22) return RTCM3_RC_NEED_DATA;

    rtcm3_bitcursor_t c;
    rtcm3_cursor_init(&c, body, body_len * 8u);

    uint16_t msg_type  = (uint16_t)RGET_U(&c, 12);
    out->gnss          = rtcm3_msm_gnss(msg_type);
    out->level         = (rtcm3_msm_level_t)(msg_type % 10);
    out->station_id    = (uint16_t)RGET_U(&c, 12);

    /* Epoch — GLONASS usa formato diferente */
    if (out->gnss == RTCM3_GNSS_GLONASS) {
        out->glonass_day = (uint8_t)RGET_U(&c, 3);   /* 3 bits día semana */
        out->epoch_ms    = RGET_U(&c, 27);            /* 27 bits TOD en ms */
    } else {
        out->glonass_day = 0;
        out->epoch_ms    = RGET_U(&c, 30);            /* 30 bits TOW en ms */
    }

    out->multiple_msg  = (uint8_t)RGET_U(&c, 1);
    RSKIP(&c, 3);   /* IODS */
    RSKIP(&c, 7);   /* reserved */
    RSKIP(&c, 2);   /* clock steering */
    RSKIP(&c, 2);   /* external clock */
    out->smoothing     = (uint8_t)RGET_U(&c, 1);
    out->smooth_int    = (uint8_t)RGET_U(&c, 3);

    /* ── Satellite mask (64 bits) ── */
    uint64_t sat_mask  = RGET_UL(&c, 64);
    out->num_satellites = 0;
    for (int i = 0; i < 64; i++) {
        if ((sat_mask >> (63 - i)) & 1u) {
            out->satellite_ids[out->num_satellites++] = (uint8_t)(i + 1);
        }
    }

    /* ── Signal mask (32 bits) ── */
    uint32_t sig_mask  = RGET_U(&c, 32);
    out->num_signals   = 0;
    for (int i = 0; i < 32; i++) {
        if ((sig_mask >> (31 - i)) & 1u) {
            out->signal_ids[out->num_signals++] = (uint8_t)(i + 1);
        }
    }

    /* ── Cell mask (N_sat × N_sig bits) ── */
    int cells = out->num_satellites * out->num_signals;
    out->num_cells = 0;
    for (int i = 0; i < cells; i++) {
        if (RGET_U(&c, 1))
            out->num_cells++;
    }

    if (c.overflow) return RTCM3_RC_NEED_DATA;
    return RTCM3_RC_OK;
}
