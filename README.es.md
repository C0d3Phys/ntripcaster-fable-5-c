<div align="center">

# NtripCaster

**Un caster NTRIP v1/v2 hecho desde cero en C17 — thread pool con epoll, relay lock-free, auth hasheada.**

[![Language](https://img.shields.io/badge/language-C17-00599C?style=for-the-badge&logo=c&logoColor=white)](src/)
[![Build](https://img.shields.io/badge/build-CMake%203.20%2B-informational?style=for-the-badge&logo=cmake&logoColor=white)](CMakeLists.txt)
[![License](https://img.shields.io/badge/license-MIT-green?style=for-the-badge)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Linux%20%2F%20WSL-lightgrey?style=for-the-badge&logo=linux&logoColor=white)](build.sh)
[![Protocol](https://img.shields.io/badge/protocol-NTRIP%20v1%20%7C%20v2-orange?style=for-the-badge)](docs/)
[![Sanitizers](https://img.shields.io/badge/verificado%20con-ASan%20%7C%20UBSan-critical?style=for-the-badge)](docs/)

[English](README.md) · [Docs](docs/) · [Guía con hardware real](docs/GUIA_GNSS_REAL.md)

</div>

---

## Qué es esto

NtripCaster es un caster [NTRIP](https://es.wikipedia.org/wiki/NTRIP) v1/v2 — el servidor relay que se para entre una base GNSS que transmite correcciones RTCM3 y los rovers que las consumen para posicionamiento RTK/PPK. Está escrito desde cero en C17, sin framework y sin ningún codebase legacy debajo: un caster de propósito único construido sobre tres decisiones de diseño centrales.

- **epoll edge-triggered + thread pool con `EPOLLONESHOT`.** Un accept loop, N workers, sin un thread por conexión. Un fd es dueño de exactamente un worker a la vez — no hace falta lock en el hot path.
- **Ring buffer SPMC lock-free por mountpoint.** El source escribe una vez; cada rover suscrito lee a su propio ritmo desde el mismo buffer circular de 256 KB, zero-copy. Un rover que se atrasa más del tamaño del buffer se desconecta en vez de frenar al source.
- **Decoder RTCM3 incremental para observabilidad, desacoplado del path de relay.** Los bytes llegan a los clientes exactamente como llegaron — corruptos o no. El decode corre en paralelo únicamente para extraer coordenadas de estación, metadata del receptor, y stats de integridad por tipo de mensaje para el log.

Este es un proyecto de sistemas en desarrollo activo, no un script de fin de semana. Tiene un historial de revisión de diseño escrito (ver [`docs/`](docs/)), stress tests bajo AddressSanitizer/UBSan, y un escenario CTest end-to-end real — no solo "compiló".

## Features

| Área | Qué incluye |
|---|---|
| **Protocolo** | NTRIP v1 (`SOURCE`, `GET` plano) y v2 (`POST`, consciente de chunked), generación de sourcetable, Basic Auth |
| **RTCM3** | Parser de frames incremental (reensambla frames partidos entre reads), validación CRC24Q, decode de coords de estación (1005/1006), antena (1007/1008), info de receptor (1033), MSM (1071–1137), efemérides (GPS/GLONASS/BeiDou/QZSS/Galileo), obs legacy GPS/GLONASS (1001–1012) |
| **Concurrencia** | epoll ET + pool de workers `EPOLLONESHOT`, reserva de cupos vía CAS (sin TOCTOU en `max_sources`/`max_clients`), stats atómicas por mountpoint |
| **Auth** | Hashing de passwords con PBKDF2-HMAC-SHA256 (SHA-256 vendoreado, comparación en tiempo constante, cero dependencias externas), fail-closed por diseño, reload en caliente vía `SIGHUP` — sin reiniciar, sin cortar conexiones |
| **Confiabilidad** | Sweep de timeouts de inactividad/handshake, shutdown ordenado (sin fugas de fd, sin `conn_t` colgantes en `SIGTERM`), contadores de bytes encolados vs. transmitidos separados |
| **Observabilidad** | Cabeceras completas de cada request logueadas (credenciales enmascaradas) en cada SOURCE/GET, stats periódicas de caudal + corrupción por mountpoint, niveles `warn`/`error`/`info` estructurados en todo el código |
| **Testing** | Escenario CTest end-to-end (RTCM3 sintético → relay → caster → rover → comparación byte a byte), validación manual contra una fuente NTRIP de producción real ([use-snip.com](https://www.use-snip.com/)) |

## Arquitectura

```
                 ┌─────────────────────────────────────────────┐
                 │                io_engine                    │
                 │  epoll (ET) + EPOLLONESHOT + pool de workers │
                 │  accept loop → cola de trabajo → N workers   │
                 └───────────────┬───────────────┬─────────────┘
                                 │               │
                     dispatch_source()   dispatch_client()
                                 │               │
                                 ▼               │
                 ┌─────────────────────────┐     │
   SOURCE ──────▶│  broker (auth, límites) │     │
   (push RTCM3)  └────────────┬────────────┘     │
                              ▼                  │
                 ┌─────────────────────────┐     │
                 │      mountpoint_t       │     │
                 │  ┌───────────────────┐  │     │
                 │  │ ring_buffer (SPMC)│──┼─────┘
                 │  │  256 KB, lock-free│  │   rb_read() por rover
                 │  └───────────────────┘  │
                 │  decoder RTCM3           │
                 │  incremental (coords,    │
                 │  stats por tipo, CRC24Q) │
                 └─────────────────────────┘
                              │
                              ▼
                  GET /mount ─── rover 1, rover 2, ... N
```

Cada conexión es un `conn_t` que pertenece a exactamente un worker a la vez (garantizado por `EPOLLONESHOT`); los mountpoints y sus ring buffers viven en un registro plano preasignado. Auth, límites de capacidad y stats viven todos en el `broker`, al que la capa de protocolo (`src/protocol/`) nunca toca directamente.

## Cómo arrancar

### Compilar

```bash
git clone https://github.com/C0d3Phys/ntripcaster-fable-5-c.git
cd ntripcaster-fable-5-c
./build.sh 1.0.0          # configura + compila en build-1.0.0/
```

Requiere CMake ≥ 3.20, un compilador C17 (GCC/Clang), y `pthread`. Sin más dependencias externas — `inih`, `uthash` y SHA-256 están vendoreados en [`vendor/`](vendor/).

### Configurar

```bash
cp conf/ntripcaster.conf.example conf/ntripcaster.conf
```

Las passwords se guardan hasheadas — **nunca** en texto plano — con PBKDF2-HMAC-SHA256. Generá el hash de una credencial de mountpoint o rover:

```bash
echo -n "tu-password-real" | ./build-1.0.0/src/ntripcaster --hash-password
# -> pbkdf2-sha256$100000$<salt>$<hash>
```

Pegá el string resultante en `conf/ntripcaster.conf`:

```ini
[source]
BASE1 = pbkdf2-sha256$100000$...        # tu base GNSS / relay publica acá

[client:BASE1]
rover1 = pbkdf2-sha256$100000$...       # credencial Basic Auth del rover 1
rover2 = pbkdf2-sha256$100000$...
```

La password en texto plano sigue siendo lo que tu receptor o cliente NTRIP manda por la red — así es el protocolo. El hash es solo cómo lo guarda el caster en disco, para que el archivo de config en sí no sea una fuga de credenciales si se llega a filtrar.

### Correr

```bash
./build-1.0.0/src/ntripcaster 2101 conf/ntripcaster.conf ntripcaster.log
```

Recargar credenciales o identidad del caster sin cortar conexiones:

```bash
kill -HUP $(pidof ntripcaster)
```

Ver [`docs/GUIA_GNSS_REAL.md`](docs/GUIA_GNSS_REAL.md) para una guía completa conectando una base GNSS real + rovers.

## Testing

```bash
ctest --test-dir build-1.0.0 --output-on-failure
```

El suite de CTest levanta un caster real en un puerto efímero, le da de comer RTCM3 sintético vía un relay, lo consume con un cliente rover, y compara byte a byte lo que entró contra lo que salió — sin mocks. Los builds de desarrollo además se validan bajo `-fsanitize=address,undefined` en los paths sensibles a concurrencia (ring buffer, reserva de cupos, ciclo de vida de conexiones).

## Estructura del proyecto

```
src/
├── core/       io_engine (epoll/threads), broker, mountpoint, ring_buffer, logger, config
├── gnss/       parsing y decode de frames RTCM3 (MSM, efemérides, info de estación/receptor)
├── protocol/   handshakes NTRIP v1/v2, sourcetable, auth + hashing de passwords
└── main.c
test/
├── tools/      CLIs standalone de relay/rover/compare usados por el harness E2E
├── helpers/    generador de RTCM3 sintético, helpers de bash para tests
└── cases/      escenarios de CTest
vendor/         inih, uthash, sha256 (todo vendoreado, cero dependencias de package manager)
docs/           docs de diseño, decisiones registradas, guía de integración con hardware
```

## Estado

Este proyecto está en desarrollo activo. El protocolo core, la concurrencia y el hardening de auth ya están hechos y probados; un refactor más grande de ownership de conexiones (ciclo de vida de `conn_t` con refcount bajo alta concurrencia) es el próximo trabajo grande pendiente. Ver [`docs/`](docs/) para el log completo de mejoras y el razonamiento de diseño detrás de cada decisión no trivial de este codebase.

## Licencia

[MIT](LICENSE) — ver el archivo `LICENSE` para más detalles.
