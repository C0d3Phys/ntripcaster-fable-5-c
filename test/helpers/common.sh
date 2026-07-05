#!/bin/bash
# common.sh -- helpers compartidos por los casos de test/cases/*.sh
#
# Uso tipico dentro de un caso:
#   source "$(dirname "$0")/../helpers/common.sh"
#   PUERTO=$(puerto_libre)
#   arranca_caster "$NTRIPCASTER_BIN" "$PUERTO" "$CONF" "$LOG"
#   espera_puerto 127.0.0.1 "$PUERTO" 5
#   ... correr el test ...
#   mata_caster
#
# Todas las funciones fallan ruidoso (exit != 0 + mensaje a stderr) en
# vez de colgarse en silencio -- un test que no puede ni arrancar el
# caster debe reportarse como FAIL, no como timeout sin explicacion.

set -u

# puerto_libre -- abre un socket efimero, lee el puerto que el SO le
# asigno, lo cierra, e imprime el numero. Permite correr varios casos
# en paralelo (ctest -j) sin pisarse el puerto entre ellos.
puerto_libre() {
    python3 -c '
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
'
}

# espera_puerto HOST PUERTO TIMEOUT_S -- poll hasta que el puerto acepte
# conexiones TCP, o falla con exit 1 si se cumple el timeout.
espera_puerto() {
    local host="$1" puerto="$2" timeout="${3:-5}"
    local intentos=$((timeout * 10))
    for _ in $(seq 1 "$intentos"); do
        if python3 -c "
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(0.2)
try:
    s.connect(('$host', $puerto))
    sys.exit(0)
except OSError:
    sys.exit(1)
" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    echo "espera_puerto: $host:$puerto no respondio en ${timeout}s" >&2
    return 1
}

# arranca_caster BIN PUERTO CONF LOG -- levanta el caster en background,
# guarda su PID en $CASTER_PID (variable global del caller).
arranca_caster() {
    local bin="$1" puerto="$2" conf="$3" log="$4"
    "$bin" "$puerto" "$conf" "$log" &
    CASTER_PID=$!
}

# mata_caster -- para el caster arrancado con arranca_caster, si sigue vivo.
mata_caster() {
    if [[ -n "${CASTER_PID:-}" ]] && kill -0 "$CASTER_PID" 2>/dev/null; then
        kill -TERM "$CASTER_PID" 2>/dev/null
        wait "$CASTER_PID" 2>/dev/null
    fi
}

# conf_test PUERTO SOURCE_PASS CLIENT_USER CLIENT_PASS MOUNT [MOUNT2 SOURCE_PASS2] ...
# Genera un conf minimo de test en un archivo temporal e imprime su path.
# Soporta 1 o 2 mountpoints (el segundo par de argumentos es opcional,
# usado por los casos que necesitan un mount "upstream" ademas de BASE1).
conf_test() {
    local puerto="$1" mount="$2" source_pass="$3" client_user="$4" client_pass="$5"
    local mount2="${6:-}" source_pass2="${7:-}"
    local conf
    conf=$(mktemp /tmp/ntripcaster_test_XXXXXX.conf)

    {
        echo "[caster]"
        echo "port = $puerto"
        echo "bind = 127.0.0.1"
        echo "name = TestCaster"
        echo "max_clients = 64"
        echo "max_sources = 16"
        echo "client_timeout = 30"
        echo "source_timeout = 30"
        echo "handshake_timeout = 5"
        echo "log_level = debug"
        echo ""
        echo "[source]"
        echo "$mount = $source_pass"
        if [[ -n "$mount2" ]]; then
            echo "$mount2 = $source_pass2"
        fi
        echo ""
        echo "[client:$mount]"
        echo "$client_user = $client_pass"
    } > "$conf"

    echo "$conf"
}
