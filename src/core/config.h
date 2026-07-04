/*
 * config.h — Configuración general del caster (sección [caster] del conf)
 *
 * Vive en el MISMO archivo que el registro de auth (conf/ntripcaster.conf):
 *   [caster]  → la parsea este módulo (config_load)
 *   [source] y [client:*] → las parsea auth.c (auth_registry_load)
 * Cada parser ignora las secciones del otro. inih ya está vendored.
 *
 * Prioridad: defaults < [caster] del conf < argumentos de línea / env.
 *   ./ntripcaster [puerto] [conf] [log]   ← los argv pisan al conf
 *   NTRIPCASTER_LOG=debug                 ← pisa a log_level del conf
 */
#ifndef NTRIPCASTER_CONFIG_H
#define NTRIPCASTER_CONFIG_H

typedef struct {
    /* Red */
    int  port;                 /* default 2101 */
    char bind_addr[48];        /* default 0.0.0.0 */
    int  num_threads;          /* 0 = auto (CPU cores) */

    /* Identidad (sourcetable CAS/NET + header Server) */
    char name[64];             /* nombre del caster */
    char operator_name[64];    /* operador (CAS line) */
    char country[8];           /* código de país por defecto (DEU, SLV...) */

    /* Límites y timeouts */
    int  max_clients;
    int  max_sources;
    int  client_timeout_s;
    int  source_timeout_s;
    int  handshake_timeout_s;

    /* Logging */
    char log_file[256];        /* "" = solo consola */
    char log_level[16];        /* debug|info|warn|error */
} caster_config_t;

/* Defaults (idénticos al comportamiento previo a este módulo) */
void config_defaults(caster_config_t *c);

/*
 * config_load — Lee la sección [caster] de conf_path sobre una config
 * ya inicializada con defaults. Claves ausentes conservan su valor.
 * Retorna 0 si OK, -1 si el archivo no se pudo abrir/parsear (la config
 * queda en defaults — el caster arranca igual).
 */
int config_load(caster_config_t *c, const char *conf_path);

#endif /* NTRIPCASTER_CONFIG_H */
