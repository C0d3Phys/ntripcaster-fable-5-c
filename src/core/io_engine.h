/*
 * io_engine.h — Motor de I/O basado en epoll + thread pool
 *
 * Reemplaza el modelo clásico thread-per-connection del caster original.
 *
 * Arquitectura:
 *
 *   ┌─────────────────────────────────────────────────┐
 *   │                  io_engine                      │
 *   │                                                 │
 *   │  listen_fd ──► accept_loop ──► epoll_fd         │
 *   │                                    │            │
 *   │              work_queue ◄──────────┘            │
 *   │                  │                              │
 *   │        ┌─────────┴─────────┐                   │
 *   │      thread[0]  ...  thread[N-1]                │
 *   │        │                   │                    │
 *   │      dispatch(fd, events)                       │
 *   │        ├─ EPOLLIN + SOURCE → broker_source_data │
 *   │        ├─ EPOLLIN + CLIENT → read & discard     │
 *   │        ├─ EPOLLOUT         → flush write_buf    │
 *   │        └─ EPOLLERR/HUP     → close conn        │
 *   └─────────────────────────────────────────────────┘
 *
 * Modelo epoll: edge-triggered (EPOLLET) + EPOLLONESHOT
 *   - EPOLLET: máximo rendimiento, sin eventos spurious
 *   - EPOLLONESHOT: un solo thread maneja cada fd a la vez
 *   - Tras dispatch, el thread re-arma el fd en epoll
 *
 * Thread pool:
 *   - N threads fijos (= CPU cores por defecto)
 *   - Cola de trabajo con mutex + condvar (simple, sin false sharing)
 *   - Work stealing no necesario para este workload (I/O bound)
 */
#ifndef NTRIPCASTER_IO_ENGINE_H
#define NTRIPCASTER_IO_ENGINE_H

#include <stdint.h>
#include <pthread.h>
#include <signal.h>   /* sig_atomic_t */
#include "broker.h"

/* ── Configuración ───────────────────────────────────────────────── */
#define IO_MAX_EVENTS     256    /* eventos epoll por batch */
#define IO_WORK_QUEUE_CAP 1024   /* capacidad de la cola de trabajo */
#define IO_THREADS_DEFAULT 4     /* threads si no se configura */

/* ── Work item: fd + epoll events ───────────────────────────────── */
typedef struct {
    int      fd;
    uint32_t events;   /* EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP */
    conn_t  *conn;     /* puntero a la conexión (del user_data del epoll) */
} io_work_t;

/* ── Cola de trabajo (MPMC ring con mutex+condvar) ───────────────── */
typedef struct {
    io_work_t        items[IO_WORK_QUEUE_CAP];
    int              head;
    int              tail;
    int              count;
    pthread_mutex_t  lock;
    pthread_cond_t   not_empty;
    pthread_cond_t   not_full;
} io_work_queue_t;

/* ── I/O Engine ──────────────────────────────────────────────────── */
typedef struct io_engine_t {
    int              epoll_fd;
    int              listen_fd;

    /*
     * running — flag de shutdown.
     * volatile sig_atomic_t: es el único tipo que el estándar C garantiza
     * seguro de escribir desde un signal handler. io_engine_stop() escribe
     * este campo directamente y NO toca mutex/condvar (ver comentario en
     * io_engine.c) precisamente para poder llamarse desde el handler de
     * SIGINT/SIGTERM en main.c sin riesgo de deadlock.
     */
    volatile sig_atomic_t running;

    /* Thread pool */
    pthread_t       *threads;
    int              num_threads;
    io_work_queue_t  queue;

    /* Thread de accept (separado de los workers) */
    pthread_t        accept_thread;

    /* Referencia al broker */
    broker_t        *broker;

    /* Config */
    int              port;
    char             bind_addr[48];

    /*
     * tick_cb — callback opcional invocado en cada ciclo del accept loop
     * (~200 ms), en contexto normal (NO signal handler). main.c lo usa
     * para ejecutar el reload de auth cuando el handler de SIGHUP marcó
     * su flag — el handler solo escribe un sig_atomic_t; el trabajo real
     * (parsear INI, tomar el wrlock) pasa acá, donde es seguro.
     */
    void           (*tick_cb)(void *ctx);
    void            *tick_ctx;

} io_engine_t;

/* ── API ─────────────────────────────────────────────────────────── */

/*
 * io_engine_init — Inicializa el engine: crea epoll fd, bind+listen,
 * arranca el pool de threads.
 *
 * @engine        Engine a inicializar
 * @broker        Broker ya inicializado
 * @bind_addr     IP a escuchar ("0.0.0.0" o específica)
 * @port          Puerto TCP (2101 por defecto NTRIP)
 * @num_threads   Threads del pool (0 = auto = CPU cores)
 *
 * Retorna 0 si OK, -1 si error (ver errno).
 */
int io_engine_init(io_engine_t *engine, broker_t *broker,
                   const char *bind_addr, int port, int num_threads);

/*
 * io_engine_run — Bloquea hasta que io_engine_stop() sea llamado.
 * Corre el accept loop en el thread actual.
 * Los workers ya están corriendo en background.
 */
void io_engine_run(io_engine_t *engine);

/*
 * io_engine_stop — Señala el shutdown. io_engine_run() retorna.
 *
 * Async-signal-safe: solo escribe engine->running. Se puede (y en este
 * proyecto SE HACE) llamar directo desde un signal handler de SIGINT/
 * SIGTERM sin riesgo de deadlock.
 */
void io_engine_stop(io_engine_t *engine);

/*
 * io_engine_destroy — Libera recursos. Llamar después de stop+run.
 *
 * IMP-01C: además de unir los threads y cerrar epoll/listen, cierra y
 * libera cualquier conexión que haya quedado viva al momento del
 * shutdown (source empujando, rovers suscritos) ANTES de que el
 * llamador destruya el broker -- evita fd leaks y conn_t sin liberar.
 * Ver comentario de io_engine_close_remaining_conns() en el .c.
 */
void io_engine_destroy(io_engine_t *engine);

/*
 * io_engine_conn_watch — Agrega/re-arma un fd en epoll.
 * events: EPOLLIN | EPOLLOUT | EPOLLET | EPOLLONESHOT
 */
int io_engine_conn_watch(io_engine_t *engine, conn_t *conn, uint32_t events);

/*
 * io_engine_conn_close — Cierra un fd y libera la conexión del broker.
 * Thread-safe: puede ser llamado desde cualquier worker.
 */
void io_engine_conn_close(io_engine_t *engine, conn_t *conn);

/*
 * io_engine_wakeup_mount_clients — Reactiva EPOLLOUT en los clientes
 * suscritos a `mp` para que consuman datos nuevos del ring.
 *
 * Expuesto para que el protocol handler (ntrip.c) pueda despertar
 * clientes cuando relay datos que llegaron pegados al handshake del
 * source (ver forward_source_payload en ntrip.c). Thread-safe.
 */
struct mountpoint_t;
void io_engine_wakeup_mount_clients(io_engine_t *engine, struct mountpoint_t *mp);

#endif /* NTRIPCASTER_IO_ENGINE_H */
