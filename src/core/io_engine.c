/*
 * io_engine.c — Motor de I/O: epoll + thread pool
 *
 * Modelo: edge-triggered + EPOLLONESHOT
 *   - El kernel notifica UNA sola vez por evento
 *   - El worker que agarra el evento lo procesa hasta EAGAIN
 *   - Luego re-arma el fd con EPOLL_CTL_MOD
 *   - Garantiza que un solo thread trabaja sobre un fd a la vez
 */
#include "io_engine.h"
#include "../protocol/ntrip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/sysinfo.h>

/* ── Helpers internos ────────────────────────────────────────────── */

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_tcp_nodelay(int fd)
{
    int one = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

static int set_reuseaddr(int fd)
{
    int one = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
}

/* ── Cola de trabajo ─────────────────────────────────────────────── */

static void wq_init(io_work_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

static void wq_destroy(io_work_queue_t *q)
{
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->lock);
}

/* Retorna 0 si OK, -1 si cola llena (nunca bloquea) */
static int wq_push(io_work_queue_t *q, const io_work_t *work)
{
    pthread_mutex_lock(&q->lock);
    if (q->count >= IO_WORK_QUEUE_CAP) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    q->items[q->tail] = *work;
    q->tail = (q->tail + 1) % IO_WORK_QUEUE_CAP;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

/* Bloquea hasta que haya trabajo. Retorna 0 si OK, -1 si shutdown. */
static int wq_pop(io_work_queue_t *q, io_work_t *out, volatile sig_atomic_t *running)
{
    pthread_mutex_lock(&q->lock);
    while (q->count == 0) {
        if (!*running) {
            pthread_mutex_unlock(&q->lock);
            return -1;
        }
        pthread_cond_wait(&q->not_empty, &q->lock);
    }
    *out = q->items[q->head];
    q->head = (q->head + 1) % IO_WORK_QUEUE_CAP;
    q->count--;
    pthread_mutex_unlock(&q->lock);
    return 0;
}

/* ── Dispatch de eventos ─────────────────────────────────────────── */

/*
 * flush_write_buf — Envía datos pendientes del write_buf al fd.
 * Retorna 1 si el buffer quedó vacío, 0 si hay más pendiente,
 * -1 si error fatal (conexión cerrada).
 */
static int flush_write_buf(io_engine_t *eng, conn_t *conn)
{
    while (conn_wbuf_pending(conn) > 0) {
        uint32_t head  = conn->wbuf.head & (CONN_WRITE_BUF_SIZE - 1);
        int      n_pend = conn_wbuf_pending(conn);
        /* Cuánto hasta el final del buffer circular */
        int chunk = (head + n_pend <= CONN_WRITE_BUF_SIZE)
                  ? n_pend
                  : (int)(CONN_WRITE_BUF_SIZE - head);

        ssize_t n = write(conn->fd, conn->wbuf.data + head, (size_t)chunk);
        if (n > 0) {
            conn->wbuf.head += (uint32_t)n;
            conn->bytes_tx  += (uint64_t)n;
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;   /* socket lleno, volver cuando EPOLLOUT */
            return -1;      /* error real */
        }
    }
    return 1;   /* buffer vacío */
}

/*
 * clients_wakeup — Despierta a todos los clientes de un mountpoint.
 *
 * Cuando el source escribe al ring, los clientes necesitan consumir los
 * datos nuevos. Con EPOLLONESHOT+EPOLLET, después del primer dispatch
 * los clientes solo están armados para EPOLLIN (GGA del rover).
 *
 * Solución: desde el worker del source, hacer epoll_ctl(MOD) en el fd
 * de cada cliente para agregar EPOLLOUT. Si el fd estaba desarmado
 * (ONESHOT ya disparó), MOD lo reactiva y epoll entrega EPOLLOUT
 * inmediatamente ya que el socket del cliente suele estar listo para
 * escritura. Si el fd está siendo procesado por otro worker (ONESHOT
 * activo), el MOD actualizará el interés para el próximo ciclo.
 *
 * epoll_ctl() es thread-safe — se puede llamar desde cualquier thread.
 *
 * Nota de concurrencia: esta función itera mp->clients_head bajo
 * rdlock. mp_client_unsubscribe() (llamado por broker_conn_free, a su
 * vez llamado por io_engine_conn_close) necesita wrlock para sacar un
 * conn_t de esta misma lista — por lo tanto no puede completarse
 * mientras haya una iteración de clients_wakeup() en curso. Esto es lo
 * que hace seguro el orden "detach antes de close(fd)" en
 * io_engine_conn_close(): mientras clients_wakeup() sostiene el rdlock,
 * ningún conn_t puede desengancharse ni liberarse bajo sus pies.
 */
static void clients_wakeup(io_engine_t *eng, mountpoint_t *mp)
{
    if (!mp) return;

    struct epoll_event ev = {
        .events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLONESHOT | EPOLLRDHUP,
    };

    pthread_rwlock_rdlock(&mp->lock);
    conn_t *c = mp->clients_head;
    while (c) {
        ev.data.ptr = c;
        /* MOD: reactiva el fd del cliente con EPOLLOUT */
        epoll_ctl(eng->epoll_fd, EPOLL_CTL_MOD, c->fd, &ev);
        c = c->next;
    }
    pthread_rwlock_unlock(&mp->lock);
}

/*
 * io_engine_wakeup_mount_clients — Wrapper público de clients_wakeup().
 * Usado por ntrip.c para despertar clientes cuando relay datos que
 * llegaron pegados al handshake del source (pipelining).
 */
void io_engine_wakeup_mount_clients(io_engine_t *engine, struct mountpoint_t *mp)
{
    clients_wakeup(engine, mp);
}

/*
 * dispatch_source — Source tiene datos listos (EPOLLIN).
 * Lee todo lo disponible y lo pasa al broker para relay.
 */
static void dispatch_source(io_engine_t *eng, conn_t *conn)
{
    uint8_t buf[65536];
    int     got_data = 0;

    for (;;) {
        ssize_t n = read(conn->fd, buf, sizeof(buf));
        if (n > 0) {
            broker_source_data(eng->broker, conn, buf, (size_t)n);
            got_data = 1;
        } else if (n == 0) {
            /* Source cerró la conexión */
            io_engine_conn_close(eng, conn);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;  /* edge-triggered: procesamos todo */
            io_engine_conn_close(eng, conn);
            return;
        }
    }

    /* Despertar clientes si llegaron datos nuevos */
    if (got_data)
        clients_wakeup(eng, conn->mp);

    /* Re-armar epoll para próximo EPOLLIN */
    io_engine_conn_watch(eng, conn, EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP);
}

/*
 * dispatch_client — Cliente tiene datos nuevos disponibles o socket listo.
 *
 * Para clientes: no esperamos datos (el rover manda GGA solo en NEAREST).
 * El trabajo es: leer del ring → llenar write_buf → flush al fd.
 */
static void dispatch_client(io_engine_t *eng, conn_t *conn, uint32_t events)
{
    /* Leer GGA si viene del cliente (NEAREST o keepalive) */
    if (events & EPOLLIN) {
        uint8_t tmp[256];
        ssize_t n;
        while ((n = read(conn->fd, tmp, sizeof(tmp))) > 0) {
            /* TODO Fase NEAREST: parsear NMEA GGA aquí */
            (void)n;
        }
        if (n == 0) {
            io_engine_conn_close(eng, conn);
            return;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            /* Error real de lectura, no solo "sin datos" */
            io_engine_conn_close(eng, conn);
            return;
        }
    }

    /*
     * Loop fill→flush: drenar el ring hacia el socket hasta que no haya
     * más datos o el socket se llene. Antes se llenaba el write_buf UNA
     * vez por dispatch — si el ring tenía más que el espacio libre, el
     * resto esperaba al próximo wakeup del source (latencia gratis).
     */
    for (;;) {
        ssize_t filled = broker_client_fill(eng->broker, conn);
        if (filled < 0) {
            /* Lag excesivo — cliente demasiado lento */
            fprintf(stderr, "[io] client fd=%d lag too high, closing\n", conn->fd);
            io_engine_conn_close(eng, conn);
            return;
        }

        int done = flush_write_buf(eng, conn);
        if (done < 0) {
            io_engine_conn_close(eng, conn);
            return;
        }
        if (done == 0) break;   /* socket lleno — esperar EPOLLOUT */
        if (filled == 0) break; /* ring al día y buffer vacío */
    }

    /* Re-armar: siempre EPOLLIN (GGA), EPOLLOUT solo si hay pendiente */
    uint32_t ev = EPOLLIN | EPOLLET | EPOLLONESHOT;
    if (conn_wbuf_pending(conn) > 0)
        ev |= EPOLLOUT;

    io_engine_conn_watch(eng, conn, ev);
}

/*
 * dispatch_handshake — Acumula el request HTTP/NTRIP en conn->read_buf.
 * Llama a ntrip_handle_request() cuando detecta el fin de headers (\r\n\r\n).
 *
 * Casos especiales:
 *   - NTRIP v1 SOURCE command termina con \r\n\r\n también (tiene headers)
 *   - Buffer lleno sin \r\n\r\n → cerrar (request malformado)
 */
static void dispatch_handshake(io_engine_t *eng, conn_t *conn)
{
    /* Leer más datos */
    ssize_t n = read(conn->fd,
                     conn->read_buf + conn->read_len,
                     CONN_READ_BUF_SIZE - conn->read_len - 1);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* Edge-triggered: sin datos por ahora, re-armar */
            io_engine_conn_watch(eng, conn,
                EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP);
        } else {
            io_engine_conn_close(eng, conn);
        }
        return;
    }

    conn->read_len += (uint32_t)n;
    conn->read_buf[conn->read_len] = '\0';

    /* Detectar fin de headers: \r\n\r\n */
    if (memmem(conn->read_buf, conn->read_len, "\r\n\r\n", 4) != NULL) {
        /* Header completo — entregar al protocol handler */
        ntrip_handle_request(eng, conn);
        return;
    }

    /* Buffer lleno y aún no hay \r\n\r\n → request inválido */
    if (conn->read_len >= CONN_READ_BUF_SIZE - 1) {
        fprintf(stderr, "[io] handshake overflow fd=%d, closing\n", conn->fd);
        io_engine_conn_close(eng, conn);
        return;
    }

    /* Aún incompleto — esperar más datos */
    io_engine_conn_watch(eng, conn,
        EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP);
}

/*
 * dispatch — Función llamada por cada worker thread para un fd + eventos.
 */
static void dispatch(io_engine_t *eng, conn_t *conn, uint32_t events)
{
    /* Error real: nunca hay datos fiables que leer, cerrar siempre */
    if (events & EPOLLERR) {
        io_engine_conn_close(eng, conn);
        return;
    }

    /*
     * EPOLLHUP/EPOLLRDHUP: el peer cerró (o medio-cerró) su lado.
     * Puede llegar junto con EPOLLIN si todavía quedan datos sin leer
     * en el socket buffer — típico cuando el peer escribe y cierra
     * rápido (p.ej. un SOURCE que manda un burst y desconecta, o un
     * cliente que manda su último GGA y cierra). Si hay EPOLLIN,
     * dejamos que el handler correspondiente lea y procese esos datos
     * primero; él mismo cierra la conexión cuando read() devuelve 0.
     * Solo cerramos aquí de una si NO hay nada pendiente por leer.
     */
    if ((events & (EPOLLHUP | EPOLLRDHUP)) && !(events & EPOLLIN)) {
        io_engine_conn_close(eng, conn);
        return;
    }

    switch (conn->state) {
    case CONN_STATE_NEW:
    case CONN_STATE_HANDSHAKE:
        dispatch_handshake(eng, conn);
        break;

    case CONN_STATE_SOURCE_ACTIVE:
        dispatch_source(eng, conn);
        break;

    case CONN_STATE_CLIENT_ACTIVE:
        dispatch_client(eng, conn, events);
        break;

    case CONN_STATE_CLOSING:
        flush_write_buf(eng, conn);
        io_engine_conn_close(eng, conn);
        break;

    default:
        io_engine_conn_close(eng, conn);
        break;
    }
}

/* ── Timeouts ────────────────────────────────────────────────────── */

/*
 * sweep_idle_conns — Recorre la lista global de conexiones y hace
 * shutdown() de las inactivas. NO cierra ni libera nada: el shutdown
 * dispara EPOLLIN/EPOLLRDHUP y el worker correspondiente hace el
 * close/free por el camino normal (io_engine_conn_close), manteniendo
 * el orden DEL→detach→close ya documentado ahí.
 *
 * Corre en el accept thread (main), cada ~5 segundos.
 */
static void sweep_idle_conns(io_engine_t *eng, time_t now)
{
    broker_t *b = eng->broker;

    pthread_mutex_lock(&b->conns_lock);
    for (conn_t *c = b->conns_head; c; c = c->gnext) {
        int idle, limit;

        switch (c->state) {
        case CONN_STATE_NEW:
        case CONN_STATE_HANDSHAKE:
            idle  = (int)(now - c->connected_at);
            limit = b->config.handshake_timeout_s;
            break;
        case CONN_STATE_SOURCE_ACTIVE:
            idle  = (int)(now - c->last_active);
            limit = b->config.source_timeout_s;
            break;
        case CONN_STATE_CLIENT_ACTIVE:
            idle  = (int)(now - c->last_active);
            limit = b->config.client_timeout_s;
            break;
        default:
            continue;
        }

        if (limit > 0 && idle > limit) {
            fprintf(stderr, "[io] timeout %s fd=%d idle=%ds (limit %ds)\n",
                    conn_state_name(c->state), c->fd, idle, limit);
            shutdown(c->fd, SHUT_RDWR);
        }
    }
    pthread_mutex_unlock(&b->conns_lock);
}

/* ── Worker thread ───────────────────────────────────────────────── */

static void *worker_thread(void *arg)
{
    io_engine_t *eng = (io_engine_t *)arg;
    io_work_t    work;

    while (wq_pop(&eng->queue, &work, &eng->running) == 0) {
        dispatch(eng, work.conn, work.events);
    }
    return NULL;
}

/* ── Accept loop ─────────────────────────────────────────────────── */

static void *accept_loop(void *arg)
{
    io_engine_t       *eng = (io_engine_t *)arg;
    struct sockaddr_in addr;
    struct epoll_event evs[IO_MAX_EVENTS];
    time_t             last_sweep = time(NULL);

    while (eng->running) {
        int n = epoll_wait(eng->epoll_fd, evs, IO_MAX_EVENTS, 200 /* ms */);

        /* Callback de tick (reload de auth por SIGHUP, etc.) */
        if (eng->tick_cb)
            eng->tick_cb(eng->tick_ctx);

        /* Sweep de timeouts cada 5s */
        time_t now = time(NULL);
        if (now - last_sweep >= 5) {
            sweep_idle_conns(eng, now);
            last_sweep = now;
        }

        for (int i = 0; i < n; i++) {
            conn_t  *conn   = (conn_t *)evs[i].data.ptr;
            uint32_t events = evs[i].events;

            /* fd de escucha: nueva conexión entrante */
            if (conn == NULL) {
                /* addrlen es value-result: accept4 lo modifica en cada
                 * llamada — resetear antes de CADA accept */
                socklen_t addrlen = sizeof(addr);
                int cfd = accept4(eng->listen_fd,
                                  (struct sockaddr *)&addr, &addrlen,
                                  SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (cfd < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK)
                        perror("accept4");
                    continue;
                }

                set_tcp_nodelay(cfd);

                char addrstr[48];
                snprintf(addrstr, sizeof(addrstr), "%s:%d",
                         inet_ntoa(addr.sin_addr),
                         ntohs(addr.sin_port));

                conn_t *c = broker_conn_alloc(eng->broker, cfd, addrstr);
                if (!c) {
                    close(cfd);
                    continue;
                }
                c->state = CONN_STATE_HANDSHAKE;

                if (io_engine_conn_watch(eng, c,
                        EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP) != 0) {
                    broker_conn_free(eng->broker, c);
                    close(cfd);
                }
                continue;
            }

            /* fd existente: encolar para el pool */
            io_work_t work = { .fd = conn->fd, .events = events, .conn = conn };
            if (wq_push(&eng->queue, &work) != 0) {
                /*
                 * Cola llena: con EPOLLONESHOT el fd quedó desarmado — si
                 * solo logueamos, la conexión se congela en silencio hasta
                 * que el peer la abandone (diagnóstico de
                 * FEATURE_relay_capacity_reload §1.1). Cerrarla activamente:
                 * falla ruidosa y honesta (§1.2 #2).
                 */
                fprintf(stderr, "[io] work queue full, closing fd=%d\n",
                        conn->fd);
                io_engine_conn_close(eng, conn);
            }
        }
    }
    return NULL;
}

/* ── API pública ─────────────────────────────────────────────────── */

int io_engine_conn_watch(io_engine_t *engine, conn_t *conn, uint32_t events)
{
    struct epoll_event ev = {
        .events   = events,
        .data.ptr = conn,
    };
    /* Intenta MOD primero, si falla con ENOENT es ADD */
    if (epoll_ctl(engine->epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev) != 0) {
        if (errno == ENOENT)
            return epoll_ctl(engine->epoll_fd, EPOLL_CTL_ADD, conn->fd, &ev);
        return -1;
    }
    return 0;
}

void io_engine_conn_close(io_engine_t *engine, conn_t *conn)
{
    int fd = conn->fd;

    /*
     * Orden importante -- NO cambiar sin releer este comentario:
     *
     *   1. EPOLL_CTL_DEL primero: nadie vuelve a recibir eventos de
     *      este fd por epoll_wait a partir de acá.
     *   2. broker_conn_free() ANTES de close(fd): si conn es un
     *      cliente, esto llama mp_client_unsubscribe() bajo wrlock de
     *      mp->lock, sacándolo de mp->clients_head. Esto serializa
     *      correctamente contra clients_wakeup() (que itera esa misma
     *      lista bajo rdlock) -- ver el comentario en clients_wakeup().
     *   3. close(fd) al final.
     *
     * Si close(fd) fuera ANTES del detach (como estaba originalmente),
     * hay una ventana sin ningún lock donde el kernel puede reciclar
     * ese número de fd para una conexión nueva aceptada en paralelo por
     * accept_loop, mientras este conn_t (con ese mismo fd guardado)
     * TODAVÍA está enganchado en mp->clients_head. Si clients_wakeup()
     * corre justo en esa ventana, haría epoll_ctl(MOD, fd_reciclado,
     * ...) con ev.data.ptr apuntando a este conn_t viejo -- pisando el
     * epoll de la conexión nueva y dejando un puntero colgante que se
     * usa en el próximo evento de ese fd. Con el orden actual esa
     * ventana no existe: para cuando el fd se libera, este conn_t ya
     * es inalcanzable desde cualquier lista del broker.
     */
    epoll_ctl(engine->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    broker_conn_free(engine->broker, conn);
    close(fd);
}

int io_engine_init(io_engine_t *engine, broker_t *broker,
                   const char *bind_addr, int port, int num_threads)
{
    memset(engine, 0, sizeof(*engine));
    engine->broker = broker;
    engine->port   = port;
    strncpy(engine->bind_addr, bind_addr ? bind_addr : "0.0.0.0",
            sizeof(engine->bind_addr) - 1);

    /* Número de threads */
    if (num_threads <= 0)
        num_threads = get_nprocs();
    if (num_threads < 1)  num_threads = 1;
    if (num_threads > 64) num_threads = 64;
    engine->num_threads = num_threads;

    /* epoll */
    engine->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (engine->epoll_fd < 0) { perror("epoll_create1"); return -1; }

    /* Listen socket */
    engine->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (engine->listen_fd < 0) { perror("socket"); return -1; }

    set_reuseaddr(engine->listen_fd);

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind_addr && strcmp(bind_addr, "0.0.0.0") != 0)
        inet_aton(bind_addr, &addr.sin_addr);

    if (bind(engine->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind"); return -1;
    }
    if (listen(engine->listen_fd, 128) != 0) {
        perror("listen"); return -1;
    }

    /* Registrar listen_fd en epoll con conn=NULL (señal de nueva conexión) */
    struct epoll_event ev = {
        .events   = EPOLLIN | EPOLLET,
        .data.ptr = NULL,   /* NULL = accept */
    };
    epoll_ctl(engine->epoll_fd, EPOLL_CTL_ADD, engine->listen_fd, &ev);

    /* Thread pool */
    wq_init(&engine->queue);
    engine->threads = calloc((size_t)num_threads, sizeof(pthread_t));
    if (!engine->threads) return -1;

    engine->running = 1;

    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&engine->threads[i], NULL, worker_thread, engine)) {
            perror("pthread_create");
            return -1;
        }
    }

    printf("[io] listening on %s:%d  threads=%d\n",
           engine->bind_addr, port, num_threads);
    return 0;
}

void io_engine_run(io_engine_t *engine)
{
    /* El accept loop corre en el thread actual (main), bloquea hasta
     * que engine->running pase a 0 (io_engine_stop, típicamente desde
     * el signal handler de SIGINT/SIGTERM). */
    accept_loop(engine);

    /*
     * accept_loop ya retornó -- estamos en contexto normal (NO señal).
     * Recién acá es seguro tomar el mutex de la cola para despertar a
     * los workers que puedan estar bloqueados en pthread_cond_wait
     * dentro de wq_pop(). io_engine_stop() no puede hacer esto directo
     * porque se llama desde un signal handler (ver su comentario).
     */
    pthread_mutex_lock(&engine->queue.lock);
    pthread_cond_broadcast(&engine->queue.not_empty);
    pthread_mutex_unlock(&engine->queue.lock);
}

void io_engine_stop(io_engine_t *engine)
{
    /*
     * Async-signal-safe a propósito: esta función se llama directo
     * desde el signal handler de SIGINT/SIGTERM en main.c. Escribir un
     * volatile sig_atomic_t es la única operación que el estándar C
     * garantiza segura dentro de un handler.
     *
     * ANTES esta función también hacía pthread_mutex_lock +
     * pthread_cond_broadcast acá mismo. Eso es incorrecto dentro de un
     * signal handler: si la señal llega mientras el thread principal
     * está en medio de wq_push() sosteniendo queue.lock, este mismo
     * thread intentaría re-tomar ese mutex desde el handler -->
     * self-deadlock (Ctrl+C deja de funcionar, hay que matar el
     * proceso con kill -9). El broadcast real ahora vive en
     * io_engine_run(), después de que accept_loop() retorna.
     */
    engine->running = 0;
}

void io_engine_destroy(io_engine_t *engine)
{
    io_engine_stop(engine);

    for (int i = 0; i < engine->num_threads; i++)
        pthread_join(engine->threads[i], NULL);

    free(engine->threads);
    wq_destroy(&engine->queue);

    if (engine->epoll_fd >= 0)  close(engine->epoll_fd);
    if (engine->listen_fd >= 0) close(engine->listen_fd);

    printf("[io] engine destroyed\n");
}
