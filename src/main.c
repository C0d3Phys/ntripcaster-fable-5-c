#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>

#include <stdlib.h>

#include "core/broker.h"
#include "core/config.h"
#include "core/io_engine.h"
#include "core/logger.h"
#include "protocol/auth.h"
#include "protocol/pwhash.h"

#ifndef DEFAULT_LOG_PATH
#define DEFAULT_LOG_PATH "ntripcaster.log"
#endif

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

#ifndef DEFAULT_CONF_PATH
#define DEFAULT_CONF_PATH "conf/ntripcaster.conf"
#endif

static io_engine_t *g_engine = NULL;

/* Flag de reload por SIGHUP — el handler SOLO escribe esto (lo único
 * async-signal-safe garantizado); el reload real corre en el tick_cb
 * del accept loop, en contexto normal. */
static volatile sig_atomic_t g_reload_pending = 0;
static const char           *g_conf_path      = NULL;

static void handle_signal(int sig)
{
    (void)sig;
    if (g_engine) io_engine_stop(g_engine);
}

static void handle_sighup(int sig)
{
    (void)sig;
    g_reload_pending = 1;
}

/* Corre en el accept loop (~cada 200ms), contexto normal — acá SÍ es
 * seguro parsear el INI y tomar el wrlock del registro. */
static void main_tick(void *ctx)
{
    (void)ctx;
    if (g_reload_pending) {
        g_reload_pending = 0;
        log_info("main: SIGHUP recibido - recargando registro de auth");
        auth_registry_reload(g_conf_path);
    }
}

/*
 * hash_password_cli — Modo utilitario (IMP-02): lee una password de
 * stdin (una línea, sin eco -- no hay terminal especial acá, es una
 * herramienta de operador para generar la línea del conf, no un login
 * interactivo) y escribe a stdout el string serializado listo para
 * pegar en [source]/[client:*]. No toca el conf, no arranca nada del
 * caster. Uso:
 *   echo -n 'passbase123' | ./ntripcaster --hash-password
 */
static int hash_password_cli(void)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, stdin);
    if (n < 0) {
        fprintf(stderr, "hash-password: no se pudo leer password de stdin\n");
        free(line);
        return 1;
    }
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
        line[--n] = '\0';
    }
    if (n == 0) {
        fprintf(stderr, "hash-password: password vacia (stdin)\n");
        free(line);
        return 1;
    }

    char out[PWHASH_STRING_MAX];
    int rc = pwhash_create(line, PWHASH_DEFAULT_ITERATIONS, out, sizeof(out));
    /* Borrar la password de la memoria del proceso apenas se usó -- no
     * queda dando vueltas en el heap más de lo necesario. */
    memset(line, 0, (size_t)n);
    free(line);
    if (rc != 0) {
        fprintf(stderr, "hash-password: fallo generando el hash\n");
        return 1;
    }
    printf("%s\n", out);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc > 1 && strcmp(argv[1], "--hash-password") == 0) {
        return hash_password_cli();
    }

    /*
     * Configuración general: defaults < [caster] del conf < argv/env.
     *   ./ntripcaster [puerto] [conf] [log]
     */
    const char *conf_path = (argc > 2) ? argv[2] : DEFAULT_CONF_PATH;
    g_conf_path = conf_path;

    caster_config_t cast;
    config_defaults(&cast);
    config_load(&cast, conf_path);          /* si falla, quedan defaults */

    int port = cast.port;
    if (argc > 1) port = atoi(argv[1]);     /* argv pisa al conf */

    /* Logger: archivo del conf (log_file), pisable por argv[3];
     * nivel del conf (log_level), pisable por env NTRIPCASTER_LOG */
    const char *log_path = (argc > 3) ? argv[3] : cast.log_file;
    const char *env_lvl  = getenv("NTRIPCASTER_LOG");
    log_init(log_path, log_level_from_str(env_lvl ? env_lvl : cast.log_level));

    log_info("NtripCaster v%s  '%s'  %s:%d  conf=%s  log=%s",
             APP_VERSION, cast.name, cast.bind_addr, port, conf_path, log_path);

    /*
     * Auth: carga inicial unica, antes de arrancar el io_engine (todavia
     * no hay threads corriendo, no hace falta lock para esta carga --
     * ver comentario en auth_registry_load). Si falla, el caster sigue
     * arrancando pero con la tabla vacia: TODO SOURCE/GET va a ser
     * rechazado hasta que el archivo exista y sea valido. Fail closed a
     * proposito -- ver docs/FEATURE_auth_registry.md.
     *
     * Reload en caliente: kill -HUP <pid> recarga el conf sin reiniciar
     * (el handler marca un flag; el reload real corre en main_tick).
     */
    if (auth_registry_load(conf_path) != 0) {
        log_warn("main: auth no se pudo cargar desde '%s' -- arrancando "
                 "de todos modos (todo se va a rechazar hasta que el "
                 "archivo exista)", conf_path);
    }

    broker_t *broker = calloc(1, sizeof(broker_t));
    if (!broker) { perror("calloc broker"); return 1; }

    broker_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_clients         = cast.max_clients;
    cfg.max_sources         = cast.max_sources;
    cfg.client_timeout_s    = cast.client_timeout_s;
    cfg.source_timeout_s    = cast.source_timeout_s;
    cfg.handshake_timeout_s = cast.handshake_timeout_s;
    cfg.port                = port;
    snprintf(cfg.caster_name, sizeof(cfg.caster_name), "%s", cast.name);
    snprintf(cfg.caster_operator, sizeof(cfg.caster_operator), "%s",
             cast.operator_name);
    snprintf(cfg.caster_country, sizeof(cfg.caster_country), "%s",
             cast.country);
    broker_init(broker, &cfg);

    io_engine_t engine;
    if (io_engine_init(&engine, broker, cast.bind_addr, port,
                       cast.num_threads) != 0) {
        log_error("main: io_engine_init fallo (puerto %d ocupado?)", port);
        return 1;
    }

    g_engine = &engine;
    engine.tick_cb = main_tick;   /* reload de auth por SIGHUP */

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP,  handle_sighup);
    signal(SIGPIPE, SIG_IGN);

    io_engine_run(&engine);

    io_engine_destroy(&engine);
    broker_destroy(broker);
    free(broker);
    auth_registry_destroy();

    log_info("main: bye");
    log_close();
    return 0;
}
