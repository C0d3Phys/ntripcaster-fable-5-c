/*
 * ntrip_v1.h — Handshake NTRIP v1 (SOURCE)
 *
 * El GET de cliente v1 vive en ntrip_common.c porque es código idéntico
 * al de v2 (ver ntrip_common.h). Acá solo queda lo que es genuinamente
 * v1-only: el comando SOURCE, formato ICY, sin HTTP.
 */
#ifndef NTRIPCASTER_NTRIP_V1_H
#define NTRIPCASTER_NTRIP_V1_H

#include <stddef.h>
#include "../core/io_engine.h"
#include "../core/conn.h"

void ntrip_v1_handle_source(io_engine_t *eng, conn_t *conn,
                             const char *buf, size_t total_len);

#endif /* NTRIPCASTER_NTRIP_V1_H */
