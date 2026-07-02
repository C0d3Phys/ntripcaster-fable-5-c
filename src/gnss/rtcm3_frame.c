/*
 * rtcm3_frame.c — Frame parser y CRC24Q
 *
 * CRC24Q validado:
 *   Python decoder: 121 frames Demo1.bin ✓
 *   librtcm test  : 121 frames Demo1.bin ✓
 *   Este decoder  : mismo algoritmo
 */
#include "rtcm3_frame.h"
#include <string.h>

/* ── CRC24Q ─────────────────────────────────────────────────────── */

#define CRC24Q_POLY 0x1864CFBU

uint32_t rtcm3_crc24q(const uint8_t *buf, size_t len)
{
    uint32_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)buf[i] << 16u;
        for (int j = 0; j < 8; j++) {
            crc <<= 1u;
            if (crc & 0x1000000u)
                crc ^= CRC24Q_POLY;
        }
    }
    return crc & 0xFFFFFFu;
}

/* ── Helpers internos ───────────────────────────────────────────── */

/* Lee la longitud del body del header RTCM3 (bytes 1 y 2 del frame) */
static inline uint32_t frame_body_len(const uint8_t *p)
{
    return ((uint32_t)(p[1] & 0x03u) << 8u) | (uint32_t)p[2];
}

/* Lee el tipo de mensaje de los primeros 12 bits del body */
static inline uint16_t frame_msg_type(const uint8_t *body)
{
    return (uint16_t)(((uint16_t)body[0] << 4u) | (body[1] >> 4u));
}

/* Verifica el CRC del frame completo */
int rtcm3_validate_frame(const uint8_t *frame_data, uint32_t frame_len)
{
    if (frame_len < RTCM3_MIN_FRAME) return 0;
    uint32_t crc_rx   = ((uint32_t)frame_data[frame_len - 3] << 16u)
                      | ((uint32_t)frame_data[frame_len - 2] <<  8u)
                      |  (uint32_t)frame_data[frame_len - 1];
    uint32_t crc_calc = rtcm3_crc24q(frame_data, frame_len - 3u);
    return (crc_rx == crc_calc) ? 1 : 0;
}

/* ── Parser principal ───────────────────────────────────────────── */

rtcm3_rc_t rtcm3_parse_stream(const uint8_t *buf, size_t len,
                               rtcm3_frame_t *frames, int max_frames,
                               int *out_count, size_t *bytes_used)
{
    *out_count = 0;
    *bytes_used = 0;

    size_t i = 0;

    while (i < len) {
        /* 1. Buscar preamble 0xD3 */
        if (buf[i] != RTCM3_PREAMBLE) {
            i++;
            continue;
        }

        /* 2. Necesitamos al menos 3 bytes para leer header */
        if (i + 3u > len) {
            /* Frame incompleto — detenerse, pedir más datos */
            break;
        }

        /* 3. Leer longitud del body */
        uint32_t body_len  = frame_body_len(buf + i);
        uint32_t frame_len = 3u + body_len + 3u;

        /* 4. Verificar que el frame completo está en el buffer */
        if (i + frame_len > len) {
            break; /* Necesitamos más datos */
        }

        /* 5. Validar CRC */
        if (!rtcm3_validate_frame(buf + i, frame_len)) {
            /* CRC inválido — no es un frame real, avanzar 1 byte */
            i++;
            continue;
        }

        /* 6. Frame válido */
        if (*out_count < max_frames) {
            const uint8_t *body  = buf + i + 3u;
            frames[*out_count].data      = buf + i;
            frames[*out_count].frame_len = frame_len;
            frames[*out_count].body_len  = body_len;
            frames[*out_count].msg_type  = (body_len >= 2u)
                                          ? frame_msg_type(body) : 0u;
            frames[*out_count].offset    = (uint32_t)i;
            (*out_count)++;
        }

        *bytes_used = i + frame_len;
        i += frame_len;
    }

    /* Avanzar bytes_used hasta el último frame completo procesado */
    if (*bytes_used == 0 && *out_count == 0) {
        /* Si no encontramos nada pero recorrimos el buffer,
         * consumir todo excepto los últimos 5 bytes (posible preamble incompleto) */
        *bytes_used = (len > RTCM3_MIN_FRAME) ? (len - RTCM3_MIN_FRAME) : 0;
    }

    return RTCM3_RC_OK;
}
