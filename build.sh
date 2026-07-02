#!/bin/bash
# build.sh — compila el caster en una carpeta build-<version> aparte,
# para poder tener varias versiones compiladas al mismo tiempo sin
# pisarse entre ellas.
#
# Uso:
#   ./build.sh 1.001.1.05
#   ./build.sh 0.2.0
#
# El binario queda en build-<version>/src/ntripcaster y al arrancar
# imprime esa misma versión, así que carpeta y binario siempre
# coinciden — no hay que adivinar qué build es cuál.
set -e

VERSION="${1:?Uso: ./build.sh <version>   ejemplo: ./build.sh 1.001.1.05}"
BUILD_DIR="build-${VERSION}"

echo "=== Configurando ${BUILD_DIR} (version ${VERSION}) ==="
cmake -S . -B "${BUILD_DIR}" -DAPP_VERSION="${VERSION}"

echo ""
echo "=== Compilando ==="
cmake --build "${BUILD_DIR}"

echo ""
echo "Listo: ${BUILD_DIR}/src/ntripcaster"
echo "Correlo con: ./${BUILD_DIR}/src/ntripcaster 2101"
