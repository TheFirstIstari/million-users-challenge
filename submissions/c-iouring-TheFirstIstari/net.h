#ifndef NET_H
#define NET_H
#include <time.h>

/* conn struct: kept <= 512B. Slab (rbuf) allocated separately. */
struct conn_ctx {
    int fd;
    unsigned int req_count;
    time_t last_active;
    time_t created_at;
    int keep_alive;
    char *rbuf;
    unsigned int rlen;
    unsigned int rcap;
    struct conn_ctx *next;
};
_Static_assert(sizeof(struct conn_ctx) <= 512, "conn struct must be <=512B");

void *worker_main(void *arg);

extern char g_host[256];
extern int g_port;
extern int g_extra_port;
extern char g_db_url[2048];
extern struct timespec g_start_mono;
long current_uptime_s(void);

#endif
