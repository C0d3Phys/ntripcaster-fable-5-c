/*
 * ntrip_v2.c — Implementación del handshake POST v2 (con auth cableada)
 *
 * El POST v2 manda las credenciales por "Authorization: Basic user:pass".
 * El password se valida contra la sección [source] del conf con la misma
 * auth_check_source() que usa ntrip_v1.c (el usuario del Basic se guarda
 * para logging; la ACL v1 de sources es por mountpoint+password).
 */
#include "ntrip_v2.h"
#include "ntrip_common.h"
#include "auth.h"
#include "../core/broker.h"

#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>

static const char RESP_HTTP_200_SOURCE[] =
    "HTTP/1.1 200 OK\r\n"
    "Ntrip-Version: Ntrip/2.0\r\n"
    "\r\n";
static const char RESP_409[] =
    "HTTP/1.1 409 Conflict\r\n"
    "Content-Length: 0\r\n"
    "\r\n";
static const char RESP_401[] =
    "HTTP/1.1 401 Unauthorized\r\n"
    "WWW-Authenticate: Basic realm=\"NtripCaster\"\r\n"
    "Content-Length: 0\r\n"
    "\r\n";

void ntrip_v2_handle_source(io_engine_t *eng, conn_t *conn,
                             const char *buf, size_t total_len,
                             const char *path)
{
    const char *mp = path[0] == '/' ? path + 1 : path;
    snprintf(conn->mountpoint, sizeof(conn->mountpoint), "%s", mp);

    const char *auth_hdr = find_header(buf, "Authorization");
    char user[64] = {0}, pass[128] = {0};
    auth_parse_basic(auth_hdr, user, sizeof(user), pass, sizeof(pass));
    snprintf(conn->user, sizeof(conn->user), "%s", user);

    printf("[ntrip] SOURCE v2 fd=%d mp=%s user=%s\n",
           conn->fd, conn->mountpoint, conn->user);

    if (auth_check_source(conn->mountpoint, pass) != 0) {
        printf("[ntrip] SOURCE v2 fd=%d mp=%s auth rechazado\n",
               conn->fd, conn->mountpoint);
        send_all(conn->fd, RESP_401, strlen(RESP_401));
        io_engine_conn_close(eng, conn);
        return;
    }

    if (broker_source_register(eng->broker, conn, conn->mountpoint) != 0) {
        send_all(conn->fd, RESP_409, strlen(RESP_409));
        io_engine_conn_close(eng, conn);
        return;
    }

    conn->ntrip_version = NTRIP_VERSION_2;
    conn->state         = CONN_STATE_SOURCE_ACTIVE;
    send_all(conn->fd, RESP_HTTP_200_SOURCE, strlen(RESP_HTTP_200_SOURCE));
    printf("[ntrip] source v2 registered  fd=%d mp=%s\n", conn->fd, conn->mountpoint);

    forward_source_payload(eng, conn, buf, total_len);
    io_engine_conn_watch(eng, conn, EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP);
}
