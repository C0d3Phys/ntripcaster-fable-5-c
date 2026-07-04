#include "ntrip_tool_common.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void usage(const char *program)
{
    printf("Usage:\n"
           "  %s UP_HOST UP_PORT UP_MOUNT UP_USER LOCAL_HOST LOCAL_PORT LOCAL_MOUNT\n\n"
           "Required environment variables:\n"
           "  UPSTREAM_PASS       Password used to consume the upstream caster\n"
           "  LOCAL_SOURCE_PASS   SOURCE password configured in the local caster\n\n"
           "Example:\n"
           "  UPSTREAM_PASS=secret LOCAL_SOURCE_PASS=passbase123 \\\n+  %s caster.example 2101 REMOTE user 127.0.0.1 2101 BASE1\n",
           program, program);
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
    if (argc != 8) {
        usage(argv[0]);
        return 2;
    }
    const char *up_pass = getenv("UPSTREAM_PASS");
    const char *local_pass = getenv("LOCAL_SOURCE_PASS");
    if (!up_pass || !*up_pass || !local_pass || !*local_pass) {
        fprintf(stderr, "UPSTREAM_PASS and LOCAL_SOURCE_PASS are required\n");
        return 2;
    }

    nt_install_signal_handlers();
    ntrip_response_t upstream_response;
    int upstream = open_upstream(argv[1], argv[2], argv[3], argv[4],
                                 up_pass, &upstream_response);
    if (upstream < 0) return 1;
    int local = open_local_source(argv[5], argv[6], argv[7], local_pass);
    if (local < 0) {
        close(upstream);
        return 1;
    }

    uint64_t total = 0;
    if (upstream_response.payload_len) {
        if (nt_send_all(local, upstream_response.payload,
                        upstream_response.payload_len) != 0) goto failed;
        total += upstream_response.payload_len;
    }
    fprintf(stderr, "relay active: %s:%s/%s -> %s:%s/%s\n",
            argv[1], argv[2], argv[3], argv[5], argv[6], argv[7]);

    uint8_t buf[64 * 1024];
    while (nt_running()) {
        ssize_t n = recv(upstream, buf, sizeof(buf), 0);
        if (n > 0) {
            if (nt_send_all(local, buf, (size_t)n) != 0) goto failed;
            total += (uint64_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        fprintf(stderr, "upstream stream ended after %llu bytes\n",
                (unsigned long long)total);
        break;
    }
    close(local);
    close(upstream);
    return 0;

failed:
    fprintf(stderr, "relay output failed after %llu bytes\n",
            (unsigned long long)total);
    close(local);
    close(upstream);
    return 1;
}
