/*
 * ntrip_common.c — Implementación de helpers y del handler de cliente GET
 */
#include "ntrip_common.h"
#include "auth.h"
#include "sourcetable.h"
#include "../core/broker.h"
#include "../core/logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/epoll.h>

/* ── Parsing de request ──────────────────────────────────────────── */

int str_icase_starts(const char *buf, const char *prefix)
{
    while (*prefix) {
        if (tolower((unsigned char)*buf) != tolower((unsigned char)*prefix))
            return 0;
        buf++; prefix++;
    }
    return 1;
}

const char *find_header(const char *buf, const char *name)
{
    char search[128];
    snprintf(search, sizeof(search), "\r\n%s:", name);
    const char *p = strcasestr(buf, search);
    if (!p) return NULL;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

void copy_until(char *dst, const char *src, char term, int dst_max)
{
    int i = 0;
    while (src[i] && src[i] != term && i < dst_max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* ── I/O ──────────────────────────────────────────────────────────── */

void send_all(int fd, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, data + sent, len - sent);
        if (n <= 0) break;
        sent += (size_t)n;
    }
}

void ntrip_send_resp(int fd, const char *resp, size_t len)
{
    send_all(fd, resp, len);

    /* Primera línea de la respuesta, sin CRLF, para el log */
    char line[128];
    size_t n = 0;
    while (n < len && n < sizeof(line) - 1 &&
           resp[n] != '\r' && resp[n] != '\n') {
        line[n] = resp[n];
        n++;
    }
    line[n] = '\0';
    log_debug("ntrip: fd=%d <- %s", fd, line);
}

void ntrip_debug_request(int fd, const char *buf)
{
    /* Copia solo los headers (hasta \r\n\r\n) con credenciales tapadas.
     *
     * A nivel INFO (no DEBUG) a propósito: el pedido es poder ver la
     * cabecera completa de cada request (SOURCE o cliente) con el nivel
     * de log normal, sin tener que levantar todo a NTRIPCASTER_LOG=debug
     * -- que además loguearía cosas mucho más verbosas (dumps internos
     * futuros, etc.). Las credenciales siempre quedan tapadas acá abajo,
     * así que subir esto a INFO no expone nada sensible. */
    char masked[2048];
    size_t o = 0;
    const char *end = strstr(buf, "\r\n\r\n");
    size_t req_len = end ? (size_t)(end - buf) : strlen(buf);
    if (req_len > sizeof(masked) - 1) req_len = sizeof(masked) - 1;

    for (size_t i = 0; i < req_len && o < sizeof(masked) - 5; ) {
        /* SOURCE <password> ... -> SOURCE *** ... */
        if (i == 0 && str_icase_starts(buf, "SOURCE ")) {
            memcpy(masked + o, buf, 7); o += 7; i += 7;
            while (i < req_len && buf[i] != ' ' && buf[i] != '\r') i++;
            memcpy(masked + o, "***", 3); o += 3;
            continue;
        }
        /* Authorization: Basic xxx -> Basic *** */
        if (str_icase_starts(buf + i, "Authorization:")) {
            const char *eol = memchr(buf + i, '\r', req_len - i);
            size_t hdr_len = eol ? (size_t)(eol - (buf + i)) : req_len - i;
            const char *tag = "Authorization: Basic ***";
            size_t tag_len = strlen(tag);
            if (o + tag_len < sizeof(masked) - 1) {
                memcpy(masked + o, tag, tag_len); o += tag_len;
            }
            i += hdr_len;
            continue;
        }
        masked[o++] = buf[i++];
    }
    masked[o] = '\0';
    log_info("ntrip: fd=%d -> request:\n%s", fd, masked);
}

/* ── Payload pipeleado con el handshake del source ──────────────────
 *
 * Compartido por SOURCE v1 y POST v2: en ambos casos el read() del
 * handshake puede traer, pegados después de "\r\n\r\n", los primeros
 * bytes RTCM3 que el source ya mandó sin esperar la respuesta.
 * Sin esto, esos bytes se pierden en silencio.
 */
void forward_source_payload(io_engine_t *eng, conn_t *conn,
                             const char *buf, size_t total_len)
{
    const char *hdr_end = strstr(buf, "\r\n\r\n");
    if (!hdr_end) return;

    size_t hdr_len = (size_t)(hdr_end - buf) + 4;
    if (hdr_len >= total_len) return;

    const uint8_t *extra     = (const uint8_t *)buf + hdr_len;
    size_t         extra_len = total_len - hdr_len;

    broker_source_data(eng->broker, conn, extra, extra_len);
    io_engine_wakeup_mount_clients(eng, conn->mp);

    log_info("ntrip: source fd=%d pipelined payload recuperado: %zu bytes",
             conn->fd, extra_len);
}

/* ── Cliente GET (v1 y v2 comparten esta misma logica) ──────────────── */

static const char RESP_ICY_200[]  = "ICY 200 OK\r\n\r\n";
static const char RESP_HTTP_200_STREAM[] =
    "HTTP/1.1 200 OK\r\n"
    "Ntrip-Version: Ntrip/2.0\r\n"
    "Content-Type: application/octet-stream\r\n"
    "Cache-Control: no-store\r\n"
    "Connection: close\r\n"
    "\r\n";
static const char RESP_404[] =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Length: 0\r\n"
    "\r\n";
static const char RESP_HTTP_401[] =
    "HTTP/1.1 401 Unauthorized\r\n"
    "WWW-Authenticate: Basic realm=\"NtripCaster\"\r\n"
    "Content-Length: 0\r\n"
    "\r\n";
static const char RESP_ICY_401_CLIENT[] = "ERROR - Bad Password\r\n";

void ntrip_handle_client_get(io_engine_t *eng, conn_t *conn,
                              const char *buf, const char *path,
                              int is_ntrip_v2, int is_browser)
{
    const char *mp = path[0] == '/' ? path + 1 : path;
    snprintf(conn->mountpoint, sizeof(conn->mountpoint), "%s", mp);

    const char *auth_hdr = find_header(buf, "Authorization");
    char user[64] = {0}, pass[128] = {0};
    auth_parse_basic(auth_hdr, user, sizeof(user), pass, sizeof(pass));
    snprintf(conn->user, sizeof(conn->user), "%s", user);

    log_info("ntrip: CLIENT fd=%d mp=%s user=%s v%d addr=%s",
             conn->fd, conn->mountpoint, conn->user,
             is_ntrip_v2 ? 2 : 1, conn->remote_addr);

    if (auth_check_client(conn->mountpoint, user, pass) != 0) {
        log_warn("ntrip: CLIENT auth rechazado fd=%d mp=%s user='%s' addr=%s",
                 conn->fd, conn->mountpoint, user, conn->remote_addr);
        if (is_ntrip_v2) {
            ntrip_send_resp(conn->fd, RESP_HTTP_401, strlen(RESP_HTTP_401));
        } else {
            ntrip_send_resp(conn->fd, RESP_ICY_401_CLIENT, strlen(RESP_ICY_401_CLIENT));
        }
        io_engine_conn_close(eng, conn);
        return;
    }

    if (broker_client_register(eng->broker, conn, conn->mountpoint) != 0) {
        if (is_ntrip_v2 || is_browser) {
            ntrip_send_resp(conn->fd, RESP_404, strlen(RESP_404));
        } else {
            sourcetable_handle_v1(eng, conn);
        }
        return;
    }

    conn->ntrip_version = is_ntrip_v2 ? NTRIP_VERSION_2 : NTRIP_VERSION_1;
    conn->state         = CONN_STATE_CLIENT_ACTIVE;

    if (is_ntrip_v2) {
        ntrip_send_resp(conn->fd, RESP_HTTP_200_STREAM, strlen(RESP_HTTP_200_STREAM));
    } else {
        ntrip_send_resp(conn->fd, RESP_ICY_200, strlen(RESP_ICY_200));
    }

    io_engine_conn_watch(eng, conn,
        EPOLLIN | EPOLLOUT | EPOLLET | EPOLLONESHOT | EPOLLRDHUP);

    log_info("ntrip: client suscrito fd=%d mp=%s", conn->fd, conn->mountpoint);
}
