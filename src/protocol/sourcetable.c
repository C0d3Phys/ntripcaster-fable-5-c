/*
 * sourcetable.c — Implementación (ver decisión de diseño en sourcetable.h)
 */
#include "sourcetable.h"
#include "ntrip_common.h"
#include "html_template.h"
#include "../core/broker.h"
#include "../core/logger.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

/*
 * html_escape — Escapa &, < y > para poder insertar texto arbitrario
 * (nombres de mountpoint/operador vienen del conf) dentro de un <pre>
 * sin romper el HTML. Trunca en silencio si dst no alcanza.
 */
static int html_escape(const char *src, char *dst, int max)
{
    int pos = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        const char *rep = NULL;
        switch (*p) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;";  break;
            case '>': rep = "&gt;";  break;
            default:  break;
        }
        if (rep) {
            int rlen = (int)strlen(rep);
            if (pos + rlen >= max) break;
            memcpy(dst + pos, rep, (size_t)rlen);
            pos += rlen;
        } else {
            if (pos + 1 >= max) break;
            dst[pos++] = (char)*p;
        }
    }
    dst[pos] = '\0';
    return pos;
}

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

    /* Identidad desde [caster] del conf */
    const char *cname = b->config.caster_name[0]     ? b->config.caster_name     : "NtripCaster";
    const char *coper = b->config.caster_operator[0] ? b->config.caster_operator : "unknown";
    const char *cctry = b->config.caster_country[0]  ? b->config.caster_country  : "DEU";
    int         cport = b->config.port > 0           ? b->config.port            : 2101;

    APPEND("CAS;%s;%d;%s;%s;0;%s;0.00;0.00;\r\n",
           cname, cport, cname, coper, cctry);
    APPEND("NET;%s;%s;B;N;none;none;none;none\r\n", cname, coper);

    for (int i = 0; i < count; i++) {
        sourcetable_entry_t *e = &entries[i];
        APPEND("STR;%s;%s;%s;;%d;%s;%s;%s;%.4f;%.4f;%d;"
               "0;%s;none;B;N;0;none\r\n",
               e->name,
               e->identifier[0] ? e->identifier : e->name,
               e->format[0] ? e->format : "RTCM 3.3",
               0,
               e->nav_system[0] ? e->nav_system : "GPS",
               cname, cctry,
               e->lat, e->lon,
               e->nmea,
               cname);
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
        "Server: %s/1.0\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %d\r\n"
        "\r\n",
        eng->broker->config.caster_name[0]
            ? eng->broker->config.caster_name : "NtripCaster",
        blen);

    send_all(conn->fd, hdr,  (size_t)hlen);
    send_all(conn->fd, body, (size_t)blen);

    log_debug("ntrip: sourcetable v1 -> fd=%d  %d bytes", conn->fd, blen);

    io_engine_conn_close(eng, conn);
}

void sourcetable_handle_v2(io_engine_t *eng, conn_t *conn, int browser)
{
    char body[65536];
    int  blen;

    if (browser) {
        const char *cname = eng->broker->config.caster_name[0]
                          ? eng->broker->config.caster_name : "NtripCaster";
        const char *coper = eng->broker->config.caster_operator[0]
                          ? eng->broker->config.caster_operator : cname;

        /* Texto crudo de la sourcetable (mismas líneas CAS/NET/STR que v1),
         * mostrado dentro de un <pre> para que el usuario vea exactamente
         * lo que recibiría un rover. */
        char raw[32768];
        int rawlen = build_sourcetable_text(eng->broker, raw, (int)sizeof(raw));

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tmv;
        localtime_r(&ts.tv_sec, &tmv);
        int hour12 = tmv.tm_hour % 12;
        if (hour12 == 0) hour12 = 12;
        char datebuf[48];
        snprintf(datebuf, sizeof(datebuf), "%d/%d/%d %d:%02d:%02d %s",
                 tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_year + 1900,
                 hour12, tmv.tm_min, tmv.tm_sec,
                 tmv.tm_hour >= 12 ? "PM" : "AM");
        char yearbuf[16];
        snprintf(yearbuf, sizeof(yearbuf), "%d", tmv.tm_year + 1900);

        char raw_block[34816];
        int rblen = snprintf(raw_block, sizeof(raw_block),
            "SOURCETABLE 200 OK\r\n"
            "Server: NTRIP %s %s/1.0\r\n"
            "Date: %s\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s",
            cname, APP_VERSION, datebuf, rawlen, raw);
        (void)rblen;

        char raw_escaped[49152];
        html_escape(raw_block, raw_escaped, (int)sizeof(raw_escaped));

        const char *tpl_path = eng->broker->config.html_template[0]
                             ? eng->broker->config.html_template
                             : "templates/sourcetable.html";

        html_var_t vars[] = {
            { "OPERATOR",    coper },
            { "SOURCETABLE", raw_escaped },
            { "YEAR",        yearbuf },
        };

        blen = html_template_render(tpl_path, vars,
                                     (int)(sizeof(vars) / sizeof(vars[0])),
                                     body, (int)sizeof(body));

        if (blen < 0) {
            /* Sin template (no existe / no se pudo leer / demasiado
             * grande): degradamos a texto plano en vez de romper la
             * respuesta -- un rover o script igual puede parsearla,
             * y queda loggeado para que el operador arregle el path. */
            log_warn("sourcetable: no se pudo leer html_template '%s' "
                     "-- sirviendo texto plano", tpl_path);
            blen = build_sourcetable_text(eng->broker, body, (int)sizeof(body));
            browser = 0; /* Content-Type correcto para el fallback */
        }
    } else {
        blen = build_sourcetable_text(eng->broker, body, (int)sizeof(body));
    }

    char hdr[256];
    int  hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Server: %s/1.0\r\n"
        "Ntrip-Version: Ntrip/2.0\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "\r\n",
        eng->broker->config.caster_name[0]
            ? eng->broker->config.caster_name : "NtripCaster",
        browser ? "text/html; charset=utf-8" : "text/plain",
        blen);

    send_all(conn->fd, hdr,  (size_t)hlen);
    send_all(conn->fd, body, (size_t)blen);

    log_debug("ntrip: sourcetable %s -> fd=%d", browser ? "html" : "v2", conn->fd);

    io_engine_conn_close(eng, conn);
}
