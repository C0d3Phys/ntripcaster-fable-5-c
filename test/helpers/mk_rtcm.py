#!/usr/bin/env python3
"""
mk_rtcm.py -- generador de frames RTCM3 validos para los tests del
harness CTest (tarea A1 de la Fase A, ver docs/FEATURE_improvements_FASE_A_20260704.md).

Usa el MISMO CRC24Q que src/gnss/rtcm3_frame.c (poly 0x1864CFB), asi que
los frames que genera son indistinguibles de RTCM3 real para el decoder
de este proyecto -- permite probar el camino completo (SOURCE -> ring ->
relay -> cliente -> decode) sin depender de un caster externo real como
SNIP ni de credenciales.

Modos:
  mk_rtcm.py frame [--type N] [--seq N]
      Imprime UN frame valido a stdout (bytes crudos).

  mk_rtcm.py push HOST PORT MOUNT PASSWORD SECONDS [--rate-hz N] [--corrupt-every N]
      Hace el handshake SOURCE v1 ("SOURCE <password> /<mount>") contra
      un ntripcaster y empuja frames validos en loop durante SECONDS
      segundos. Con --corrupt-every N intercala 1 byte de ruido cada N
      frames (para probar deteccion de CRC roto, ej. t06).

Esto reemplaza la necesidad de "nc" + un generador separado: un solo
script hace de "GNSS sintetico" para toda la suite.
"""
import argparse
import socket
import sys
import time

CRC24Q_POLY = 0x1864CFB


def crc24q(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b << 16
        for _ in range(8):
            crc <<= 1
            if crc & 0x1000000:
                crc ^= CRC24Q_POLY
    return crc & 0xFFFFFF


def make_frame(msg_type: int, seq: int, body_extra_len: int = 8) -> bytes:
    """
    Arma un frame RTCM3 valido: preamble 0xD3 + longitud (10 bits) +
    body + CRC24Q (3 bytes).

    El body no necesita ser un mensaje RTCM3 semanticamente correcto
    para este proyecto -- rtcm3_decode.c solo intenta interpretar tipos
    conocidos (1005/1006/1074 etc.) y ya maneja "tipo desconocido" sin
    problema (ver mp_relay -> switch default). Lo que SI debe ser
    correcto es el framing (preamble + longitud + CRC), que es lo que
    ejercitan los tests de esta suite.

    Los primeros 12 bits del body son el numero de mensaje (msg_type).
    Se agregan `seq` y padding para variar el contenido entre frames.
    """
    # 12 bits de tipo + 4 bits altos de un campo cualquiera (0) empacados
    # en los primeros 2 bytes del body.
    b0 = (msg_type >> 4) & 0xFF
    b1 = ((msg_type & 0x0F) << 4) | 0x0
    body = bytes([b0, b1]) + seq.to_bytes(4, "big") + bytes(body_extra_len)

    body_len = len(body)
    if body_len > 1023:
        raise ValueError("body demasiado largo para 10 bits de longitud")

    header = bytes([0xD3, (body_len >> 8) & 0x03, body_len & 0xFF])
    crc = crc24q(header + body)
    crc_bytes = bytes([(crc >> 16) & 0xFF, (crc >> 8) & 0xFF, crc & 0xFF])
    return header + body + crc_bytes


def cmd_frame(args):
    frame = make_frame(args.type, args.seq)
    sys.stdout.buffer.write(frame)
    sys.stdout.buffer.flush()


def cmd_push(args):
    sock = socket.create_connection((args.host, args.port), timeout=5)
    request = f"SOURCE {args.password} /{args.mount}\r\n" \
              f"Source-Agent: mk_rtcm.py/1.0\r\n\r\n"
    sock.sendall(request.encode("ascii"))
    sock.settimeout(5)
    resp = sock.recv(4096)
    if b"200" not in resp and b"OK" not in resp:
        sys.stderr.write(f"mk_rtcm.py: SOURCE rechazado: {resp!r}\n")
        sock.close()
        sys.exit(1)

    sys.stderr.write(f"mk_rtcm.py: push activo -> {args.host}:{args.port}/{args.mount} "
                      f"por {args.seconds}s (rate={args.rate_hz}Hz)\n")

    seq = 0
    msg_types = [1074, 1084, 1094, 1114, 1124, 1006, 1033, 1013]
    started = time.monotonic()
    period = 1.0 / args.rate_hz if args.rate_hz > 0 else 0.05
    sent_frames = 0

    try:
        while time.monotonic() - started < args.seconds:
            frame = bytearray(make_frame(msg_types[seq % len(msg_types)], seq))
            seq += 1

            if args.corrupt_every > 0 and seq % args.corrupt_every == 0:
                # Meter 1 byte de ruido ANTES del frame (no adentro --
                # así el frame en sí sigue siendo válido y solo se agrega
                # basura detectable como "skipped" por el decoder, igual
                # que ruido de línea serial real).
                sock.sendall(b"\x00")

            sock.sendall(bytes(frame))
            sent_frames += 1
            time.sleep(period)
    except (BrokenPipeError, ConnectionResetError) as e:
        sys.stderr.write(f"mk_rtcm.py: conexion cortada: {e}\n")
    finally:
        sock.close()

    sys.stderr.write(f"mk_rtcm.py: listo, {sent_frames} frames enviados\n")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    pf = sub.add_parser("frame", help="imprime 1 frame valido a stdout")
    pf.add_argument("--type", type=int, default=1074)
    pf.add_argument("--seq", type=int, default=0)
    pf.set_defaults(func=cmd_frame)

    pp = sub.add_parser("push", help="hace SOURCE handshake y empuja frames en loop")
    pp.add_argument("host")
    pp.add_argument("port", type=int)
    pp.add_argument("mount")
    pp.add_argument("password")
    pp.add_argument("seconds", type=float)
    pp.add_argument("--rate-hz", type=float, default=20.0)
    pp.add_argument("--corrupt-every", type=int, default=0,
                     help="cada N frames, intercalar 1 byte de ruido (0 = nunca)")
    pp.set_defaults(func=cmd_push)

    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
