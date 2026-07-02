/*
 * ntrip_v2.h — Handshake NTRIP v2 (POST source)
 *
 * El GET de cliente v2 vive en ntrip_common.c (compartido con v1).
 * Acá solo queda POST -- el registro de un source vía HTTP/2.0.
 */
#ifndef NTRIPCASTER_NTRIP_V2_H
#define NTRIPCASTER_NTRIP_V2_H

#include <stddef.h>
#include "../core/io_engine.h"
#include "../core/conn.h"

void ntrip_v2_handle_source(io_engine_t *eng, conn_t *conn,
                             const char *buf, size_t total_len,
                             const char *path);

#endif /* NTRIPCASTER_NTRIP_V2_H */
