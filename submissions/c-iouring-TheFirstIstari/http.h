#ifndef HTTP_H
#define HTTP_H
#include <stddef.h>

/* Parses one request in buf[0..len). On success returns header+body total
 * consumed bytes (>0) and fills outputs. Returns 0 if more data needed,
 * -1 on malformed (caller should close or 500). */
typedef struct {
    int method;              /* 0=GET 1=POST other=-1 */
    const char *target;      /* pointer into buf */
    size_t target_len;
    int ver_major, ver_minor;
    long content_length;     /* -1 if absent */
    const char *auth;        /* NULL if absent */
    size_t auth_len;
    const char *connection;  /* NULL if absent */
    size_t connection_len;
    const char *body;
    size_t body_len;
    size_t header_len;       /* bytes incl \r\n\r\n */
} http_req_t;

int http_parse(const char *buf, size_t len, http_req_t *out);

/* Handles one request; writes response via send_all callback context.
 * Returns 1 if connection should stay open (keep-alive), 0 to close. */
struct conn_ctx;
int http_respond(struct conn_ctx *c, const http_req_t *rq, long uptime_s);

int http_send(int fd, const char *hdr, size_t hlen, const char *body, size_t blen);

#endif
