/*
 * sourcetable.c — Implementación (ver decisión de diseño en sourcetable.h)
 */
#include "sourcetable.h"
#include "ntrip_common.h"
#include "../core/broker.h"

#include <stdio.h>
#include <string.h>

static int build_sourcetable_text(broker_t *b, char *buf, int max)
{
    sourcetable_entry_t entries[MOUNTPOINT_MAX];
    int count = broker_sourcetable_fill(b, entries, MOUNTPOINT_MAX);

    int pos = 0;

#define APPEND(...) \
    do { \
        int _n = snprintf(buf + pos, (size_t)(max - pos), __VA_ARGS__); \
        if (_n > 0) pos += _n; \
    } while (0)

    APPEND("CAS;localhost;2101;NtripCaster;unknown;0;DEU;0.00;0.00;\r\n");
    APPEND("NET;NTRIPCASTER;NtripCaster;B;N;none;none;none;none\r\n");

    for (int i = 0; i < count; i++) {
        sourcetable_entry_t *e = &entries[i];
        APPEND("STR;%s;%s;%s;;%d;%s;NTRIPCASTER;DEU;%.4f;%.4f;%d;"
               "0;NtripCaster;none;N;N;0;none\r\n",
               e->name,
               e->identifier[0] ? e->identifier : e->name,
               e->format[0] ? e->format : "RTCM 3.3",
               0,
               e->nav_system[0] ? e->nav_system : "GPS",
               e->lat, e->lon,
               e->nmea);
    }

    APPEND("ENDSOURCETABLE\r\n");
#undef APPEND

    return pos;
}

void sourcetable_handle_v1(io_engine_t *eng, conn_t *conn)
{
    char body[32768];
    int  blen = build_sourcetable_text(eng->broker, body, (int)sizeof(body));

    char hdr[256];
    int  hlen = snprintf(hdr, sizeof(hdr),
        "SOURCETABLE 200 OK\r\n"
        "Server: NtripCaster/0.1\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %d\r\n"
        "\r\n",
        blen);

    send_all(conn->fd, hdr,  (size_t)hlen);
    send_all(conn->fd, body, (size_t)blen);

    printf("[ntrip] sourcetable v1 -> fd=%d  %d bytes\n",
           conn->fd, blen);

    io_engine_conn_close(eng, conn);
}

void sourcetable_handle_v2(io_engine_t *eng, conn_t *conn, int browser)
{
    char body[32768];
    int  blen;

    if (browser) {
        char rows[24576];
        int  rpos = 0;

        sourcetable_entry_t entries[MOUNTPOINT_MAX];
        int count = broker_sourcetable_fill(eng->broker, entries, MOUNTPOINT_MAX);

        for (int i = 0; i < count; i++) {
            sourcetable_entry_t *e = &entries[i];
            rpos += snprintf(rows + rpos, sizeof(rows) - (size_t)rpos,
                "<tr><td>%s</td><td>%s</td><td>%s</td>"
                "<td>%.4f</td><td>%.4f</td><td>%s</td></tr>\n",
                e->name,
                e->format[0] ? e->format : "RTCM 3.3",
                e->active ? "Online" : "Offline",
                e->lat, e->lon,
                e->nav_system[0] ? e->nav_system : "GPS");
        }

        blen = snprintf(body, sizeof(body),
            "<!DOCTYPE html><html><head>"
            "<meta charset=\"utf-8\">"
            "<title>NtripCaster Sourcetable</title>"
            "<style>body{font-family:monospace;background:#111;color:#0f0;padding:16px}"
            "table{border-collapse:collapse;width:100%%}"
            "th,td{border:1px solid #333;padding:6px 12px;text-align:left}"
            "th{background:#222}</style>"
            "</head><body>"
            "<h2>NtripCaster / Sourcetable</h2>"
            "<table><tr><th>Mountpoint</th><th>Format</th><th>Status</th>"
            "<th>Lat</th><th>Lon</th><th>Systems</th></tr>"
            "%s"
            "</table>"
            "<p style=\"color:#555\">%d mountpoints</p>"
            "</body></html>",
            rows, count);
    } else {
        blen = build_sourcetable_text(eng->broker, body, (int)sizeof(body));
    }

    char hdr[256];
    int  hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Server: NtripCaster/0.1\r\n"
        "Ntrip-Version: Ntrip/2.0\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        browser ? "text/html; charset=utf-8" : "text/plain",
        blen);

    send_all(conn->fd, hdr,  (size_t)hlen);
    send_all(conn->fd, body, (size_t)blen);

    printf("[ntrip] sourcetable %s -> fd=%d\n",
           browser ? "html" : "v2", conn->fd);

    io_engine_conn_close(eng, conn);
}
