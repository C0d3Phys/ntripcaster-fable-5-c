#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>

#include <stdlib.h>

#include "core/broker.h"
#include "core/io_engine.h"
#include "core/logger.h"
#include "protocol/auth.h"

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

int main(int argc, char *argv[])
{
    int port = 2101;
    if (argc > 1) port = atoi(argv[1]);

    const char *conf_path = (argc > 2) ? argv[2] : DEFAULT_CONF_PATH;
    g_conf_path = conf_path;

    /* Logger (punto 100.001): argv[3] = archivo de log opcional.
     * Nivel por env: NTRIPCASTER_LOG=debug|info|warn|error */
    const char *log_path = (argc > 3) ? argv[3] : DEFAULT_LOG_PATH;
    log_init(log_path, log_level_from_str(getenv("NTRIPCASTER_LOG")));

    log_info("NtripCaster v%s  port=%d  conf=%s  log=%s",
             APP_VERSION, port, conf_path, log_path);

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
    cfg.max_clients         = 1024;
    cfg.max_sources         = 128;
    cfg.client_timeout_s    = 60;
    cfg.source_timeout_s    = 30;
    cfg.handshake_timeout_s = 10;
    broker_init(broker, &cfg);

    io_engine_t engine;
    if (io_engine_init(&engine, broker, "0.0.0.0", port, 0) != 0) {
        fprintf(stderr, "[main] io_engine_init failed\n");
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
