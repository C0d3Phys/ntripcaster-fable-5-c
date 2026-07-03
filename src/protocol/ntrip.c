/*
 * ntrip.c — Dispatcher del protocolo NTRIP v1/v2
 *
 * Solo decide QUÉ manejador llamar según el request. La implementación
 * de cada rama vive en su propio archivo:
 *   ntrip_common.c  — helpers compartidos + GET cliente (v1 y v2)
 *   ntrip_v1.c      — SOURCE (v1)
 *   ntrip_v2.c      — POST source (v2)
 *   sourcetable.c   — sourcetable en sus 3 variantes (v1, v2, html)
 */
#include "ntrip.h"
#include "ntrip_common.h"
#include "ntrip_v1.h"
#include "ntrip_v2.h"
#include "sourcetable.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char RESP_404[] =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Length: 0\r\n"
    "\r\n";

void ntrip_handle_request(io_engine_t *eng, conn_t *conn)
{
    const char *buf = (const char *)conn->read_buf;
    size_t      total_len = conn->read_len;

    /* Debug: request completo con credenciales enmascaradas
     * (activar con NTRIPCASTER_LOG=debug) */
    ntrip_debug_request(conn->fd, buf);

    int is_ntrip_v2 = (strcasestr(buf, "Ntrip-Version:") != NULL);
    int is_browser  = 0;
    const char *ua  = find_header(buf, "User-Agent");
    if (ua && str_icase_starts(ua, "Mozilla")) is_browser = 1;

    if (str_icase_starts(buf, "SOURCE ")) {
        ntrip_v1_handle_source(eng, conn, buf, total_len);
        return;
    }

    char method[16], path[64], version[32];
    method[0] = path[0] = version[0] = '\0';
    sscanf(buf, "%15s %63s %31s", method, path, version);

    if (strcmp(path, "/") == 0 || path[0] == '\0') {
        if (is_browser) {
            sourcetable_handle_v2(eng, conn, 1);
        } else if (is_ntrip_v2) {
            sourcetable_handle_v2(eng, conn, 0);
        } else {
            sourcetable_handle_v1(eng, conn);
        }
        return;
    }

    if (strcasecmp(method, "POST") == 0) {
        ntrip_v2_handle_source(eng, conn, buf, total_len, path);
        return;
    }

    if (strcasecmp(method, "GET") != 0) {
        ntrip_send_resp(conn->fd, RESP_404, strlen(RESP_404));
        io_engine_conn_close(eng, conn);
        return;
    }

    ntrip_handle_client_get(eng, conn, buf, path, is_ntrip_v2, is_browser);
}
