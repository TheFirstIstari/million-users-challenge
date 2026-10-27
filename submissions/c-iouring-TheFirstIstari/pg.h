#ifndef PG_H
#define PG_H
#include <stddef.h>
/* Forward-declared libpq result (libpq-fe.h is included by pg.c/http.c). */
typedef struct pg_result PGresult;

/* Per-thread pool of 32 PG connections (16 workers -> 512 max).
 * Single-flight per conn; blocking exec on the owning worker thread.
 * Pass 6 adds a plain-async path (PQsendQuery/PQconsumeInput, no pipeline
 * mode) so one slow query no longer head-of-line blocks the worker. */
void pg_init_thread(const char *conninfo);
int pg_health_check(char *errbuf, unsigned long errcap);

/* Feed / single post / create render compact JSON into thread-local buffers.
 * 0 ok, -1 db error (errbuf set). pg_get_post returns -2 when missing. */
int pg_feed(const char **out, size_t *len, char *errbuf, size_t errcap);
int pg_get_post(const char *idstr, const char **out, size_t *len, char *errbuf, size_t errcap);
int pg_create_post(const char *uid, const char *body, size_t body_n, const char *author,
                   const char **out, size_t *len, char *errbuf, size_t errcap);

/* Like insert. 0 ok (*already 0 first / 1 repeat), -2 post not found (23503), -1 db error. */
int pg_like(const char *uid, const char *pid, int *already, char *errbuf, size_t errcap);

/* ---- Pass 6 async primitives (plain async, NO pipeline mode) ---- */
#define PG_ASYNC_OK 0
#define PG_ASYNC_BUSY 1
#define PG_ASYNC_ERR -1
#define PG_POLL_PENDING 0
#define PG_POLL_READY 1
#define PG_POLL_ERROR -1

/* Round-robin slot claim shared with the blocking path. */
int pg_claim_slot(void);
/* Send one prepared query on slot. OKBUSY/ERR as above. Single in-flight
 * per conn is preserved; BUSY means the slot is still busy. */
int pg_send_async(int slot, const char *stmt, int npar, const char *const *par);
/* Poll a busy slot. PENDING = not ready; READY = *out holds the PGresult
 * (caller owns it, must PQclear even on query-error results); ERROR =
 * transport failure (slot already reset for reconnect-on-demand). */
int pg_poll(int slot, PGresult **out);
/* Underlying socket for epoll arming, or -1 when unusable (caller must use
 * the blocking pg_run fallback). */
int pg_socket(int slot);
/* Number of slots with an async query in flight on this thread. */
int pg_busy_count(void);
/* 1 when libpq still holds unsent bytes for the slot (arm POLLOUT too),
 * else 0 (EPOLLIN suffices; an idle PG socket is always writable). */
int pg_need_write(int slot);

/* Render helpers for async completion (same wire semantics as the blocking
 * pg_* above). They do NOT PQclear(r); the caller owns r. */
int pg_render_health(PGresult *r, char *errbuf, size_t errcap);
int pg_render_feed(PGresult *r, const char **out, size_t *len, char *errbuf, size_t errcap);
int pg_render_post(PGresult *r, const char **out, size_t *len, char *errbuf, size_t errcap);
int pg_render_mkpost(PGresult *r, const char *body, size_t body_n, const char *author,
                     const char **out, size_t *len, char *errbuf, size_t errcap);
int pg_render_like(PGresult *r, int *already, char *errbuf, size_t errcap);

void pg_fini_thread(void);

#endif
