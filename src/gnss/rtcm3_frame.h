/*
 * rtcm3_frame.h — Frame parser RTCM3
 *
 * Estructura de un frame RTCM3:
 *   [0xD3] [6 bits reservados | 10 bits longitud] [body N bytes] [CRC24Q 3 bytes]
 *   Total: 3 + N + 3 bytes
 *
 * Verificado: idéntico a librtcm. 121 frames en Demo1.bin, 9 bytes skip.
 */
#ifndef NTRIPCASTER_RTCM3_FRAME_H
#define NTRIPCASTER_RTCM3_FRAME_H

#include <stddef.h>
#include <stdint.h>

/* Preamble byte */
#define RTCM3_PREAMBLE     0xD3u

/* Máxima longitud del body (10 bits → 1023 bytes) */
#define RTCM3_MAX_BODY     1023u

/* Tamaño mínimo de un frame (preamble + 2 header + 0 body + 3 CRC) */
#define RTCM3_MIN_FRAME    6u

/* ── Tipos ──────────────────────────────────────────────────────── */

typedef enum {
    RTCM3_RC_OK          =  0,
    RTCM3_RC_NEED_DATA   = -1, /* Buffer incompleto — pedir más bytes */
    RTCM3_RC_BAD_CRC     = -2,
    RTCM3_RC_BAD_PREAMBLE= -3,
} rtcm3_rc_t;

/*
 * rtcm3_frame_t — Descripción de un frame encontrado en el stream.
 * NO copia bytes — apunta al buffer original (zero-copy).
 */
typedef struct {
    const uint8_t *data;    /* Puntero al inicio del frame (0xD3) */
    uint32_t       frame_len; /* Longitud total: 3 + body_len + 3 */
    uint32_t       body_len;  /* Longitud del body (0-1023 bytes) */
    uint16_t       msg_type;  /* Tipo de mensaje RTCM3 (12 bits) */
    uint32_t       offset;    /* Offset en el buffer original */
} rtcm3_frame_t;

/* ── API ────────────────────────────────────────────────────────── */

/*
 * rtcm3_crc24q — Calcula CRC24Q sobre N bytes.
 * Polinomio: 0x1864CFB (verificado vs librtcm y Python decoder).
 */
uint32_t rtcm3_crc24q(const uint8_t *buf, size_t len);

/*
 * rtcm3_parse_stream — Extrae frames RTCM3 válidos de un buffer.
 *
 * @buf        Buffer de entrada (stream de bytes del socket)
 * @len        Longitud del buffer
 * @frames     Array de salida para frames encontrados
 * @max_frames Tamaño del array frames[]
 * @out_count  Número de frames encontrados
 * @bytes_used Bytes consumidos del buffer (para avanzar el ring buffer)
 *
 * Retorna RTCM3_RC_OK si procesó hasta el final o encontró frame incompleto.
 * Los frames apuntan dentro de buf (zero-copy) — buf debe permanecer válido.
 */
rtcm3_rc_t rtcm3_parse_stream(const uint8_t *buf, size_t len,
                               rtcm3_frame_t *frames, int max_frames,
                               int *out_count, size_t *bytes_used);

/*
 * rtcm3_validate_frame — Valida CRC de un frame ya encontrado.
 * Para usar cuando se tiene un frame completo en buffer separado.
 */
int rtcm3_validate_frame(const uint8_t *frame_data, uint32_t frame_len);

#endif /* NTRIPCASTER_RTCM3_FRAME_H */
