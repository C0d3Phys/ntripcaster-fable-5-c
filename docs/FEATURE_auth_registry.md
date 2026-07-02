# Feature: Auth + Registro de clientes/sources

Plan de trabajo para la siguiente fase del caster: autenticación NTRIP v1/v2
contra un registro persistente de usuarios/mountpoints permitidos, con
reorganización de `ntrip.c` como paso previo.

Estado: planeado, sin código todavía. Este documento es la referencia para
no perder el hilo entre sesiones.

---

## 0. Motivación

Hoy `broker_source_register()` y `broker_client_register()` aceptan
cualquier SOURCE/GET sin verificar password ni permisos. Todo el mundo
puede publicar o leer cualquier mountpoint. Esta fase cierra eso.

De paso, `ntrip.c` (418 líneas) mezcla en una sola función
(`ntrip_handle_request`) el dispatch por versión con la implementación
completa de cada rama (SOURCE v1, POST v2, GET cliente, sourcetable).
Antes de meter auth ahí adentro conviene separar los archivos, para no
seguir haciendo crecer un solo archivo monolítico.

---

## 1. Split de `protocol/ntrip.c`

| Archivo | Contenido | Por qué |
|---|---|---|
| `ntrip.c` (dispatcher) | `ntrip_handle_request()` reducido a: detectar `is_ntrip_v2`/`is_browser`, decidir SOURCE/POST/GET/sourcetable, y delegar. | Es la única parte que realmente depende de "qué rama llamar". |
| `ntrip_common.c/h` | `str_icase_starts`, `find_header`, `copy_until`, `send_all`, `build_sourcetable_text`, `forward_source_payload`, `RESP_404`, el handler de `GET` (cliente) completo. | Código idéntico para v1 y v2 hoy — no hay nada que dividir, ya está unificado. |
| `ntrip_v1.c/h` | Parseo de `SOURCE pass path` (con el fix del `/` inicial), `handle_sourcetable_v1`, `RESP_ICY_200`, `RESP_ICY_401`. | Formato ICY, sin HTTP, específico de la spec v1. |
| `ntrip_v2.c/h` | Parseo de `POST` (source v2), `handle_sourcetable_v2` (texto + HTML), `RESP_HTTP_200_STREAM`, `RESP_HTTP_200_SOURCE`, `RESP_409`. | Framing HTTP + header `Ntrip-Version`, específico de v2. |
| `auth.c/h` (nuevo) | Base64 decoder + `parse_basic_auth`. | Hoy solo lo usa v2, pero el registro de auth (fase 2) lo va a necesitar desde el broker/config también — mejor no atarlo a `ntrip_v2.c`. |

Nota de la auditoría: `broker_sourcetable_fill()` y `sourcetable_entry_t`
en `broker.h` tienen nombre "NTRIP-flavored" aunque la lógica está bien
separada (el broker expone datos crudos, `ntrip.c` arma las líneas
`STR;/CAS;/NET;`). Renombrar a algo neutro (`broker_mount_snapshot()` /
`mount_info_t`) queda como limpieza opcional, no bloqueante.
También hay que borrar del `CLAUDE.md` la referencia a
`src/core/sourcetable.h` — ese archivo nunca se creó, es un rastro de un
diseño anterior abandonado.

---

## 2. Registro de auth (clientes + sources permitidos)

### 2.1 Fuente de verdad

SQLite **o** JSON plano (vía `inih`, ya vendored) — cualquiera de los dos
sirve porque ninguno se consulta directo en el hot path de conexión
(ver 2.3). SQLite gana si además queremos historial de conexiones más
adelante (fase 4); JSON gana en simplicidad si el registro es chico y se
edita a mano. Se puede empezar con JSON y migrar a SQLite después sin
tocar el resto del diseño, porque el consumidor real (la hash table en
RAM) es igual en ambos casos.

Esquema mínimo por entrada:

- mountpoint (o `*` para wildcard de source)
- usuario
- password (hash, no texto plano)
- rol: `source` / `client` / `admin`
- baneado: 0/1

### 2.2 Cache en RAM

Tabla `uthash` (ya vendored, sin usar) poblada al arrancar desde
SQLite/JSON. Es lo que realmente se consulta en cada handshake — nunca
se toca disco en el path de conexión.

### 2.3 Concurrencia

`pthread_rwlock_t` (mismo primitivo que ya usa `mountpoint_t` para su
lista de clientes — no se introduce un concepto nuevo):

- Lookup (cada `broker_source_register`/`broker_client_register`):
  `rdlock` → `HASH_FIND` → copiar lo necesario a variable local →
  `rdunlock`. Múltiples threads leyendo en paralelo sin bloquearse.
- Reload (SIGHUP o timer liviano que chequea `PRAGMA data_version` /
  `mtime` del JSON): construir la tabla nueva completa **aparte**, y solo
  al final `wrlock` → reemplazar el puntero + liberar la vieja →
  `wrunlock`. La sección crítica dura microsegundos sin importar el
  tamaño del registro, porque el trabajo pesado (parsear + poblar) pasa
  fuera del lock.

Se descartó un diseño RCU/atomic-swap: es el patrón correcto para el
hot path de relay (por eso el ring buffer ya es lock-free), pero acá el
auth check pasa una vez por conexión, no por byte — el rwlock no le
agrega contención real y evita la complejidad de reclamar memoria de
forma segura sin una librería RCU externa.

### 2.4 Dónde se conecta con el código actual

- `broker_source_register()` / `broker_client_register()` en `broker.c`
  pasan a llamar `auth_check(user, pass, mountpoint, role)` antes de
  hacer `mp_source_attach`/`mp_client_subscribe`.
- SOURCE v1 (`ntrip_v1.c`) ya extrae `password` — falta extraer/usar
  `user` (hoy se hardcodea `"source"`, ver línea 320 de `ntrip.c`
  actual).
- POST v2 / GET v2 (`ntrip_v2.c`, `ntrip_common.c`) ya extraen
  `user`/`pass` vía `parse_basic_auth` — solo falta que el resultado se
  verifique contra algo en vez de aceptarse siempre.

---

## 3. GGA de clientes (queda anotado, no es parte de esta fase)

`io_engine.c` ya descarta explícitamente los bytes GGA
(`/* TODO Fase NEAREST */`). Guardarlos (para NEAREST y para logging)
es un paso posterior, después de que auth esté funcionando. No se toca
en esta fase.

---

## 4. Fuera de alcance por ahora (explícitamente pedido por Cesar)

Confirmado: esto se hace **al final**, cuando el registro de datos y la
versión de auth ya no den problemas.

- Admin HTTP/CLI para agregar/quitar usuarios sin redeploy.
- Historial de conexiones persistente (quién, cuándo, cuántos bytes).
- Start/stop/ban/kick de sources y clientes en caliente.
- Vista de mapa de mountpoints/clientes.

Estas cuatro dependen todas del mismo registro (SQLite lo hace más fácil
que JSON para esta parte, por el historial), así que si se termina
usando SQLite en la fase 2, esta fase 4 se beneficia directo.

---

## 5. Orden de ejecución propuesto

1. Split de `ntrip.c` → `ntrip_common.c/h`, `ntrip_v1.c/h`, `ntrip_v2.c/h`,
   `auth.c/h` (sin cambiar comportamiento, solo mover código).
   Actualizar `src/CMakeLists.txt` (`CORE_SOURCES`) para incluir los
   archivos nuevos.
2. Definir el esquema final del registro (JSON primero, o directo
   SQLite — a decidir) y el loader a uthash.
3. `auth_check()` + rwlock + reload por SIGHUP.
4. Conectar `auth_check()` en `broker_source_register`/
   `broker_client_register`.
5. Probar contra el relay real de SNIP (igual que se validó el fix del
   `/` inicial) para confirmar que el flujo real no se rompe con auth
   activo.
6. (Fase futura, al final) admin/historial/kick/mapa.

