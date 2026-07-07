# syntax=docker/dockerfile:1
#
# Dockerfile — NtripCaster
#
# Build multi-stage: la etapa "build" tiene cmake/gcc y compila el
# binario; la etapa final es una imagen liviana que SOLO tiene el
# binario ya compilado + sus dependencias de runtime (glibc/pthread,
# ya presentes en la base). Nada de toolchain queda en la imagen final.
#
# Uso rapido:
#   docker build -t ntripcaster .
#   docker run --rm -p 2101:2101 ntripcaster
#
# Con credenciales propias (en vez de las de prueba por defecto) y
# datos persistentes, ver docker/README.md o docker-compose.yml.

# ── Etapa 1: build ──────────────────────────────────────────────────
FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Target "ntripcaster" nomas -- el configure igual define los targets
# de test/tools (test/CMakeLists.txt), pero no hace falta compilarlos
# para correr el caster: solo alargarian el build y la imagen.
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAPP_VERSION=docker \
    && cmake --build build --target ntripcaster -j"$(nproc)"

# ── Etapa 2: runtime ────────────────────────────────────────────────
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        util-linux \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --home-dir /home/ntrip \
               --shell /usr/sbin/nologin ntrip

COPY --from=build /src/build/src/ntripcaster /usr/local/bin/ntripcaster
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

# templates/: el .html que se le sirve a un browser (User-Agent: Mozilla)
# en GET / (ver src/protocol/html_template.h). Se lee del disco en cada
# request -- si montas tu propio volumen acá, el diseño se puede retocar
# sin recompilar ni reiniciar el contenedor.
COPY templates /app/templates

# /app/conf: donde vive (o se genera) ntripcaster.conf.
# /app/logs: reservado por si algun dia se loguea a archivo -- por
# defecto el conf generado por el entrypoint loguea a stdout/consola,
# que es lo correcto para un contenedor (docker logs).
RUN mkdir -p /app/conf /app/logs && chown -R ntrip:ntrip /app
WORKDIR /app

# OJO: NO hacemos "USER ntrip" acá. El contenedor arranca como root a
# proposito -- es el UNICO modo de poder arreglar el dueño de volumenes
# bind-mount (./docker/conf, ./templates) que Docker crea en el host
# como root:root la primera vez que no existen. entrypoint.sh hace el
# chown que hace falta y despues DROPEA privilegios de verdad con
# `setpriv` antes de ejecutar el binario del caster -- el proceso que
# queda corriendo (PID 1 real, via exec) es "ntrip", nunca root. Ver
# docker/entrypoint.sh y docker/README.md.
EXPOSE 2101

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
