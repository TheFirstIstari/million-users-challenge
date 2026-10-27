#ifndef PG_H
#define PG_H
#include <stddef.h>

/* Per-thread pool of 32 PG connections (16 workers -> 512 max).
 * Single-flight per conn; blocking exec on the owning worker thread. */
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

void pg_fini_thread(void);

#endif
