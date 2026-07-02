/*
 * auth.h — Registro de credenciales (fase 1: NTRIP v1) + parseo Basic Auth
 *
 * Carga un archivo INI (vendor/inih) a una tabla en RAM (uthash) protegida
 * por rwlock. auth_check_*() son las únicas funciones llamadas en el hot
 * path del handshake -- nunca tocan disco.
 *
 * Fase actual: solo carga inicial en auth_registry_load(), llamada una vez
 * desde main() antes de arrancar el io_engine. El reload en caliente
 * (SIGHUP / timer -- ver docs/FEATURE_auth_registry.md) queda para la
 * siguiente pasada; el rwlock ya está en su lugar para que agregar reload
 * después no cambie esta interfaz pública.
 *
 * Fail closed: si un mountpoint/usuario no está en la tabla, se rechaza.
 * Password en texto plano (strcmp) por ahora -- hashear + comparación en
 * tiempo constante es un TODO antes de exponer esto fuera de testing.
 */
#ifndef NTRIPCASTER_AUTH_H
#define NTRIPCASTER_AUTH_H

/*
 * auth_registry_load — Parsea el INI en conf_path y puebla la tabla.
 * Retorna 0 si OK, -1 si el archivo no existe o tiene un error de sintaxis
 * (en ese caso la tabla queda vacía -- todo se rechaza).
 */
int auth_registry_load(const char *conf_path);

/*
 * auth_registry_reload — Reload en caliente (SIGHUP). Construye las
 * tablas nuevas completas SIN lock y las swapea bajo wrlock (sección
 * crítica de microsegundos). Si el archivo está roto conserva la tabla
 * anterior y retorna -1. Llamar SOLO desde contexto normal (el tick_cb
 * del io_engine), nunca desde el signal handler.
 */
int auth_registry_reload(const char *conf_path);

/* auth_registry_destroy — Libera la tabla. Llamar en shutdown. */
void auth_registry_destroy(void);

/*
 * auth_check_source — SOURCE v1: solo password, sin usuario (así es como
 * lo manda el comando "SOURCE <pass> <mountpoint>").
 * Retorna 0 si autorizado, -1 si no.
 */
int auth_check_source(const char *mountpoint, const char *password);

/*
 * auth_check_client — GET (v1 o v2): usuario + password vía Basic Auth.
 * Retorna 0 si autorizado, -1 si no.
 */
int auth_check_client(const char *mountpoint, const char *user, const char *password);

/*
 * auth_parse_basic — Decodifica un header "Authorization: Basic ...".
 * user/pass quedan en "" si no hay header o es inválido.
 */
void auth_parse_basic(const char *auth_header,
                       char *user, int user_max,
                       char *pass, int pass_max);

#endif /* NTRIPCASTER_AUTH_H */
