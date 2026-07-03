# Guía: conectar tus GNSS reales (1 base + 2 rovers)

Escenario: tu GNSS base publica correcciones RTCM3 al caster (SOURCE), y tus
otros 2 GNSS consumen como clientes, cada uno con su propia credencial.
Verificado en sandbox 2026-07-02 con este flujo exacto (base + rover1 +
rover2 + credencial mala rechazada + log a archivo).

---

## 1. Compilar

```bash
cd wsl-project-tested
./build.sh 1.001.1.08
```

> Fix aplicado hoy: `src/CMakeLists.txt` no incluía `core/logger.c` (quedó
> pendiente de la sesión que se cortó) — sin eso el link fallaba con
> `undefined reference to log_write`. Ya está agregado.

## 2. Configurar credenciales — `conf/ntripcaster.conf`

```ini
; La BASE publica en el mountpoint BASE1 con esta password (SOURCE v1)
[source]
BASE1 = passbase123          ; CAMBIALA — es la que va en tu GNSS base

; Cada rover tiene SU usuario y SU password (Basic Auth en el GET)
[client:BASE1]
rover1 = clave-rover1        ; GNSS cliente #1
rover2 = clave-rover2        ; GNSS cliente #2
```

Claves: el nombre del mountpoint es libre (usa algo corto, sin espacios).
Auth es **fail-closed**: lo que no está en este archivo, se rechaza.
Cambios en caliente: editá el archivo y `kill -HUP $(pidof ntripcaster)` —
no hace falta reiniciar (los rovers conectados no se cortan).

## 3. Arrancar y verificar

```bash
./build-1.001.1.08/src/ntripcaster 2101 conf/ntripcaster.conf ntripcaster.log
# args: [puerto] [conf] [archivo de log]     Nivel: NTRIPCASTER_LOG=debug|info|warn|error

# En otra terminal — ver la sourcetable:
curl -s -A "NTRIP Test/1.0" --http0.9 http://localhost:2101/
# Seguir el log en vivo:
tail -f ntripcaster.log
```

## 4. Red: WSL2 no es visible desde tu LAN (paso CRÍTICO)

Tus GNSS están en la red local, pero WSL2 vive en una red NAT interna.
Dos opciones:

**Opción A — Windows 11 22H2+ (la simple): modo mirrored.**
En `%UserProfile%\.wslconfig`:
```ini
[wsl2]
networkingMode=mirrored
```
`wsl --shutdown`, reabrir. WSL comparte la IP de Windows: los GNSS apuntan
directo a la IP de tu PC.

**Opción B — portproxy (cualquier Windows).** PowerShell como admin:
```powershell
$wslip = (wsl hostname -I).Trim().Split(" ")[0]
netsh interface portproxy add v4tov4 listenport=2101 listenaddress=0.0.0.0 connectport=2101 connectaddress=$wslip
New-NetFirewallRule -DisplayName "NtripCaster 2101" -Direction Inbound -Protocol TCP -LocalPort 2101 -Action Allow
```
> La IP de WSL cambia al reiniciar — rehacer el `portproxy` (o script al inicio).

En ambos casos, la IP que va en los GNSS es **la IP de tu PC Windows en la
LAN** (`ipconfig` → IPv4). Probá desde otra máquina de la red:
`curl -s -A "NTRIP T/1.0" --http0.9 http://IP_DE_TU_PC:2101/`

## 5. GNSS base (la fuente)

**Si el receptor tiene modo "NTRIP Server"** (u-blox F9P vía u-center,
Trimble, Emlid Reach, etc.), configurá:

| Campo | Valor |
|---|---|
| Caster host | IP de tu PC Windows |
| Caster port | 2101 |
| Mountpoint | `BASE1` |
| Password | `passbase123` (la de `[source]`) |
| Protocolo | NTRIP v1 (el clásico "SOURCE") — v2/POST también soportado |

Y en el receptor: activar salida RTCM3 (mínimo 1005 + MSM4/MSM7 de las
constelaciones que uses, a 1 Hz). El 1005 importa: de ahí el caster extrae
las coordenadas para el futuro mapa.

**Si el receptor solo da serial/USB**, usá str2str de RTKLIB como puente:
```bash
str2str -in serial://ttyUSB0:115200 -out ntrips://:passbase123@IP_PC:2101/BASE1
```
(`ntrips://` = modo NTRIP server; el password va antes de la `@`.)

## 6. Los 2 GNSS rovers (clientes)

En cada rover, configurá su **NTRIP Client**:

| Campo | Rover 1 | Rover 2 |
|---|---|---|
| Host / Port | IP de tu PC : 2101 | igual |
| Mountpoint | `BASE1` | `BASE1` |
| Usuario | `rover1` | `rover2` |
| Password | `clave-rover1` | `clave-rover2` |

Notas: el envío de GGA del rover al caster es opcional e inofensivo (hoy se
descarta — NEAREST/VRS no está implementado, conectan directo al mountpoint).
Cada rover con credencial propia = podés revocar uno editando el conf +
`kill -HUP`, sin tocar el otro.

## 7. Problemas comunes

| Síntoma | Causa probable |
|---|---|
| `ERROR - Bad Password` en la base | Password del GNSS ≠ `[source]` del conf. ¿Editaste el conf sin `kill -HUP`? |
| `401` / `Bad Password` en un rover | user/pass no coinciden con `[client:BASE1]`, o el mountpoint del rover no es exactamente `BASE1` |
| `409` / `Mount Point Taken` | Ya hay una base conectada a ese mountpoint (¿instancia vieja? esperá el `source_timeout` de 30 s o reiniciá la base) |
| Rover conecta pero no recibe datos | La base no está enviando RTCM (verificá en el log `pipelined payload` / bytes de la base), o conectaste el rover a un mountpoint sin source |
| Nada conecta desde la LAN | Paso 4: portproxy/firewall/mirrored. Probá primero `curl` local dentro de WSL, después desde otra máquina |
| Se desconectan solos | Timeouts: base inactiva >30 s o rover >60 s se kickea. Una base a 1 Hz nunca lo toca |
| Ver más detalle | `NTRIPCASTER_LOG=debug ./build-.../src/ntripcaster ...` y `tail -f ntripcaster.log` |
