# Feature: Registro persistente (SQLite) + Dashboard admin

Plan de diseño — **sin código todavía**. Reemplaza el INI actual
(`conf/ntripcaster.conf`) como fuente de verdad de credenciales, y sienta la
base del dashboard (ban/kick, mapa de mountpoints, historial).

Estado: planeado. Complementa y supera a `FEATURE_auth_registry.md` §2
(que dejaba abierta la decisión JSON vs SQLite — acá se cierra).

---

## 0. Requerimientos que motivan este rediseño

1. El conf global INI no escala: editar un archivo a mano por cada rover
   nuevo no sirve cuando haya decenas, y no soporta el dashboard.
2. **Separar credenciales v1 y v2**: v1 SOURCE solo manda password (sin
   usuario); v2 manda user+password por Basic. Hoy comparten la misma
   entrada y no se puede saber ni restringir por versión.
3. **Múltiples passwords por mountpoint en v1**: hoy hay UNA password por
   mountpoint para el SOURCE. Si se filtra o se ataca por fuerza bruta,
   es la llave de todo. Se quiere una credencial POR dispositivo, revocable
   individualmente.
4. **Fuerza bruta**: un atacante martillando SOURCE/GET con passwords al
   azar no debe tumbar el caster ni adivinar credenciales.
5. **Dashboard**: ban/kick en caliente, mapa con la ubicación de cada
   mountpoint (coords que ya extraemos del msg 1005), estado online/offline,
   historial de conexiones.

---

## 1. Opciones analizadas para la persistencia

### Opción A — JSON plano (cJSON o parser propio)

Cómo sería: un `registry.json` con arrays de credenciales; se parsea completo
al arrancar y en cada reload; el hot path sigue leyendo la cache uthash.

| Pros | Contras |
|---|---|
| Legible y editable a mano | **Sin escritura concurrente segura**: el dashboard tendría que reescribir el archivo ENTERO en cada alta/ban — race con el editor humano, corrupción si crashea a mitad de write |
| Cero dependencias nuevas si el parser es propio | Sin queries: historial de conexiones y "últimos N eventos" habría que inventarlos aparte |
| Diff-eable en git | Sin transacciones ni integridad referencial |
|  | El dashboard necesita OTRO mecanismo de persistencia para bans/historial → terminan siendo 2 sistemas |

**Veredicto**: sirve solo si el registro fuera chico, estático y sin dashboard.
Los requerimientos 4 y 5 lo descartan — se necesitaría reinventar la mitad
de una base de datos.

### Opción B — SQLite embebido ⭐ RECOMENDADA

Cómo sería: `data/caster.db`, un solo archivo. SQLite se vendorea como
**amalgamation** (`sqlite3.c` + `sqlite3.h`, un solo .c gigante) en
`vendor/sqlite/` — exactamente el mismo patrón que ya usamos con inih y
uthash. Sin dependencia del sistema, compila con el proyecto.

| Pros | Contras |
|---|---|
| C nativo, la librería embebida más desplegada del mundo — dominio absoluto del caso "una app, un archivo de datos" | ~250 KLOC vendoreadas (no se mantienen, se actualizan como blob) |
| Transacciones ACID: un ban desde el dashboard nunca deja el registro a medias | Binario crece ~1 MB |
| **WAL mode**: el caster lee/escribe mientras el dashboard (u otro proceso) lee — sin bloquearse | Escrituras concurrentes de VARIOS procesos requieren cuidado (acá solo escribe el caster → no aplica) |
| `PRAGMA data_version`: detectar cambios externos sin parsear nada → dispara el mismo reload que ya existe | |
| Historial, bans, credenciales y metadata de mounts en UN solo lugar con queries | |
| El dashboard sale casi gratis: sus datos YA están estructurados | |

**Veredicto**: es la opción correcta dados los requerimientos 4 y 5. La
analogía con nginx es válida pero con un matiz importante (ver §2).

### Opción C — Híbrido: config en archivo + datos en SQLite ⭐⭐ LO QUE HACE NGINX-LAND

El matiz de la analogía nginx: nginx usa archivos de config para lo que es
**configuración** (puertos, límites, rutas) y deja el **estado dinámico**
(sesiones, rate-limit counters) en memoria/almacenes aparte. La separación
correcta acá es la misma:

- **Configuración** (cambia poco, la edita un humano, quiere estar en git):
  puerto, bind, threads, timeouts, límites → archivo (el INI actual o el
  formato que sea). NO va a SQLite.
- **Datos** (cambian en runtime, los toca el dashboard, quieren queries):
  credenciales, bans, historial, coords/estado de mounts → SQLite.

**Veredicto final: Opción C = B para los datos + archivo para la config.**
Así ni el dashboard edita archivos de config, ni el humano hace UPDATEs a
mano para cambiar un puerto.

---

## 2. Arquitectura resultante

```
                    ┌────────────────────────────┐
                    │  conf/ntripcaster.conf     │  config estática
                    │  (puerto, threads, límites)│  (humano + git)
                    └────────────┬───────────────┘
                                 │ al arrancar
                                 ▼
┌──────────┐  SQL (arranque/   ┌──────────────┐   rdlock, O(1)   ┌─────────┐
│ data/    │   reload/flush)   │ Cache RAM    │ ◄──────────────── │ Hot path│
│caster.db │ ◄───────────────► │ (uthash +    │   por handshake   │ (auth,  │
│ (SQLite  │                   │  rwlock)     │                   │  bans)  │
│  WAL)    │                   └──────────────┘                   └─────────┘
└────┬─────┘
     │ lectura directa (WAL permite lector concurrente)
     ▼
┌──────────────┐    HTTP JSON     ┌──────────────┐
│ Admin API    │ ◄──────────────► │ Dashboard    │
│ (en el caster│                  │ (HTML+JS     │
│  :8080 local)│                  │  estático,   │
└──────────────┘                  │  Leaflet map)│
                                  └──────────────┘
```

**Regla de oro (ya establecida en FEATURE_auth_registry §2.3 y se mantiene):
el hot path del handshake NUNCA toca disco.** SQLite se consulta solo en:
arranque, reload, flush periódico de estado, y requests del admin API.
El handshake consulta la cache uthash bajo rdlock, igual que hoy — cambiar
el backend de INI a SQLite **no toca la interfaz de `auth.h`**:
`auth_check_source()` / `auth_check_client()` quedan idénticas por fuera.

---

## 3. Esquema SQL propuesto

```sql
-- Credenciales. Resuelve los requerimientos 2 y 3:
--   * ntrip_version separa v1 / v2 / ambas
--   * username NULL = credencial estilo v1-SOURCE (solo password)
--   * VARIAS filas por mountpoint = una credencial por dispositivo (v1)
CREATE TABLE credentials (
    id            INTEGER PRIMARY KEY,
    role          TEXT NOT NULL CHECK (role IN ('source','client','admin')),
    mountpoint    TEXT NOT NULL,             -- '*' = wildcard (solo admin)
    username      TEXT,                      -- NULL para SOURCE v1
    password_hash TEXT NOT NULL,             -- ver §5, nunca texto plano
    ntrip_version INTEGER NOT NULL DEFAULT 0 -- 0=ambas, 1=solo v1, 2=solo v2
                  CHECK (ntrip_version IN (0,1,2)),
    label         TEXT,                      -- "base Auckland", "rover Juan"
    enabled       INTEGER NOT NULL DEFAULT 1,
    created_at    INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE INDEX idx_cred_lookup ON credentials(role, mountpoint, username);

-- Bans (dashboard). ip O credencial, con vencimiento opcional.
CREATE TABLE bans (
    id         INTEGER PRIMARY KEY,
    ip         TEXT,                -- ban por IP (fuerza bruta)
    cred_id    INTEGER REFERENCES credentials(id),  -- o por credencial
    reason     TEXT,
    expires_at INTEGER,             -- NULL = permanente
    created_at INTEGER NOT NULL DEFAULT (unixepoch())
);

-- Metadata viva de mountpoints (mapa del dashboard).
-- La escribe el caster con flush periódico (ver §6), NUNCA en el hot path.
CREATE TABLE mounts (
    name        TEXT PRIMARY KEY,
    lat         REAL,               -- del msg RTCM 1005 (ya lo decodificamos)
    lon         REAL,
    identifier  TEXT,
    format      TEXT,
    nav_system  TEXT,
    country     TEXT,
    online      INTEGER NOT NULL DEFAULT 0,
    last_seen   INTEGER,
    clients_now INTEGER NOT NULL DEFAULT 0
);

-- Historial de conexiones (dashboard, fase posterior).
CREATE TABLE connections_log (
    id         INTEGER PRIMARY KEY,
    ts_start   INTEGER NOT NULL,
    ts_end     INTEGER,
    ip         TEXT NOT NULL,
    role       TEXT NOT NULL,       -- source/client
    mountpoint TEXT,
    username   TEXT,
    bytes_rx   INTEGER DEFAULT 0,
    bytes_tx   INTEGER DEFAULT 0,
    close_why  TEXT                 -- eof/timeout/kick/ban/lag
);
```

### Reglas de matching en el handshake (requerimiento 2 y 3)

- **SOURCE v1** (`SOURCE pass MP`): buscar filas `role=source, mountpoint=MP,
  username IS NULL, ntrip_version IN (0,1), enabled=1` y comparar el password
  contra CADA una (son pocas por mount — una por dispositivo). Match con
  cualquiera = autorizado. Así se revoca UN dispositivo sin tocar los demás.
- **SOURCE v2** (`POST /MP` + Basic user:pass): fila exacta `role=source,
  mountpoint=MP, username=user, ntrip_version IN (0,2), enabled=1`.
- **CLIENT** (GET + Basic, v1 y v2): fila exacta `role=client, mountpoint=MP,
  username=user, version compatible, enabled=1` (los clientes ya son
  por-usuario hoy; se conserva).
- En la cache RAM esto se materializa como: hash `mount:user` → entrada
  (para lookups con usuario) + lista corta por mount (para SOURCE v1
  multi-password). Sigue siendo O(1) + una comparación por credencial v1
  del mount.

---

## 4. Anti fuerza bruta (requerimiento 4)

Tres capas, de la más barata a la más pesada:

1. **Rate limit por IP en RAM** (uthash `ip → {fallos, ventana, ban_hasta}`):
   - N fallos de auth (ej. 5) en una ventana (ej. 60 s) → la IP queda
     bloqueada X minutos con backoff exponencial (1→5→30 min).
   - El check es lo PRIMERO del handshake: una IP bloqueada se corta antes
     de parsear nada (barato, no llega ni al registro).
   - Vive en RAM: se pierde al reiniciar — correcto para bloqueos temporales.
2. **Bans persistentes** (tabla `bans`): los pone el operador desde el
   dashboard, o automáticamente cuando una IP reincide demasiado. Se cargan
   a la cache al arrancar/reload, se consultan junto con el rate limit.
3. **Hash + comparación en tiempo constante** (§5): aunque adivinen, cada
   intento cuesta; y no hay oráculo de timing.

Extra que ya tenemos gratis: `handshake_timeout` (10 s) corta a los que
abren sockets sin hablar (slowloris básico), y `max_clients` global acota
el daño de una inundación de conexiones.

---

## 5. Passwords hasheados

Nota ya existente en `auth.h`: "hashear + comparación en tiempo constante es
un TODO antes de exponer esto fuera de testing". Se resuelve acá.

- **Formato en DB**: `pbkdf2-sha256$<iteraciones>$<salt_hex>$<hash_hex>`.
  Autodescriptivo → se pueden subir las iteraciones a futuro sin migrar todo.
- **Implementación**: PBKDF2-HMAC-SHA256 vendoreado (un `sha256.c/h` de
  dominio público + ~40 líneas de PBKDF2 propias). Mismo espíritu
  zero-dependency del proyecto. Argon2/libsodium sería "más correcto" pero
  arrastra una dependencia grande; PBKDF2 con 100-200k iteraciones es más
  que suficiente para credenciales de rovers.
- **Matiz de performance**: PBKDF2 lento es UN costo por handshake legítimo
  (decenas de ms — irrelevante para un rover que conecta una vez), pero
  multiplica el costo de CADA intento de fuerza bruta. Combinado con el
  rate limit de §4, el ataque se vuelve inviable.
- **Comparación en tiempo constante**: `volatile` XOR-acumulado sobre
  longitud fija (el hash), no `strcmp`.
- **CLI mínima para altas**: `ntripcaster --hash-password` (lee de stdin,
  imprime el string para INSERT) — necesario hasta que exista el dashboard.

---

## 6. Ban / Kick en caliente (dashboard, requerimiento 5)

Los mecanismos YA construidos esta semana hacen esto casi gratis:

- **Kick**: la lista global de conexiones (`broker->conns_head` +
  `conns_lock`, agregada para los timeouts) permite localizar la conexión
  por fd/IP/user y hacerle `shutdown(fd)` — exactamente el mismo camino
  seguro del sweep de timeouts (el worker la cierra por el flujo normal,
  sin races). Solo falta exponerlo: `broker_kick(fd)` / `broker_kick_user()`.
- **Ban**: INSERT en `bans` (vía admin API) → reload de cache (mismo
  patrón swap-bajo-wrlock del SIGHUP, ya implementado) → kick inmediato de
  las conexiones existentes que matcheen.
- **Reload**: además del SIGHUP actual, el tick del accept loop (ya existe)
  chequea `PRAGMA data_version` cada ~2 s: si el dashboard escribió la DB,
  se recarga sola. Es la "segunda vía" que FEATURE_relay §2.3 dejó planeada.

---

## 7. Admin API + Dashboard (requerimiento 5)

### Transporte

El caster YA habla HTTP (el handshake NTRIP v2 es HTTP). El admin API es un
segundo listener en el mismo io_engine: **puerto separado (ej. 8080) bindeado
a 127.0.0.1 por defecto** — el dashboard nunca queda expuesto a internet por
accidente; para acceso remoto, SSH tunnel o reverse proxy (nginx 😉) delante.

### Endpoints (JSON, auth por token de `credentials` con role=admin)

```
GET  /api/status            → uptime, conexiones, bytes, versión
GET  /api/mounts            → lista con lat/lon/online/clients (para el mapa)
GET  /api/connections       → conexiones vivas (ip, user, mount, bytes, edad)
POST /api/kick              → {fd | ip | user}
GET/POST/DELETE /api/bans   → listar / crear / quitar
GET/POST/PATCH  /api/credentials → altas/bajas/enable de credenciales
GET  /api/log               → últimas N filas de connections_log
```

### Dashboard

- **HTML+JS estático servido por el propio admin API** (archivos embebidos o
  carpeta `www/`). Sin framework de build: un `index.html` + [Leaflet](https://leafletjs.com)
  para el mapa (tiles de OpenStreetMap) + `fetch()` polling cada 2-5 s.
- Mapa: un marker por mountpoint con sus coords de la tabla `mounts`
  (verde=online, gris=offline), popup con clients/bytes/last_seen.
- Tabla de conexiones vivas con botón kick/ban por fila.
- CRUD de credenciales (el password se manda una vez, el server lo hashea).
- SSE o WebSocket para tiempo real: **fase posterior**; polling alcanza sobra
  para empezar y no agrega complejidad al io_engine.

### Escrituras a `mounts` / `connections_log` sin tocar el hot path

El flush corre en el tick del accept loop (cada ~5 s, junto al sweep):
recorre mountpoints y vuelca coords/online/clients con UPDATEs batcheados en
una transacción. El log de conexiones se escribe al CERRAR cada conexión
(un INSERT, fuera del path de relay). Si SQLite está ocupado, se saltea el
ciclo — la DB es un espejo del estado, la verdad vive en RAM.

---

## 8. Fases de ejecución propuestas

| Fase | Contenido | Tamaño |
|---|---|---|
| **1. Vendorear SQLite + capa `registry.c`** | Amalgamation en `vendor/sqlite/`; `registry_open/load_credentials/load_bans` → poblar la MISMA cache uthash actual. `auth.h` no cambia. Migración: script/flag `--import-ini` que lee el conf viejo e inserta. | M |
| **2. Esquema v1/v2 + multi-password** | Tablas de §3, reglas de matching de §3, `ntrip_v1.c` pasa a comparar contra la lista de credenciales del mount. | M |
| **3. Hashing PBKDF2 + CLI `--hash-password`** | §5. Desde acá, nunca más passwords en texto plano. | S-M |
| **4. Rate limit + bans** | §4: tabla RAM por IP, checks al inicio del handshake, tabla `bans`, autoban por reincidencia. | M |
| **5. Reload por `data_version`** | Tick chequea la DB cada ~2 s, reusa el swap del SIGHUP. | S |
| **6. Flush de `mounts` + `connections_log`** | §7 último punto. | S-M |
| **7. Admin API** | Listener 127.0.0.1:8080, endpoints de §7, kick/ban cableados. | L |
| **8. Dashboard estático** | `www/` + Leaflet + polling. | M |
| **9. (después) SSE, historial con gráficos, roles finos** | | L |

Cada fase compila y corre sola; 1-3 no cambian nada observable para los
rovers (misma auth por fuera, backend nuevo por dentro).

---

## 9. Decisiones cerradas / abiertas

**Cerradas por este doc:**
- SQLite (amalgamation vendoreada) para DATOS; archivo de config solo para
  configuración estática. JSON descartado (§1.A).
- Interfaz `auth.h` se conserva; solo cambia el backend.
- Hot path jamás toca disco (se reafirma).
- Kick/ban reutilizan la lista global de conexiones + shutdown() ya probados.
- PBKDF2-SHA256 vendoreado, formato de hash autodescriptivo.
- Admin API en puerto separado, 127.0.0.1 por defecto.

**Abiertas (decidir al llegar):**
- ¿Iteraciones PBKDF2? (empezar 100k, medir en el hardware real de WSL).
- ¿Autoban persistente automático o solo manual? (arrancar: manual + temp
  automático en RAM).
- ¿Tiles de mapa offline? (si el dashboard corre sin internet, vendorear
  tiles o usar un tile server local — decidir cuando exista el mapa).
- ¿Retención de `connections_log`? (ej. DELETE > 90 días en el flush).
