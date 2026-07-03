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

### B10. Accept único por evento con EPOLLET — ráfagas dejan conexiones varadas — medio ✅ corregido
`src/core/io_engine.c` — descubierto empíricamente en el load test de 200
clientes: el `listen_fd` está registrado con `EPOLLET` (el kernel notifica
una vez por transición del backlog), pero `accept_loop` hacía UN solo
`accept4()` por evento. Ante una ráfaga (200 rovers reconectando a la vez
tras un corte), el resto quedaba varado en el backlog hasta que llegara
OTRA conexión nueva: **solo 124 de 200 clientes simultáneos entraban**.
Fix: aceptar en loop hasta `EAGAIN` (obligatorio con edge-triggered).
Post-fix: 200/200 aceptados.

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

## Parte 2b — Sesión 2026-07-03 (debug de sources GNSS reales)

### B11. Parser RTCM3 perdía frames partidos por TCP — medio ✅ corregido
`src/gnss/rtcm3_frame.c` — bug histórico descubierto con test de frames
fragmentados: si el buffer terminaba en un frame a medio llegar SIN ningún
frame completo antes, el fallback consumía `len-6` bytes — se comía el
preamble `0xD3` del frame pendiente y el frame entero se perdía como basura
al llegar el resto. En streaming (fragmentación TCP normal) descartaba
frames válidos sistemáticamente. Fix: `bytes_used` = inicio exacto del tail
incompleto. Verificado: 51/51 frames (antes 41/51).

### F4. Decode incremental por source + stats de integridad ✅
`mountpoint.h/c` — buffer de reensamblado por mount (8KB): los frames
partidos entre `read()`s se completan en el próximo chunk. Contadores:
frames válidos (CRC ok), bytes corruptos (preamble falso / CRC malo), y
tabla por tipo de mensaje. Verificado con inyección de ruido + CRC roto:
118B corruptos detectados exactos, 0 falsos positivos.

### F5. Reporte periódico de integridad (30 s) ✅
`io_engine.c` — por cada mount activo:
`stats: mount=BASE1 ONLINE clientes=2  1.4 KB/s  frames+210 (tot 2516)  corrupto+0B (tot 0)  tipos: 1074:60 1084:60 1006:6 ...`
Comparable 1:1 contra el panel de SNIP para detectar corrupción en el camino.

### F6. Debug de protocolo: request y response visibles ✅
Con `NTRIPCASTER_LOG=debug`: cada request entrante se loguea COMPLETO con
credenciales enmascaradas (`SOURCE *** BASE1`, `Authorization: Basic ***`)
y cada respuesta enviada se loguea (`<- ICY 200 OK`, `<- HTTP/1.1 401 ...`).

### Fix operativo
`conf/ntripcaster.conf` registraba `Demo1` pero los equipos publicaban a
`BASE1` → rechazo fail-closed. El "36.1% connected" de SNIP era el promedio
acumulado incluyendo la ventana de rechazos, no un problema del relay.
También: `src/CMakeLists.txt` no incluía `core/logger.c` (link roto) — el
único daño real de la sesión interrumpida; el resto de archivos estaba
íntegro.

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

### Load test: 1 source → 200 clientes (VM 2 CPUs, sandbox)

Source emitiendo 14 KB por epoch (MSM7 típico a ~1 Hz), 200 clientes
suscritos simultáneamente:

| Métrica | Valor |
|---|---|
| Threads del proceso | 3 (1 accept + 2 workers = nproc) — constante, NO crece con conexiones |
| RSS arranque | ~35 MB (dominado por 128 rings × 256 KB pre-tocados por el memset de broker_init) |
| RSS con 200 clientes activos | ~35.3 MB (+~0.3 MB; el heap por conexión es lazy) |
| Clientes aceptados en ráfaga | 200/200 (tras fix B10; antes 124/200) |
| Frames entregados | 6/6 epochs × 200 clientes = 100 % |
| Latencia source→cliente (mín) | ~1–9 ms |
| Latencia source→cliente (máx, el último de 200) | ~7–21 ms |
| Spread primero→último | 3–16 ms por epoch |
| Kicks por lag | 0 |

El spread incluye el ruido del harness Python compartiendo las mismas 2
CPUs con el caster; el costo real del caster por epoch es ~0.3 ms de
`clients_wakeup` (200 × epoll_ctl) + ~200 × (memcpy 14 KB + write) repartido
entre los workers. Para RTK (correcciones útiles por 1–2 s) es ruido.
