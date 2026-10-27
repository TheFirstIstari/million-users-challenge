#include "http.h"
#include "json.h"
#include "jwt.h"
#include "pg.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

/* conn_ctx is defined in net.c; mirror the fields we need. */
struct conn_ctx {
    int fd;
    unsigned int req_count;
    time_t last_active;
    int keep_alive;
};

/* Thread-local scratch for decoded POST bodies (raw cap 16K + slack). */
static _Thread_local char t_dec[17408];

static int ci_eq(const char *a, size_t al, const char *b) {
    size_t bl = strlen(b);
    if (al != bl) return 0;
    return strncasecmp(a, b, bl) == 0;
}

static const char *find_crlf(const char *p, const char *end) {
    for (; p + 1 < end; p++) if (p[0] == '\r' && p[1] == '\n') return p;
    return NULL;
}

int http_parse(const char *buf, size_t len, http_req_t *out) {
    memset(out, 0, sizeof *out);
    out->method = -1; out->content_length = -1;
    out->ver_major = 1; out->ver_minor = 1;
    if (len < 16) return 0;
    const char *end = buf + len;
    /* Request line: METHOD SP TARGET SP HTTP/1.x CRLF */
    const char *sp1 = memchr(buf, ' ', (size_t)(end - buf));
    if (!sp1) return 0; /* need more */
    const char *sp2 = NULL;
    /* find next space after sp1+1, but only within first line */
    const char *eol = find_crlf(buf, end);
    if (!eol) {
        if (len > 16384) return -1;
        return 0;
    }
    for (const char *p = sp1 + 1; p < eol; p++) if (*p == ' ') { sp2 = p; break; }
    if (!sp2) return -1;
    size_t mlen = (size_t)(sp1 - buf);
    if (mlen == 3 && memcmp(buf, "GET", 3) == 0) out->method = 0;
    else if (mlen == 4 && memcmp(buf, "POST", 4) == 0) out->method = 1;
    else out->method = -1;
    out->target = sp1 + 1;
    out->target_len = (size_t)(sp2 - (sp1 + 1));
    /* version */
    const char *ver = sp2 + 1;
    if ((size_t)(eol - ver) >= 8 && memcmp(ver, "HTTP/1.", 7) == 0 &&
        (ver[7] == '0' || ver[7] == '1')) {
        out->ver_major = 1; out->ver_minor = ver[7] - '0';
    } else if ((size_t)(eol - ver) >= 6 && memcmp(ver, "HTTP/", 5) == 0) {
        out->ver_major = 1; out->ver_minor = 1;
    } else {
        return -1;
    }
    /* Headers: scan lines until \r\n\r\n */
    const char *hs = eol + 2;
    const char *hdr_end = NULL;
    for (const char *p = hs; p + 3 < end; p++) {
        if (p[0]=='\r' && p[1]=='\n' && p[2]=='\r' && p[3]=='\n') { hdr_end = p + 4; break; }
    }
    if (!hdr_end) {
        if (len > 16384 + 16384) return -1;
        if (len > 16384 && (size_t)(end - buf) > 16384) {
            return 0; /* wait until cap then caller closes */
        }
        return 0;
    }
    if ((size_t)(hdr_end - buf) > 16384) return -1;
    out->header_len = (size_t)(hdr_end - buf);
    /* Walk header lines */
    const char *line = hs;
    while (line < hdr_end - 2) {
        const char *le = find_crlf(line, hdr_end);
        if (!le) break;
        if (le == line) break; /* empty line = end */
        const char *colon = memchr(line, ':', (size_t)(le - line));
        if (colon) {
            const char *nm = line; size_t nl = (size_t)(colon - line);
            while (nl && (nm[nl-1]==' '||nm[nl-1]=='\t')) nl--;
            const char *vs = colon + 1; size_t vl = (size_t)(le - vs);
            while (vl && (*vs==' '||*vs=='\t')) { vs++; vl--; }
            while (vl && (vs[vl-1]==' '||vs[vl-1]=='\t')) vl--;
            if (ci_eq(nm, nl, "content-length")) {
                char tmp[32]; size_t c = vl < sizeof tmp - 1 ? vl : sizeof tmp - 1;
                memcpy(tmp, vs, c); tmp[c] = '\0';
                long v = -1;
                if (c > 0) {
                    char *e = NULL; v = strtol(tmp, &e, 10);
                    if (e == tmp) v = -1;
                }
                out->content_length = v;
            } else if (ci_eq(nm, nl, "authorization")) {
                out->auth = vs; out->auth_len = vl;
            } else if (ci_eq(nm, nl, "connection")) {
                out->connection = vs; out->connection_len = vl;
            } else if (ci_eq(nm, nl, "accept-encoding")) {
                /* scanned only; we always send identity. */
            } else if (ci_eq(nm, nl, "host")) {
                /* scanned only. */
            }
        }
        line = le + 2;
    }
    long cl = out->content_length;
    if (cl < 0) cl = 0;
    if (cl > 16384) return -2; /* body too large (out of scope) */
    if ((size_t)(end - hdr_end) < (size_t)cl) return 0; /* need body */
    out->body = hdr_end;
    out->body_len = (size_t)cl;
    return (int)(out->header_len + (size_t)cl);
}

int http_send(int fd, const char *hdr, size_t hlen, const char *body, size_t blen) {
    /* Header + body coalesced into one sendmsg (single syscall typically,
     * MSG_NOSIGNAL). Same contract as before: any transport error (incl.
     * EAGAIN on the nonblocking fd) fails and the caller closes. */
    struct iovec iv[2];
    iv[0].iov_base = (void *)hdr; iv[0].iov_len = hlen;
    iv[1].iov_base = (void *)body; iv[1].iov_len = blen;
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = iv;
    mh.msg_iovlen = (blen > 0) ? 2 : 1;
    size_t total = hlen + blen, off = 0;
    while (off < total) {
        ssize_t r = sendmsg(fd, &mh, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1;
        off += (size_t)r;
        size_t left = (size_t)r;
        while (left > 0 && mh.msg_iovlen > 0) {
            if (left >= mh.msg_iov->iov_len) {
                left -= mh.msg_iov->iov_len;
                mh.msg_iov++;
                mh.msg_iovlen--;
            } else {
                mh.msg_iov->iov_base = (char *)mh.msg_iov->iov_base + left;
                mh.msg_iov->iov_len -= left;
                left = 0;
            }
        }
    }
    return 0;
}

/* Route helpers */
static int target_eq(const http_req_t *rq, const char *s) {
    size_t n = strlen(s);
    if (rq->target_len != n) return 0;
    return memcmp(rq->target, s, n) == 0;
}

/* Strict positive-integer id: ^[1-9][0-9]*$, 1..15 digits. 0 else. */
static int valid_id(const char *s, size_t n, unsigned long long *v) {
    if (n == 0 || n > 15 || s[0] == '0') return 0;
    unsigned long long x = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        x = x * 10 + (unsigned)(s[i] - '0');
    }
    if (x == 0 || x > 9223372036854775807ULL) return 0;
    if (v) *v = x;
    return 1;
}

/* Auth check for protected routes. 0 ok (sub/user filled), -1 + 401 sent. */
static void send_resp(struct conn_ctx *c, const http_req_t *rq, int code, const char *reason,
                      const char *body, size_t blen, int force_close);
static int check_auth(struct conn_ctx *c, const http_req_t *rq,
                      char *sub, size_t subcap, char *user, size_t usercap) {
    const char *emsg = NULL;
    if (unlikely(!rq->auth || rq->auth_len <= 7 || memcmp(rq->auth, "Bearer ", 7) != 0)) {
        emsg = JERR_MISSING_BEARER;
        send_resp(c, rq, 401, "Unauthorized", emsg, strlen(emsg), 0);
        return -1;
    }
    long long uid = 0;
    int r = jwt_verify(rq->auth + 7, rq->auth_len - 7, jwt_secret(),
                       sub, subcap, &uid, user, usercap);
    if (r == JWT_PAYLOAD) emsg = JERR_INVALID_PAYLOAD;
    else if (r != JWT_OK) emsg = JERR_INVALID_TOKEN;
    if (unlikely(emsg)) {
        send_resp(c, rq, 401, "Unauthorized", emsg, strlen(emsg), 0);
        return -1;
    }
    return 0;
}

static int wants_close(const http_req_t *rq) {
    if (rq->connection && rq->connection_len) {
        char tmp[64]; size_t c = rq->connection_len < sizeof tmp - 1 ? rq->connection_len : sizeof tmp - 1;
        for (size_t i = 0; i < c; i++) tmp[i] = (char)tolower((unsigned char)rq->connection[i]);
        tmp[c] = '\0';
        if (strstr(tmp, "close")) return 1;
        return 0;
    }
    if (rq->ver_major == 1 && rq->ver_minor == 0) return 1; /* HTTP/1.0 default close */
    return 0;
}

/* Prebuilt status lines: no snprintf/printf on the hot path. */
static const char ST200[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ";
static const char ST201[] = "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: ";
static const char ST400[] = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: ";
static const char ST401[] = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: ";
static const char ST404[] = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: ";
static const char ST500[] = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: ";
static const char ST503[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nContent-Length: ";
static const char CONN_HDR_CLOSE[] = "\r\nConnection: close\r\n\r\n";
static const char CONN_HDR_KEEP[] = "\r\nConnection: keep-alive\r\n\r\n";

/* Decimal append without locale/printf overhead. Returns end pointer. */
static char *append_u64(char *p, size_t v) {
    char tmp[20];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    }
    while (n > 0) *p++ = tmp[--n];
    return p;
}

static void send_resp(struct conn_ctx *c, const http_req_t *rq, int code, const char *reason,
                      const char *body, size_t blen, int force_close) {
    const char *st = NULL;
    size_t sl = 0;
    switch (code) {
    case 200: st = ST200; sl = sizeof ST200 - 1; break;
    case 201: st = ST201; sl = sizeof ST201 - 1; break;
    case 400: st = ST400; sl = sizeof ST400 - 1; break;
    case 401: st = ST401; sl = sizeof ST401 - 1; break;
    case 404: st = ST404; sl = sizeof ST404 - 1; break;
    case 500: st = ST500; sl = sizeof ST500 - 1; break;
    case 503: st = ST503; sl = sizeof ST503 - 1; break;
    default: break;
    }
    char hdr[160];
    int close_it = force_close || wants_close(rq) || c->req_count >= 999;
    if (likely(st != NULL)) {
        char *p = hdr;
        memcpy(p, st, sl); p += sl;
        p = append_u64(p, blen);
        const char *ce = close_it ? CONN_HDR_CLOSE : CONN_HDR_KEEP;
        size_t cl = close_it ? sizeof CONN_HDR_CLOSE - 1 : sizeof CONN_HDR_KEEP - 1;
        memcpy(p, ce, cl); p += cl;
        http_send(c->fd, hdr, (size_t)(p - hdr), body, blen);
        c->keep_alive = !close_it;
        return;
    }
    /* Unknown code (defensive; no current caller): printf fallback. */
    (void)reason;
    const char *conn = close_it ? "close" : "keep-alive";
    int hlen = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: %s\r\n\r\n",
        code, reason ? reason : "", blen, conn);
    if (hlen < 0 || (size_t)hlen >= sizeof hdr) return;
    http_send(c->fd, hdr, (size_t)hlen, body, blen);
    c->keep_alive = !close_it;
}

/* ---- minimal JSON body parser for POST /posts ---- */

static const char *jws(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* String at *p == '"': inner in [s,n), returns past closing quote or NULL. */
static const char *jstr_end(const char *p, const char *e, const char **s, size_t *n) {
    const char *q = p + 1;
    while (q < e) {
        if (*q == '\\') { q += 2; continue; }
        if (*q == '"') { *s = p + 1; *n = (size_t)(q - (p + 1)); return q + 1; }
        q++;
    }
    return NULL;
}

/* Skip one JSON value; returns past it or NULL. */
static const char *jskip(const char *p, const char *e) {
    p = jws(p, e);
    if (p >= e) return NULL;
    if (*p == '"') {
        const char *s; size_t n;
        return jstr_end(p, e, &s, &n);
    }
    if (*p == '{') {
        p++;
        p = jws(p, e);
        if (p < e && *p == '}') return p + 1;
        while (1) {
            const char *s; size_t n;
            p = jws(p, e);
            if (p >= e || *p != '"') return NULL;
            p = jstr_end(p, e, &s, &n);
            if (!p) return NULL;
            p = jws(p, e);
            if (p >= e || *p != ':') return NULL;
            p = jskip(p + 1, e);
            if (!p) return NULL;
            p = jws(p, e);
            if (p >= e) return NULL;
            if (*p == ',') { p++; continue; }
            if (*p == '}') return p + 1;
            return NULL;
        }
    }
    if (*p == '[') {
        p++;
        p = jws(p, e);
        if (p < e && *p == ']') return p + 1;
        while (1) {
            p = jskip(p, e);
            if (!p) return NULL;
            p = jws(p, e);
            if (p >= e) return NULL;
            if (*p == ',') { p++; continue; }
            if (*p == ']') return p + 1;
            return NULL;
        }
    }
    /* literal: true / false / null / number */
    const char *q = p;
    while (q < e && *q != ',' && *q != '}' && *q != ']' &&
           *q != ' ' && *q != '\t' && *q != '\n' && *q != '\r') q++;
    if (q == p) return NULL;
    size_t L = (size_t)(q - p);
    if (L == 4 && (memcmp(p, "true", 4) == 0 || memcmp(p, "null", 4) == 0)) return q;
    if (L == 5 && memcmp(p, "false", 5) == 0) return q;
    /* number: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)? */
    {
        const char *r = p;
        if (r < q && *r == '-') r++;
        if (r >= q) return NULL;
        if (*r == '0') { r++; }
        else if (*r >= '1' && *r <= '9') { while (r < q && *r >= '0' && *r <= '9') r++; }
        else return NULL;
        if (r < q && *r == '.') {
            r++;
            const char *fs = r;
            while (r < q && *r >= '0' && *r <= '9') r++;
            if (r == fs) return NULL;
        }
        if (r < q && (*r == 'e' || *r == 'E')) {
            r++;
            if (r < q && (*r == '+' || *r == '-')) r++;
            const char *es = r;
            while (r < q && *r >= '0' && *r <= '9') r++;
            if (r == es) return NULL;
        }
        if (r != q) return NULL;
    }
    return q;
}

/* Extract top-level "body" member.
 * Returns -1 malformed, 0 no-body-key, 1 found (*is_string; decoded in dec). */
static int extract_body(const char *b, size_t n, char *dec, size_t deccap,
                        size_t *declen, int *is_string) {
    const char *e = b + n;
    const char *p = jws(b, e);
    if (p >= e || *p != '{') return -1;
    p++;
    p = jws(p, e);
    if (p < e && *p == '}') {
        p = jws(p + 1, e);
        return (p == e) ? 0 : -1;
    }
    int found = 0;
    while (1) {
        const char *ks; size_t kn;
        p = jws(p, e);
        if (p >= e || *p != '"') return -1;
        const char *np = jstr_end(p, e, &ks, &kn);
        if (!np) return -1;
        p = jws(np, e);
        if (p >= e || *p != ':') return -1;
        p = jws(p + 1, e);
        if (p >= e) return -1;
        int is_body = (kn == 4 && memcmp(ks, "body", 4) == 0);
        if (*p == '"') {
            const char *vs; size_t vn;
            np = jstr_end(p, e, &vs, &vn);
            if (!np) return -1;
            if (is_body && !found) {
                size_t dl = json_decode_string(vs, vn, dec, deccap);
                if (dl == (size_t)-1) return -1;
                *declen = dl;
                *is_string = 1;
                found = 1;
            }
            p = np;
        } else {
            if (is_body && !found) { *is_string = 0; found = 1; }
            p = jskip(p, e);
            if (!p) return -1;
        }
        p = jws(p, e);
        if (p >= e) return -1;
        if (*p == ',') { p++; continue; }
        if (*p == '}') { p++; break; }
        return -1;
    }
    p = jws(p, e);
    if (p != e) return -1;
    return found ? 1 : 0;
}

int http_respond(struct conn_ctx *c, const http_req_t *rq, long uptime_s) {
    /* Validation order: route -> auth -> id -> body -> DB. */
    if (likely(rq->method == 0) && target_eq(rq, "/health")) {
        char err[512]; err[0] = '\0';
        char body[1024];
        size_t bl;
        if (pg_health_check(err, sizeof err) == 0) {
            bl = json_render_health(body, sizeof body, uptime_s);
            send_resp(c, rq, 200, "OK", body, bl, 0);
        } else {
            static const char degraded[] = "{\"status\":\"degraded\",\"db\":\"unreachable\"}";
            if (!err[0]) snprintf(err, sizeof err, "db unreachable");
            fprintf(stderr, "health degraded: %s\n", err);
            send_resp(c, rq, 503, "Service Unavailable", degraded, sizeof degraded - 1, 0);
        }
        return c->keep_alive;
    }
    if (likely(rq->method == 0) && target_eq(rq, "/feed")) {
        const char *body = NULL;
        size_t bl = 0;
        char err[512];
        if (pg_feed(&body, &bl, err, sizeof err) == 0) {
            send_resp(c, rq, 200, "OK", body, bl, 0);
        } else {
            send_resp(c, rq, 500, "Internal Server Error", JERR_INTERNAL, strlen(JERR_INTERNAL), 0);
        }
        return c->keep_alive;
    }
    if (likely(rq->method == 0) && rq->target_len > 7 && memcmp(rq->target, "/posts/", 7) == 0) {
        const char *t = rq->target;
        size_t tl = rq->target_len;
        /* extra '/' or query string: unknown path -> 404 */
        for (size_t k = 7; k < tl; k++) {
            if (t[k] == '/' || t[k] == '?') {
                send_resp(c, rq, 404, "Not Found", JERR_NOT_FOUND, strlen(JERR_NOT_FOUND), 0);
                return c->keep_alive;
            }
        }
        unsigned long long idv = 0;
        if (unlikely(!valid_id(t + 7, tl - 7, &idv))) {
            send_resp(c, rq, 400, "Bad Request", JERR_INVALID_POST_ID, strlen(JERR_INVALID_POST_ID), 0);
            return c->keep_alive;
        }
        char ids[24];
        snprintf(ids, sizeof ids, "%llu", idv);
        const char *body = NULL;
        size_t bl = 0;
        char err[512];
        int r = pg_get_post(ids, &body, &bl, err, sizeof err);
        if (r == 0) {
            send_resp(c, rq, 200, "OK", body, bl, 0);
        } else if (r == -2) {
            send_resp(c, rq, 404, "Not Found", JERR_POST_NOT_FOUND, strlen(JERR_POST_NOT_FOUND), 0);
        } else {
            send_resp(c, rq, 500, "Internal Server Error", JERR_INTERNAL, strlen(JERR_INTERNAL), 0);
        }
        return c->keep_alive;
    }
    if (unlikely(rq->method == 1) && target_eq(rq, "/posts")) {
        /* auth first */
        char sub[32], user[256];
        if (check_auth(c, rq, sub, sizeof sub, user, sizeof user) != 0)
            return c->keep_alive;
        /* missing Content-Length on POST -> malformed JSON */
        if (rq->content_length < 0) {
            send_resp(c, rq, 400, "Bad Request", JERR_MALFORMED_JSON, strlen(JERR_MALFORMED_JSON), 0);
            return c->keep_alive;
        }
        size_t declen = 0;
        int is_string = 0;
        int pr = extract_body(rq->body ? rq->body : "", rq->body_len,
                              t_dec, sizeof t_dec, &declen, &is_string);
        if (unlikely(pr < 0)) {
            send_resp(c, rq, 400, "Bad Request", JERR_MALFORMED_JSON, strlen(JERR_MALFORMED_JSON), 0);
            return c->keep_alive;
        }
        if (unlikely(pr == 0 || !is_string)) {
            send_resp(c, rq, 400, "Bad Request", JERR_BODY_REQUIRED, strlen(JERR_BODY_REQUIRED), 0);
            return c->keep_alive;
        }
        size_t tl2 = json_trim_inplace(t_dec, declen);
        t_dec[tl2] = '\0';
        if (tl2 == 0) {
            send_resp(c, rq, 400, "Bad Request", JERR_BODY_REQUIRED, strlen(JERR_BODY_REQUIRED), 0);
            return c->keep_alive;
        }
        if (json_charlen(t_dec, tl2) > 500) {
            send_resp(c, rq, 400, "Bad Request", JERR_BODY_TOO_LONG, strlen(JERR_BODY_TOO_LONG), 0);
            return c->keep_alive;
        }
        const char *body = NULL;
        size_t bl = 0;
        char err[512];
        if (pg_create_post(sub, t_dec, tl2, user, &body, &bl, err, sizeof err) == 0) {
            send_resp(c, rq, 201, "Created", body, bl, 0);
        } else {
            send_resp(c, rq, 500, "Internal Server Error", JERR_INTERNAL, strlen(JERR_INTERNAL), 0);
        }
        return c->keep_alive;
    }
    if (unlikely(rq->method == 1) && rq->target_len > 12 &&
        memcmp(rq->target, "/posts/", 7) == 0 &&
        memcmp(rq->target + rq->target_len - 5, "/like", 5) == 0) {
        /* auth before id (spec). */
        char sub[32], user[256];
        if (check_auth(c, rq, sub, sizeof sub, user, sizeof user) != 0)
            return c->keep_alive;
        size_t midlen = rq->target_len - 12;
        const char *mid = rq->target + 7;
        if (midlen == 0) {
            send_resp(c, rq, 400, "Bad Request", JERR_INVALID_POST_ID, strlen(JERR_INVALID_POST_ID), 0);
            return c->keep_alive;
        }
        int weird = 0;
        for (size_t k = 0; k < midlen; k++) {
            if (mid[k] == '/' || mid[k] == '?') { weird = 1; break; }
        }
        if (weird) {
            send_resp(c, rq, 404, "Not Found", JERR_NOT_FOUND, strlen(JERR_NOT_FOUND), 0);
            return c->keep_alive;
        }
        unsigned long long idv = 0;
        if (unlikely(!valid_id(mid, midlen, &idv))) {
            send_resp(c, rq, 400, "Bad Request", JERR_INVALID_POST_ID, strlen(JERR_INVALID_POST_ID), 0);
            return c->keep_alive;
        }
        char ids[24];
        snprintf(ids, sizeof ids, "%llu", idv);
        int already = 0;
        char err[512];
        int r = pg_like(sub, ids, &already, err, sizeof err);
        if (r == 0) {
            char lb[96];
            int n = snprintf(lb, sizeof lb,
                "{\"liked\":true,\"already_liked\":%s,\"post_id\":%llu}",
                already ? "true" : "false", idv);
            if (n < 0 || (size_t)n >= sizeof lb) {
                send_resp(c, rq, 500, "Internal Server Error", JERR_INTERNAL, strlen(JERR_INTERNAL), 0);
            } else {
                send_resp(c, rq, already ? 200 : 201, already ? "OK" : "Created", lb, (size_t)n, 0);
            }
        } else if (r == -2) {
            send_resp(c, rq, 404, "Not Found", JERR_POST_NOT_FOUND, strlen(JERR_POST_NOT_FOUND), 0);
        } else {
            send_resp(c, rq, 500, "Internal Server Error", JERR_INTERNAL, strlen(JERR_INTERNAL), 0);
        }
        return c->keep_alive;
    }
    /* Unknown method on known path is out of scope -> 404. Unknown path -> 404. */
    send_resp(c, rq, 404, "Not Found", JERR_NOT_FOUND, strlen(JERR_NOT_FOUND), 0);
    return c->keep_alive;
}
