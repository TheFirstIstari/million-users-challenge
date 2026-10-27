# c-iouring-TheFirstIstari

C11 feed API: one process, N worker threads, io_uring multishot accept with an
epoll fallback, a per-thread libpq connection pool, HS256 JWT auth, and a
flat hand-written HTTP/JSON layer. No framework, no router, no ORM.

## Stack

| Piece | Exact version (build host) | Ubuntu 24.04 equivalent |
| --- | --- | --- |
| Language | C11, `gcc 16.2.1` | `build-essential` |
| Async I/O | `liburing 2.15-1.1` | `liburing-dev` |
| Postgres driver | libpq from `postgresql-libs 18.6-4.1` | `libpq-dev` |
| Crypto (HMAC-SHA256) | `openssl 3.6.4-1.1` | `libssl-dev` |
| Database | `postgres:18` (see `test/run.sh`) | — |

Build flags (`Makefile`): `-O3 -march=x86-64-v3 -flto -Wall -Wextra
-D_GNU_SOURCE`, linked with `-luring -lpq -lcrypto`. The build is warning-free.

### Why `-march=x86-64-v3` is safe here

x86-64-v3 means AVX/AVX2/BMI1/BMI2/FMA/LZCNT/MOVBE. The build host is Ryzen
(which has it) and the bench box is Xeon 8260-class (Cascade Lake: Haswell+
ISA, so v3 is covered with room to spare). If a future bench CPU ever predates
v3, drop the flag back to plain `-O3` — nothing else changes.

## Process / thread model

`server.c`: one process, `workers = min(ncpu, 16)` pthreads. There is no
master/acceptor process — **every worker binds both ports itself** and owns:

- one epoll fd (+ one io_uring ring while multishot accept is usable),
- one 32-connection Postgres pool (`pg.c`, thread-local),
- thread-local render/scratch buffers (so the hot path never mallocs).

`SO_REUSEPORT` spreads accepted connections across workers in the kernel.
Startup prints `server: <N> workers on <host>:<port>+<extra>` plus per-thread
`pg pool: n/32 conns ok` lines to stderr.

Config is env-only (`server.c`, all required, no defaults except `HOST`):

| Var | Meaning |
| --- | --- |
| `HOST` | bind address (default `0.0.0.0`) |
| `PORT` / `EXTRA_PORT` | **both ports serve all routes**; `test/run.sh` health-checks both |
| `DATABASE_URL` | libpq conninfo (never logged) |
| `JWT_SECRET` | HMAC secret; server refuses to start without it |

## Postgres pool: why 32 × 16 = 512

`pg.h`: each worker keeps 32 blocking libpq connections (round-robin,
single-flight per connection — a connection is only ever used by its owning
thread, so no locking). 16 workers × 32 = **512 server-side connections max**,
which fits under the CI database's `max_connections 600` with headroom left
for `psql`/seed/test connections. A slot reconnects on demand (reconnect-once
per query, then an inline non-prepared fallback), and every new connection gets
`SET statement_timeout=5000` plus the 5 prepared statements below. Query
latency is histogrammed to stderr (`pg stats: ops=… slow_gt50ms=…
reconnects=…`, once a minute at most).

## SQL (verbatim, `pg.c`)

```sql
SELECT 1
```
```sql
SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id)::int AS like_count FROM posts p JOIN users u ON u.id=p.user_id ORDER BY p.created_at DESC,p.id DESC LIMIT 20
```
```sql
SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id)::int AS like_count FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=$1
```
```sql
INSERT INTO posts(user_id,body) VALUES($1,$2) RETURNING id,created_at
```
```sql
INSERT INTO likes(user_id,post_id) VALUES($1,$2) ON CONFLICT(user_id,post_id) DO NOTHING
```

Prepared once per connection as `q_health`, `q_feed`, `q_post`, `q_mkpost`,
`q_like`. `PQcmdTuples` distinguishes first like (`201`) from repeat (`200`).
FK/range/invalid-text failures (`23503`, `22003`, `22P02`) on read/like map to
`404 post not found`, never `500`.

## HTTP behavior

Routes (`http.c`): `GET /health`, `GET /feed`, `GET /posts/<id>`,
`POST /posts`, `POST /posts/<id>/like`. Anything else is `404
{"error":"not found"}`. Validation order is route → auth → id → body → DB.

- **Dual ports:** `PORT` and `EXTRA_PORT` serve identical traffic (two
  listeners per worker, `g_port`/`g_extra_port` in `server.c`).
- **Keep-alive:** `Connection: keep-alive` by default; idle connections close
after **80 s** (`IDLE_S`, `net.c`); a connection also closes after ~1000
requests (`MAX_REQS`). `HTTP/1.0` without `Connection: keep-alive` closes.
- **Header timeout:** a connection that has completed zero requests and still
holds unparsed bytes is closed **20 s** after accept (`HDR_TIMEOUT_S`), so
slow-loris header drips cannot pin a slot. Idle reaping runs on a 1 s tick.
- **Caps:** 16 KiB headers + 16 KiB body (`HDR_CAP`/`BODY_CAP`); oversized
bodies fail the request. At most 64000 connections per worker
(`MAX_CONNS_PER_WORKER`); excess accepts are dropped immediately.
- **No gzip:** responses are always `identity`. `Accept-Encoding` is parsed but
ignored on purpose — bodies are small JSON (feed ≤ ~64 KiB typical) and gzip
would trade server CPU and p99 latency for bytes nobody required; the spec
never asks for content encodings.

## Auth: JWT verified on every request, nothing cached

`jwt.c` `jwt_verify()` runs per protected request (`POST /posts`,
`POST /posts/<id>/like`): split `h.p.s` → reject `=` padding/extra segments →
base64url-decode the header → require it to contain `"alg"` followed by
exactly `"HS256"` (quote-aware: `find_key` in `json.c` skips `"alg"`
occurrences inside string literals, so `{"note":"alg HS256"}` cannot spoof
it) → base64url-decode the 32-byte signature → HMAC-SHA256 over `h.p` with
constant-time compare → `sub` must be a JSON **string** of digits, no leading
zero (else `401 invalid token payload`) → `username` must be a JSON string
(used verbatim as `author`) → `exp` must be a JSON **number** greater than
`now` (missing/stale `exp` is `401 invalid or expired token`, never a payload
error). Only the secret pointer/length is memoized per thread to skip
`strlen`; tokens and claims are never cached. Embedded NUL bytes terminate C
strings at the first NUL, so they cannot smuggle content past validation.

## IDs, bodies, timestamps

- Post ids are strict `^[1-9][0-9]*$`, ≤ 15 digits, ≤ int8max (`valid_id`,
`http.c`); anything else is `400 invalid post id`. Out-of-range numerics that
reach Postgres surface as `22003`/`22P02` and still map to `404`.
- `POST /posts` needs a present `Content-Length`, a JSON object with a string
`body` (missing/non-string → `400 body is required`), trimmed non-empty,
≤ 500 Unicode code points (`json_charlen` counts UTF-8 lead bytes) or
`400 body must be at most 500 characters`. Malformed JSON →
`400 malformed JSON body`. Auth is checked before the body is parsed.
- `created_at` renders `YYYY-MM-DDTHH:MI:SS.mmmZ` (`json_fmt_ts`, `json.c`):
the PG `timestamptz` text's microseconds are **truncated to milliseconds, never
rounded**.

## Per-connection memory budget

`net.h`: `struct conn_ctx` is compile-time asserted to ≤ 512 B (fd, counters,
timestamps, keep-alive flag, slab pointer/len/cap, list link). Each connection
adds one receive slab: 4 KiB initial, doubling to at most 32 KiB (16 KiB
header + 16 KiB body). No per-request malloc on the hot path — rendering uses
thread-local buffers (`t_feed` 128 KiB, `t_post` 8 KiB, `t_dec`/`t_body`
~17 KiB, JWT payload 8 KiB).

## Non-default settings

| Setting | Value | Where | Why |
| --- | --- | --- | --- |
| `statement_timeout` | 5000 ms (`SET` + connect `options=`) | `pg.c` | bound worst-case query latency |
| `connect_timeout` | 5 s | `pg.c` | fail fast on dead DB |
| `application_name` | `c-iouring` | `pg.c` | identify conns in `pg_stat_activity` |
| `TCP_NODELAY` | 1, listeners + accepted | `net.c` | small JSON, no Nagle delay |
| `TCP_DEFER_ACCEPT` | 1 s (Linux only) | `net.c` | fewer wakeups from half-open conns |
| `SO_REUSEPORT`/`REUSEADDR` | on | `net.c` | kernel fan-out to all workers |
| `listen()` backlog | 65535 | `net.c` | absorb burst accepts |
| io_uring ring | 2048 SQ / 4096 CQ, `SINGLE_ISSUER \| COOP_TASKRUN` | `net.c` | multishot accept, low syscall cost |
| Multishot accepts | 2 per listener; throwaway-loopback probe at startup; runtime `-EINVAL` ×4 falls back to `accept4` | `net.c` | portable: fast path where supported, epoll everywhere else |
| Idle / header timeouts | 80 s / 20 s, 1 s sweep | `net.c` | slow-loris resistance |
| Requests per conn | ~1000 then close | `net.c`/`http.c` | bound per-conn state lifetime |
| PG reconnect | reconnect-once + inline non-prepared retry | `pg.c` | survive restarts without dropping |

## Validation results (last measured)

- `test/test.sh`: **42/42 green** (both ports health-checked).
- `bench/mload`: 5000 requests, 0 failures, **p95 ~1.1 ms, p99 ~1.6 ms**.
- Footprint under load: **RSS ~50 MB, 579 FDs**, 512 worst-case PG clients
(32 × 16) against `max_connections 600`.

## Files

`server.c` (env, threads) · `net.c`/`net.h` (listeners, io_uring probe,
epoll loop, timeouts) · `http.c`/`http.h` (parser, routes, validation) ·
`pg.c`/`pg.h` (pool, prepared SQL, renderers) · `jwt.c`/`jwt.h` (HS256
verify) · `json.c`/`json.h` (escape/decode/scan/timestamp, error bodies) ·
`Makefile` · `install.sh` · `build.sh` · `start.sh`.
