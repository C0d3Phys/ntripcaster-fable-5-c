#include "ntrip_tool_common.h"
#include "gnss/rtcm3_frame.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DECODE_BUFFER_SIZE (128u * 1024u)
#define FRAME_BATCH 512

static void usage(const char *program)
{
    printf("Usage:\n"
           "  %s --config FILE\n"
           "  %s HOST PORT MOUNT USER [SECONDS] [OUTPUT.bin]\n\n"
           "Required environment variable:\n"
           "  LOCAL_ROVER_PASS    Password configured for USER on MOUNT\n\n"
           "SECONDS=0 (default) runs until Ctrl+C. OUTPUT.bin is optional.\n\n"
           "Example:\n"
           "  LOCAL_ROVER_PASS=passrover1 %s 127.0.0.1 2101 BASE1 rover1 30 capture.rtcm3\n",
           program, program, program);
}

static void inspect(uint8_t *decode, size_t *decode_len,
                    const uint8_t *data, size_t len,
                    uint64_t *frames, uint64_t *skipped)
{
    if (len > DECODE_BUFFER_SIZE - *decode_len) {
        size_t drop = len - (DECODE_BUFFER_SIZE - *decode_len);
        if (drop >= *decode_len) *decode_len = 0;
        else {
            memmove(decode, decode + drop, *decode_len - drop);
            *decode_len -= drop;
        }
        *skipped += drop;
    }
    memcpy(decode + *decode_len, data, len);
    *decode_len += len;

    rtcm3_frame_t parsed[FRAME_BATCH];
    int count = 0;
    size_t used = 0;
    rtcm3_parse_stream(decode, *decode_len, parsed, FRAME_BATCH, &count, &used);
    size_t frame_bytes = 0;
    for (int i = 0; i < count; i++) frame_bytes += parsed[i].frame_len;
    *frames += (uint64_t)count;
    if (used > frame_bytes) *skipped += (uint64_t)(used - frame_bytes);
    if (used) {
        memmove(decode, decode + used, *decode_len - used);
        *decode_len -= used;
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(argv[0]);
        return 0;
    }
    ntrip_tool_config_t config;
    memset(&config, 0, sizeof(config));
    if (argc == 3 && strcmp(argv[1], "--config") == 0) {
        if (nt_config_load(argv[2], &config) != 0 ||
            nt_config_validate_rover(&config) != 0) return 2;
    } else if (argc < 5 || argc > 7) {
        usage(argv[0]);
        return 2;
    }
    if (!(argc == 3 && strcmp(argv[1], "--config") == 0)) {
        snprintf(config.local_host, sizeof(config.local_host), "%s", argv[1]);
        snprintf(config.local_port, sizeof(config.local_port), "%s", argv[2]);
        snprintf(config.local_mount, sizeof(config.local_mount), "%s", argv[3]);
        snprintf(config.rover_user, sizeof(config.rover_user), "%s", argv[4]);
        config.rover_seconds = argc >= 6 ? strtol(argv[5], NULL, 10) : 0;
        if (argc == 7) snprintf(config.rover_output, sizeof(config.rover_output), "%s", argv[6]);
        const char *password = getenv("LOCAL_ROVER_PASS");
        if (!password || !*password) {
            fprintf(stderr, "LOCAL_ROVER_PASS is required\n");
            return 2;
        }
        snprintf(config.rover_password, sizeof(config.rover_password), "%s", password);
    }
    long duration = config.rover_seconds;
    FILE *capture = NULL;
    if (config.rover_output[0]) {
        capture = fopen(config.rover_output, "wb");
        if (!capture) {
            perror(config.rover_output);
            return 1;
        }
    }

    nt_install_signal_handlers();
    int fd = nt_connect(config.local_host, config.local_port);
    if (fd < 0) return 1;
    char basic[768], request[2048];
    if (nt_base64_basic(config.rover_user, config.rover_password,
                        basic, sizeof(basic)) != 0) return 1;
    int request_len = snprintf(request, sizeof(request),
        "GET /%s HTTP/1.0\r\n"
        "User-Agent: NTRIP ntripcaster-test-rover/1.0\r\n"
        "Authorization: Basic %s\r\n"
        "Accept: */*\r\n\r\n", config.local_mount, basic);
    ntrip_response_t response;
    if (request_len < 0 || (size_t)request_len >= sizeof(request) ||
        nt_send_all(fd, request, (size_t)request_len) != 0 ||
        nt_read_response(fd, &response) != 0 || !nt_response_ok(&response)) {
        fprintf(stderr, "caster rejected rover%s%s\n",
                response.header_len ? ": " : "",
                response.header_len ? response.header : "");
        close(fd);
        if (capture) fclose(capture);
        return 1;
    }

    fprintf(stderr, "rover connected: %s:%s/%s as %s\n",
            config.local_host, config.local_port, config.local_mount,
            config.rover_user);
    uint8_t decode[DECODE_BUFFER_SIZE];
    size_t decode_len = 0;
    uint64_t bytes = 0, frames = 0, skipped = 0;
    time_t started = time(NULL);
    uint8_t buf[64 * 1024];

    if (response.payload_len) {
        inspect(decode, &decode_len, response.payload, response.payload_len,
                &frames, &skipped);
        bytes += response.payload_len;
        if (capture) fwrite(response.payload, 1, response.payload_len, capture);
    }
    while (nt_running() && (duration <= 0 || time(NULL) - started < duration)) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            inspect(decode, &decode_len, buf, (size_t)n, &frames, &skipped);
            bytes += (uint64_t)n;
            if (capture) fwrite(buf, 1, (size_t)n, capture);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }

    time_t elapsed = time(NULL) - started;
    if (elapsed < 1) elapsed = 1;
    fprintf(stderr,
        "summary: bytes=%llu frames_valid=%llu skipped_or_corrupt=%llu "
        "rate=%.1f KB/s elapsed=%lds\n",
        (unsigned long long)bytes, (unsigned long long)frames,
        (unsigned long long)skipped, (double)bytes / 1024.0 / elapsed,
        (long)elapsed);
    close(fd);
    if (capture) fclose(capture);
    return bytes > 0 && frames > 0 && skipped == 0 ? 0 : 1;
}
