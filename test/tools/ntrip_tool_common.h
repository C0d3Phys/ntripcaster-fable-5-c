#ifndef NTRIP_TEST_TOOL_COMMON_H
#define NTRIP_TEST_TOOL_COMMON_H

#include <stddef.h>
#include <stdint.h>

#define NTRIP_TOOL_HEADER_MAX (16u * 1024u)

typedef struct {
    char    header[NTRIP_TOOL_HEADER_MAX + 1];
    size_t  header_len;
    uint8_t payload[NTRIP_TOOL_HEADER_MAX];
    size_t  payload_len;
} ntrip_response_t;

int  nt_connect(const char *host, const char *port);
int  nt_send_all(int fd, const void *data, size_t len);
int  nt_read_response(int fd, ntrip_response_t *response);
int  nt_response_ok(const ntrip_response_t *response);
int  nt_base64_basic(const char *user, const char *password,
                     char *out, size_t out_size);
void nt_install_signal_handlers(void);
int  nt_running(void);
void nt_stop(void);

#endif
