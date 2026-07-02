/*
 * rtcm3_decode.c — Dispatcher principal de mensajes RTCM3
 *
 * rtcm3_decode() recibe un frame ya validado (CRC OK) y lo
 * decodifica al rtcm3_message_t apropiado según msg_type.
 *
 * El caster solo necesita decodificar:
 *   - 1005/1006: coords para NEAREST routing
 *   - 1033:      nombre del receptor para sourcetable
 *   - MSM:       epoch + satélites para monitoring/logging
 *   - 1019/1020/1042/1044/1045/1046: efemérides para logging
 *
 * Todo lo demás se retransmite intacto (zero-copy).
 */
#include "rtcm3.h"
#include <string.h>

/* ── Dispatch table ──────────────────────────────────────────────── */

rtcm3_rc_t rtcm3_decode(const rtcm3_frame_t *frame, rtcm3_message_t *msg)
{
    memset(msg, 0, sizeof(*msg));
    msg->msg_type = frame->msg_type;
    msg->type     = RTCM3_MSG_UNKNOWN;

    /* Pointer al body del frame (después de los 3 bytes de header) */
    const uint8_t *body     = frame->data + 3;
    uint32_t       body_len = frame->body_len;
    rtcm3_rc_t     rc;

    switch (frame->msg_type) {

    /* ── Coordenadas de la estación base ─── */
    case 1005:
        rc = rtcm3_decode_1005(body, body_len, &msg->coords);
        if (rc == RTCM3_RC_OK) {
            msg->type       = RTCM3_MSG_COORDS;
            msg->station_id = msg->coords.station_id;
        }
        return rc;

    case 1006:
        rc = rtcm3_decode_1006(body, body_len, &msg->coords);
        if (rc == RTCM3_RC_OK) {
            msg->type       = RTCM3_MSG_COORDS;
            msg->station_id = msg->coords.station_id;
        }
        return rc;

    /* ── Descriptor de antena ────────────── */
    case 1007:
        rc = rtcm3_decode_1007(body, body_len, &msg->antenna);
        if (rc == RTCM3_RC_OK) {
            msg->type       = RTCM3_MSG_ANTENNA;
            msg->station_id = msg->antenna.station_id;
        }
        return rc;

    case 1008:
        rc = rtcm3_decode_1008(body, body_len, &msg->antenna);
        if (rc == RTCM3_RC_OK) {
            msg->type       = RTCM3_MSG_ANTENNA;
            msg->station_id = msg->antenna.station_id;
        }
        return rc;

    /* ── Receiver + Antenna Descriptor ───── */
    case 1033:
        rc = rtcm3_decode_1033(body, body_len, &msg->receiver);
        if (rc == RTCM3_RC_OK) {
            msg->type       = RTCM3_MSG_RECEIVER;
            msg->station_id = msg->receiver.station_id;
        }
        return rc;

    /* ── Efemérides GPS ─────────────────── */
    case 1019:
        rc = rtcm3_decode_1019(body, body_len, &msg->eph);
        if (rc == RTCM3_RC_OK) msg->type = RTCM3_MSG_EPH_GPS;
        return rc;

    /* ── Efemérides GLONASS ─────────────── */
    case 1020:
        rc = rtcm3_decode_1020(body, body_len, &msg->eph_glo);
        if (rc == RTCM3_RC_OK) msg->type = RTCM3_MSG_EPH_GLO;
        return rc;

    /* ── Efemérides BeiDou ──────────────── */
    case 1042:
        rc = rtcm3_decode_1042(body, body_len, &msg->eph);
        if (rc == RTCM3_RC_OK) msg->type = RTCM3_MSG_EPH_BDS;
        return rc;

    /* ── Efemérides QZSS ────────────────── */
    case 1044:
        rc = rtcm3_decode_1044(body, body_len, &msg->eph);
        if (rc == RTCM3_RC_OK) msg->type = RTCM3_MSG_EPH_QZSS;
        return rc;

    /* ── Efemérides Galileo F/NAV ────────── */
    case 1045:
        rc = rtcm3_decode_1045(body, body_len, &msg->eph);
        if (rc == RTCM3_RC_OK) msg->type = RTCM3_MSG_EPH_GAL;
        return rc;

    /* ── Efemérides Galileo I/NAV ────────── */
    case 1046:
        rc = rtcm3_decode_1046(body, body_len, &msg->eph);
        if (rc == RTCM3_RC_OK) msg->type = RTCM3_MSG_EPH_GAL;
        return rc;

    /* ── Mensajes MSM (1071-1137) ────────── */
    default:
        if (rtcm3_is_msm(frame->msg_type)) {
            rc = rtcm3_decode_msm_header(body, body_len, &msg->msm);
            if (rc == RTCM3_RC_OK) {
                msg->type       = RTCM3_MSG_MSM;
                msg->station_id = msg->msm.station_id;
            }
            return rc;
        }

        /* Legado 1001-1012 (GPS/GLONASS legacy observations) */
        if (frame->msg_type >= 1001 && frame->msg_type <= 1012) {
            msg->type = RTCM3_MSG_LEGACY;
            /* station_id está en bits 12-23 para todos los legacy */
            if (body_len >= 3)
                msg->station_id = (uint16_t)rtcm3_getbitu(body, 12, 12);
        }

        /* No soportado: se retransmite intacto sin decode */
        return RTCM3_RC_OK;
    }
}

/* ── Nombres para logging ──────────────────────────────────────── */
const char *rtcm3_msg_category_name(rtcm3_msg_category_t cat)
{
    static const char *names[] = {
        "unknown",   /* 0 */
        "coords",    /* 1 */
        "antenna",   /* 2 */
        "receiver",  /* 3 */
        "msm",       /* 4 */
        "eph-gps",   /* 5 */
        "eph-glo",   /* 6 */
        "eph-bds",   /* 7 */
        "eph-qzss",  /* 8 */
        "eph-gal",   /* 9 */
        "legacy",    /* 10 */
    };
    int i = (int)cat;
    return (i >= 0 && i <= 10) ? names[i] : "?";
}
