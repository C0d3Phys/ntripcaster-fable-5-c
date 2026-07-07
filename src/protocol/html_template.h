/*
 * html_template.h — Motor minimo de templates HTML ({{CLAVE}})
 *
 * Por que existe: sourcetable.c necesitaba servir una pagina HTML al
 * navegador (ver ntrip.h, deteccion de User-Agent: Mozilla). Tenerla
 * como un monton de snprintf con el HTML pegado en el .c mezclaba
 * "vista" con logica de protocolo y hacia molesto retocar el diseno
 * sin recompilar. Esto lee un archivo .html de disco (con
 * placeholders tipo {{OPERATOR}}) y lo devuelve ya resuelto.
 *
 * Deliberadamente simple: sin cache, sin dependencias -- se lee el
 * archivo en cada request. La sourcetable HTML la pide un browser
 * humano de vez en cuando, no un rover a 1Hz, asi que el I/O extra es
 * irrelevante. A cambio: el operador puede editar el .html y ver el
 * cambio en el proximo refresh, sin reiniciar el caster.
 */
#ifndef NTRIPCASTER_HTML_TEMPLATE_H
#define NTRIPCASTER_HTML_TEMPLATE_H

typedef struct {
    const char *key;    /* nombre sin llaves, ej "OPERATOR" */
    const char *value;  /* ya debe venir HTML-escapado si hace falta */
} html_var_t;

/*
 * html_template_render — Lee path, sustituye cada {{KEY}} por su
 * valor segun vars[]. Un placeholder sin match en vars[] se deja tal
 * cual en la salida (visible a proposito -- ayuda a pescar typos en
 * el template en vez de tragarselos en silencio).
 *
 * Retorna bytes escritos en out (sin contar el '\0' final), o -1 si
 * el archivo no se pudo abrir/leer o excede el limite interno de
 * tamano de archivo soportado.
 */
int html_template_render(const char *path, const html_var_t *vars, int nvars,
                          char *out, int max);

#endif /* NTRIPCASTER_HTML_TEMPLATE_H */
