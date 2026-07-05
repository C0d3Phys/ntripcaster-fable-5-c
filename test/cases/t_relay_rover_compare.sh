#!/bin/bash
# t_relay_rover_compare.sh -- Escenario end-to-end completo, 100% local
# (sin depender de red externa ni credenciales reales, a diferencia de
# la prueba manual que se hizo contra SNIP):
#
#   mk_rtcm.py (fuente RTCM3 sintetica, CRC24Q real)
#     -> SOURCE v1 -> mount "FEED" (upstream)
#     -> ntrip_source_relay -> mount "BASE1" (local)
#     -> ntrip_rover_client (consume BASE1, decodifica, valida)
#     -> ntrip_capture_compare (relay_rtcm3.bin vs rover_rtcm3.bin)
#
# PASS si:
#   - ntrip_rover_client termina con exit 0 (recibio bytes, decodifico
#     al menos 1 frame valido, cero bytes saltados/corruptos).
#   - ntrip_capture_compare reporta lost_inside == 0 (cero frames
#     perdidos en la ventana comun donde el relay y el rover estuvieron
#     activos a la vez).
#
# Ejercita el mismo camino de codigo que se valido a mano contra SNIP
# (incluye el fix de nt_read_response para respuestas NTRIP v1 de una
# sola linea Y el fix del blank-line de cortesia de nuestro propio
# caster -- ver docs/FEATURE_improvements_FASE_A_20260704.md).

set -uo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$DIR/../helpers/common.sh"

: "${NTRIPCASTER_BIN:?falta NTRIPCASTER_BIN (set_tests_properties en test/CMakeLists.txt)}"
: "${RELAY_BIN:?falta RELAY_BIN}"
: "${ROVER_BIN:?falta ROVER_BIN}"
: "${COMPARE_BIN:?falta COMPARE_BIN}"
: "${MK_RTCM_PY:?falta MK_RTCM_PY}"

WORKDIR=$(mktemp -d /tmp/ntrip_e2e_test_XXXXXX)

UPSTREAM_PASS_VAL="up_test_pass"
LOCAL_SOURCE_PASS_VAL="base_test_pass"
RELAY_CLIENT_USER="relay_user"
ROVER_USER_VAL="rover_test"
ROVER_PASS_VAL="rover_test_pass"
UPSTREAM_MOUNT="FEED"
LOCAL_MOUNT="BASE1"

PUERTO=$(puerto_libre)
CONF=$(conf_test "$PUERTO" "$LOCAL_MOUNT" "$LOCAL_SOURCE_PASS_VAL" \
                  "$ROVER_USER_VAL" "$ROVER_PASS_VAL" \
                  "$UPSTREAM_MOUNT" "$UPSTREAM_PASS_VAL")
{
    echo ""
    echo "[client:$UPSTREAM_MOUNT]"
    echo "$RELAY_CLIENT_USER = $UPSTREAM_PASS_VAL"
} >> "$CONF"

LOG="$WORKDIR/caster.log"
PIDS_TO_KILL=()
RC=1

cleanup() {
    for pid in "${PIDS_TO_KILL[@]:-}"; do
        [[ -n "$pid" ]] && kill -TERM "$pid" 2>/dev/null
    done
    mata_caster
    sleep 0.3
    for pid in "${PIDS_TO_KILL[@]:-}"; do
        [[ -n "$pid" ]] && kill -KILL "$pid" 2>/dev/null
    done
    rm -f "$CONF"
    if [[ "${TEST_KEEP_WORKDIR:-0}" == "1" ]]; then
        echo "workdir conservado en: $WORKDIR (TEST_KEEP_WORKDIR=1)" >&2
    else
        rm -rf "$WORKDIR"
    fi
}
trap 'cleanup; exit 1' INT TERM
trap cleanup EXIT

echo "=== 1. Arrancando caster en 127.0.0.1:$PUERTO ===" >&2
arranca_caster "$NTRIPCASTER_BIN" "$PUERTO" "$CONF" "$LOG"
PIDS_TO_KILL+=("$CASTER_PID")
if ! espera_puerto 127.0.0.1 "$PUERTO" 5; then
    echo "FAIL: el caster no llego a escuchar en el puerto" >&2
    exit 1
fi

echo "=== 2. Empujando RTCM3 sintetico a $UPSTREAM_MOUNT (upstream) ===" >&2
python3 "$MK_RTCM_PY" push 127.0.0.1 "$PUERTO" "$UPSTREAM_MOUNT" \
    "$UPSTREAM_PASS_VAL" 16 --rate-hz 30 >"$WORKDIR/feeder.out" 2>&1 &
FEEDER_PID=$!
PIDS_TO_KILL+=("$FEEDER_PID")
sleep 1.5   # que $UPSTREAM_MOUNT ya tenga un source activo antes de que el relay intente el GET

echo "=== 3. Relay: $UPSTREAM_MOUNT -> $LOCAL_MOUNT ===" >&2
# exec acá es clave: sin él, el subshell "(...)" queda como proceso
# padre del binario del relay -- kill -TERM "$RELAY_PID" mataría solo el
# subshell wrapper y el relay quedaría huérfano corriendo en segundo
# plano, todavía escribiendo su captura cuando el script ya la lee (bug
# real encontrado: la captura quedaba fija en 4096 bytes -- exactamente
# el buffer interno de stdio antes del primer flush automático, con el
# resto de los datos todavía sin escribir en el proceso huérfano). Con
# exec, el subshell SE REEMPLAZA por el binario -- mismo PID, sin capas.
(
    cd "$WORKDIR" || exit 1
    UPSTREAM_PASS="$UPSTREAM_PASS_VAL" LOCAL_SOURCE_PASS="$LOCAL_SOURCE_PASS_VAL" \
        exec "$RELAY_BIN" 127.0.0.1 "$PUERTO" "$UPSTREAM_MOUNT" "$RELAY_CLIENT_USER" \
                          127.0.0.1 "$PUERTO" "$LOCAL_MOUNT"
) >"$WORKDIR/relay.out" 2>&1 &
RELAY_PID=$!
PIDS_TO_KILL+=("$RELAY_PID")
sleep 1.5   # que el relay ya este registrado como SOURCE en BASE1

if ! kill -0 "$RELAY_PID" 2>/dev/null; then
    echo "FAIL: el relay murio antes de arrancar el rover" >&2
    cat "$WORKDIR/relay.out" >&2
    exit 1
fi

echo "=== 4. Rover consumiendo $LOCAL_MOUNT por 8s ===" >&2
(
    cd "$WORKDIR" || exit 1
    LOCAL_ROVER_PASS="$ROVER_PASS_VAL" \
        exec "$ROVER_BIN" 127.0.0.1 "$PUERTO" "$LOCAL_MOUNT" "$ROVER_USER_VAL" 8
) >"$WORKDIR/rover.out" 2>&1
ROVER_RC=$?

echo "=== 5. Deteniendo feeder y relay (esperando cierre limpio) ===" >&2
# Orden importante: matar el feeder PRIMERO y esperarlo -- asi el relay
# se queda sin datos entrantes, su recv() del upstream devuelve EOF, y
# sale por su propio camino normal (fclose + nt_capture_write_meta) en
# vez de que lo cortemos a mitad de un fwrite. Un sleep fijo aca no
# alcanza: hay que esperar a que el proceso realmente termine antes de
# leer su captura, si no el archivo puede quedar truncado a mitad de
# escritura (causa real de un "rover_unmatched" espurio visto en una
# corrida anterior de este mismo test).
kill -TERM "$FEEDER_PID" 2>/dev/null
wait "$FEEDER_PID" 2>/dev/null

kill -TERM "$RELAY_PID" 2>/dev/null
RELAY_WAIT_OK=0
for _ in $(seq 1 50); do   # hasta 5s
    if ! kill -0 "$RELAY_PID" 2>/dev/null; then
        RELAY_WAIT_OK=1
        break
    fi
    sleep 0.1
done
if [[ "$RELAY_WAIT_OK" -eq 0 ]]; then
    echo "WARN: el relay no salio solo tras SIGTERM, forzando kill -9" >&2
    kill -KILL "$RELAY_PID" 2>/dev/null
fi
wait "$RELAY_PID" 2>/dev/null

if [[ ! -f "$WORKDIR/capture_rtcm3_bin_UTC/current_session.txt" ]]; then
    echo "FAIL: no se genero sesion de captura (el relay nunca llego a conectar?)" >&2
    echo "--- relay.out ---" >&2; cat "$WORKDIR/relay.out" >&2
    echo "--- rover.out ---" >&2; cat "$WORKDIR/rover.out" >&2
    exit 1
fi

SESSION_REL=$(cat "$WORKDIR/capture_rtcm3_bin_UTC/current_session.txt")
SESSION_DIR="$WORKDIR/$SESSION_REL"

echo "=== 6. Comparando capturas ($SESSION_DIR) ===" >&2
COMPARE_OUT=$("$COMPARE_BIN" "$SESSION_DIR" 2>&1)
COMPARE_RC=$?
echo "$COMPARE_OUT" >&2

LOST=$(echo "$COMPARE_OUT" | grep -o 'lost_inside=[0-9]*' | cut -d= -f2)

FAIL=0
if [[ "$ROVER_RC" -ne 0 ]]; then
    echo "FAIL: ntrip_rover_client exit=$ROVER_RC (revisar bytes/frames/corrupt)" >&2
    cat "$WORKDIR/rover.out" >&2
    FAIL=1
fi
if [[ "$COMPARE_RC" -ne 0 ]]; then
    echo "FAIL: ntrip_capture_compare exit=$COMPARE_RC" >&2
    FAIL=1
fi
if [[ -z "$LOST" ]]; then
    echo "FAIL: no se pudo leer 'lost_inside=' de la salida del compare" >&2
    FAIL=1
elif [[ "$LOST" -ne 0 ]]; then
    echo "FAIL: lost_inside=$LOST (se esperaba 0 -- hay frames perdidos en el tramo comun)" >&2
    FAIL=1
fi

if [[ "$FAIL" -eq 0 ]]; then
    echo "PASS: relay+rover+compare end-to-end, lost_inside=0" >&2
    RC=0
else
    RC=1
fi

exit "$RC"
