# NtripCaster en Docker

Imagen multi-stage: se compila con `cmake`/`gcc` en una etapa de build y
la imagen final solo tiene el binario ya compilado (sin toolchain).

## Arranque rápido (credenciales de prueba)

```bash
docker build -t ntripcaster .
docker run --rm -p 2101:2101 ntripcaster
```

Al primer arranque, si no hay un `ntripcaster.conf` ya generado, el
contenedor crea uno con credenciales de **prueba** y las imprime en su
log de arranque (`docker logs ntripcaster`):

```
SOURCE  mount=BASE1       password=passbase123
ROVER1  user=rover1       password=passrover1
ROVER2  user=rover2       password=passrover2
```

Con eso ya podés conectar un source de prueba (`SOURCE passbase123 /BASE1`)
o un rover (`GET /BASE1` + Basic Auth `rover1:passrover1`) contra
`localhost:2101` y probar que todo funciona, sin tocar nada más.

## Con `docker-compose` (recomendado para no perder la config)

```bash
docker compose up -d --build
docker compose logs -f
```

El `docker-compose.yml` monta `./docker/conf` como volumen — el conf
generado (o el que vos pongas ahí) sobrevive a reinicios/recreaciones
del contenedor.

## Usar tus propias credenciales

Antes de exponer el contenedor a internet o a una red que no controlás,
**no dejes las credenciales de prueba**. Dos formas de cambiarlas:

### Opción A — variables de entorno (solo aplica en el primer arranque, cuando el conf todavía no existe)

```bash
docker run --rm -p 2101:2101 \
  -e NTRIP_MOUNT=MIBASE \
  -e NTRIP_SOURCE_PASSWORD='mi-password-real' \
  -e NTRIP_ROVER1_USER=rover1 \
  -e NTRIP_ROVER1_PASSWORD='otro-password-real' \
  -v "$(pwd)/docker/conf:/app/conf" \
  ntripcaster
```

Variables disponibles: `NTRIP_PORT`, `NTRIP_MOUNT`,
`NTRIP_SOURCE_PASSWORD`, `NTRIP_ROVER1_USER`/`NTRIP_ROVER1_PASSWORD`,
`NTRIP_ROVER2_USER`/`NTRIP_ROVER2_PASSWORD`, `NTRIP_CASTER_NAME`,
`NTRIP_OPERATOR`, `NTRIP_COUNTRY`, `NTRIP_HTML_TEMPLATE`,
`NTRIP_MAX_CLIENTS`, `NTRIP_MAX_SOURCES`, `NTRIP_LOG_LEVEL`.

### Opción B — montar tu propio `conf/ntripcaster.conf` ya armado

Si ya generaste un conf en tu máquina (ver el `README.md` de la raíz del
repo, sección "Configure", con `--hash-password`), montalo directo y el
entrypoint lo va a respetar tal cual, sin regenerar nada:

```bash
docker run --rm -p 2101:2101 \
  -v "$(pwd)/mi-ntripcaster.conf:/app/conf/ntripcaster.conf:ro" \
  ntripcaster
```

## Recargar credenciales sin reiniciar el contenedor

El caster soporta reload en caliente por `SIGHUP` (ver `README.md`).
Editá el conf montado y mandale la señal al proceso del contenedor:

```bash
docker kill --signal=HUP ntripcaster
```

## Notas

- La imagen loguea a `stderr` siempre (además de a archivo si `log_file`
  no está vacío) — por eso `docker logs` muestra todo sin configuración
  extra. El conf generado por el entrypoint deja `log_file` vacío a
  propósito para no escribir a disco dentro del contenedor.
- `docker stop` manda `SIGTERM` al proceso (PID 1, gracias a `exec` en
  el entrypoint), que dispara el shutdown ordenado que ya tiene el
  caster (cierra conexiones residuales antes de salir).
- No se recomienda usar el binario de esta imagen fuera de Docker —
  para eso, compilá con `./build.sh` como indica el `README.md`.
- Permisos de `./docker/conf` y `./templates`: no hace falta `chmod`/`chown`
  manual en el host. El contenedor arranca como root (a propósito) y el
  entrypoint corrige el dueño de esos volúmenes montados a `ntrip` antes
  de generar el conf; recién ahí dropea privilegios de verdad (vía
  `setpriv`) y ejecuta el caster. Si ves `Permission denied` seguido de
  reinicios en loop (`docker compose ps` mostrando `Restarting`), es
  casi siempre porque estás corriendo una imagen vieja de antes de este
  cambio — reconstruí con `docker compose up -d --build`.
