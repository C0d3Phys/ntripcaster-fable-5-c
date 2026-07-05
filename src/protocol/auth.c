/*
 * auth.c — Implementación del registro de credenciales + Basic Auth
 */
#include "auth.h"
#include "../core/logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "ini.h"      /* vendor/inih -- CMake ya agrega su include dir */
#include "uthash.h"   /* vendor/uthash */

/* ── Tablas en RAM ────────────────────────────────────────────────── */

typedef struct {
    char mountpoint[64];   /* clave */
    char password[128];
    UT_hash_handle hh;
} source_entry_t;

typedef struct {
    char key[192];         /* clave: "mountpoint:usuario" */
    char password[128];
    UT_hash_handle hh;
} client_entry_t;

static source_entry_t   *g_sources = NULL;
static client_entry_t   *g_clients = NULL;
static pthread_rwlock_t  g_lock    = PTHREAD_RWLOCK_INITIALIZER;

/* Tablas en construcción — el handler de inih escribe acá (vía el
 * puntero `user`), nunca directo a las globales. Así la carga inicial
 * y el reload comparten exactamente el mismo código de parseo. */
typedef struct {
    source_entry_t *sources;
    client_entry_t *clients;
} auth_tables_t;

/* ── Carga desde INI ─────────────────────────────────────────────── */

/*
 * Nota: el nombre NO puede ser "ini_handler" -- ese identificador ya lo
 * usa inih como typedef del tipo puntero a función (ver ini.h), y GCC
 * lo marca error ("redeclared as different kind of symbol") si una
 * función local se llama igual.
 *
 * Nota 2: inih trata ':' igual que '=' como separador de "name = value"
 * (ver ini_find_chars_or_comment en ini.c). Por eso el mountpoint de los
 * clientes NO puede ir en el nombre de la clave ("Demo1:rover1 = pass"
 * se parsea mal -- corta en el primer ':'). En vez de eso, el mountpoint
 * va en el nombre de la SECCION ("[client:Demo1]"), donde ':' es literal
 * -- las secciones solo se cortan en ']'.
 */
static int auth_ini_handler(void *user, const char *section,
                             const char *name, const char *value)
{
    auth_tables_t *t = (auth_tables_t *)user;

    if (strcmp(section, "source") == 0) {
        source_entry_t *e = calloc(1, sizeof(*e));
        if (!e) return 0;
        snprintf(e->mountpoint, sizeof(e->mountpoint), "%s", name);
        snprintf(e->password,   sizeof(e->password),   "%s", value);
        HASH_ADD_STR(t->sources, mountpoint, e);
        return 1;
    }

    static const char CLIENT_PREFIX[] = "client:";
    size_t prefix_len = sizeof(CLIENT_PREFIX) - 1;
    if (strncmp(section, CLIENT_PREFIX, prefix_len) == 0 && section[prefix_len]) {
        const char *mountpoint = section + prefix_len;
        client_entry_t *e = calloc(1, sizeof(*e));
        if (!e) return 0;
        /* clave interna "mountpoint:usuario" -- la construimos nosotros
         * con snprintf, nunca la parsea inih, asi que el ':' acá es
         * seguro. */
        snprintf(e->key, sizeof(e->key), "%s:%s", mountpoint, name);
        snprintf(e->password, sizeof(e->password), "%s", value);
        HASH_ADD_STR(t->clients, key, e);
        return 1;
    }

    /* [caster] es del modulo core/config.c — ignorar en silencio */
    if (strcasecmp(section, "caster") == 0)
        return 1;

    log_warn("auth: seccion desconocida en conf: [%s] (linea ignorada: %s)",
             section, name);
    return 1;   /* no aborta el parseo por una seccion rara */
}

/* Libera un par de tablas (las que sean — nuevas fallidas o viejas
 * reemplazadas). No toca las globales. */
static void tables_free(source_entry_t *sources, client_entry_t *clients)
{
    source_entry_t *se, *se_tmp;
    HASH_ITER(hh, sources, se, se_tmp) {
        HASH_DEL(sources, se);
        free(se);
    }
    client_entry_t *ce, *ce_tmp;
    HASH_ITER(hh, clients, ce, ce_tmp) {
        HASH_DEL(clients, ce);
        free(ce);
    }
}

/* Parsea el INI a un par de tablas NUEVAS. 0 si OK, -1 si error
 * (en cuyo caso las tablas parciales ya quedaron liberadas). */
static int tables_load(const char *conf_path, auth_tables_t *t)
{
    t->sources = NULL;
    t->clients = NULL;

    int rc = ini_parse(conf_path, auth_ini_handler, t);
    if (rc < 0) {
        log_error("auth: no se pudo abrir '%s'", conf_path);
        tables_free(t->sources, t->clients);
        return -1;
    }
    if (rc > 0) {
        log_error("auth: error de sintaxis en '%s' linea %d", conf_path, rc);
        tables_free(t->sources, t->clients);
        return -1;
    }
    return 0;
}

int auth_registry_load(const char *conf_path)
{
    /*
     * Carga inicial, llamada una sola vez desde main() antes de que el
     * io_engine arranque threads -- no hay lectores concurrentes todavía.
     */
    auth_tables_t t;
    if (tables_load(conf_path, &t) != 0) {
        log_error("auth: ningun SOURCE/GET va a poder autenticarse hasta que "
                  "'%s' exista y sea valido", conf_path);
        return -1;
    }

    g_sources = t.sources;
    g_clients = t.clients;

    log_info("auth: registro cargado desde '%s': %d source(s), %d cliente(s)",
             conf_path, HASH_COUNT(g_sources), HASH_COUNT(g_clients));
    return 0;
}

int auth_registry_reload(const char *conf_path)
{
    /*
     * Reload en caliente (FEATURE_auth_registry §2.3): construir las
     * tablas nuevas COMPLETAS fuera del lock (el trabajo pesado — abrir
     * el archivo, parsear, poblar — pasa acá sin bloquear a nadie), y
     * solo al final wrlock → swap de punteros → unlock. La sección
     * crítica dura microsegundos sin importar el tamaño del registro.
     *
     * Si el archivo nuevo está roto, se conserva la tabla vieja intacta
     * (mejor seguir con credenciales conocidas que quedarse sin nada).
     */
    auth_tables_t t;
    if (tables_load(conf_path, &t) != 0) {
        log_error("auth: reload FALLIDO -- se conserva el registro "
                  "anterior en memoria");
        return -1;
    }

    pthread_rwlock_wrlock(&g_lock);
    source_entry_t *old_sources = g_sources;
    client_entry_t *old_clients = g_clients;
    g_sources = t.sources;
    g_clients = t.clients;
    pthread_rwlock_unlock(&g_lock);

    tables_free(old_sources, old_clients);

    log_info("auth: registro RECARGADO desde '%s': %d source(s), %d cliente(s)",
             conf_path, HASH_COUNT(g_sources), HASH_COUNT(g_clients));
    return 0;
}

void auth_registry_destroy(void)
{
    tables_free(g_sources, g_clients);
    g_sources = NULL;
    g_clients = NULL;
}

/* ── Checks (hot path) ───────────────────────────────────────────── */

int auth_check_source(const char *mountpoint, const char *password)
{
    if (!mountpoint || !password) return -1;

    int ok = -1;
    pthread_rwlock_rdlock(&g_lock);
    source_entry_t *e = NULL;
    HASH_FIND_STR(g_sources, mountpoint, e);
    if (e && strcmp(e->password, password) == 0)
        ok = 0;
    pthread_rwlock_unlock(&g_lock);
    return ok;
}

int auth_check_client(const char *mountpoint, const char *user, const char *password)
{
    if (!mountpoint || !user || !user[0] || !password) return -1;

    char key[192];
    snprintf(key, sizeof(key), "%s:%s", mountpoint, user);

    int ok = -1;
    pthread_rwlock_rdlock(&g_lock);
    client_entry_t *e = NULL;
    HASH_FIND_STR(g_clients, key, e);
    if (e && strcmp(e->password, password) == 0)
        ok = 0;
    pthread_rwlock_unlock(&g_lock);
    return ok;
}

/* ── Base64 + Basic Auth ─────────────────────────────────────────── */

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int base64_decode(const char *src, char *dst, int dst_max)
{
    int out = 0;
    while (*src && *src != '=' && out + 3 < dst_max) {
        int a = b64val(src[0]);
        int b = (src[1]) ? b64val(src[1]) : -1;
        if (a < 0 || b < 0) break;
        dst[out++] = (char)((a << 2) | (b >> 4));
        if (!src[2] || src[2] == '=') break;
        int c = b64val(src[2]);
        if (c < 0) break;
        dst[out++] = (char)(((b & 0xF) << 4) | (c >> 2));
        if (!src[3] || src[3] == '=') break;
        int d = b64val(src[3]);
        if (d < 0) break;
        dst[out++] = (char)(((c & 0x3) << 6) | d);
        src += 4;
    }
    if (out < dst_max) dst[out] = '\0';
    return out;
}

static int str_icase_starts_local(const char *buf, const char *prefix)
{
    while (*prefix) {
        char a = *buf, b = *prefix;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
        buf++; prefix++;
    }
    return 1;
}

void auth_parse_basic(const char *auth_header,
                       char *user, int user_max,
                       char *pass, int pass_max)
{
    user[0] = pass[0] = '\0';
    if (!auth_header) return;
    if (str_icase_starts_local(auth_header, "Basic ")) auth_header += 6;
    while (*auth_header == ' ') auth_header++;

    char decoded[256];
    int n = base64_decode(auth_header, decoded, (int)sizeof(decoded));
    if (n <= 0) return;

    char *colon = memchr(decoded, ':', (size_t)n);
    if (!colon) {
        snprintf(user, (size_t)user_max, "%.*s", n, decoded);
        return;
    }
    int ulen = (int)(colon - decoded);
    snprintf(user, (size_t)user_max, "%.*s", ulen, decoded);
    snprintf(pass, (size_t)pass_max, "%s", colon + 1);
}
