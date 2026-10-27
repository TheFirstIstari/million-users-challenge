#include "net.h"
#include "http.h"
#include "pg.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define HDR_CAP 16384
#define BODY_CAP 16384
#define SLAB_INIT 4096
#define MAX_REQS 1000
#define IDLE_S 80
#define HDR_TIMEOUT_S 20
#define MAX_CONNS_PER_WORKER 64000
static _Thread_local int t_nconns = 0;

long current_uptime_s(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long d = (long)(now.tv_sec - g_start_mono.tv_sec);
    return d < 0 ? 0 : d;
}

static int set_nb(int fd) {
    int f = fcntl(fd, F_GETFL, 0);
    if (f < 0) return -1;
    return fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

static int make_listener(const char *host, int port) {
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    /* Dual-stack: try IPv6 any then bind host. Simplest: resolve host. */
    struct addrinfo hints, *res = NULL, *rp = NULL;
    char ps[16]; snprintf(ps, sizeof ps, "%d", port);
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ps, &hints, &res) != 0) { close(fd); return -1; }
    int bound = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        int s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s < 0) continue;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
        setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (rp->ai_family == AF_INET6) {
            int off = 0;
            setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
        }
        if (bind(s, rp->ai_addr, rp->ai_addrlen) == 0) { bound = s; break; }
        close(s);
    }
    freeaddrinfo(res);
    close(fd); /* template closed; use bound */
    if (bound < 0) return -1;
#ifdef TCP_DEFER_ACCEPT
    /* Wake the listener only once peer data arrives (1s cap): fewer accept
     * wakeups from half-open / speculative connections. */
    { int dval = 1; setsockopt(bound, IPPROTO_TCP, TCP_DEFER_ACCEPT, &dval, sizeof dval); }
#endif
    if (listen(bound, 65535) != 0) { close(bound); return -1; }
    set_nb(bound);
    return bound;
}

static void conn_free(struct conn_ctx *c, int epfd) {
    if (!c) return;
    t_nconns--;
    if (epfd >= 0) epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    free(c->rbuf);
    free(c);
}

/* ---- Pass 3: io_uring multishot accept with epoll fallback ---- */
#define URING_ENTRIES 2048
#define ACCEPTS_PER_LISTENER 2
#define TAG_L1 ((void *)(intptr_t)-1)
#define TAG_L2 ((void *)(intptr_t)-2)
#define TAG_URING ((void *)(intptr_t)-3)

static void conn_register(int epfd, struct conn_ctx **head, int afd, time_t now) {
    int one = 1;
    setsockopt(afd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (t_nconns >= MAX_CONNS_PER_WORKER) { close(afd); return; }
    struct conn_ctx *c = calloc(1, sizeof *c);
    if (!c) { close(afd); return; }
    c->fd = afd; c->last_active = now; c->created_at = now; c->keep_alive = 1;
    t_nconns++;
    c->rbuf = malloc(SLAB_INIT);
    if (!c->rbuf) { close(afd); free(c); t_nconns--; return; }
    c->rcap = SLAB_INIT;
    c->next = *head; *head = c;
    struct epoll_event cev;
    cev.events = EPOLLIN;
    cev.data.ptr = c;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, afd, &cev) != 0) {
        *head = c->next;
        conn_free(c, -1);
    }
}

/* Classic accept4 drain (fallback path). */
static void epoll_accept_all(int epfd, struct conn_ctx **head, int lfd, time_t now) {
    while (1) {
        int afd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (afd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EMFILE || errno == ENFILE) { usleep(10*1000); break; }
            break;
        }
        conn_register(epfd, head, afd, now);
    }
}

/* Submit one multishot accept for lfd. 0 ok, -1 ring full. */
static int uring_accept_submit(struct io_uring *ring, int lfd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        if (io_uring_submit(ring) < 0) return -1;
        sqe = io_uring_get_sqe(ring);
        if (!sqe) return -1;
    }
    io_uring_prep_multishot_accept(sqe, lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    io_uring_sqe_set_data64(sqe, (unsigned long long)(unsigned int)lfd);
    return 0;
}

/* Probe multishot-accept support on a throwaway loopback listener so real
 * traffic is never disturbed. Returns 1 if a multishot accept arms. */
static int uring_mshot_probe(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    int supported = 0;
    struct io_uring pring;
    struct io_uring_params pp;
    memset(&pp, 0, sizeof pp);
    pp.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN;
    if (bind(s, (struct sockaddr *)&a, sizeof a) == 0 && listen(s, 8) == 0 &&
        io_uring_queue_init_params(8, &pring, &pp) == 0) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&pring);
        if (sqe) {
            io_uring_prep_multishot_accept(sqe, s, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            io_uring_sqe_set_data64(sqe, 0);
            if (io_uring_submit(&pring) == 1) {
                struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };
                struct io_uring_cqe *cqe = NULL;
                int rc = io_uring_wait_cqe_timeout(&pring, &cqe, &ts);
                if (rc != 0) {
                    supported = 1; /* still pending: multishot armed */
                } else if (cqe) {
                    if (cqe->res >= 0) { close(cqe->res); supported = 1; }
                    else if (cqe->res != -EINVAL) supported = 1;
                    io_uring_cqe_seen(&pring, cqe);
                }
            }
        }
        io_uring_queue_exit(&pring);
    }
    close(s);
    return supported;
}

/* Drain ready accept completions; registers new conns. *einval counts
 * consecutive -EINVAL verdicts (kernel without multishot support). */
static void uring_drain(struct io_uring *ring, int epfd, struct conn_ctx **head,
                        time_t now, int *einval) {
    struct io_uring_cqe *cqe;
    int resub = 0;
    while (io_uring_peek_cqe(ring, &cqe) == 0) {
        int lfd = (int)io_uring_cqe_get_data64(cqe);
        int res = cqe->res;
        unsigned flags = cqe->flags;
        io_uring_cqe_seen(ring, cqe);
        if (res >= 0) {
            conn_register(epfd, head, res, now);
            *einval = 0;
        } else if (res == -EMFILE || res == -ENFILE) {
            usleep(10 * 1000);
        } else if (res == -EINVAL) {
            (*einval)++;
        }
        if (!(flags & IORING_CQE_F_MORE)) {
            if (uring_accept_submit(ring, lfd) == 0) resub = 1;
        }
    }
    if (resub) io_uring_submit(ring);
}

/* Returns 1 keep-alive, 0 close. Handles pipelined requests in a loop. */
static int handle_data(struct conn_ctx *c) {
    /* Grow slab if needed. */
    while (1) {
        if (c->rlen == c->rcap) {
            unsigned int ncap = c->rcap ? c->rcap * 2 : SLAB_INIT;
            if (ncap > HDR_CAP + BODY_CAP) ncap = HDR_CAP + BODY_CAP;
            if (ncap <= c->rcap) return 0; /* cap hit -> close */
            char *nb = realloc(c->rbuf, ncap);
            if (!nb) return 0;
            c->rbuf = nb; c->rcap = ncap;
        }
        ssize_t r = recv(c->fd, c->rbuf + c->rlen, c->rcap - c->rlen, 0);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return 0;
        }
        if (r == 0) return 0; /* peer closed */
        c->rlen += (unsigned int)r;
        if (c->rlen > HDR_CAP + BODY_CAP) return 0;
        break;
    }
    /* Pipelining loop: parse + respond while a full request is buffered. */
    while (c->rlen > 0) {
        http_req_t rq;
        int n = http_parse(c->rbuf, c->rlen, &rq);
        if (n == 0) {
            if (c->rlen > HDR_CAP) return 0; /* header cap 16K */
            break; /* need more */
        }
        if (n == -2) return 0; /* body too large: close (out of scope) */
        if (n < 0) return 0;   /* malformed: close */
        c->req_count++;
        long up = current_uptime_s();
        int ka = http_respond(c, &rq, up);
        /* Shift consumed bytes. */
        size_t consumed = (size_t)n;
        if (consumed < c->rlen) memmove(c->rbuf, c->rbuf + consumed, c->rlen - consumed);
        c->rlen -= (unsigned int)consumed;
        c->last_active = time(NULL);
        if (!ka) return 0;
        if (c->req_count >= MAX_REQS) return 0; /* close after 1000 reqs */
        /* loop for pipelined next request */
    }
    return 1;
}

void *worker_main(void *arg) {
    long idx = (long)arg;
    (void)idx;

    int l1 = make_listener(g_host, g_port);
    int l2 = make_listener(g_host, g_extra_port);
    if (l1 < 0 || l2 < 0) {
        fprintf(stderr, "worker %ld: bind %s:%d/%d failed: %s\n", idx, g_host, g_port, g_extra_port, strerror(errno));
        if (l1 >= 0) close(l1);
        if (l2 >= 0) close(l2);
        exit(1);
    }

    pg_init_thread(g_db_url[0] ? g_db_url : NULL);

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { perror("epoll_create1"); exit(1); }
    /* Pass 3: io_uring multishot accept (2 per listener, depth 2048,
     * SINGLE_ISSUER|COOP_TASKRUN). The ring fd is pollable, so one epoll_wait
     * still drives everything; classic accept4 is the fallback. */
    struct io_uring ring;
    int have_uring = 0;
    memset(&ring, 0, sizeof ring);
    if (uring_mshot_probe()) {
        struct io_uring_params p;
        memset(&p, 0, sizeof p);
        p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN;
        p.sq_entries = URING_ENTRIES; p.cq_entries = URING_ENTRIES * 2;
        if (io_uring_queue_init_params(URING_ENTRIES, &ring, &p) == 0) {
            int ok = 1;
            for (int k = 0; k < ACCEPTS_PER_LISTENER; k++) {
                if (uring_accept_submit(&ring, l1) != 0) ok = 0;
                if (uring_accept_submit(&ring, l2) != 0) ok = 0;
            }
            if (ok && io_uring_submit(&ring) == 2 * ACCEPTS_PER_LISTENER) {
                struct epoll_event uev;
                uev.events = EPOLLIN;
                uev.data.ptr = TAG_URING;
                if (epoll_ctl(epfd, EPOLL_CTL_ADD, ring.ring_fd, &uev) == 0)
                    have_uring = 1;
            }
            if (!have_uring) io_uring_queue_exit(&ring);
        }
    }
    int uring_einval = 0;
    if (have_uring) {
        fprintf(stderr, "worker %ld: io_uring multishot accept active\n", idx);
    } else {
        /* listeners use distinct ptr markers (-1/-2). */
        struct epoll_event lev;
        lev.events = EPOLLIN;
        lev.data.ptr = TAG_L1; epoll_ctl(epfd, EPOLL_CTL_ADD, l1, &lev);
        lev.data.ptr = TAG_L2; epoll_ctl(epfd, EPOLL_CTL_ADD, l2, &lev);
    }

    struct conn_ctx *head = NULL;
    struct epoll_event events[256];
    time_t last_sweep = time(NULL);

    while (1) {
        int n = epoll_wait(epfd, events, 256, 1000); /* 1s idle sweep tick */
        time_t now = time(NULL);
        if (now - last_sweep >= 1) {
            last_sweep = now;
            struct conn_ctx **pp = &head;
            while (*pp) {
                struct conn_ctx *c = *pp;
                if (now - c->last_active > IDLE_S ||
                    (c->req_count == 0 && c->rlen > 0 && now - c->created_at > HDR_TIMEOUT_S)) {
                    *pp = c->next;
                    conn_free(c, epfd);
                } else pp = &c->next;
            }
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            void *ptr = events[i].data.ptr;
            if (ptr == TAG_URING) {
                if (have_uring) {
                    uring_drain(&ring, epfd, &head, now, &uring_einval);
                    if (uring_einval >= 4) {
                        /* Kernel rejected multishot at runtime: drop the ring,
                         * fall back to epoll accept4. */
                        epoll_ctl(epfd, EPOLL_CTL_DEL, ring.ring_fd, NULL);
                        io_uring_queue_exit(&ring);
                        have_uring = 0;
                        uring_einval = 0;
                        struct epoll_event lev;
                        lev.events = EPOLLIN;
                        lev.data.ptr = TAG_L1; epoll_ctl(epfd, EPOLL_CTL_ADD, l1, &lev);
                        lev.data.ptr = TAG_L2; epoll_ctl(epfd, EPOLL_CTL_ADD, l2, &lev);
                        fprintf(stderr, "worker %ld: io_uring disabled, epoll accept fallback\n", idx);
                    }
                }
            } else if (ptr == TAG_L1 || ptr == TAG_L2) {
                if (!have_uring) {
                    int lfd = (ptr == TAG_L1) ? l1 : l2;
                    epoll_accept_all(epfd, &head, lfd, now);
                }
            } else {
                struct conn_ctx *c = (struct conn_ctx *)ptr;
                int alive = handle_data(c);
                if (!alive) {
                    /* unlink */
                    struct conn_ctx **pp = &head;
                    while (*pp && *pp != c) pp = &(*pp)->next;
                    if (*pp) *pp = c->next;
                    conn_free(c, epfd);
                }
            }
        }
    }
    return NULL;
}
