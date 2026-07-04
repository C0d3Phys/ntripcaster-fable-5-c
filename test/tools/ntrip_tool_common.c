#include "ntrip_tool_common.h"

#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ini.h"

static volatile sig_atomic_t g_running = 1;

static void stop_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

void nt_install_signal_handlers(void)
{
    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
    signal(SIGPIPE, SIG_IGN);
}

int nt_running(void) { return g_running != 0; }
void nt_stop(void) { g_running = 0; }

static void copy_value(char *dst, size_t size, const char *value)
{
    snprintf(dst, size, "%s", value);
}

static int config_handler(void *user, const char *section,
                          const char *name, const char *value)
{
    ntrip_tool_config_t *c = user;
    if (strcmp(section, "upstream") == 0) {
        if      (strcmp(name, "host") == 0)     copy_value(c->upstream_host, sizeof(c->upstream_host), value);
        else if (strcmp(name, "port") == 0)     copy_value(c->upstream_port, sizeof(c->upstream_port), value);
        else if (strcmp(name, "mountpoint") == 0) copy_value(c->upstream_mount, sizeof(c->upstream_mount), value);
        else if (strcmp(name, "user") == 0)     copy_value(c->upstream_user, sizeof(c->upstream_user), value);
        else if (strcmp(name, "password") == 0) copy_value(c->upstream_password, sizeof(c->upstream_password), value);
        else return 0;
        return 1;
    }
    if (strcmp(section, "local") == 0) {
        if      (strcmp(name, "host") == 0)            copy_value(c->local_host, sizeof(c->local_host), value);
        else if (strcmp(name, "port") == 0)            copy_value(c->local_port, sizeof(c->local_port), value);
        else if (strcmp(name, "mountpoint") == 0)      copy_value(c->local_mount, sizeof(c->local_mount), value);
        else if (strcmp(name, "source_password") == 0) copy_value(c->source_password, sizeof(c->source_password), value);
        else return 0;
        return 1;
    }
    if (strcmp(section, "rover") == 0) {
        if      (strcmp(name, "user") == 0)     copy_value(c->rover_user, sizeof(c->rover_user), value);
        else if (strcmp(name, "password") == 0) copy_value(c->rover_password, sizeof(c->rover_password), value);
        else if (strcmp(name, "seconds") == 0)  c->rover_seconds = strtol(value, NULL, 10);
        else if (strcmp(name, "output") == 0)   copy_value(c->rover_output, sizeof(c->rover_output), value);
        else return 0;
        return 1;
    }
    return 0;
}

int nt_config_load(const char *path, ntrip_tool_config_t *config)
{
    memset(config, 0, sizeof(*config));
    copy_value(config->upstream_port, sizeof(config->upstream_port), "2101");
    copy_value(config->local_host, sizeof(config->local_host), "127.0.0.1");
    copy_value(config->local_port, sizeof(config->local_port), "2101");
    int rc = ini_parse(path, config_handler, config);
    if (rc < 0) fprintf(stderr, "cannot open config '%s'\n", path);
    else if (rc > 0) fprintf(stderr, "invalid config '%s' at line %d\n", path, rc);
    return rc == 0 ? 0 : -1;
}

static int required(const char *value, const char *key)
{
    if (value[0]) return 0;
    fprintf(stderr, "missing required config key: %s\n", key);
    return -1;
}

int nt_config_validate_relay(const ntrip_tool_config_t *c)
{
    int rc = 0;
    rc |= required(c->upstream_host, "upstream.host");
    rc |= required(c->upstream_port, "upstream.port");
    rc |= required(c->upstream_mount, "upstream.mountpoint");
    rc |= required(c->upstream_user, "upstream.user");
    rc |= required(c->upstream_password, "upstream.password");
    rc |= required(c->local_host, "local.host");
    rc |= required(c->local_port, "local.port");
    rc |= required(c->local_mount, "local.mountpoint");
    rc |= required(c->source_password, "local.source_password");
    return rc == 0 ? 0 : -1;
}

int nt_config_validate_rover(const ntrip_tool_config_t *c)
{
    int rc = 0;
    rc |= required(c->local_host, "local.host");
    rc |= required(c->local_port, "local.port");
    rc |= required(c->local_mount, "local.mountpoint");
    rc |= required(c->rover_user, "rover.user");
    rc |= required(c->rover_password, "rover.password");
    return rc == 0 ? 0 : -1;
}

int nt_connect(const char *host, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host, port, &hints, &result);
    if (rc != 0) {
        fprintf(stderr, "resolve %s:%s: %s\n", host, port, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *it = result; it; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);
    if (fd < 0) fprintf(stderr, "connect %s:%s: %s\n", host, port, strerror(errno));
    return fd;
}

int nt_send_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len > 0 && nt_running()) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n > 0) {
            p += (size_t)n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    return len == 0 ? 0 : -1;
}

int nt_read_response(int fd, ntrip_response_t *response)
{
    memset(response, 0, sizeof(*response));
    uint8_t buf[NTRIP_TOOL_HEADER_MAX];
    size_t used = 0;

    while (used < sizeof(buf)) {
        ssize_t n = recv(fd, buf + used, sizeof(buf) - used, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        used += (size_t)n;

        for (size_t i = 3; i < used; i++) {
            if (buf[i - 3] == '\r' && buf[i - 2] == '\n' &&
                buf[i - 1] == '\r' && buf[i] == '\n') {
                size_t header_len = i + 1;
                memcpy(response->header, buf, header_len);
                response->header[header_len] = '\0';
                response->header_len = header_len;
                response->payload_len = used - header_len;
                if (response->payload_len)
                    memcpy(response->payload, buf + header_len,
                           response->payload_len);
                return 0;
            }
        }
    }
    fprintf(stderr, "NTRIP response header exceeds %u bytes\n",
            (unsigned)NTRIP_TOOL_HEADER_MAX);
    return -1;
}

int nt_response_ok(const ntrip_response_t *response)
{
    return strncmp(response->header, "ICY 200", 7) == 0 ||
           strncmp(response->header, "HTTP/1.0 200", 12) == 0 ||
           strncmp(response->header, "HTTP/1.1 200", 12) == 0;
}

int nt_base64_basic(const char *user, const char *password,
                    char *out, size_t out_size)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char plain[512];
    int plain_len = snprintf(plain, sizeof(plain), "%s:%s", user, password);
    if (plain_len < 0 || (size_t)plain_len >= sizeof(plain)) return -1;

    size_t needed = 4u * ((size_t)plain_len + 2u) / 3u + 1u;
    if (needed > out_size) return -1;

    size_t i = 0, o = 0;
    while (i < (size_t)plain_len) {
        size_t remain = (size_t)plain_len - i;
        uint32_t a = (uint8_t)plain[i++];
        uint32_t b = remain > 1 ? (uint8_t)plain[i++] : 0;
        uint32_t c = remain > 2 ? (uint8_t)plain[i++] : 0;
        uint32_t value = (a << 16) | (b << 8) | c;
        out[o++] = alphabet[(value >> 18) & 63u];
        out[o++] = alphabet[(value >> 12) & 63u];
        out[o++] = remain > 1 ? alphabet[(value >> 6) & 63u] : '=';
        out[o++] = remain > 2 ? alphabet[value & 63u] : '=';
    }
    out[o] = '\0';
    return 0;
}
