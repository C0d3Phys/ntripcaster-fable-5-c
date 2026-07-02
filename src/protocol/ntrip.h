/*
 * ntrip.h — Parser y handler del protocolo NTRIP v1/v2
 *
 * Soporta:
 *   NTRIP v1 — sourcetable, cliente (GET /mp NTRIP/1.0), source (SOURCE pwd mp)
 *   NTRIP v2 — cliente (GET /mp HTTP/1.1 + Ntrip-Version: Ntrip/2.0)
 *               source  (POST /mp HTTP/1.1 + Ntrip-Version: Ntrip/2.0)
 *   Browser  — GET / HTTP/1.1 (User-Agent: Mozilla) → HTML sourcetable
 *
 * Flujo:
 *   dispatch_handshake() acumula datos en conn->read_buf
 *   → llama ntrip_handle_request() cuando detecta \r\n\r\n
 *   → ntrip_handle_request() parsea, responde, y transiciona conn->state
 */
#ifndef NTRIPCASTER_NTRIP_H
#define NTRIPCASTER_NTRIP_H

/* Forward declarations — evita ciclos de includes */
struct io_engine_t;
struct conn_t;

/*
 * ntrip_handle_request — Entry point del protocol handler.
 *
 * Precondición: conn->read_buf[0..read_len-1] contiene el header completo
 * (hasta \r\n\r\n inclusive).
 *
 * Postcondición:
 *   - Si sourcetable: response enviado, conn->state = CLOSING
 *   - Si client:      suscrito al mountpoint, conn->state = CLIENT_ACTIVE
 *   - Si source:      registrado en mountpoint, conn->state = SOURCE_ACTIVE
 *   - Si error:       conn cerrada
 *
 * La función re-arma el fd en epoll según el nuevo estado.
 */
void ntrip_handle_request(struct io_engine_t *eng, struct conn_t *conn);

#endif /* NTRIPCASTER_NTRIP_H */
