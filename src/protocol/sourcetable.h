/*
 * sourcetable.h — Formateo y envío del sourcetable (STR;/CAS;/NET;)
 *
 * Decisión de diseño: el sourcetable NO se dividió por versión de
 * protocolo (v1/v2) como el resto de ntrip.c, porque las tres variantes
 * (texto v1, texto v2, HTML para navegador) comparten los mismos datos
 * (broker_sourcetable_fill) y el mismo propósito -- describir los
 * mountpoints disponibles. Separarlas en ntrip_v1.c/ntrip_v2.c hubiera
 * duplicado el formateo del cuerpo del mensaje sin ninguna ganancia.
 * Vive en protocol/ (no en core/) porque el formato STR;/CAS;/NET; es
 * 100% spec de NTRIP, no un concepto del broker.
 */
#ifndef NTRIPCASTER_SOURCETABLE_H
#define NTRIPCASTER_SOURCETABLE_H

#include "../core/io_engine.h"
#include "../core/conn.h"

/* Formato v1: "SOURCETABLE 200 OK" + texto plano. Cierra la conexión. */
void sourcetable_handle_v1(io_engine_t *eng, conn_t *conn);

/*
 * Formato v2: "HTTP/1.1 200 OK" + Ntrip-Version header.
 * browser=1 -> HTML; browser=0 -> texto plano (mismo formato que v1,
 * solo cambian los headers de la respuesta).
 */
void sourcetable_handle_v2(io_engine_t *eng, conn_t *conn, int browser);

#endif /* NTRIPCASTER_SOURCETABLE_H */
