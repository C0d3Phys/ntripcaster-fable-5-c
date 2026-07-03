/*
 * logger.h — Logger del NtripCaster (punto 100.001)
 *
 * Reemplaza los printf/fprintf(stderr) sueltos por un log con:
 *   - timestamp con milisegundos
 *   - niveles DEBUG / INFO / WARN / ERROR (filtrables)
 *   - salida dual: consola (stderr) + archivo (opcional)
 *   - thread-safe (mutex interno; los workers loguean concurrentemente)
 *
 * Uso:
 *   log_init("ntripcaster.log", LOG_INFO);   // NULL = solo consola
 *   log_info("source registrado mp=%s", mp);
 *   log_warn("auth rechazado fd=%d", fd);
 *   log_close();
 *
 * Nivel por variable de entorno (pisa al del init):
 *   NTRIPCASTER_LOG=debug|info|warn|error
 *
 * Formato de línea:
 *   2026-07-02 10:15:33.482 [INFO ] ntrip: source registrado mp=Demo1
 *
 * NO llamar log_* desde signal handlers (usa mutex + stdio). El patrón
 * correcto ya está en main.c: el handler marca un flag, el tick loguea.
 */
#ifndef NTRIPCASTER_LOGGER_H
#define NTRIPCASTER_LOGGER_H

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3,
} log_level_t;

/*
 * log_init — Abre el log. file_path NULL = solo stderr.
 * El archivo se abre en append (sobrevive reinicios).
 * Retorna 0 si OK, -1 si no se pudo abrir el archivo (sigue por stderr).
 */
int log_init(const char *file_path, log_level_t min_level);

/* log_close — Cierra el archivo. Llamar en shutdown. */
void log_close(void);

void        log_set_level(log_level_t min_level);
log_level_t log_level_from_str(const char *s);   /* "debug"→LOG_DEBUG, etc. */

/* No llamar directo — usar las macros de abajo. */
void log_write(log_level_t lvl, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define log_debug(...) log_write(LOG_DEBUG, __VA_ARGS__)
#define log_info(...)  log_write(LOG_INFO,  __VA_ARGS__)
#define log_warn(...)  log_write(LOG_WARN,  __VA_ARGS__)
#define log_error(...) log_write(LOG_ERROR, __VA_ARGS__)

#endif /* NTRIPCASTER_LOGGER_H */
