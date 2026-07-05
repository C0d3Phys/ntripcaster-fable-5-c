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

typedef struct {
    char upstream_host[256];
    char upstream_port[16];
    char upstream_mount[128];
    char upstream_user[128];
    char upstream_password[256];
    char local_host[256];
    char local_port[16];
    char local_mount[128];
    char source_password[256];
    char rover_user[128];
    char rover_password[256];
    long rover_seconds;
    char rover_output[512];
    char capture_root[512];
} ntrip_tool_config_t;

int  nt_connect(const char *host, const char *port);
int  nt_send_all(int fd, const void *data, size_t len);
int  nt_read_response(int fd, ntrip_response_t *response);
int  nt_response_ok(const ntrip_response_t *response);
int  nt_base64_basic(const char *user, const char *password,
                     char *out, size_t out_size);
int  nt_config_load(const char *path, ntrip_tool_config_t *config);
int  nt_config_validate_relay(const ntrip_tool_config_t *config);
int  nt_config_validate_rover(const ntrip_tool_config_t *config);
void nt_install_signal_handlers(void);
int  nt_running(void);
void nt_stop(void);
int  nt_capture_new_session(const char *root, char *session, size_t size);
int  nt_capture_current_session(const char *root, char *session, size_t size);
int  nt_capture_write_meta(const char *session, const char *role,
                           long long start_utc, long long end_utc,
                           unsigned long long bytes);

#endif
