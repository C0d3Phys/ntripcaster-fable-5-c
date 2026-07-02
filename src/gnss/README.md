# rtcm3 — Decoder RTCM3 para NtripCaster

Librería C17 zero-dependency para parsear y decodificar mensajes RTCM3.
Escrita desde cero usando librtcm (Swift Navigation) como referencia de bits,
sin copiar código ni heredar sus dependencias (libswiftnav, CMake custom, etc.).

## Archivos

| Archivo | Descripción |
|---|---|
| `rtcm3_bits.h` | Operaciones de bits inline. Sin .c — header-only. |
| `rtcm3_frame.h/c` | CRC24Q + parser de frames del stream. |
| `rtcm3_station.h/c` | Mensajes de estación base: coords ECEF, antena, receptor. |
| `rtcm3_msm.h/c` | Header MSM (Multiple Signal Messages) para todas las constelaciones. |
| `rtcm3_eph.h/c` | Efemérides: GPS, GLONASS, BeiDou, QZSS, Galileo F/NAV + I/NAV. |
| `rtcm3.h` | Include único — incluye todo lo anterior. |
| `rtcm3_decode.c` | Dispatcher: recibe un frame validado, retorna `rtcm3_message_t`. |

## Uso mínimo (caster — solo relay de bytes)

```c
#include "gnss/rtcm3.h"

// Recibir datos de red en buf[len]
rtcm3_frame_t frames[64];
int           count;
size_t        used;

rtcm3_parse_stream(buf, len, frames, 64, &count, &used);

for (int i = 0; i < count; i++) {
    rtcm3_message_t msg;
    rtcm3_decode(&frames[i], &msg);

    // Actualizar coords para routing NEAREST
    if (msg.type == RTCM3_MSG_COORDS) {
        mountpoint_set_coords(mp,
            msg.coords.lat_deg,
            msg.coords.lon_deg);
    }

    // Retransmitir el frame intacto (zero-copy)
    client_write(frames[i].data, frames[i].frame_len);
}

// Avanzar el buffer solo los bytes consumidos
memmove(buf, buf + used, len - used);
```

## Uso completo (logging + monitoring)

```c
rtcm3_message_t msg;
rtcm3_decode(&frame, &msg);

switch (msg.type) {
case RTCM3_MSG_MSM:
    printf("[%s] MSM%d  epoch=%u ms  sats=%d\n",
           rtcm3_gnss_name(msg.msm.gnss),
           msg.msm.level,
           msg.msm.epoch_ms,
           msg.msm.num_satellites);
    break;

case RTCM3_MSG_COORDS:
    printf("Base station %u: lat=%.6f lon=%.6f\n",
           msg.station_id,
           msg.coords.lat_deg,
           msg.coords.lon_deg);
    break;

case RTCM3_MSG_EPH_GPS:
case RTCM3_MSG_EPH_BDS:
case RTCM3_MSG_EPH_GAL: {
    char prn[8];
    printf("Eph %s  week=%u  toe=%u  health=%u\n",
           rtcm3_eph_gnss_prn(&msg.eph, prn),
           msg.eph.week, msg.eph.toe, msg.eph.health);
    break;
}

case RTCM3_MSG_EPH_GLO:
    printf("GLONASS slot=%u ch=%+d  pos=(%.1f,%.1f,%.1f) km\n",
           msg.eph_glo.slot, msg.eph_glo.freq_channel,
           msg.eph_glo.pos_x_km,
           msg.eph_glo.pos_y_km,
           msg.eph_glo.pos_z_km);
    break;

default:
    break;
}
```

## Mensajes soportados

### Estación base
| Msg | Descripción |
|---|---|
| 1005 | Coordenadas ECEF (sin altura antena) |
| 1006 | Coordenadas ECEF + altura antena |
| 1007 | Descriptor de antena |
| 1008 | Descriptor de antena + número de serie |
| 1033 | Receptor + Antena completo |

### Observaciones MSM
| Rango | Constelación |
|---|---|
| 1071–1077 | GPS |
| 1081–1087 | GLONASS |
| 1091–1097 | Galileo |
| 1101–1107 | SBAS |
| 1111–1117 | QZSS |
| 1121–1127 | BeiDou |
| 1131–1137 | NavIC |

Se decodifica el **header** (epoch, satélites, señales). El cuerpo de
observaciones se retransmite intacto — el rover hace el procesamiento.

### Efemérides
| Msg | Constelación | Tipo |
|---|---|---|
| 1019 | GPS | Keplerianos |
| 1020 | GLONASS | PVT ECEF (PZ-90) |
| 1042 | BeiDou | Keplerianos |
| 1044 | QZSS | Keplerianos |
| 1045 | Galileo | F/NAV Keplerianos |
| 1046 | Galileo | I/NAV Keplerianos |

### Legacy (identificados, no decodificados)
1001–1012 — GPS/GLONASS observaciones antiguas. Se detecta el `station_id`
y se retransmiten intactos.

## Detalles de implementación

### Zero-copy
`rtcm3_frame_t.data` apunta directamente al buffer de entrada.
No hay `malloc` en el path crítico. El caller gestiona el buffer.

### CRC24Q
Polinomio `0x1864CFB`. Verificado idéntico a librtcm (Swift Navigation)
y al decoder Python de referencia.

### Fix GLONASS epoch
GLONASS usa epoch de **27 bits** (ms desde medianoche Moscú UTC+3),
precedido de **3 bits** de día de la semana.
GPS/Galileo/BeiDou/QZSS/NavIC usan **30 bits** (TOW en ms).

```
// INCORRECTO (bug del decoder Python de referencia):
epoch_ms = read_bits(30);   // lee 30 bits para GLONASS → valor erróneo

// CORRECTO:
glonass_day = read_bits(3);
epoch_ms    = read_bits(27);
```

Validado: Demo1.bin Auckland NZ → GLONASS `77520000 ms` (21:32:00 Moscú)
vs Python buggy `210986728 ms`.

### ECEF → Lat/Lon
Algoritmo de Bowring, 4 iteraciones, error < 0.1 mm. WGS84.
Usado para el routing NEAREST del caster.

## Compilar (standalone)

```bash
gcc -std=c17 -Wall -Isrc \
    src/gnss/rtcm3_frame.c \
    src/gnss/rtcm3_station.c \
    src/gnss/rtcm3_msm.c \
    src/gnss/rtcm3_eph.c \
    src/gnss/rtcm3_decode.c \
    -lm
```

## Integrar en el proyecto (CMake)

```cmake
# En src/CMakeLists.txt — descomentar en Fase 3
list(APPEND CORE_SOURCES
    gnss/rtcm3_frame.c
    gnss/rtcm3_station.c
    gnss/rtcm3_msm.c
    gnss/rtcm3_eph.c
    gnss/rtcm3_decode.c
)
```

## Dependencias

Ninguna. Solo libc estándar (`<stdint.h>`, `<string.h>`, `<math.h>`, `<stdio.h>`).
