#include "net.h"
#include "jwt.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

char g_host[256] = "0.0.0.0";
int g_port = 0;
int g_extra_port = 0;
char g_db_url[2048] = "";
static char g_jwt_secret[1024] = "";
struct timespec g_start_mono;

static const char *req_env(const char *k) {
    const char *v = getenv(k);
    return (v && *v) ? v : NULL;
}

int main(void) {
    /* Block SIGPIPE process-wide. */
    signal(SIGPIPE, SIG_IGN);
    sigset_t ss; sigemptyset(&ss); sigaddset(&ss, SIGPIPE);
    sigprocmask(SIG_BLOCK, &ss, NULL);

    clock_gettime(CLOCK_MONOTONIC, &g_start_mono);

    const char *host = req_env("HOST");
    const char *port = req_env("PORT");
    const char *xport = req_env("EXTRA_PORT");
    const char *db = req_env("DATABASE_URL");
    const char *jwt = req_env("JWT_SECRET");
    if (host) { strncpy(g_host, host, sizeof g_host - 1); g_host[sizeof g_host-1] = '\0'; }
    if (!port || !xport) {
        fprintf(stderr, "PORT and EXTRA_PORT are required\n");
        return 1;
    }
    g_port = atoi(port); g_extra_port = atoi(xport);
    if (g_port <= 0 || g_extra_port <= 0) { fprintf(stderr, "bad ports\n"); return 1; }
    if (!jwt || !*jwt) { fprintf(stderr, "JWT_SECRET is required\n"); return 1; }
    if (db) { strncpy(g_db_url, db, sizeof g_db_url - 1); g_db_url[sizeof g_db_url - 1] = '\0'; }
    { strncpy(g_jwt_secret, jwt, sizeof g_jwt_secret - 1); g_jwt_secret[sizeof g_jwt_secret - 1] = '\0'; }

    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    fprintf(stderr, "server: %ld workers on %s:%d+%d (liburing probe, epoll fallback)\n",
        n, g_host, g_port, g_extra_port);

    pthread_t *th = calloc((size_t)n, sizeof *th);
    if (!th) return 1;
    for (long i = 0; i < n; i++) {
        if (pthread_create(&th[i], NULL, worker_main, (void *)i) != 0) {
            fprintf(stderr, "pthread_create %ld: %s\n", i, strerror(errno));
            return 1;
        }
    }
    for (long i = 0; i < n; i++) pthread_join(th[i], NULL);
    return 0;
}

const char *jwt_secret(void) {
    return g_jwt_secret;
}
