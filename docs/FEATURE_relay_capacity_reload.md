# Feature: Capacidad de relay + reload de configuración

Plan de trabajo sobre dos hallazgos puntuales de la auditoría de esta
semana: qué tan lejos aguanta el relay source→clientes antes de afectar
al protocolo, y cuál es la forma correcta de recargar configuración sin
pegarle a disco en el hot path.

Estado: planeado, sin código todavía. Complementa a
`FEATURE_auth_registry.md` (comparten el mecanismo de reload).

---

## 1. Relay: qué límites hay hoy y cuáles corregir

### 1.1 Diagnóstico (ya verificado en código, no es especulación)

- **Límite duro real:** `CLIENT_MAX_PER_MOUNT = 256` (`mountpoint.h`).
  El cliente 257 a un mismo mountpoint se rechaza limpio (`mp_client_subscribe`
  retorna -1 → 404). `MOUNTPOINT_MAX = 128` limita mountpoints totales.
- **Config decorativa (bug):** `broker_config_t.max_clients=1024` y
  `max_sources=128` se imprimen en el log de arranque pero **no se
  usan en ningún lado** — `broker_conn_alloc()` nunca los chequea. El
  log da la impresión de un tope global que no existe.
- **Cuello de botella que crece con N:** `clients_wakeup()` recorre
  TODOS los clientes de un mountpoint bajo `rdlock`, haciendo un
  `epoll_ctl(MOD)` secuencial por cada uno, en el mismo thread que
  atiende al source. Con epochs a 1 Hz y cientos de clientes, esto le
  agrega latencia directa al ciclo de lectura del source.
- **Riesgo bajo ráfaga — el más serio de los cuatro:** si un burst
  del source despierta más clientes de los que
  `IO_WORK_QUEUE_CAP = 1024` puede sostener en ese instante, el
  exceso se descarta (`wq_push` falla) y solo se loguea
  `"work queue full, dropping fd=%d"`. Como el fd queda en
  EPOLLONESHOT sin re-armar, esa conexión no se cierra — se congela
  en silencio hasta que el cliente la abandone por timeout de su lado.
- **Comentario desactualizado:** `ring_buffer.h` dice "256 KB ≈ ~2
  segundos de datos MSM7 a 115 kbps". La cuenta real es
  256KB / 14KB/s ≈ **18 segundos** (coincide con lo que ya dice el
  CLAUDE.md). Es solo el comentario, no afecta comportamiento.

### 1.2 Qué se corrige

| # | Acción | Por qué |
|---|---|---|
| 1 | Enforce real de `config.max_clients` / `max_sources` en `broker_conn_alloc`/`broker_*_register`, usando los atomics `active_clients`/`active_sources` que ya existen en `broker_t` — no hace falta agregar contadores nuevos. | Hoy el número en el log miente. O se aplica o se saca del log. |
| 2 | `wq_push` que falla en `accept_loop` → cerrar la conexión activamente (`io_engine_conn_close`) en vez de solo loguear. | Falla ruidosa y honesta en vez de una conexión congelada sin explicación. Es un bug de correctness, no solo de capacidad. |
| 3 | Corregir el comentario de `ring_buffer.h` (2s → 18s). | Documentación, cero riesgo. |
| 4 | `CLIENT_MAX_PER_MOUNT` pasa de `#define` fijo a valor leído del config (mismo mecanismo de la fase de config-loading de auth). Default se mantiene en 256. | Hoy para subir el límite hay que recompilar. Con el loader de config ya construido para auth, es prácticamente gratis exponer este valor también. |

### 1.3 Qué se documenta pero se difiere

`clients_wakeup()` O(N) por mountpoint no se toca todavía — a la
escala esperada de este proyecto (decenas a un par de cientos de
clientes por mountpoint) no es un problema real. Umbral para
revisitarlo: si algún mountpoint necesita sostener más de ~500-1000
clientes concurrentes, ahí vale la pena mover el wakeup a un modelo
por lotes o asíncrono en vez del walk secuencial actual.

`ulimit -n` / límite de file descriptors del SO es una preocupación
operativa (systemd unit / configuración de WSL), no de código — queda
anotado para cuando armemos el deploy, no es parte de esta fase.

### 1.4 Capacidad resultante (con las correcciones de 1.2 aplicadas)

Hasta 256 clientes por mountpoint sin degradación (rechazo limpio más
allá de eso, configurable a futuro). Por debajo de eso, en LAN/WSL no
debería notarse latencia. El techo real para escalar más pasa por la
work queue bajo ráfagas de reconexión simultánea, no por ancho de
banda ni por el ring buffer.

---

## 2. Reload de configuración

### 2.1 Decisión: no se lee en cada request

Leer SQLite/JSON (o cualquier disco) en el handshake sería peor que
cualquiera de los cuellos de botella de la sección 1 — I/O de disco en
el path crítico, y si se protege con un lock, serializa TODAS las
conexiones entre sí (peor que el walk de `clients_wakeup`, que al
menos es O(N) sobre clientes de un solo mountpoint, no sobre todas las
conexiones del caster).

### 2.2 Mecanismo (comparte diseño con `FEATURE_auth_registry.md` §2.3)

Cache en RAM (`uthash`) + `pthread_rwlock_t`, poblada al arrancar
desde SQLite/JSON:

- Lookup (cada handshake): `rdlock` → `HASH_FIND` → copiar a variable
  local → `rdunlock`. No toca disco nunca.
- Reload: construir la tabla nueva completa **aparte**, y solo al
  final `wrlock` → reemplazar el puntero + liberar la vieja →
  `wrunlock`. Sección crítica de microsegundos sin importar el tamaño
  del registro.

### 2.3 Qué dispara el reload — se agregan las dos vías, no una sola

- **`SIGHUP`** — reload inmediato bajo pedido explícito. Mismo patrón
  que ya usa el caster legacy (`sig_hup` en su `main.c`), así que es
  un concepto conocido, no algo nuevo que inventamos.
- **Timer liviano de respaldo** (ej. cada 30-60s, corriendo en el
  mismo thread que ya vamos a necesitar para timeouts de
  clientes/sources) que chequea `PRAGMA data_version` (SQLite) o
  `mtime` (JSON) y dispara el mismo reload si cambió. Cubre el caso de
  automatización (un script que edita el registro sin mandar la señal
  manualmente).

### 2.4 Un solo snapshot atómico, no varios reloads independientes

Detalle importante que no estaba explícito antes: si a futuro
`CLIENT_MAX_PER_MOUNT` y otros valores (timeouts, etc.) también pasan
a vivir en el config recargable (ver 1.2 punto 4), el reload **no**
debería ser "una tabla ACL con su propio rwlock + un entero con el
suyo" — eso puede dejar al sistema leyendo una combinación
inconsistente a mitad de un reload (ACL nueva con límite viejo, por
ejemplo). La forma correcta es un único struct `config_snapshot_t`
que agrupe ACL + límites + timeouts, y el swap con `wrlock` reemplaza
el puntero a ese struct completo de una sola vez. Todo lo demás del
mecanismo (RAM cache, rdlock en el hot path, reload async aparte) es
igual a lo ya descrito.

### 2.5 Dónde se conecta con el código actual

Mismo punto que ya estaba anotado en `FEATURE_auth_registry.md` §2.4
(`broker_source_register`/`broker_client_register` consultando el
snapshot antes de attach/subscribe) — ahora ese mismo snapshot también
resuelve el límite de `max_clients_per_mount` de la sección 1.2, sin
agregar un mecanismo de reload separado.

---

## 3. Orden de ejecución propuesto

1. Fix rápido, independiente de todo lo demás: `wq_push` fallido cierra
   la conexión en vez de solo loguear (1.2 #2). Es un bug de
   correctness, no depende del resto del plan.
2. Fix rápido: comentario de `ring_buffer.h` (1.2 #3).
3. Enforce de `max_clients`/`max_sources` con los atomics existentes
   (1.2 #1) — no depende de tener config-loading todavía, funciona con
   los valores hardcodeados actuales de `broker_config_t`.
4. Diseñar `config_snapshot_t` (ACL + límites + timeouts) como una sola
   unidad — esto reemplaza/amplía el paso 2 de `FEATURE_auth_registry.md`
   (que hablaba solo de la tabla ACL).
5. Loader (JSON o SQLite, a decidir) + `auth_check()` + rwlock + reload
   por SIGHUP — igual que ya estaba planeado.
6. Agregar el timer de respaldo para reload automático (2.3).
7. Mover `CLIENT_MAX_PER_MOUNT` de `#define` a campo del snapshot
   (1.2 #4).
8. Probar contra el relay real de SNIP con varios clientes simulados en
   paralelo para confirmar que el enforce de límites y el reload no
   rompen el flujo real.

