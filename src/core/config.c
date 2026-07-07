/*
 * config.c — Parser de la sección [caster] (inih, mismo conf que auth)
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ini.h"   /* vendor/inih */

void config_defaults(caster_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->port                = 2101;
    snprintf(c->bind_addr, sizeof(c->bind_addr), "0.0.0.0");
    c->num_threads         = 0;
    snprintf(c->name, sizeof(c->name), "NtripCaster");
    snprintf(c->operator_name, sizeof(c->operator_name), "unknown");
    snprintf(c->country, sizeof(c->country), "DEU");
    snprintf(c->html_template, sizeof(c->html_template),
             "templates/sourcetable.html");
    c->max_clients         = 1024;
    c->max_sources         = 128;
    c->client_timeout_s    = 60;
    c->source_timeout_s    = 30;
    c->handshake_timeout_s = 10;
    snprintf(c->log_file, sizeof(c->log_file), "ntripcaster.log");
    snprintf(c->log_level, sizeof(c->log_level), "info");
}

static int caster_ini_handler(void *user, const char *section,
                               const char *name, const char *value)
{
    caster_config_t *c = (caster_config_t *)user;

    /* Solo nos interesa [caster] — [source]/[client:*] son de auth.c */
    if (strcasecmp(section, "caster") != 0)
        return 1;

    if      (strcasecmp(name, "port") == 0)
        c->port = atoi(value);
    else if (strcasecmp(name, "bind") == 0)
        snprintf(c->bind_addr, sizeof(c->bind_addr), "%s", value);
    else if (strcasecmp(name, "threads") == 0)
        c->num_threads = atoi(value);
    else if (strcasecmp(name, "name") == 0)
        snprintf(c->name, sizeof(c->name), "%s", value);
    else if (strcasecmp(name, "operator") == 0)
        snprintf(c->operator_name, sizeof(c->operator_name), "%s", value);
    else if (strcasecmp(name, "country") == 0)
        snprintf(c->country, sizeof(c->country), "%s", value);
    else if (strcasecmp(name, "html_template") == 0)
        snprintf(c->html_template, sizeof(c->html_template), "%s", value);
    else if (strcasecmp(name, "max_clients") == 0)
        c->max_clients = atoi(value);
    else if (strcasecmp(name, "max_sources") == 0)
        c->max_sources = atoi(value);
    else if (strcasecmp(name, "client_timeout") == 0)
        c->client_timeout_s = atoi(value);
    else if (strcasecmp(name, "source_timeout") == 0)
        c->source_timeout_s = atoi(value);
    else if (strcasecmp(name, "handshake_timeout") == 0)
        c->handshake_timeout_s = atoi(value);
    else if (strcasecmp(name, "log_file") == 0)
        snprintf(c->log_file, sizeof(c->log_file), "%s", value);
    else if (strcasecmp(name, "log_level") == 0)
        snprintf(c->log_level, sizeof(c->log_level), "%s", value);
    else
        fprintf(stderr, "[config] clave desconocida '%s' en [caster] (ignorada)\n",
                name);

    return 1;
}

int config_load(caster_config_t *c, const char *conf_path)
{
    int rc = ini_parse(conf_path, caster_ini_handler, c);
    if (rc < 0) {
        fprintf(stderr, "[config] no se pudo abrir '%s' -- usando defaults\n",
                conf_path);
        return -1;
    }
    if (rc > 0) {
        fprintf(stderr, "[config] error de sintaxis en '%s' linea %d\n",
                conf_path, rc);
        return -1;
    }
    return 0;
}
