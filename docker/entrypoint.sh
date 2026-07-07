#!/bin/bash
# entrypoint.sh — arranca ntripcaster dentro del contenedor.
#
# Si /app/conf/ntripcaster.conf NO existe (primer arranque, o volumen
# vacio), lo genera solo con credenciales de PRUEBA -- overrideables por
# variable de entorno. Las passwords se hashean con el mismo binario
# (ntripcaster --hash-password), nunca se escriben en texto plano en el
# conf. Si el archivo ya existe (volumen persistente de una corrida
# anterior, o uno que vos mismo montaste), se respeta tal cual.
#
# Variables de entorno soportadas (todas opcionales, con default de
# demo/prueba):
#   NTRIP_PORT              puerto de escucha            (2101)
#   NTRIP_MOUNT              nombre del mountpoint         (BASE1)
#   NTRIP_SOURCE_PASSWORD    password del SOURCE           (passbase123)
#   NTRIP_ROVER1_USER        usuario rover 1               (rover1)
#   NTRIP_ROVER1_PASSWORD    password rover 1              (passrover1)
#   NTRIP_ROVER2_USER        usuario rover 2               (rover2)
#   NTRIP_ROVER2_PASSWORD    password rover 2              (passrover2)
#   NTRIP_CASTER_NAME        nombre del caster (sourcetable)(NtripCaster-Docker)
#   NTRIP_OPERATOR           operador (sourcetable)         (unknown)
#   NTRIP_COUNTRY            codigo de pais, 3 letras       (DEU)
#   NTRIP_HTML_TEMPLATE      pagina html para browsers      (templates/sourcetable.html)
#   NTRIP_MAX_CLIENTS        limite de clientes             (1024)
#   NTRIP_MAX_SOURCES        limite de sources              (128)
#   NTRIP_LOG_LEVEL          debug|info|warn|error          (info)
#
# IMPORTANTE: las credenciales de prueba por defecto (passbase123 /
# passrover1 / passrover2) son SOLO para levantar algo rapido y probar.
# Si vas a exponer el contenedor a una red que no controlas, seteá las
# variables NTRIP_*_PASSWORD con algo propio, o montá tu propio
# ntripcaster.conf ya generado (ver README.md, seccion Docker).
set -euo pipefail

CONF_DIR="${NTRIP_CONF_DIR:-/app/conf}"
CONF_FILE="${CONF_DIR}/ntripcaster.conf"
PORT="${NTRIP_PORT:-2101}"

mkdir -p "$CONF_DIR" /app/logs

# El contenedor arranca como root a proposito (ver Dockerfile, ya no
# hace "USER ntrip"). Motivo: los bind-mounts (./docker/conf,
# ./templates) heredan el dueño del path en el HOST -- si Docker los
# creo el mismo porque no existian antes del primer "docker compose
# up", quedan root:root, y el proceso sin privilegios no puede ni
# escribir el conf inicial ni leer el template. Lo arreglamos ACA,
# todavia como root, antes de tocar nada. "|| true" porque un volumen
# montado ":ro" (ver docker/README.md, Opcion B) va a fallar el chown
# de ese archivo puntual -- no es un error real, seguimos igual.
chown -R ntrip:ntrip "$CONF_DIR" /app/logs /app/templates 2>/dev/null || true

if [ ! -f "$CONF_FILE" ]; then
    echo "[entrypoint] no existe $CONF_FILE -- generando uno nuevo"

    MOUNT="${NTRIP_MOUNT:-BASE1}"
    SOURCE_PASS="${NTRIP_SOURCE_PASSWORD:-passbase123}"
    ROVER1_USER="${NTRIP_ROVER1_USER:-rover1}"
    ROVER1_PASS="${NTRIP_ROVER1_PASSWORD:-passrover1}"
    ROVER2_USER="${NTRIP_ROVER2_USER:-rover2}"
    ROVER2_PASS="${NTRIP_ROVER2_PASSWORD:-passrover2}"

    SOURCE_HASH=$(printf '%s' "$SOURCE_PASS" | ntripcaster --hash-password)
    ROVER1_HASH=$(printf '%s' "$ROVER1_PASS" | ntripcaster --hash-password)
    ROVER2_HASH=$(printf '%s' "$ROVER2_PASS" | ntripcaster --hash-password)

    cat > "$CONF_FILE" <<CONF_EOF
; Generado automaticamente por docker/entrypoint.sh en el primer
; arranque de este contenedor/volumen. Las passwords estan hasheadas
; (PBKDF2-HMAC-SHA256) -- ver README.md para el detalle del formato.
[caster]
port = ${PORT}
bind = 0.0.0.0
threads = 0
name = ${NTRIP_CASTER_NAME:-NtripCaster-Docker}
operator = ${NTRIP_OPERATOR:-unknown}
country = ${NTRIP_COUNTRY:-DEU}
html_template = ${NTRIP_HTML_TEMPLATE:-templates/sourcetable.html}
max_clients = ${NTRIP_MAX_CLIENTS:-1024}
max_sources = ${NTRIP_MAX_SOURCES:-128}
client_timeout = 60
source_timeout = 30
handshake_timeout = 10
log_file =
log_level = ${NTRIP_LOG_LEVEL:-info}

[source]
${MOUNT} = ${SOURCE_HASH}

[client:${MOUNT}]
${ROVER1_USER} = ${ROVER1_HASH}
${ROVER2_USER} = ${ROVER2_HASH}
CONF_EOF

    chown ntrip:ntrip "$CONF_FILE"

    echo "[entrypoint] conf generado en $CONF_FILE"
    echo "[entrypoint] credenciales de esta corrida (SOLO texto plano en este log --"
    echo "[entrypoint] son las que tu SOURCE/rover deben usar para conectarse):"
    echo "[entrypoint]   SOURCE  mount=${MOUNT}       password=${SOURCE_PASS}"
    echo "[entrypoint]   ROVER1  user=${ROVER1_USER}  password=${ROVER1_PASS}"
    echo "[entrypoint]   ROVER2  user=${ROVER2_USER}  password=${ROVER2_PASS}"
else
    echo "[entrypoint] usando conf existente en $CONF_FILE (no se regenera)"
fi

# setpriv dropea privilegios de verdad (uid/gid real+efectivo -> ntrip)
# justo antes del exec -- el proceso que queda corriendo y escuchando
# la red NUNCA es root, aunque el entrypoint haya arrancado como tal.
# exec: ntripcaster (vía setpriv) queda como PID 1 -- recibe
# SIGTERM/SIGINT de "docker stop" directo y hace el shutdown ordenado
# (ver src/main.c).
exec setpriv --reuid=ntrip --regid=ntrip --init-groups \
    ntripcaster "$PORT" "$CONF_FILE"
