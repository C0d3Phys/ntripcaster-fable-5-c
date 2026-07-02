/*
 * ntrip_common.h — Helpers y lógica compartida entre NTRIP v1 y v2
 *
 * Todo lo que acá adentro es idéntico sin importar la versión: parsing
 * de headers HTTP, envío de datos, recuperación de payload pipeleado, y
 * el handler de cliente (GET) -- que ya era código único para v1/v2
 * antes de este split, solo diferenciado por qué línea de respuesta
 * manda.
 */
#ifndef NTRIPCASTER_NTRIP_COMMON_H
#define NTRIPCASTER_NTRIP_COMMON_H

#include <stddef.h>
#include "../core/io_engine.h"
#include "../core/conn.h"

/* ── Parsing de request ──────────────────────────────────────────── */
int str_icase_starts(const char *buf, const char *prefix);
const char *find_header(const char *buf, const char *name);
void copy_until(char *dst, const char *src, char term, int dst_max);

/* ── I/O ──────────────────────────────────────────────────────────── */
void send_all(int fd, const char *data, size_t len);

/*
 * forward_source_payload — Recupera bytes RTCM3 que llegaron pegados al
 * handshake del source (pipelining). Usado por SOURCE v1 y POST v2 --
 * debe llamarse ANTES de io_engine_conn_watch() para evitar la race de
 * dos workers escribiendo al ring buffer a la vez (ver ntrip_v1.c /
 * ntrip_v2.c).
 */
void forward_source_payload(io_engine_t *eng, conn_t *conn,
                             const char *buf, size_t total_len);

/*
 * ntrip_handle_client_get — Maneja GET /mountpoint, tanto v1 como v2
 * (comparten toda la lógica, solo cambia qué línea de respuesta y
 * qué formato de rechazo en caso de mountpoint no encontrado).
 */
void ntrip_handle_client_get(io_engine_t *eng, conn_t *conn,
                              const char *buf, const char *path,
                              int is_ntrip_v2, int is_browser);

#endif /* NTRIPCASTER_NTRIP_COMMON_H */
