# wsl-project-tested — Bugs y Features (orden: más fácil → más difícil)

Análisis 2026-07-02. Este proyecto ya tiene resuelto lo que la variante
`ntripcaster/` tenía pendiente en io_engine (half-close con datos pendientes,
orden DEL→detach→close, `io_engine_stop` async-signal-safe, payload
pipelineado del source) y la fase 1 de auth (SOURCE v1 + clientes GET contra
`conf/ntripcaster.conf` vía inih+uthash). Lo de abajo es lo que faltaba,
cruzado con los planes de `docs/FEATURE_auth_registry.md` y
`docs/FEATURE_relay_capacity_reload.md`.

---

## Parte 1 — Bugs

### B1. Comentario desactualizado en ring_buffer.h — trivial ✅ corregido
"256 KB ≈ ~2 segundos" → la cuenta real es 256KB / 14KB/s ≈ **18 segundos**
(coincide con CLAUDE.md). Ya estaba diagnosticado en
FEATURE_relay_capacity_reload §1.1. Solo documentación.

### B2. `addrlen` no se resetea en el accept loop — trivial ✅ corregido
`src/core/io_engine.c` — `accept4()` es value-result sobre `addrlen`: lo
modifica en cada llamada y no se reseteaba. Fix: `addrlen = sizeof(addr)`
dentro del loop, antes de cada accept.

### B3. Entradas de sourcetable sin inicializar — trivial ✅ corregido
`src/core/broker.c` — `broker_sourcetable_fill()` hace `strncpy(..., size-1)`
sobre `entries[]` de stack sin `memset` → strings potencialmente sin `\0`.

### B4. `EPOLLRDHUP` faltante al re-armar el handshake — trivial ✅ corregido
`src/core/io_engine.c` — `dispatch_handshake()` re-armaba sin `EPOLLRDHUP`
(inconsistente con accept y dispatch_source).

### B5. Log de sourcetable v1 dice "entries" pero imprime bytes — trivial ✅ corregido
`src/protocol/sourcetable.c` — cosmético.

### B6. Error de `read()` ignorado en `dispatch_client` — fácil ✅ corregido
`src/core/io_engine.c` — un read de GGA que falla con errno real (≠EAGAIN)
seguía como si nada. Fix: cerrar la conexión.

### B7. Cola de trabajo llena congela la conexión — fácil ✅ corregido
Diagnóstico de FEATURE_relay_capacity_reload §1.1 ("el más serio"): si
`wq_push` falla, el fd queda en EPOLLONESHOT sin re-armar — conexión
congelada en silencio. Fix elegido por el propio doc (§1.2 #2): cerrar la
conexión activamente. Falla ruidosa y honesta.

### B8. `max_clients` / `max_sources` decorativos — fácil ✅ corregido
FEATURE_relay_capacity_reload §1.2 #1: se imprimían en el log pero nadie los
chequeaba. Fix: enforce en `broker_client_register`/`broker_source_register`
usando los atomics `active_clients`/`active_sources` que ya existían.

### B9. `dispatch_client` llena el write_buf una sola vez — medio ✅ corregido
Si el ring tiene más datos que el espacio libre del write_buf, el resto
esperaba al próximo wakeup del source. Fix: loop fill→flush hasta drenar
el ring o llenar el socket.

---

## Parte 2 — Features implementadas en esta pasada

### F1. Auth para POST v2 (source) — fácil ✅ implementado
`src/protocol/ntrip_v2.c` tenía la nota "auth_check_source() todavía NO está
cableada acá" — era el siguiente paso explícito del plan. Ahora el POST v2
valida el password del Basic Auth contra `[source]` del conf con la misma
`auth_check_source()` que usa v1. Falla → `401 Unauthorized`.

### F2. Timeouts de inactividad — medio ✅ implementado
Pendiente del CLAUDE.md ("kick de sources/clientes inactivos"):
- Lista global de conexiones en `broker_t` (`conns_head` + mutex, campos
  `gnext`/`gprev` en `conn_t`).
- Sweep cada 5 s en el accept loop (mismo thread, sin timer nuevo):
  handshake > `handshake_timeout_s` (10 s), source > `source_timeout_s`
  (30 s), cliente > `client_timeout_s` (60 s) → `shutdown(fd)`.
- El shutdown NO libera nada: dispara el evento epoll y el worker cierra
  por el camino normal (`io_engine_conn_close`) — sin races con el pool,
  compatible con el orden DEL→detach→close ya documentado.

### F3. Reload en caliente del registro de auth (SIGHUP) — medio ✅ implementado
Fase 3 de FEATURE_auth_registry, con el patrón exacto del doc (§2.3):
- `auth_registry_reload()` construye las tablas nuevas COMPLETAS aparte
  (sin lock), y solo al final hace `wrlock` → swap de punteros → libera
  las viejas → `wrunlock`. Sección crítica de microsegundos.
- Disparador: `SIGHUP` → el handler (async-signal-safe, solo escribe un
  `volatile sig_atomic_t`) marca el flag; el accept loop lo detecta en su
  ciclo (≤200 ms) y ejecuta el reload vía callback `tick_cb` del engine —
  nunca dentro del signal handler.
- Uso: editar `conf/ntripcaster.conf` → `kill -HUP <pid>` → sin reiniciar.

---

## Parte 3 — Pendientes (siguientes fases, según los docs)

En orden sugerido:

1. **Timer de respaldo para reload** (FEATURE_relay §2.3) — chequear `mtime`
   del conf cada 30-60 s en el sweep ya existente y disparar el mismo reload.
   Fácil ahora que el reload existe.
2. **`config_snapshot_t` unificado** (FEATURE_relay §2.4) — ACL + límites +
   timeouts en un solo struct swapeable, para que `CLIENT_MAX_PER_MOUNT` y
   los timeouts sean configurables sin recompilar. Medio.
3. **Passwords hasheados + comparación en tiempo constante** (nota en
   auth.h) — antes de exponer el caster fuera de testing. Medio.
4. **NEAREST** — parsear GGA NMEA del cliente (`io_engine.c` ya los descarta
   explícitamente) → `broker_nearest()` → redirigir. Difícil.
5. **Chunked Transfer-Encoding** para sources v2. Difícil.
6. **Admin/historial/kick/mapa** (FEATURE_auth §4) — explícitamente al final.
7. **Race B10 heredado** — `clients_wakeup()` puede encolar un evento de un
   conn que otro worker está cerrando (ventana mínima; el orden
   DEL→detach→close ya elimina el caso del fd reciclado, pero un evento ya
   encolado en `work_queue` apuntando al conn liberado sigue siendo posible).
   Fix real: refcount por conexión. Difícil.

---

## Parte 4 — Verificación (sandbox Linux, 2026-07-02)

Compilado con `gcc -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wpedantic` — **0 warnings**.

| Test | Resultado |
|------|-----------|
| SOURCE v1 password malo | ✅ `ERROR - Bad Password` |
| SOURCE v1 mountpoint fuera del registro | ✅ rechazado (fail closed) |
| SOURCE v1 con `/Demo1` + payload pipelineado | ✅ registrado, 10 bytes recuperados |
| Cliente sin credenciales (v1) | ✅ `ERROR - Bad Password` |
| Cliente `rover1:clientpass1` + relay | ✅ `ICY 200 OK` + frames RTCM3 completos |
| POST v2 password malo | ✅ `401 Unauthorized` (antes aceptaba todo) |
| POST v2 password OK | ✅ `HTTP/1.1 200 OK` |
| Reload SIGHUP (`rover9` agregado en caliente) | ✅ rechazado antes → aceptado después, sin reiniciar |
| Handshake timeout (10 s) | ✅ kick a los ~12 s (sweep cada 5 s) |
| Shutdown limpio (SIGINT) | ✅ |
