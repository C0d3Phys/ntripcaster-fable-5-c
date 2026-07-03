/*
 * ntrip_v1.c — Implementación del handshake SOURCE v1 (con auth cableada)
 */
#include "ntrip_v1.h"
#include "ntrip_common.h"
#include "auth.h"
#include "../core/broker.h"
#include "../core/logger.h"

#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>

static const char RESP_ICY_200[] = "ICY 200 OK\r\n\r\n";
static const char RESP_ICY_401[] = "ERROR - Bad Password\r\n";

void ntrip_v1_handle_source(io_engine_t *eng, conn_t *conn,
                             const char *buf, size_t total_len)
{
    char password[128] = {0};
    char path[64]       = {0};
    sscanf(buf, "SOURCE %127s %63s", password, path);

    /*
     * Algunos clientes/relays (confirmado con un relay real de SNIP)
     * mandan el mountpoint con "/" adelante en el propio SOURCE, ej:
     * "SOURCE pass /Demo1". GET (cliente) y POST (source v2) ya le
     * quitan la barra antes de guardar el nombre -- sin este strip
     * acá, un source registrado como "/Demo1" nunca hace match con un
     * cliente pidiendo "Demo1".
     */
    const char *mp_v1 = path[0] == '/' ? path + 1 : path;
    snprintf(conn->mountpoint, sizeof(conn->mountpoint), "%s", mp_v1);
    snprintf(conn->user,       sizeof(conn->user), "source");

    log_info("ntrip: SOURCE v1 fd=%d mp=%s addr=%s",
             conn->fd, conn->mountpoint, conn->remote_addr);

    if (auth_check_source(conn->mountpoint, password) != 0) {
        log_warn("ntrip: SOURCE v1 auth rechazado fd=%d mp=%s addr=%s",
                 conn->fd, conn->mountpoint, conn->remote_addr);
        ntrip_send_resp(conn->fd, RESP_ICY_401, strlen(RESP_ICY_401));
        io_engine_conn_close(eng, conn);
        return;
    }

    if (broker_source_register(eng->broker, conn, conn->mountpoint) != 0) {
        ntrip_send_resp(conn->fd, RESP_ICY_401, strlen(RESP_ICY_401));
        io_engine_conn_close(eng, conn);
        return;
    }

    conn->ntrip_version = NTRIP_VERSION_1;
    conn->state         = CONN_STATE_SOURCE_ACTIVE;
    ntrip_send_resp(conn->fd, RESP_ICY_200, strlen(RESP_ICY_200));
    log_info("ntrip: source v1 registrado fd=%d mp=%s", conn->fd, conn->mountpoint);

    /* Debe ir ANTES de conn_watch -- ver comentario en forward_source_payload */
    forward_source_payload(eng, conn, buf, total_len);
    io_engine_conn_watch(eng, conn, EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP);
}
