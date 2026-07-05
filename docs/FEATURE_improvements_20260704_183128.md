# Features e improvements pendientes

**Proyecto:** NtripCaster  
**Fecha de registro:** 2026-07-04 18:31:28 (America/Managua)  
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

### IMP-01 — Ciclo de vida seguro de `conn_t`

**Problema:** un evento que ya esté dentro de la cola de trabajo puede conservar
un puntero a una conexión liberada por otro worker. El orden actual de
`EPOLL_CTL_DEL`, detach y `close()` reduce la ventana, pero no elimina eventos
ya encolados.

**Propuesta:**

- Agregar un contador de referencias atómico a `conn_t`.
- Obtener una referencia antes de insertar la conexión en la work queue.
- Liberar esa referencia al terminar o descartar el trabajo.
- Separar `closing` de la liberación física de memoria.
- Garantizar que sólo un worker ejecute el cierre lógico.
- Agregar una prueba de estrés con conexiones y desconexiones simultáneas.

**Criterios de aceptación:**

- Ningún `use-after-free` bajo AddressSanitizer.
- Ninguna carrera de ciclo de vida bajo ThreadSanitizer.
- Cierre idempotente aunque dos eventos detecten error al mismo tiempo.
- Prueba repetida de conexión/desconexión durante al menos 10 minutos.

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
- `SIGTERM` con shutdown ordenado y tiempo máximo de drenaje.
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
  crear o activar el mountpoint.
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

- IMP-01 ciclo de vida/refcount.
- IMP-04 suite de integración.
- IMP-05 sanitizers y CI.
- IMP-06 higiene y README.

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

Comenzar por **IMP-01** y construir simultáneamente el test de estrés que
reproduzca cierres concurrentes. Es el cambio que más reduce el riesgo de una
falla difícil de diagnosticar. Después, automatizar el flujo local
source → caster → rover permitirá desarrollar el resto con mucha más seguridad.

