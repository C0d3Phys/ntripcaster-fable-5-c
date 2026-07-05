# Fase A — Plan ejecutable: base confiable

**Proyecto:** NtripCaster (wsl-project-tested)
**Fecha:** 2026-07-04
**Continúa a:** `FEATURE_improvements_20260704_183128.md` (roadmap maestro)
**Alcance:** re-análisis del estado real + plan paso a paso de la Fase A,
con diseño concreto de cada fix para poder implementarlo sin re-decidir nada.

---

## A. Re-análisis: roadmap vs código real (build 1.001.1.10)

Verificado contra el código actual y las corridas en vivo con SNIP/STRSVR:

| Ítem | Estado real | Nota |
|---|---|---|
| Relay v1/v2, auth INI, reload SIGHUP | ✅ operativo | verificado con Trimble Alloy vía SNIP + STRSVR |
| Timeouts (handshake/source/client) | ✅ operativo | sweep 5 s + motivo del kick en el log |
| Config general `[caster]` | ✅ operativo | puerto/bind/nombre/operador/país/límites/log |
| Decode incremental + stats de integridad | ✅ operativo | reset por source; `corrupto+0B` en campo |
| Debug req/resp con credenciales tapadas | ✅ operativo | cubre parte de IMP-02 |
| Accept en loop hasta EAGAIN (ráfagas) | ✅ corregido | antes 124/200; ahora 200/200 |
| Parser RTCM3 frames partidos por TCP | ✅ corregido | bug histórico del fallback `len-6` |
| IMP-01 refcount/exclusión | 🔴 pendiente | el race B10 sigue (ventana chica) |
| IMP-01B ring torn-read | 🔴 pendiente | fix chico, diseño en §C.3 |
| IMP-01C shutdown ordenado | 🔴 pendiente | hoy no cierra conns vivas al apagar |
| IMP-01D límites CAS / stats atómicas | 🟡 parcial | límites se aplican pero no son indivisibles |
| IMP-02 hashing | 🔴 pendiente | decisión unificada en §D |
| IMP-04 suite | 🟡 60% en scripts | falta empaquetar en CTest (§C.1) |
| IMP-05 sanitizers | 🟡 parcial | existe ASan/UBSan en build.sh de la otra variante; falta TSan+CI |
| IMP-06 higiene repo | 🔴 pendiente | .rar, builds, credenciales reales en conf |

**Errores del roadmap corregidos en esta pasada:** `bytes_tx` NO se cuenta
doble (verificado); el masking de logs YA está hecho; hashing unificado (§D);
`ntrip_source_relay` no existe en el árbol; referencias cruzadas agregadas.

---

## B. Orden de ejecución de Fase A (con tamaños)

| # | Tarea | Tamaño | Depende de |
|---|---|---|---|
| A1 | Harness CTest con los scripts existentes | M | — |
| A2 | Targets ASan/UBSan + TSan; suite corriendo bajo ambos | S | A1 |
| A3 | IMP-01B: fix torn-read del ring | S | A1+A2 (para probarlo) |
| A4 | IMP-01D: reserva de cupos con CAS + stats con snapshot | S-M | A2 |
| A5 | IMP-01: refcount + exclusión por conexión | L | A1+A2 verdes |
| A6 | IMP-01C: shutdown ordenado | M | A5 (usa el mismo ownership) |
| A7 | IMP-06: .gitignore, README, conf.example sin secretos | S | — (paralelo) |

Regla: **no empezar A5 hasta que A1+A2 estén verdes** — el refcount es la
cirugía mayor y necesita la red de seguridad.

---

## C. Diseño concreto por tarea

### C.1 — A1: Harness CTest (formalizar lo que ya existe)

Estructura:

```
test/
├── CMakeLists.txt          # add_test() por escenario, timeout 30-60s
├── helpers/
│   ├── common.sh           # puerto_libre(), arranca_caster(), espera_puerto()
│   ├── mk_rtcm.py          # genera frames RTCM3 con CRC válido/roto
│   └── conf_test.tpl       # conf mínimo con credenciales de TEST
└── cases/
    ├── t01_source_v1_auth.sh        # pass ok / mala / mount no registrado
    ├── t02_source_v2_auth.sh        # POST 200 / 401
    ├── t03_client_auth_relay.sh     # rover ok recibe frames; sin auth 401/ERROR
    ├── t04_payload_pipelined.sh     # bytes pegados al handshake
    ├── t05_frames_fragmentados.sh   # frame partido entre writes (integridad exacta)
    ├── t06_crc_basura.sh            # ruido + CRC roto → corrupto == N exacto
    ├── t09_limites.sh               # max_clients/max_sources rechazan limpio
    ├── t10_timeouts.sh              # handshake/source/client kick
    ├── t11_reload_sighup.sh         # credencial nueva en caliente
    ├── t12_burst_accept.sh          # 200 conexiones simultáneas → 200 aceptadas
    └── t16_ring_lag.sh              # cliente pausado (SIGSTOP) hasta lag → kick
```

Claves de diseño:
- **Puerto dinámico**: `puerto_libre()` abre un socket efímero, lo cierra y
  usa ese número — permite `ctest -j` en paralelo.
- **Cero dependencias de red externa**: el "GNSS" es `mk_rtcm.py` + nc/python.
- Cada caso imprime PASS/FAIL con diff del valor esperado (frames, bytes,
  código de respuesta) — no "parece que anda".
- `t16`: compilar una variante con `RING_SIZE` chico (p.ej. 4 KB vía
  `-DRING_SIZE_OVERRIDE`) para provocar wrap-around en segundos.

DoD: `ctest --output-on-failure` verde en limpio, y falla si se re-introduce
cualquiera de los bugs ya corregidos (regresión del accept-burst, del parser
fragmentado, etc.).

### C.2 — A2: Sanitizers

- `build.sh` gana modos: `asan` (`-fsanitize=address,undefined -O1 -g`) y
  `tsan` (`-fsanitize=thread -O1 -g`) — binarios separados, nunca combinados.
- CTest con `NTRIPCASTER_BIN` parametrizable → la misma suite corre 3 veces:
  normal, asan, tsan.
- Se ACEPTA temporalmente una lista corta de supresiones TSan documentadas
  (las stats del sweep, hasta A4) — pero cada supresión lleva número de IMP.

### C.3 — A3: Fix torn-read del ring (IMP-01B)

Diseño elegido: **doble validación con reintento acotado** (mantiene lectores
lock-free y al productor sin esperas):

```c
ssize_t rb_read(...) {
    for (int intento = 0; intento < 2; intento++) {
        uint64_t wp1 = atomic_load_acquire(&rb->write_pos);
        if (wp1 - *client_pos > RING_SIZE) return -1;      /* lag ya declarado */
        n = min(wp1 - *client_pos, max_len);
        memcpy(out, ...);                                   /* copia optimista */
        uint64_t wp2 = atomic_load_acquire(&rb->write_pos);
        if (wp2 - *client_pos <= RING_SIZE) {               /* nadie pisó lo copiado */
            *client_pos += n;
            return n;
        }
        /* el productor dio la vuelta DURANTE la copia → lo copiado puede
         * estar mezclado; declarar lag (el cliente estaba al borde igual) */
    }
    return -1;
}
```

Propiedad: o los bytes entregados pertenecen a una sola vista del ring, o se
retorna lag y el caller desconecta — nunca mezcla silenciosa. El productor
jamás espera. Test: `t16_ring_lag.sh` con ring de 4 KB + productor a fondo +
lector con pausas; verificar con el comparador que TODO lo entregado es
prefijo-contiguo del stream original.

### C.4 — A4: Cupos indivisibles y stats coherentes (IMP-01D)

- Reserva con CAS-loop sobre `active_clients`/`active_sources`:
  `do { v = load; if (v >= max) return -1; } while (!CAS(&a, v, v+1));`
  y `fetch_sub` en TODO camino de error posterior (registro fallido, etc.).
- Stats de mountpoint: los contadores calientes pasan a `_Atomic uint64_t`
  con `fetch_add` relaxed (costo ~0 en x86); el reporte lee con `load` —
  se elimina la carrera formal sin locks nuevos.
- Separar `bytes_queued` (fill) de `bytes_sent` (flush) en `conn_t`.

### C.5 — A5: Ownership de `conn_t` (IMP-01) — la cirugía mayor

Mapa de referencias (quién puede tener un puntero vivo):

| Dueño | Toma ref | Suelta ref |
|---|---|---|
| epoll (data.ptr) | al ADD | al DEL |
| work queue (io_work_t) | en wq_push | al terminar dispatch o al descartar |
| lista clientes del mount | en subscribe | en unsubscribe |
| lista global (timeouts) | en conn_alloc | en conn_free |

Estructura:

```c
_Atomic int refs;          /* nace en 1 (lista global) */
_Atomic int state_flags;   /* bit CLOSING | bit IN_FLIGHT | bit PENDING_WAKEUP */
```

Reglas:
1. `wq_push` hace `ref_get()`; el worker hace `ref_put()` al salir del dispatch.
2. `io_engine_conn_close` se vuelve `conn_request_close`: setea CLOSING
   (CAS — solo el primero gana), hace DEL de epoll, detach de listas, y
   `ref_put()` de las refs estructurales. `free()` real solo cuando refs==0.
3. **Exclusión**: el worker toma IN_FLIGHT por CAS antes de tocar la conexión;
   si ya estaba tomado, setea PENDING_WAKEUP y retorna (sin bloquear). Al
   soltar IN_FLIGHT, si PENDING_WAKEUP estaba seteado → re-procesar una vez.
   Esto elimina el doble-dispatch de `clients_wakeup` sin perder eventos.
4. `clients_wakeup` deja de re-armar a ciegas: solo MOD si la conexión no
   está IN_FLIGHT; si lo está, PENDING_WAKEUP hace el trabajo.

DoD: suite completa verde bajo TSan y ASan + `t_stress_churn.sh` nuevo
(conexiones/desconexiones caóticas + wakeups, 10 min) sin un solo reporte.

### C.6 — A6: Shutdown ordenado (IMP-01C)

Secuencia en `io_engine_destroy` (usa la maquinaria de A5):
listener DEL+close → marcar todas CLOSING vía lista global → drenar cola
(descartar trabajos soltando refs) → join workers → liberar conns restantes
→ destroy broker/registros → `auth_registry_destroy` → `log_close`.
DoD: LeakSanitizer limpio apagando con: 0 conns / source+2 rovers activos /
cola saturada.

### C.7 — A7: Higiene (IMP-06) — paralelo, sin dependencias

- `.gitignore`: `build*/`, `*.log`, `captures/`, `*.rar`, `*.zip`,
  `conf/ntripcaster.conf` (queda solo `conf/ntripcaster.conf.example` con
  placeholders OBVIOS tipo `CHANGE_ME`).
- `README.md` raíz: qué es, arquitectura (1 diagrama), compilar (build.sh y
  CMake), configurar, correr, probar (ctest), licencia.
- Actualizar comentario desactualizado de `auth.h` ("reload... queda para la
  siguiente pasada" — ya existe).

---

## D. Decisiones unificadas (cierra conflictos entre docs)

1. **Hashing de passwords: PBKDF2-HMAC-SHA256 vendoreado** (sha256.c de
   dominio público + ~40 líneas), formato `pbkdf2-sha256$iter$salt$hash`,
   comparación en tiempo constante. Argon2id queda como opción futura detrás
   de `-DUSE_ARGON2` si algún día se acepta la dependencia. (Coincide con
   `FEATURE_registry_sqlite_dashboard.md` §5; reemplaza la sugerencia abierta
   de IMP-02.)
2. **TLS: reverse proxy primero** (nginx/stunnel), TLS nativo no antes de
   Fase C. (Confirma IMP-03.)
3. **Persistencia: SQLite** según `FEATURE_registry_sqlite_dashboard.md`
   (esquema, WAL, data_version) — no se re-discute en Fase D.
4. **Init lazy de rings**: aprobado como micro-tarea de A4 (es un `memset`
   movido a `mp_get_or_create`), no como "optimización a medir".

---

## E. Definition of Done de la Fase A completa

- `ctest` con ≥ 14 escenarios reales, verde en normal + ASan + TSan.
- Cero supresiones de sanitizer sin número de IMP asociado.
- Churn de 10 minutos sin UAF/races.
- Shutdown limpio bajo LeakSanitizer en los 3 escenarios.
- Repo clonable que compila solo con el README.
- `FEATURE_improvements_20260704_183128.md` actualizado marcando IMP-01,
  01B, 01C, 01D, 04, 05 y 06 como ✅ con fecha.

## F. Qué NO hace la Fase A (para resistir la tentación)

Ni NEAREST, ni chunked, ni SQLite, ni TLS nativo, ni dashboard, ni el agente
str2str propio. Todo eso construye SOBRE esta base — el orden es el valor.
