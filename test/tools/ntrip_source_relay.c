#include "ntrip_tool_common.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static void usage(const char *program)
{
    printf("Usage:\n"
           "  %s --config FILE\n"
           "  %s UP_HOST UP_PORT UP_MOUNT UP_USER LOCAL_HOST LOCAL_PORT LOCAL_MOUNT\n\n"
           "Required environment variables:\n"
           "  UPSTREAM_PASS       Password used to consume the upstream caster\n"
           "  LOCAL_SOURCE_PASS   SOURCE password configured in the local caster\n\n"
           "Example:\n"
           "  UPSTREAM_PASS=secret LOCAL_SOURCE_PASS=passbase123 \\\n+  %s caster.example 2101 REMOTE user 127.0.0.1 2101 BASE1\n",
           program, program, program);
}

static int open_upstream(const char *host, const char *port,
                         const char *mount, const char *user,
                         const char *password, ntrip_response_t *response)
{
    int fd = nt_connect(host, port);
    if (fd < 0) return -1;

    char basic[768];
    char request[2048];
    if (nt_base64_basic(user, password, basic, sizeof(basic)) != 0) {
        close(fd);
        return -1;
    }
    int n = snprintf(request, sizeof(request),
        "GET /%s HTTP/1.0\r\n"
        "User-Agent: NTRIP ntripcaster-test-relay/1.0\r\n"
        "Authorization: Basic %s\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n\r\n", mount, basic);
    if (n < 0 || (size_t)n >= sizeof(request) ||
        nt_send_all(fd, request, (size_t)n) != 0 ||
        nt_read_response(fd, response) != 0 || !nt_response_ok(response)) {
        fprintf(stderr, "upstream rejected request%s%s\n",
                response->header_len ? ": " : "",
                response->header_len ? response->header : "");
        close(fd);
        return -1;
    }
    return fd;
}

static int open_local_source(const char *host, const char *port,
                             const char *mount, const char *password)
{
    int fd = nt_connect(host, port);
    if (fd < 0) return -1;
    char request[1024];
    int n = snprintf(request, sizeof(request),
        "SOURCE %s /%s\r\n"
        "Source-Agent: ntripcaster-test-relay/1.0\r\n\r\n",
        password, mount);
    ntrip_response_t response;
    if (n < 0 || (size_t)n >= sizeof(request) ||
        nt_send_all(fd, request, (size_t)n) != 0 ||
        nt_read_response(fd, &response) != 0 || !nt_response_ok(&response)) {
        fprintf(stderr, "local caster rejected SOURCE%s%s\n",
                response.header_len ? ": " : "",
                response.header_len ? response.header : "");
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(argv[0]);
        return 0;
    }
    ntrip_tool_config_t config;
    memset(&config, 0, sizeof(config));
    snprintf(config.capture_root, sizeof(config.capture_root),
             "capture_rtcm3_bin_UTC");
    if (argc == 3 && strcmp(argv[1], "--config") == 0) {
        if (nt_config_load(argv[2], &config) != 0 ||
            nt_config_validate_relay(&config) != 0) return 2;
    } else if (argc != 8) {
        usage(argv[0]);
        return 2;
    }
    if (argc == 8) {
        snprintf(config.upstream_host, sizeof(config.upstream_host), "%s", argv[1]);
        snprintf(config.upstream_port, sizeof(config.upstream_port), "%s", argv[2]);
        snprintf(config.upstream_mount, sizeof(config.upstream_mount), "%s", argv[3]);
        snprintf(config.upstream_user, sizeof(config.upstream_user), "%s", argv[4]);
        snprintf(config.local_host, sizeof(config.local_host), "%s", argv[5]);
        snprintf(config.local_port, sizeof(config.local_port), "%s", argv[6]);
        snprintf(config.local_mount, sizeof(config.local_mount), "%s", argv[7]);
        const char *up_pass = getenv("UPSTREAM_PASS");
        const char *local_pass = getenv("LOCAL_SOURCE_PASS");
        if (!up_pass || !*up_pass || !local_pass || !*local_pass) {
            fprintf(stderr, "UPSTREAM_PASS and LOCAL_SOURCE_PASS are required\n");
            return 2;
        }
        snprintf(config.upstream_password, sizeof(config.upstream_password), "%s", up_pass);
        snprintf(config.source_password, sizeof(config.source_password), "%s", local_pass);
    }

    nt_install_signal_handlers();
    ntrip_response_t upstream_response;
    int upstream = open_upstream(config.upstream_host, config.upstream_port,
                                 config.upstream_mount, config.upstream_user,
                                 config.upstream_password, &upstream_response);
    if (upstream < 0) return 1;
    int local = open_local_source(config.local_host, config.local_port,
                                  config.local_mount, config.source_password);
    if (local < 0) {
        close(upstream);
        return 1;
    }

    char session[1024], capture_path[1200];
    if (nt_capture_new_session(config.capture_root, session, sizeof(session)) != 0 ||
        snprintf(capture_path, sizeof(capture_path), "%s/relay_rtcm3.bin", session) >=
            (int)sizeof(capture_path)) {
        close(local); close(upstream); return 1;
    }
    FILE *capture = fopen(capture_path, "wb");
    if (!capture) {
        perror(capture_path); close(local); close(upstream); return 1;
    }
    long long started = (long long)time(NULL);

    /*
     * Timeout de recepcion en el socket upstream -- SOLO a partir de
     * aca (no antes del handshake/nt_read_response, que ya maneja sus
     * propios reintentos y no espera este comportamiento).
     *
     * Sin esto, si el mount upstream se queda sin source (el feeder
     * murio, el receptor GNSS se desconecto, etc.) la conexion GET de
     * este relay NO se cierra sola -- el caster no desconecta clientes
     * solo porque el source desaparecio. recv() se queda bloqueado
     * para siempre esperando datos que no van a llegar, y
     * while(nt_running()) nunca se vuelve a evaluar porque nunca
     * retorna: SIGINT/SIGTERM solo marcan el flag pero no interrumpen
     * el recv() bloqueado (bug real encontrado armando el harness de
     * test: el relay necesitaba kill -9 para morir, y perdia hasta 4KB
     * de captura sin flushear en el proceso).
     *
     * Con este timeout, recv() vuelve cada 1s con EAGAIN/EWOULDBLOCK
     * aunque no haya datos, dandole al loop la chance de re-chequear
     * nt_running() y salir limpio (fclose + meta) en <=1s tras la senal.
     */
    struct timeval rcv_tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(upstream, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));

    uint64_t total = 0;
    if (upstream_response.payload_len) {
        if (nt_send_all(local, upstream_response.payload,
                        upstream_response.payload_len) != 0) goto failed;
        total += upstream_response.payload_len;
        fwrite(upstream_response.payload, 1, upstream_response.payload_len, capture);
        fflush(capture);
    }
    fprintf(stderr, "relay active: %s:%s/%s -> %s:%s/%s\n",
            config.upstream_host, config.upstream_port, config.upstream_mount,
            config.local_host, config.local_port, config.local_mount);

    uint8_t buf[64 * 1024];
    while (nt_running()) {
        ssize_t n = recv(upstream, buf, sizeof(buf), 0);
        if (n > 0) {
            if (nt_send_all(local, buf, (size_t)n) != 0) goto failed;
            total += (uint64_t)n;
            fwrite(buf, 1, (size_t)n, capture);
            /* fflush por chunk: cuesta poco (1 syscall write() por
             * chunk de red, no por byte) y garantiza que un kill
             * abrupto o el timeout de arriba nunca pierdan datos que
             * ya se le mandaron al caster local -- clave para que las
             * capturas de relay/rover sean comparables byte a byte. */
            fflush(capture);
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        fprintf(stderr, "upstream stream ended after %llu bytes\n",
                (unsigned long long)total);
        break;
    }
    fclose(capture);
    nt_capture_write_meta(session, "relay", started, (long long)time(NULL),
                          (unsigned long long)total);
    close(local);
    close(upstream);
    return 0;

failed:
    fprintf(stderr, "relay output failed after %llu bytes\n",
            (unsigned long long)total);
    fclose(capture);
    nt_capture_write_meta(session, "relay", started, (long long)time(NULL),
                          (unsigned long long)total);
    close(local);
    close(upstream);
    return 1;
}
