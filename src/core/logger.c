/*
 * logger.c — Implementación del logger (punto 100.001)
 */
#include "logger.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>

static FILE           *g_file  = NULL;
static log_level_t     g_min   = LOG_INFO;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *level_tag(log_level_t l)
{
    switch (l) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO ";
    case LOG_WARN:  return "WARN ";
    case LOG_ERROR: return "ERROR";
    default:        return "?????";
    }
}

log_level_t log_level_from_str(const char *s)
{
    if (!s) return LOG_INFO;
    if (strcasecmp(s, "debug") == 0) return LOG_DEBUG;
    if (strcasecmp(s, "warn")  == 0) return LOG_WARN;
    if (strcasecmp(s, "error") == 0) return LOG_ERROR;
    return LOG_INFO;
}

int log_init(const char *file_path, log_level_t min_level)
{
    g_min = min_level;

    if (file_path && file_path[0]) {
        g_file = fopen(file_path, "a");
        if (!g_file) {
            fprintf(stderr, "[logger] no se pudo abrir '%s' -- "
                            "logueando solo a consola\n", file_path);
            return -1;
        }
    }
    return 0;
}

void log_close(void)
{
    pthread_mutex_lock(&g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = NULL;
    }
    pthread_mutex_unlock(&g_mutex);
}

void log_set_level(log_level_t min_level)
{
    g_min = min_level;
}

void log_write(log_level_t lvl, const char *fmt, ...)
{
    if (lvl < g_min) return;

    /* Armar la línea completa en un buffer local — un solo write por
     * destino, sin interleaving entre threads. */
    char line[1024];

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);

    int pos = snprintf(line, sizeof(line),
                       "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] ",
                       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                       tm.tm_hour, tm.tm_min, tm.tm_sec,
                       (int)(tv.tv_usec / 1000),
                       level_tag(lvl));

    va_list ap;
    va_start(ap, fmt);
    pos += vsnprintf(line + pos, sizeof(line) - (size_t)pos - 2, fmt, ap);
    va_end(ap);

    if (pos > (int)sizeof(line) - 2) pos = (int)sizeof(line) - 2;
    line[pos++] = '\n';
    line[pos]   = '\0';

    pthread_mutex_lock(&g_mutex);
    fputs(line, stderr);
    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);   /* volumen bajo: 1 línea por evento, no por byte */
    }
    pthread_mutex_unlock(&g_mutex);
}
