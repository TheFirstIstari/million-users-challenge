#include "json.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

const char *JERR_NOT_FOUND = "{\"error\":\"not found\"}";
const char *JERR_INVALID_POST_ID = "{\"error\":\"invalid post id\"}";
const char *JERR_POST_NOT_FOUND = "{\"error\":\"post not found\"}";
const char *JERR_MISSING_BEARER = "{\"error\":\"missing bearer token\"}";
const char *JERR_INVALID_TOKEN = "{\"error\":\"invalid or expired token\"}";
const char *JERR_INVALID_PAYLOAD = "{\"error\":\"invalid token payload\"}";
const char *JERR_MALFORMED_JSON = "{\"error\":\"malformed JSON body\"}";
const char *JERR_BODY_REQUIRED = "{\"error\":\"body is required\"}";
const char *JERR_BODY_TOO_LONG = "{\"error\":\"body must be at most 500 characters\"}";
const char *JERR_INTERNAL = "{\"error\":\"internal server error\"}";

static const char E_00[] = "\\u0000";
static const char E_01[] = "\\u0001";
static const char E_02[] = "\\u0002";
static const char E_03[] = "\\u0003";
static const char E_04[] = "\\u0004";
static const char E_05[] = "\\u0005";
static const char E_06[] = "\\u0006";
static const char E_07[] = "\\u0007";
static const char E_08[] = "\\b";
static const char E_09[] = "\\t";
static const char E_0A[] = "\\n";
static const char E_0B[] = "\\u000b";
static const char E_0C[] = "\\f";
static const char E_0D[] = "\\r";
static const char E_0E[] = "\\u000e";
static const char E_0F[] = "\\u000f";
static const char E_10[] = "\\u0010";
static const char E_11[] = "\\u0011";
static const char E_12[] = "\\u0012";
static const char E_13[] = "\\u0013";
static const char E_14[] = "\\u0014";
static const char E_15[] = "\\u0015";
static const char E_16[] = "\\u0016";
static const char E_17[] = "\\u0017";
static const char E_18[] = "\\u0018";
static const char E_19[] = "\\u0019";
static const char E_1A[] = "\\u001a";
static const char E_1B[] = "\\u001b";
static const char E_1C[] = "\\u001c";
static const char E_1D[] = "\\u001d";
static const char E_1E[] = "\\u001e";
static const char E_1F[] = "\\u001f";
static const char E_22[] = "\\\"";
static const char E_5C[] = "\\\\";
static const char *const ESC[256] = {
    E_00, E_01, E_02, E_03, E_04, E_05, E_06, E_07,
    E_08, E_09, E_0A, E_0B, E_0C, E_0D, E_0E, E_0F,
    E_10, E_11, E_12, E_13, E_14, E_15, E_16, E_17,
    E_18, E_19, E_1A, E_1B, E_1C, E_1D, E_1E, E_1F,
    0, 0, E_22, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, E_5C, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};
static const unsigned char ESCLEN[256] = {
    6, 6, 6, 6, 6, 6, 6, 6,
    2, 2, 2, 6, 2, 2, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6,
    0, 0, 2, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 2, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

/* Table-driven escape: identical output to the branchy version (same caps,
 * same lowercase hex for control chars, same truncation behavior).
 * Pass 9: bulk-copy runs of plain bytes (memcpy) instead of byte-at-a-time
 * when no escape is needed; per-char output for runs is bit-identical. */
size_t json_escape_n(const char *src, size_t n, char *dst, size_t cap) {
    size_t o = 0;
    if (cap == 0) return 0;
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)src[i];
        if (ESC[c] == 0) {
            size_t j = i + 1;
            while (j < n && ESC[(unsigned char)src[j]] == 0) j++;
            size_t run = j - i;
            size_t room = (o + 1 >= cap) ? 0 : (cap - 1 - o);
            size_t k = run < room ? run : room;
            if (k) { memcpy(dst + o, src + i, k); o += k; i += k; }
            if (k < run) break;
        } else {
            size_t el = ESCLEN[c];
            if (o + el >= cap) break;
            memcpy(dst + o, ESC[c], el);
            o += el;
            i++;
        }
    }
    if (o < cap) dst[o] = '\0';
    else dst[cap - 1] = '\0';
    return o;
}

size_t json_escape(const char *src, char *dst, size_t cap) {
    return json_escape_n(src, strlen(src), dst, cap);
}

size_t json_render_health(char *out, size_t cap, long uptime_s) {
    int n = snprintf(out, cap, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":%ld}", uptime_s);
    if (n < 0) { if (cap) out[0] = '\0'; return 0; }
    if ((size_t)n >= cap) return cap ? cap - 1 : 0;
    return (size_t)n;
}

size_t json_render_degraded(char *out, size_t cap, const char *errmsg) {
    const char *pre = "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"";
    const char *suf = "\"}";
    size_t o = 0;
    size_t pl = strlen(pre), sl = strlen(suf);
    char esc[3072];
    if (cap == 0) return 0;
    if (!errmsg || !*errmsg) errmsg = "db unreachable";
    json_escape_n(errmsg, strlen(errmsg), esc, sizeof esc);
    size_t el = strlen(esc);
    if (pl + el + sl + 1 > cap) { out[0] = '\0'; return 0; }
    memcpy(out, pre, pl); o = pl;
    memcpy(out + o, esc, el); o += el;
    memcpy(out + o, suf, sl); o += sl;
    out[o] = '\0';
    return o;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int emit_utf8(char *out, size_t cap, size_t *o, unsigned cp) {
    if (cp < 0x80) {
        if (*o + 1 >= cap) return -1;
        out[(*o)++] = (char)cp;
    } else if (cp < 0x800) {
        if (*o + 2 >= cap) return -1;
        out[(*o)++] = (char)(0xC0 | (cp >> 6));
        out[(*o)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        if (*o + 3 >= cap) return -1;
        out[(*o)++] = (char)(0xE0 | (cp >> 12));
        out[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*o)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF) {
        if (*o + 4 >= cap) return -1;
        out[(*o)++] = (char)(0xF0 | (cp >> 18));
        out[(*o)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*o)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        return -1;
    }
    return 0;
}

size_t json_decode_string(const char *s, size_t n, char *out, size_t cap) {
    size_t o = 0;
    if (cap == 0) return (size_t)-1;
    for (size_t i = 0; i < n;) {
        char c = s[i];
        if (c != '\\') {
            if (o + 1 >= cap) return (size_t)-1;
            out[o++] = c;
            i++;
            continue;
        }
        i++;
        if (i >= n) return (size_t)-1;
        char e = s[i++];
        switch (e) {
        case '"': if (o + 1 >= cap) return (size_t)-1; out[o++] = '"'; break;
        case '\\': if (o + 1 >= cap) return (size_t)-1; out[o++] = '\\'; break;
        case '/': if (o + 1 >= cap) return (size_t)-1; out[o++] = '/'; break;
        case 'b': if (o + 1 >= cap) return (size_t)-1; out[o++] = '\b'; break;
        case 'f': if (o + 1 >= cap) return (size_t)-1; out[o++] = '\f'; break;
        case 'n': if (o + 1 >= cap) return (size_t)-1; out[o++] = '\n'; break;
        case 'r': if (o + 1 >= cap) return (size_t)-1; out[o++] = '\r'; break;
        case 't': if (o + 1 >= cap) return (size_t)-1; out[o++] = '\t'; break;
        case 'u': {
            unsigned cp = 0;
            if (i + 4 > n) return (size_t)-1;
            for (int k = 0; k < 4; k++) {
                int v = hexv(s[i + k]);
                if (v < 0) return (size_t)-1;
                cp = (cp << 4) | (unsigned)v;
            }
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                /* high surrogate: must be followed by \uDC00..DFFF */
                unsigned lo = 0;
                if (i + 6 > n || s[i] != '\\' || s[i + 1] != 'u') return (size_t)-1;
                for (int k = 0; k < 4; k++) {
                    int v = hexv(s[i + 2 + k]);
                    if (v < 0) return (size_t)-1;
                    lo = (lo << 4) | (unsigned)v;
                }
                if (lo < 0xDC00 || lo > 0xDFFF) return (size_t)-1;
                i += 6;
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                return (size_t)-1; /* lone low surrogate */
            }
            if (emit_utf8(out, cap, &o, cp) != 0) return (size_t)-1;
            break;
        }
        default:
            return (size_t)-1;
        }
    }
    if (o >= cap) return (size_t)-1;
    out[o] = '\0';
    return o;
}

static const char *skip_ws(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* Find '"key"\s*:' ; returns pointer just past ':' or NULL. */
static const char *find_key(const char *js, size_t len, const char *key) {
    const char *e = js + len;
    size_t kl = strlen(key);
    if (len < kl + 2) return NULL;
    int in_str = 0, esc = 0;
    for (size_t i = 0; i + kl + 2 <= len; i++) {
        char ch = js[i];
        if (in_str) {
            if (esc) esc = 0;
            else if (ch == '\\') esc = 1;
            else if (ch == '"') in_str = 0;
            continue;
        }
        if (ch != '"') continue;
        if (memcmp(js + i + 1, key, kl) == 0 && js[i + 1 + kl] == '"') {
            const char *p = skip_ws(js + i + 1 + kl + 1, e);
            if (p < e && *p == ':') return p + 1;
        }
        in_str = 1;
    }
    return NULL;
}

int json_scan_string(const char *js, size_t len, const char *key, char *out, size_t cap) {
    const char *e = js + len;
    const char *p = find_key(js, len, key);
    if (!p) return -1;
    p = skip_ws(p, e);
    if (p >= e || *p != '"') return -1;
    p++;
    const char *start = p;
    while (p < e) {
        if (*p == '\\') { p += 2; continue; }
        if (*p == '"') break;
        p++;
    }
    if (p >= e) return -1;
    if (json_decode_string(start, (size_t)(p - start), out, cap) == (size_t)-1) return -1;
    return 0;
}

int json_scan_number(const char *js, size_t len, const char *key, long long *out) {
    const char *e = js + len;
    const char *p = find_key(js, len, key);
    if (!p) return -1;
    p = skip_ws(p, e);
    if (p >= e) return -1;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    if (p >= e || *p < '0' || *p > '9') return -1;
    long long v = 0;
    while (p < e && *p >= '0' && *p <= '9') {
        int d = *p - '0';
        if (v > (LLONG_MAX - d) / 10) v = LLONG_MAX;
        else v = v * 10 + d;
        p++;
    }
    /* tolerate fraction / exponent suffixes */
    if (p < e && *p == '.') {
        p++;
        while (p < e && *p >= '0' && *p <= '9') p++;
    }
    if (p < e && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < e && (*p == '+' || *p == '-')) p++;
        while (p < e && *p >= '0' && *p <= '9') p++;
    }
    *out = neg ? -v : v;
    return 0;
}

int json_header_is_hs256(const char *js, size_t len) {
    const char *e = js + len;
    const char *p = find_key(js, len, "alg");
    if (!p) return 0;
    p = skip_ws(p, e);
    if (p >= e || *p != '"') return 0;
    p++;
    if ((size_t)(e - p) < 6) return 0;
    if (memcmp(p, "HS256", 5) != 0) return 0;
    if (p[5] != '"') return 0;
    return 1;
}

static int is_trim_c(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

size_t json_trim_inplace(char *s, size_t n) {
    size_t a = 0;
    while (a < n && is_trim_c(s[a])) a++;
    size_t b = n;
    while (b > a && is_trim_c(s[b - 1])) b--;
    size_t m = b - a;
    if (a && m) memmove(s, s + a, m);
    return m;
}

size_t json_charlen(const char *s, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) c++;
    }
    return c;
}

int json_fmt_ts(const char *s, size_t n, char *out, size_t cap) {
    if (cap < 25 || n < 19) return -1;
    memcpy(out, s, 10);
    out[10] = 'T';
    memcpy(out + 11, s + 11, 8);
    out[19] = '.';
    char ms[3] = { '0', '0', '0' };
    if (n > 19 && s[19] == '.') {
        for (int i = 0; i < 3; i++) {
            if ((size_t)(20 + i) >= n) break;
            char d = s[20 + i];
            if (d < '0' || d > '9') break;
            ms[i] = d;
        }
    }
    out[20] = ms[0]; out[21] = ms[1]; out[22] = ms[2];
    out[23] = 'Z'; out[24] = '\0';
    return 0;
}
