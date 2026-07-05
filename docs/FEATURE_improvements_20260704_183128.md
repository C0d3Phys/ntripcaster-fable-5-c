# Features e improvements pendientes

**Proyecto:** NtripCaster  
**Fecha de registro:** 2026-07-04 18:31:28 (America/Managua)  
**Revisión técnica incorporada:** 2026-07-04  
**Estado:** Propuesta de roadmap  
**Objetivo:** llevar el caster desde su estado funcional actual hasta una base segura, verificable y operable en producción.

---

## 1. Resumen del estado actual

El proyecto ya dispone de relay NTRIP v1/v2, autenticación por archivo INI,
reload mediante `SIGHUP`, sourcetable, límites globales, timeouts, métricas,
decodificación RTCM3 incremental y herramientas manuales de integración.

Las mejoras más importantes no consisten en agregar más protocolo, sino en
fortalecer el ciclo de vida de las conexiones, la seguridad, las pruebas y la
operación. Después de esa base se pueden desarrollar NEAREST, administración
en caliente y persistencia.

## 2. Prioridad crítica — estabilidad y seguridad

### IMP-01 — Ownership y exclusión segura de `conn_t`

**Problema:** un evento que ya esté dentro de la cola de trabajo puede conservar
un puntero a una conexión liberada por otro worker. El orden actual de
`EPOLL_CTL_DEL`, detach y `close()` reduce la ventana, pero no elimina eventos
ya encolados. Además, `clients_wakeup()` puede hacer `EPOLL_CTL_MOD` y reactivar
un cliente mientras otro worker todavía procesa ese mismo `conn_t`. Mantener el
objeto vivo no basta: también hay que impedir que dos workers modifiquen a la
vez `wbuf`, `read_offset`, estado, timestamps o estadísticas de la conexión.

**Propuesta:**

- Agregar un contador de referencias atómico a `conn_t`.
- Obtener una referencia antes de insertar la conexión en la work queue.
- Liberar esa referencia al terminar o descartar el trabajo.
- Separar `closing` de la liberación física de memoria.
- Garantizar que sólo un worker ejecute el cierre lógico.
- Añadir una garantía de exclusión por conexión (`in_flight`, mutex pequeño o
  máquina de estados atómica) independiente del contador de referencias.
- Hacer que un wakeup concurrente acumule interés pendiente sin crear un
  segundo procesamiento simultáneo del mismo cliente.
- Definir explícitamente quién posee cada referencia: registro global,
  mountpoint, epoll, work queue y worker activo.
- Agregar una prueba de estrés con conexiones y desconexiones simultáneas.

**Criterios de aceptación:**

- Ningún `use-after-free` bajo AddressSanitizer.
- Ninguna carrera de ciclo de vida bajo ThreadSanitizer.
- Cierre idempotente aunque dos eventos detecten error al mismo tiempo.
- Como máximo un worker modifica el estado mutable de una conexión.
- Un wakeup ocurrido durante el dispatch no se pierde y se procesa después.
- Prueba repetida de conexión/desconexión durante al menos 10 minutos.

### IMP-01B — Lectura consistente del ring buffer SPMC

**Problema:** `rb_read()` comprueba el lag usando un snapshot de `write_pos` y
después copia directamente desde el ring. Durante ese `memcpy`, el único
productor puede avanzar una vuelta y sobrescribir la región que está leyendo
un rover lento. El acquire/release de `write_pos` publica los bytes nuevos,
pero no protege bytes antiguos contra sobrescritura concurrente.

**Propuesta:** elegir y documentar una estrategia que garantice un snapshot
consistente, por ejemplo slots/chunks con número de secuencia, doble validación
antes y después de copiar con reintento seguro, o sincronización acotada entre
escritor y lectores. La solución debe mantener la regla de que un rover lento
nunca bloquea indefinidamente al source.

**Criterios de aceptación:**

- Ninguna data race sobre `ring.buf` bajo ThreadSanitizer.
- El lector entrega bytes pertenecientes a una única versión del ring o
  informa lag; nunca una mezcla silenciosa de dos vueltas.
- Pruebas con buffer pequeño, productor rápido y consumidores deliberadamente
  pausados reproducen wrap-around muchas veces.
- La política de lag conserva el framing RTCM3 o desconecta explícitamente.

### IMP-01C — Shutdown ordenado y ownership al apagar

**Problema:** `io_engine_destroy()` une workers y destruye la cola, epoll y el
listener, pero no define un cierre explícito de todas las conexiones vivas
antes de destruir broker y mountpoints.

**Secuencia requerida:**

1. Dejar de aceptar conexiones nuevas y retirar el listener de epoll.
2. Marcar todas las conexiones como `closing` sin liberar memoria prematuramente.
3. Drenar o cancelar la work queue liberando sus referencias.
4. Cerrar y desregistrar todas las conexiones vivas.
5. Despertar y unir workers.
6. Destruir epoll, broker, mountpoints y registros globales.

**Criterios de aceptación:** shutdown repetible con cero conexiones, con source
y rovers activos y con la cola saturada; sin leaks bajo LeakSanitizer y sin
esperas indefinidas.

### IMP-01D — Contadores, límites y snapshots concurrentes

**Problemas detectados:**

- `atomic_load(active_*)` seguido por `atomic_fetch_add()` no reserva cupo de
  forma indivisible; varios workers pueden superar temporalmente el límite.
- Las estadísticas de mountpoint se escriben desde workers y se leen/resetan
  desde el sweep sin atomics ni un snapshot protegido.
- [CORREGIDO 2026-07-04 tras verificación en código] `bytes_tx` se incrementa
  ÚNICAMENTE en `flush_write_buf()` al enviar al socket (`broker_client_fill`
  no lo toca, tiene comentario explícito). No hay doble conteo. La mejora que
  SÍ aplica: separar `bytes_queued` (copiado del ring) de `bytes_sent`
  (aceptado por el socket) para diagnosticar backpressure por cliente.

**Propuesta:** reservar cupos con CAS o bajo el lock del registro y revertir la
reserva en todo camino de error; publicar snapshots coherentes de estadísticas;
y separar métricas como `bytes_queued` y `bytes_sent`.

**Criterios de aceptación:** los límites nunca se exceden bajo registro
concurrente, TSan no reporta carreras y cada contador tiene una definición
única comprobada por tests.

### IMP-02 — Gestión segura de credenciales

**Problema:** las claves se almacenan en texto plano y se comparan con
`strcmp()`. El archivo de configuración de trabajo también contiene
credenciales de ejemplo que podrían confundirse con secretos reales.

**Propuesta:**

- Mover las credenciales operativas a un archivo no versionado.
- Mantener únicamente `ntripcaster.conf.example` en el repositorio.
- Almacenar hashes con Argon2id o, como alternativa, bcrypt/scrypt.
- Usar comparación en tiempo constante.
- Limpiar de memoria los buffers temporales que contienen passwords.
- Documentar rotación de credenciales y permisos `0600`.
- Añadir rate limiting por IP, usuario y mountpoint.

**Criterios de aceptación:**

- El repositorio no contiene credenciales utilizables.
- Una autenticación correcta e incorrecta produce respuestas equivalentes en
  estructura y sin filtrar información sensible.
- Existe migración documentada desde el formato actual.
- Los logs nunca contienen `Authorization`, passwords ni Base64 sin censurar.
  [YA CUMPLIDO desde build 1.001.1.09: `ntrip_debug_request()` enmascara
  `SOURCE ***` y `Authorization: Basic ***` — verificado en vivo.]

**Nota de unificación (2026-07-04):** la elección de hash está decidida en
`FEATURE_improvements_FASE_A` §D (documento maestro de decisiones): PBKDF2-SHA256
vendoreado como base (zero-dependency, coherente con el proyecto), con Argon2id
como opción detrás de flag de compilación si se acepta la dependencia. Este
párrafo reemplaza la sugerencia abierta Argon2id/bcrypt/scrypt de arriba —
`FEATURE_registry_sqlite_dashboard.md` §5 ya coincidía con PBKDF2.

### IMP-03 — Transporte cifrado

**Problema:** Basic Auth sin TLS expone las credenciales y el stream a cualquier
equipo que pueda observar el tráfico.

**Propuesta inicial:** documentar una configuración soportada mediante Nginx,
Caddy, HAProxy o stunnel. El TLS nativo puede evaluarse después para no mezclar
la lógica criptográfica con el hot path antes de estabilizarlo.

**Criterios de aceptación:**

- Guía reproducible para exponer NTRIP mediante TLS.
- Puerto interno del caster restringido a la interfaz o red necesaria.
- Validación con cliente NTRIP compatible y certificado válido.

## 3. Prioridad alta — pruebas y mantenibilidad

### IMP-04 — Suite automática de pruebas

Los tests actuales registrados en CTest sólo validan las opciones `--help`.
Las herramientas de relay, rover y comparación son valiosas, pero requieren
orquestación manual.

**Inventario 2026-07-04:** los escenarios 1-6, 9, 10, 11 y parcialmente 16 de
la lista de abajo YA existen como scripts bash/python de las sesiones de
verificación (auth v1/v2 válida/inválida, payload pipelineado, frames
fragmentados por TCP, CRC roto + ruido, límites, timeouts, reload SIGHUP en
caliente, carga de 200 clientes). Falta formalizarlos en CTest con puertos
dinámicos — trabajo de empaquetado, no de creación. Ver plan concreto en
`FEATURE_improvements_FASE_A`.

Agregar pruebas automáticas para:

1. SOURCE v1 con credencial válida e inválida.
2. POST NTRIP v2 con credencial válida e inválida.
3. Rover v1 y v2 con mountpoint activo e inexistente.
4. Payload unido al handshake y payload fragmentado.
5. Frames RTCM3 divididos entre múltiples escrituras TCP.
6. CRC válido, CRC inválido, basura previa y frame truncado.
7. Desconexión de source con varios rovers conectados.
8. Cliente lento hasta provocar lag respecto al ring buffer.
9. Límites de sources, clientes y clientes por mountpoint.
10. Timeouts de handshake, source y rover.
11. Reload válido e inválido sin interrumpir conexiones existentes.
12. Saturación y recuperación de la cola de trabajo.
13. Dos wakeups concurrentes sobre el mismo cliente sin doble dispatch.
14. Shutdown con conexiones y trabajos activos.
15. Límite global disputado simultáneamente por muchos workers.
16. Wrap-around del ring con un lector lento.
17. Comparador: capturas idénticas, pérdida interior, desfase inicial, frame
    corrupto y frame adicional.

**Criterios de aceptación:**

- `ctest --output-on-failure` ejecuta escenarios reales, no sólo ayuda CLI.
- Las pruebas no dependen de servicios NTRIP externos.
- Los puertos son dinámicos para permitir ejecución paralela.
- Toda regresión encontrada obtiene primero una prueba reproducible.

### IMP-05 — Sanitizers, análisis estático y CI

Crear presets o targets para:

- AddressSanitizer + UndefinedBehaviorSanitizer.
- ThreadSanitizer en ejecución separada.
- `clang-tidy` y, opcionalmente, `cppcheck`.
- Build Debug y Release con GCC y Clang.
- CTest automático en cada cambio.

**Criterios de aceptación:** pipeline sin warnings, errores de sanitizer ni
tests inestables.

### IMP-06 — Higiene del repositorio

- Crear `.gitignore` para builds, logs, capturas, credenciales y archivos de IDE.
- Retirar del control de versiones binarios, objetos y directorios generados.
- Evitar paquetes `.rar` como mecanismo de versionado.
- Añadir un `README.md` raíz con arquitectura, compilación, configuración,
  ejecución y pruebas.
- Definir licencia y política básica de contribución.
- Corregir comentarios que aún describen el reload como pendiente.

**Criterios de aceptación:** un clon limpio compila siguiendo únicamente el
README y `git status` permanece limpio después del build y los tests.

## 4. Prioridad media — configuración y operación

> **Referencias cruzadas:** IMP-07 amplía `FEATURE_relay_capacity_reload.md`
> §2.4 (config_snapshot_t); IMP-09 y FEAT-05 solapan ~80% con
> `FEATURE_registry_sqlite_dashboard.md` (esquema SQL, admin API, dashboard,
> anti-fuerza-bruta). Ante conflicto entre documentos, este roadmap manda en
> PRIORIDADES y aquellos mandan en DISEÑO técnico ya decidido.

### IMP-07 — Snapshot unificado y reload completo

**Propuesta:** crear un `config_snapshot_t` inmutable que contenga ACL,
timeouts, límites, datos del caster y políticas por mountpoint. Construir un
nuevo snapshot fuera del hot path, validarlo completamente y hacer un swap
atómico sólo si todo es válido.

Incluir:

- Reload por `SIGHUP`.
- Detección opcional por cambio de `mtime` cada 30–60 segundos.
- Validación de rangos y campos obligatorios.
- Mensaje claro cuando un valor requiere reinicio.
- Conservación del snapshot anterior si el archivo nuevo es inválido.

### IMP-08 — Límites configurables por mountpoint

Actualmente existen constantes compiladas como `MOUNTPOINT_MAX` y
`CLIENT_MAX_PER_MOUNT`.

Agregar políticas configurables:

- Máximo global de mountpoints.
- Máximo de rovers por mountpoint.
- Ancho de banda máximo por source o mountpoint.
- Tiempo máximo sin RTCM válido aunque continúen llegando bytes.
- Estrategia ante lag: desconectar, saltar al dato reciente o emitir alerta.

### IMP-09 — Observabilidad operativa

- Métricas Prometheus o endpoint JSON de sólo lectura.
- Contadores de conexiones aceptadas, rechazadas y autenticaciones fallidas.
- Bytes, frames, CRC inválidos, lag y uptime por mountpoint.
- Profundidad y saturación de la work queue.
- Latencia aproximada source → rover.
- Rotación de logs por tamaño/fecha o integración documentada con `logrotate`.
- Identificador de conexión para correlacionar eventos sin registrar secretos.

### IMP-10 — Empaquetado y servicio

- Archivo de servicio `systemd` con usuario sin privilegios.
- Límites de archivos abiertos y reinicio controlado.
- `SIGTERM` con el shutdown ordenado definido en IMP-01C y tiempo máximo de
  drenaje.
- Health check de proceso y readiness de escucha.
- Instalación mediante CMake con rutas configurables.
- Contenedor opcional con imagen mínima y filesystem de sólo lectura.

## 5. Nuevas features de protocolo y GNSS

### FEAT-01 — Mountpoint virtual `NEAREST`

Implementar el flujo ya anticipado por `broker_nearest()`:

1. Aceptar un rover en `/NEAREST`.
2. Leer y validar su sentencia GGA.
3. Extraer latitud y longitud.
4. Seleccionar el source activo más cercano mediante Haversine.
5. Suscribir al rover sin reconexión si el diseño lo permite.
6. Reevaluar la estación cuando cambie significativamente de posición o el
   source elegido quede offline.

Se deben definir distancia máxima, frecuencia de reevaluación, hysteresis para
evitar saltos constantes y respuesta cuando ningún mountpoint tenga coordenadas.

### FEAT-02 — GGA upstream hacia el source

Algunos servicios VRS requieren que el caster reenvíe el GGA del rover hacia
la fuente. Agregar una política por mountpoint que indique si se reenvía, qué
rover tiene prioridad y con qué frecuencia máxima.

### FEAT-03 — `Transfer-Encoding: chunked` para sources NTRIP v2

- Parser incremental de chunks.
- Límites estrictos para tamaño de línea y chunk.
- Soporte de fragmentación arbitraria entre lecturas.
- Rechazo explícito de codificaciones no soportadas.
- Tests con chunks divididos y terminación incorrecta.

### FEAT-04 — Reconexión supervisada del relay externo

[Nota 2026-07-04: `ntrip_source_relay` no está en el árbol actual del repo —
esta sección aplica al futuro "agente de base" estilo str2str planificado
sobre librtk (ver `RTKLIB-2.4.3-b34/GUIA_RTKLIB_MOTOR.md` §3.2), o a la
herramienta si se agrega después.]

Las herramientas actuales son diagnósticas. Si `ntrip_source_relay` se convierte
en servicio, añadir:

- Reintento con exponential backoff y jitter.
- Resolución DNS y reconexión sin reiniciar el proceso.
- Timeout de conexión y de datos.
- Estado visible y códigos de salida útiles.
- Protección contra loops de reconexión y archivos de captura ilimitados.

### FEAT-05 — Registro persistente y administración

Desarrollar después de completar seguridad y ciclo de vida:

- SQLite para usuarios, roles, mountpoints, bans e historial.
- API administrativa separada del puerto NTRIP.
- Roles `admin`, `source` y `rover`.
- Kick y ban por conexión, usuario, mountpoint o IP.
- Auditoría de cambios administrativos.
- Dashboard para estado, mapa, métricas y gestión de credenciales.

La API administrativa debe requerir TLS, tokens revocables y protección contra
CSRF si utiliza cookies.

## 6. Mejoras de rendimiento a medir, no asumir

- Evitar inicializar todos los rings de mountpoints al arrancar; reservarlos al
  crear o activar el mountpoint. [Ya medido: los ~35 MB de RSS al arranque son
  el memset del broker completo (128 × 256 KB); con init lazy el arranque
  bajaría a ~2 MB. Beneficio real solo de RAM, cero de CPU.]
- Medir el coste de un `epoll_ctl(MOD)` por rover y epoch.
- Evaluar batching de wakeups solamente si aparece como cuello de botella.
- Medir copias de memoria source → ring → write buffer → socket.
- Definir una política explícita para rovers lentos.
- Probar 1, 10, 100, 500 y 1000 clientes con streams RTCM representativos.

Toda optimización debe registrar CPU, RSS, latencia, frames perdidos y contexto
de hardware. No se recomienda introducir una arquitectura lock-free sin una
medición que lo justifique.

## 7. Plan sugerido por fases

### Fase A — Base confiable

Ejecutar en este orden:

1. IMP-04: prueba local determinista source → caster → rover y casos del
   comparador, sin depender de Internet.
2. IMP-05: ASan/UBSan/TSan y CI para convertir carreras en fallos observables.
3. IMP-01: ownership/refcount y cierre idempotente.
4. IMP-01: exclusión por conexión y wakeups sin doble dispatch.
5. IMP-01B: consistencia del ring buffer y política de rover lento.
6. IMP-01C: shutdown ordenado.
7. IMP-01D: límites estrictos, snapshots y semántica de contadores.
8. IMP-06: limpieza del repositorio y build reproducible.

### Fase B — Seguridad y despliegue

- IMP-02 credenciales seguras.
- IMP-03 TLS.
- IMP-07 snapshot de configuración.
- IMP-09 observabilidad.
- IMP-10 servicio y empaquetado.

### Fase C — Funcionalidad GNSS avanzada

- FEAT-01 NEAREST.
- FEAT-02 GGA upstream/VRS.
- FEAT-03 chunked NTRIP v2.
- Pruebas de campo con base y rovers reales.

### Fase D — Producto administrable

- FEAT-04 relay supervisado, si se requiere como servicio.
- FEAT-05 SQLite, API y dashboard.
- Auditoría, backups y procedimientos operativos.

## 8. Definition of Done general

Una feature no se considera terminada hasta que:

- Tiene tests automáticos positivos, negativos y de límites.
- Compila sin warnings con GCC y Clang.
- Pasa ASan/UBSan y, cuando toque concurrencia, TSan.
- No introduce secretos ni datos sensibles en logs.
- Actualiza configuración de ejemplo y documentación.
- Define comportamiento de error y recuperación.
- Incluye una medición antes/después si afecta rendimiento.

## 9. Siguiente acción recomendada

Comenzar por el **harness local source → caster → rover** y activar sanitizers;
esa red de seguridad debe preceder los cambios de concurrencia. A continuación,
implementar IMP-01 como tres garantías separadas: el objeto continúa vivo
mientras exista un trabajo, sólo un worker modifica cada conexión y el ring
entrega un snapshot consistente o declara lag. Después completar shutdown,
límites y snapshots antes de avanzar a credenciales, TLS o nuevas features.
