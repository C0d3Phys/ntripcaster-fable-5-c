#include "ntrip_tool_common.h"
#include "gnss/rtcm3_frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MATCH_LOOKAHEAD 2048u

typedef struct { size_t offset; uint32_t length; uint16_t type; } frame_ref_t;
typedef struct {
    uint8_t *data; size_t size;
    frame_ref_t *frames; size_t count; size_t capacity;
    uint64_t corrupt;
} capture_t;

static void usage(const char *p)
{
    printf("Usage: %s SESSION_DIR\n"
           "Compares SESSION_DIR/relay_rtcm3.bin with rover_rtcm3.bin.\n", p);
}

static int add_frame(capture_t *c, size_t offset, const rtcm3_frame_t *f)
{
    if (c->count == c->capacity) {
        size_t capacity = c->capacity ? c->capacity * 2 : 4096;
        frame_ref_t *next = realloc(c->frames, capacity * sizeof(*next));
        if (!next) return -1;
        c->frames = next; c->capacity = capacity;
    }
    c->frames[c->count++] = (frame_ref_t){offset, f->frame_len, f->msg_type};
    return 0;
}

static int load_capture(const char *path, capture_t *c)
{
    memset(c, 0, sizeof(*c));
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return -1; }
    fseek(fp, 0, SEEK_END); long size = ftell(fp); rewind(fp);
    if (size < 0) { fclose(fp); return -1; }
    c->size = (size_t)size;
    c->data = malloc(c->size ? c->size : 1);
    if (!c->data || fread(c->data, 1, c->size, fp) != c->size) {
        fclose(fp); return -1;
    }
    fclose(fp);

    size_t pos = 0;
    while (pos < c->size) {
        if (c->data[pos] != RTCM3_PREAMBLE) {
            c->corrupt++; pos++; continue;
        }
        if (c->size - pos < 3) break;
        uint32_t body_len = ((uint32_t)(c->data[pos + 1] & 0x03u) << 8) |
                            c->data[pos + 2];
        uint32_t frame_len = 3u + body_len + 3u;
        if (frame_len > c->size - pos) break;
        if (!rtcm3_validate_frame(c->data + pos, frame_len)) {
            c->corrupt++; pos++; continue;
        }
        rtcm3_frame_t frame = {
            .data = c->data + pos,
            .frame_len = frame_len,
            .body_len = body_len,
            .msg_type = body_len >= 2
                ? (uint16_t)(((uint16_t)c->data[pos + 3] << 4) |
                             (c->data[pos + 4] >> 4)) : 0,
            .offset = (uint32_t)pos,
        };
        if (add_frame(c, pos, &frame) != 0) return -1;
        pos += frame_len;
    }
    c->corrupt += c->size - pos;
    return 0;
}

static int same_frame(const capture_t *a, size_t ai,
                      const capture_t *b, size_t bi)
{
    const frame_ref_t *x = &a->frames[ai], *y = &b->frames[bi];
    return x->length == y->length &&
           memcmp(a->data + x->offset, b->data + y->offset, x->length) == 0;
}

static long long meta_value(const char *dir, const char *role, const char *key)
{
    char path[1200], line[256];
    snprintf(path, sizeof(path), "%s/%s.meta", dir, role);
    FILE *fp = fopen(path, "r"); if (!fp) return 0;
    long long value = 0; size_t n = strlen(key);
    while (fgets(line, sizeof(line), fp))
        if (strncmp(line, key, n) == 0 && line[n] == '=') value = strtoll(line+n+1, NULL, 10);
    fclose(fp); return value;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) { usage(argv[0]); return 0; }
    if (argc != 2) { usage(argv[0]); return 2; }
    char relay_path[1200], rover_path[1200];
    snprintf(relay_path, sizeof(relay_path), "%s/relay_rtcm3.bin", argv[1]);
    snprintf(rover_path, sizeof(rover_path), "%s/rover_rtcm3.bin", argv[1]);
    capture_t relay, rover;
    if (load_capture(relay_path, &relay) || load_capture(rover_path, &rover)) return 1;

    size_t ri = 0, vi = 0, first = (size_t)-1, last = 0;
    uint64_t common = 0, lost_inside = 0, rover_extra = 0;
    while (vi < rover.count && ri < relay.count) {
        if (same_frame(&relay, ri, &rover, vi)) {
            if (first == (size_t)-1) first = ri;
            last = ri; common++; ri++; vi++; continue;
        }
        size_t found = ri + 1, limit = ri + MATCH_LOOKAHEAD;
        if (limit > relay.count) limit = relay.count;
        while (found < limit && !same_frame(&relay, found, &rover, vi)) found++;
        if (found < limit) {
            if (first != (size_t)-1) lost_inside += found - ri;
            ri = found;
        } else { rover_extra++; vi++; }
    }
    rover_extra += rover.count - vi;
    size_t before = first == (size_t)-1 ? relay.count : first;
    size_t after = first == (size_t)-1 ? 0 : relay.count - last - 1;
    uint64_t expected = common + lost_inside;
    double similarity = expected ? 100.0 * (double)common / (double)expected : 0.0;
    long long rs = meta_value(argv[1], "relay", "start_utc");
    long long re = meta_value(argv[1], "relay", "end_utc");
    long long vs = meta_value(argv[1], "rover", "start_utc");
    long long ve = meta_value(argv[1], "rover", "end_utc");
    long long overlap_start = rs > vs ? rs : vs, overlap_end = re < ve ? re : ve;

    printf("relay: bytes=%zu frames=%zu corrupt_bytes=%llu\n", relay.size, relay.count, (unsigned long long)relay.corrupt);
    printf("rover: bytes=%zu frames=%zu corrupt_bytes=%llu\n", rover.size, rover.count, (unsigned long long)rover.corrupt);
    printf("UTC overlap: %lld seconds\n", overlap_end > overlap_start ? overlap_end-overlap_start : 0);
    printf("alignment: before_rover=%zu common=%llu lost_inside=%llu after_rover=%zu rover_unmatched=%llu\n",
           before, (unsigned long long)common, (unsigned long long)lost_inside,
           after, (unsigned long long)rover_extra);
    printf("similarity_in_common_span=%.3f%%\n", similarity);
    free(relay.frames); free(relay.data); free(rover.frames); free(rover.data);
    return common > 0 && lost_inside == 0 && rover_extra == 0 &&
           relay.corrupt == 0 && rover.corrupt == 0 ? 0 : 1;
}
