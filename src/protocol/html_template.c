/*
 * html_template.c — Implementación (ver decisión de diseño en html_template.h)
 */
#include "html_template.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Limite de tamano del archivo de template -- una pagina de sourcetable
 * no tiene por que pasar de esto ni de cerca; es solo una salvaguarda
 * contra apuntar por error a un archivo gigante. */
#define TEMPLATE_FILE_MAX (256 * 1024)

/* safe_append — copia hasta 'len' bytes de src a out[pos..], sin pasarse
 * de max (deja siempre lugar para el '\0' final). Devuelve el nuevo pos. */
static int safe_append(char *out, int pos, int max, const char *src, int len)
{
    if (pos >= max - 1) return pos;
    int avail = max - 1 - pos;
    if (len > avail) len = avail;
    if (len > 0) memcpy(out + pos, src, (size_t)len);
    return pos + len;
}

int html_template_render(const char *path, const html_var_t *vars, int nvars,
                          char *out, int max)
{
    if (max <= 0) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long fsize = ftell(f);
    if (fsize < 0 || fsize > TEMPLATE_FILE_MAX) { fclose(f); return -1; }
    rewind(f);

    char *tpl = malloc((size_t)fsize + 1);
    if (!tpl) { fclose(f); return -1; }

    size_t rd = fread(tpl, 1, (size_t)fsize, f);
    fclose(f);
    tpl[rd] = '\0';

    int pos = 0;
    for (size_t i = 0; i < rd; ) {
        if (tpl[i] == '{' && i + 1 < rd && tpl[i + 1] == '{') {
            size_t j = i + 2;
            while (j + 1 < rd && !(tpl[j] == '}' && tpl[j + 1] == '}')) j++;

            if (j + 1 < rd && tpl[j] == '}' && tpl[j + 1] == '}') {
                size_t klen = j - (i + 2);
                const char *value = NULL;

                for (int k = 0; k < nvars; k++) {
                    size_t kl = strlen(vars[k].key);
                    if (kl == klen && strncmp(vars[k].key, tpl + i + 2, klen) == 0) {
                        value = vars[k].value;
                        break;
                    }
                }

                if (value) {
                    pos = safe_append(out, pos, max, value, (int)strlen(value));
                } else {
                    /* placeholder desconocido -- se deja literal (visible)
                     * para que un typo en el .html no pase desapercibido */
                    pos = safe_append(out, pos, max, tpl + i, (int)(j + 2 - i));
                }
                i = j + 2;
                continue;
            }
        }
        pos = safe_append(out, pos, max, tpl + i, 1);
        i++;
    }
    out[(pos < max) ? pos : max - 1] = '\0';

    free(tpl);
    return pos;
}
