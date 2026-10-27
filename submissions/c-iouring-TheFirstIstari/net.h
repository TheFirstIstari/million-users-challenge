#ifndef NET_H
#define NET_H
#include <time.h>
#include <stddef.h>
#include <stdalign.h>

/* Pass 6: PG_WAIT conn state. While a request waits for its async PG query,
 * the worker keeps the client fd open, arms the PQsocket in the same epoll,
 * and resumes on POLLIN|POLLOUT. One in-flight per PG conn is preserved. */
enum { CONN_ACTIVE = 0, CONN_PG_WAIT = 1 };
enum {
    PG_ROUTE_NONE = 0,
    PG_ROUTE_HEALTH = 1,
    PG_ROUTE_FEED = 2,
    PG_ROUTE_POST = 3,
    PG_ROUTE_MKPOST = 4,
    PG_ROUTE_LIKE = 5
};

struct pg_pending {
    int route;                 /* one of PG_ROUTE_* */
    int slot;                  /* claimed PG slot */
    int sent;                  /* 0 = not yet sent (BUSY), 1 = awaiting result */
    int npar;
    char *p0;                  /* malloc'd param copies (or NULL) */
    char *p1;
    char *author;              /* mkpost render only */
    size_t body_len;           /* mkpost trimmed body length */
    unsigned long long idv;    /* like render */
    int ver_major, ver_minor;
    char *conn_hdr;            /* malloc'd Connection value or NULL */
    size_t conn_hdr_len;
    long uptime_s;
    int accept_gzip;           /* client Accept-Encoding: gzip (preserved across PG_WAIT) */
};

/* Pass 10: hot/cold conn split. The per-request hot path touches ONLY
 * struct conn_hot (one 64B cache line). Everything rare -- creation time,
 * EAGAIN stash, PG_WAIT stash -- lives in struct conn_cold, allocated
 * adjacently in the same malloc block (hot at offset 0, cold at +64) so
 * cold access is one predictable pointer chase, never a hot-line polluter.
 * The 1s wheel tick touches cold only for the created_at check (expiry
 * visits, not per-req). */
struct conn_cold {
    time_t created_at;         /* cold: header-timeout + accounting only */
    char *wbuf;                /* cold: stashed unsent response bytes (EAGAIN) */
    size_t wlen;               /* cold: total stashed */
    size_t woff;               /* cold: already sent */
    struct pg_pending pend;    /* cold: valid only when state == CONN_PG_WAIT */
};

struct __attribute__((aligned(64))) conn_hot {
    int fd;                    /* hot: client socket */
    unsigned int req_count;    /* hot: requests served on this conn */
    time_t last_active;        /* hot: 1s-sweep idle timeout */
    char *rbuf;                /* hot: read slab (pooled 2K..32K, lazy) */
    unsigned int rlen;         /* hot: buffered bytes */
    unsigned int rcap;         /* hot: slab class size */
    struct conn_hot *next;     /* hot: timing-wheel slot link (intrusive, Pass 11) */
    int keep_alive;            /* hot: response verdict cache */
    int state;                 /* hot: CONN_ACTIVE or CONN_PG_WAIT */
    int wheel_slot;            /* hot: wheel slot index (-1 = unlinked) */
    struct conn_cold *cold;    /* hot: one chase to rare state (cold-only) */
};
_Static_assert(sizeof(struct conn_hot) <= 64, "conn hot must be <=64B");
_Static_assert(alignof(struct conn_hot) == 64, "conn hot must be 64B-aligned");

void *worker_main(void *arg);

/* Re-arm a conn for EPOLLOUT (unsent response pending). 0 ok, -1 on error. */
int net_arm_out(struct conn_hot *c);
/* 1s-tick-cached wall clock for this worker (no time() per request). */
time_t net_now_wall(void);

extern char g_host[256];
extern int g_port;
extern int g_extra_port;
extern char g_db_url[2048];
extern struct timespec g_start_mono;
long current_uptime_s(void);

#endif
