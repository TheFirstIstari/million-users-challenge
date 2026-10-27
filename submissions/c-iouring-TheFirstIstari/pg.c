#include "pg.h"
#include "json.h"
#include <libpq-fe.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define POOL_N 32

static _Thread_local PGconn *t_pool[POOL_N];
static _Thread_local unsigned t_rr = 0;
static _Thread_local char t_conninfo[2048];

/* Pass 3 acquire-wait histogram (log only; query path behavior unchanged). */
static _Thread_local unsigned long t_qops = 0;
static _Thread_local unsigned long t_qslow = 0;
static _Thread_local unsigned long t_recon = 0;
static _Thread_local time_t t_last_rep = 0;

static void pg_report_maybe(void) {
    if ((t_qops & 0x3FFu) != 0) return;
    time_t now = time(NULL);
    if (now - t_last_rep < 60) return;
    t_last_rep = now;
    fprintf(stderr, "pg stats: ops=%lu slow_gt50ms=%lu reconnects=%lu\n",
            t_qops, t_qslow, t_recon);
}

static const char *SQL_HEALTH =
    "SELECT 1";
static const char *SQL_FEED =
    "SELECT p.id,p.body,p.created_at,u.username,"
    "(SELECT count(*) FROM likes l WHERE l.post_id=p.id)::int AS like_count "
    "FROM posts p JOIN users u ON u.id=p.user_id "
    "ORDER BY p.created_at DESC,p.id DESC LIMIT 20";
static const char *SQL_POST =
    "SELECT p.id,p.body,p.created_at,u.username,"
    "(SELECT count(*) FROM likes l WHERE l.post_id=p.id)::int AS like_count "
    "FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=$1";
static const char *SQL_MKPOST =
    "INSERT INTO posts(user_id,body) VALUES($1,$2) RETURNING id,created_at";
static const char *SQL_LIKE =
    "INSERT INTO likes(user_id,post_id) VALUES($1,$2) "
    "ON CONFLICT(user_id,post_id) DO NOTHING";

/* Thread-local render buffers (feed worst case ~20 x 3.2KB). */
static _Thread_local char t_feed[131072];
static _Thread_local char t_post[8192];

static int wait_conn(PGconn *c, int timeout_ms) {
    int waited = 0;
    while (1) {
        PostgresPollingStatusType st = PQconnectPoll(c);
        if (st == PGRES_POLLING_OK) return 0;
        if (st == PGRES_POLLING_FAILED) return -1;
        int sock = PQsocket(c);
        if (sock < 0) {
            struct timespec ts = { 0, 10 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            waited += 10;
        } else {
            short ev = (st == PGRES_POLLING_READING) ? POLLIN : POLLOUT;
            struct pollfd pfd;
            pfd.fd = sock; pfd.events = ev; pfd.revents = 0;
            if (poll(&pfd, 1, 100) < 0) return -1;
            waited += 100;
        }
        if (waited > timeout_ms) return -1;
    }
}

static void pg_prepare_all(PGconn *c) {
    struct { const char *name; const char *sql; int n; } stmts[] = {
        { "q_health", SQL_HEALTH, 0 },
        { "q_feed", SQL_FEED, 0 },
        { "q_post", SQL_POST, 1 },
        { "q_mkpost", SQL_MKPOST, 2 },
        { "q_like", SQL_LIKE, 2 },
    };
    for (size_t i = 0; i < sizeof stmts / sizeof stmts[0]; i++) {
        PGresult *r = PQprepare(c, stmts[i].name, stmts[i].sql, stmts[i].n, NULL);
        if (r) PQclear(r);
    }
}

static PGconn *pg_connect_finish(PGconn *c) {
    /* Drive an async-started connection to completion, SET timeout, prepare.
     * Returns c on success, NULL on failure (already finished). */
    if (!c) return NULL;
    if (PQstatus(c) == CONNECTION_BAD) { PQfinish(c); return NULL; }
    if (PQsetnonblocking(c, 1) != 0) { PQfinish(c); return NULL; }
    if (wait_conn(c, 5000) != 0) { PQfinish(c); return NULL; }
    if (PQstatus(c) != CONNECTION_OK) { PQfinish(c); return NULL; }
    {
        PGresult *r = PQexec(c, "SET statement_timeout=5000");
        if (r) PQclear(r);
    }
    pg_prepare_all(c);
    return c;
}

static PGconn *pg_connect_one(void) {
    if (t_conninfo[0] == '\0') return NULL;
    /* 1) URI-aware async start: URI + extra keywords in one conninfo string. */
    {
        char ci[2560];
        snprintf(ci, sizeof ci,
            "%s connect_timeout=5 options='-c statement_timeout=5000' application_name=c-iouring",
            t_conninfo);
        PGconn *c = pg_connect_finish(PQconnectStart(ci));
        if (c) return c;
    }
    /* 2) Plain blocking connectdb fallback (handles keyword edge cases). */
    {
        PGconn *c = PQconnectdb(t_conninfo);
        if (c && PQstatus(c) == CONNECTION_OK) {
            PGresult *r = PQexec(c, "SET statement_timeout=5000;SET application_name='c-iouring'");
            if (r) PQclear(r);
            pg_prepare_all(c);
            return c;
        }
        if (c) PQfinish(c);
        return NULL;
    }
}

void pg_init_thread(const char *conninfo) {
    if (conninfo && *conninfo) {
        strncpy(t_conninfo, conninfo, sizeof t_conninfo - 1);
        t_conninfo[sizeof t_conninfo - 1] = '\0';
    }
    if (t_conninfo[0] == '\0') return;
    int ok = 0;
    for (int i = 0; i < POOL_N; i++) {
        t_pool[i] = pg_connect_one();
        if (t_pool[i]) ok++;
    }
    fprintf(stderr, "pg pool: %d/%d conns ok\n", ok, POOL_N);
}

/* Round-robin slot; pg_run connects/reconnects on demand. */
static int pg_slot(void) {
    int s = (int)(t_rr % POOL_N);
    t_rr++;
    return s;
}

/* Exec a prepared statement with reconnect-once + inline fallback.
 * Returns result (caller checks status + PQclear) or NULL on transport failure. */
/* Timed exec wrapper for the histogram: 2 vdso clock reads per query. */
static PGresult *pg_exec_timed(PGconn *c, const char *stmt, const char *sql,
                               int npar, const char *const *par, int prepared) {
    struct timespec q0, q1;
    clock_gettime(CLOCK_MONOTONIC, &q0);
    PGresult *r = prepared
        ? PQexecPrepared(c, stmt, npar, par, NULL, NULL, 0)
        : PQexecParams(c, sql, npar, NULL, par, NULL, NULL, 0);
    clock_gettime(CLOCK_MONOTONIC, &q1);
    long ms = (q1.tv_sec - q0.tv_sec) * 1000L + (q1.tv_nsec - q0.tv_nsec) / 1000000L;
    t_qops++;
    if (ms > 50) t_qslow++;
    pg_report_maybe();
    return r;
}

static PGresult *pg_run(int slot, const char *stmt, const char *sql, int npar, const char *const *par) {
    PGconn *c = t_pool[slot];
    if (!c || PQstatus(c) == CONNECTION_BAD) {
        if (c) PQfinish(c);
        c = pg_connect_one();
        t_pool[slot] = c;
        t_recon++;
        pg_report_maybe();
        if (!c) return NULL;
    }
    /* Blocking API consumes inline; defensively drain stale input on a reused
     * conn (PQisBusy is buffer-state only: no syscall when idle). */
    { int guard = 0; while (PQisBusy(c) && guard++ < 8) { if (!PQconsumeInput(c)) break; } }
    PGresult *r = pg_exec_timed(c, stmt, sql, npar, par, 1);
    if (r) {
        ExecStatusType st = PQresultStatus(r);
        if (st == PGRES_TUPLES_OK || st == PGRES_COMMAND_OK) return r;
        /* Real query errors (e.g. FK 23503) keep the connection healthy:
         * return as-is so the caller can inspect SQLSTATE. */
        if (PQstatus(c) == CONNECTION_OK) {
            const char *m = PQresultErrorMessage(r);
            if (!m || !strstr(m, "does not exist")) return r;
        }
        PQclear(r);
        r = NULL;
    }
    if (!t_pool[slot] || PQstatus(t_pool[slot]) == CONNECTION_BAD) {
        if (t_pool[slot]) PQfinish(t_pool[slot]);
        c = pg_connect_one();
        t_pool[slot] = c;
        t_recon++;
        pg_report_maybe();
        if (!c) return NULL;
    } else {
        c = t_pool[slot];
    }
    return pg_exec_timed(c, stmt, sql, npar, par, 0);
}

static void set_err(char *eb, size_t ecap, PGconn *c, PGresult *r) {
    if (!eb || ecap == 0) return;
    const char *m = NULL;
    if (r) m = PQresultErrorMessage(r);
    if ((!m || !*m) && c) m = PQerrorMessage(c);
    if (!m || !*m) m = "db unreachable";
    snprintf(eb, ecap, "%s", m);
    size_t l = strlen(eb);
    while (l && (eb[l - 1] == '\n' || eb[l - 1] == '\r')) eb[--l] = '\0';
}

int pg_health_check(char *errbuf, unsigned long errcap) {
    int slot = pg_slot();
    if (!t_pool[slot]) {
        PGconn *tmp = pg_connect_one();
        if (tmp) PQfinish(tmp);
    }
    PGresult *r = pg_run(slot, "q_health", SQL_HEALTH, 0, NULL);
    if (!r) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "db unreachable");
        return -1;
    }
    ExecStatusType st = PQresultStatus(r);
    if (st != PGRES_TUPLES_OK) {
        set_err(errbuf, errcap, t_pool[slot], r);
        PQclear(r);
        return -1;
    }
    PQclear(r);
    return 0;
}

/* Render one post row (PG text values) into o/cap. Returns length. */
static size_t render_row(char *o, size_t cap,
                         const char *id, const char *body, int bodylen,
                         const char *cts, const char *author, int authorlen,
                         const char *likes) {
    char ts[32];
    char eb[3100];
    char ea[512];
    if (json_fmt_ts(cts, strlen(cts), ts, sizeof ts) != 0) {
        snprintf(ts, sizeof ts, "1970-01-01T00:00:00.000Z");
    }
    json_escape_n(body, bodylen < 0 ? 0 : (size_t)bodylen, eb, sizeof eb);
    json_escape_n(author, authorlen < 0 ? 0 : (size_t)authorlen, ea, sizeof ea);
    int n = snprintf(o, cap,
        "{\"id\":%s,\"body\":\"%s\",\"created_at\":\"%s\",\"author\":\"%s\",\"like_count\":%s}",
        id, eb, ts, ea, likes);
    if (n < 0) { if (cap) o[0] = '\0'; return 0; }
    if ((size_t)n >= cap) return cap ? cap - 1 : 0;
    return (size_t)n;
}

int pg_feed(const char **out, size_t *len, char *errbuf, size_t errcap) {
    PGresult *r = pg_run(pg_slot(), "q_feed", SQL_FEED, 0, NULL);
    if (!r) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "db unreachable");
        return -1;
    }
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        set_err(errbuf, errcap, NULL, r);
        PQclear(r);
        return -1;
    }
    int rows = PQntuples(r);
    size_t o = 0, cap = sizeof t_feed;
    t_feed[0] = '\0';
    {
        const char *pre = "{\"posts\":[";
        size_t pl = strlen(pre);
        if (pl + 2 > cap) { PQclear(r); return -1; }
        memcpy(t_feed, pre, pl);
        o = pl;
    }
    for (int i = 0; i < rows; i++) {
        char row[4200];
        size_t rl = render_row(row, sizeof row,
            PQgetvalue(r, i, 0), PQgetvalue(r, i, 1), PQgetlength(r, i, 1),
            PQgetvalue(r, i, 2), PQgetvalue(r, i, 3), PQgetlength(r, i, 3),
            PQgetvalue(r, i, 4));
        if (o + rl + 2 > cap) { PQclear(r); return -1; }
        if (i) t_feed[o++] = ',';
        memcpy(t_feed + o, row, rl);
        o += rl;
    }
    if (o + 3 > cap) { PQclear(r); return -1; }
    t_feed[o++] = ']';
    t_feed[o++] = '}';
    t_feed[o] = '\0';
    PQclear(r);
    *out = t_feed;
    *len = o;
    return 0;
}

int pg_get_post(const char *idstr, const char **out, size_t *len, char *errbuf, size_t errcap) {
    const char *par[1] = { idstr };
    PGresult *r = pg_run(pg_slot(), "q_post", SQL_POST, 1, par);
    if (!r) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "db unreachable");
        return -1;
    }
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        const char *sqlstate = PQresultErrorField(r, PG_DIAG_SQLSTATE);
        if (sqlstate && (strcmp(sqlstate, "23503") == 0 || strcmp(sqlstate, "22003") == 0 || strcmp(sqlstate, "22P02") == 0)) {
            PQclear(r);
            return -2;
        }
        set_err(errbuf, errcap, NULL, r);
        PQclear(r);
        return -1;
    }
    if (PQntuples(r) != 1) {
        PQclear(r);
        return -2;
    }
    char row[4200];
    size_t rl = render_row(row, sizeof row,
        PQgetvalue(r, 0, 0), PQgetvalue(r, 0, 1), PQgetlength(r, 0, 1),
        PQgetvalue(r, 0, 2), PQgetvalue(r, 0, 3), PQgetlength(r, 0, 3),
        PQgetvalue(r, 0, 4));
    PQclear(r);
    {
        const char *pre = "{\"post\":";
        size_t pl = strlen(pre);
        if (pl + rl + 2 > sizeof t_post) return -1;
        memcpy(t_post, pre, pl);
        memcpy(t_post + pl, row, rl);
        t_post[pl + rl] = '}';
        t_post[pl + rl + 1] = '\0';
        *out = t_post;
        *len = pl + rl + 1;
    }
    return 0;
}

int pg_create_post(const char *uid, const char *body, size_t body_n, const char *author,
                   const char **out, size_t *len, char *errbuf, size_t errcap) {
    /* NUL-terminate the trimmed body for the text param. */
    static _Thread_local char t_body[17408];
    size_t bn = body_n < sizeof t_body - 1 ? body_n : sizeof t_body - 1;
    memcpy(t_body, body, bn);
    t_body[bn] = '\0';
    const char *par[2] = { uid, t_body };
    PGresult *r = pg_run(pg_slot(), "q_mkpost", SQL_MKPOST, 2, par);
    if (!r) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "db unreachable");
        return -1;
    }
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) != 1) {
        set_err(errbuf, errcap, NULL, r);
        PQclear(r);
        return -1;
    }
    const char *new_id = PQgetvalue(r, 0, 0);
    const char *new_ts = PQgetvalue(r, 0, 1);
    char idbuf[32], ts[32], eb[3100], ea[512];
    snprintf(idbuf, sizeof idbuf, "%s", new_id);
    if (json_fmt_ts(new_ts, strlen(new_ts), ts, sizeof ts) != 0)
        snprintf(ts, sizeof ts, "1970-01-01T00:00:00.000Z");
    json_escape_n(t_body, bn, eb, sizeof eb);
    json_escape_n(author ? author : "", author ? strlen(author) : 0, ea, sizeof ea);
    PQclear(r);
    int n = snprintf(t_post, sizeof t_post,
        "{\"post\":{\"id\":%s,\"body\":\"%s\",\"created_at\":\"%s\",\"author\":\"%s\",\"like_count\":0}}",
        idbuf, eb, ts, ea);
    if (n < 0 || (size_t)n >= sizeof t_post) return -1;
    *out = t_post;
    *len = (size_t)n;
    return 0;
}

int pg_like(const char *uid, const char *pid, int *already, char *errbuf, size_t errcap) {
    const char *par[2] = { uid, pid };
    PGresult *r = pg_run(pg_slot(), "q_like", SQL_LIKE, 2, par);
    if (!r) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "db unreachable");
        return -1;
    }
    if (PQresultStatus(r) == PGRES_COMMAND_OK) {
        const char *t = PQcmdTuples(r);
        *already = (t && strcmp(t, "0") == 0) ? 1 : 0;
        PQclear(r);
        return 0;
    }
    /* FK/range/invalid-text (post missing or out-of-range id) -> 404. */
    {
        const char *sqlstate = PQresultErrorField(r, PG_DIAG_SQLSTATE);
        if (sqlstate && (strcmp(sqlstate, "23503") == 0 || strcmp(sqlstate, "22003") == 0 || strcmp(sqlstate, "22P02") == 0)) {
            PQclear(r);
            return -2;
        }
    }
    set_err(errbuf, errcap, NULL, r);
    PQclear(r);
    return -1;
}

void pg_fini_thread(void) {
    for (int i = 0; i < POOL_N; i++) {
        if (t_pool[i]) { PQfinish(t_pool[i]); t_pool[i] = NULL; }
    }
}
