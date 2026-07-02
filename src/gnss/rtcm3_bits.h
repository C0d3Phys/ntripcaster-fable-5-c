/*
 * rtcm3_bits.h — Bit extraction para mensajes RTCM3
 *
 * Equivalente a libswiftnav/bits.c pero sin dependencias externas.
 * Todo inline — no requiere .c correspondiente.
 *
 * RTCM3 usa big-endian bit ordering:
 *   bit 0 = MSB del byte 0
 *   bit 7 = LSB del byte 0
 *   bit 8 = MSB del byte 1
 *   ...
 */
#ifndef NTRIPCASTER_RTCM3_BITS_H
#define NTRIPCASTER_RTCM3_BITS_H

#include <stdint.h>

/* ── Lectura de bits sin signo ──────────────────────────────────── */

static inline uint32_t rtcm3_getbitu(const uint8_t *buf,
                                     uint32_t pos, uint8_t n)
{
    uint32_t val = 0;
    for (uint8_t i = 0; i < n; i++) {
        uint32_t byte = (pos + i) >> 3;           /* byte index */
        uint32_t bit  = 7u - ((pos + i) & 0x07u); /* bit within byte */
        val = (val << 1u) | ((buf[byte] >> bit) & 1u);
    }
    return val;
}

static inline uint64_t rtcm3_getbitul(const uint8_t *buf,
                                      uint32_t pos, uint8_t n)
{
    uint64_t val = 0;
    for (uint8_t i = 0; i < n; i++) {
        uint32_t byte = (pos + i) >> 3;
        uint32_t bit  = 7u - ((pos + i) & 0x07u);
        val = (val << 1u) | ((buf[byte] >> bit) & 1u);
    }
    return val;
}

/* ── Lectura de bits con signo (complemento a 2) ────────────────── */

static inline int32_t rtcm3_getbits(const uint8_t *buf,
                                    uint32_t pos, uint8_t n)
{
    uint32_t val = rtcm3_getbitu(buf, pos, n);
    /* Extensión de signo */
    if (n > 0 && (val >> (n - 1u)) & 1u)
        val |= (~0u << n);
    return (int32_t)val;
}

static inline int64_t rtcm3_getbitsl(const uint8_t *buf,
                                     uint32_t pos, uint8_t n)
{
    uint64_t val = rtcm3_getbitul(buf, pos, n);
    if (n > 0 && (val >> (n - 1u)) & 1u)
        val |= (~(uint64_t)0u << n);
    return (int64_t)val;
}

/* ── Cursor de lectura (stateful, evita pasar pos manualmente) ───── */

typedef struct {
    const uint8_t *buf;
    uint32_t       pos;   /* bit actual */
    uint32_t       len;   /* bits totales disponibles */
    int            overflow; /* 1 si se intentó leer más de len bits */
} rtcm3_bitcursor_t;

static inline void rtcm3_cursor_init(rtcm3_bitcursor_t *c,
                                     const uint8_t *buf, uint32_t len_bits)
{
    c->buf      = buf;
    c->pos      = 0;
    c->len      = len_bits;
    c->overflow = 0;
}

static inline uint32_t rtcm3_cursor_getbitu(rtcm3_bitcursor_t *c, uint8_t n)
{
    if (c->pos + n > c->len) { c->overflow = 1; return 0; }
    uint32_t v = rtcm3_getbitu(c->buf, c->pos, n);
    c->pos += n;
    return v;
}

static inline uint64_t rtcm3_cursor_getbitul(rtcm3_bitcursor_t *c, uint8_t n)
{
    if (c->pos + n > c->len) { c->overflow = 1; return 0; }
    uint64_t v = rtcm3_getbitul(c->buf, c->pos, n);
    c->pos += n;
    return v;
}

static inline int32_t rtcm3_cursor_getbits(rtcm3_bitcursor_t *c, uint8_t n)
{
    if (c->pos + n > c->len) { c->overflow = 1; return 0; }
    int32_t v = rtcm3_getbits(c->buf, c->pos, n);
    c->pos += n;
    return v;
}

static inline int64_t rtcm3_cursor_getbitsl(rtcm3_bitcursor_t *c, uint8_t n)
{
    if (c->pos + n > c->len) { c->overflow = 1; return 0; }
    int64_t v = rtcm3_getbitsl(c->buf, c->pos, n);
    c->pos += n;
    return v;
}

static inline void rtcm3_cursor_skip(rtcm3_bitcursor_t *c, uint8_t n)
{
    c->pos += n;
}

/* Macros de conveniencia */
#define RGET_U(c, n)  rtcm3_cursor_getbitu((c),  (n))
#define RGET_UL(c, n) rtcm3_cursor_getbitul((c), (n))
#define RGET_S(c, n)  rtcm3_cursor_getbits((c),  (n))
#define RGET_SL(c, n) rtcm3_cursor_getbitsl((c), (n))
#define RSKIP(c, n)   rtcm3_cursor_skip((c), (n))

#endif /* NTRIPCASTER_RTCM3_BITS_H */
