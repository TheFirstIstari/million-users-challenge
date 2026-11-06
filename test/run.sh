#!/usr/bin/env bash
# Builds a submission, starts it on a fresh copy of the seed database and runs the test suite.
# This is exactly what CI runs on every pull request.
#
#   bash test/run.sh submissions/<your-folder>
#
# Run install.sh yourself first (once, as root). Needs the feed_seed database: bash seed/make-seed.sh
# Postgres connection: the usual libpq env vars (PGHOST, PGPORT, PGUSER, PGPASSWORD), as for make-seed.sh.
set -euo pipefail
DIR="$(cd "${1:?usage: test/run.sh submissions/<folder>}" && pwd)"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
for f in build.sh start.sh; do [ -f "$DIR/$f" ] || { echo "missing $DIR/$f"; exit 1; }; done
psql -qXAt -d postgres -c "SELECT 1 FROM pg_database WHERE datname = 'feed_seed'" | grep -q 1 \
  || { echo "no feed_seed database: run bash seed/make-seed.sh"; exit 1; }

DB=feed_test
psql -qX -v ON_ERROR_STOP=1 -d postgres -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" -c "CREATE DATABASE $DB TEMPLATE feed_seed"
PGU="${PGUSER:-$(whoami)}"
if [ -n "${PGPASSWORD:-}" ]; then
  if command -v python3 >/dev/null 2>&1; then
    ENC="$(PGPASSWORD="$PGPASSWORD" python3 -c 'import urllib.parse,os;print(urllib.parse.quote(os.environ.get("PGPASSWORD",""),safe=""))')"
  elif command -v jq >/dev/null 2>&1; then
    ENC="$(printf '%s' "$PGPASSWORD" | jq -Rr @uri)"
  else
    ENC="$PGPASSWORD"
  fi
  AUTH="$PGU:$ENC"
else
  AUTH="$PGU"
fi
H="${PGHOST:-localhost}"; case "$H" in /*) H=localhost;; esac
export DATABASE_URL="postgres://$AUTH@$H:${PGPORT:-5432}/$DB"
export JWT_SECRET=million-users-challenge HOST=127.0.0.1 PORT="${PORT:-3000}" EXTRA_PORT="${EXTRA_PORT:-3001}"
LOG="$(mktemp)"

echo "== build"; bash "$DIR/build.sh"
echo "== start (PORT=$PORT, EXTRA_PORT=$EXTRA_PORT, database $DB)"
bash "$DIR/start.sh" > "$LOG" 2>&1 &
PID=$!
PID=${PID:-}
cleanup() { if [ -n "${PID:-}" ]; then pkill -P "$PID" 2>/dev/null || true; kill "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; fi; rm -f "$LOG"; }
trap cleanup EXIT
for _ in $(seq 60); do curl -sf --max-time 10 --connect-timeout 5 "http://$HOST:$PORT/health" >/dev/null && break; sleep 1; done
for p in $PORT $EXTRA_PORT; do
  curl -sf --max-time 10 --connect-timeout 5 "http://$HOST:$p/health" >/dev/null || { echo "no healthy /health on port $p after 60 s"; cat "$LOG"; exit 1; }
done

echo "== test (port $PORT; port $EXTRA_PORT answers /health)"
rc=0; bash "$ROOT/test/test.sh" "http://$HOST:$PORT" || rc=$?
if [ $rc = 0 ]; then
  echo "== extra port + gzip smoke"
  EB="http://$HOST:$EXTRA_PORT"
  # the suite above writes posts/likes, so only unmutated fixtures still match goldens here
  if ! curl -sf --max-time 10 --connect-timeout 5 "$EB/health" >/dev/null; then echo "EXTRA_PORT /health failed"; rc=1; fi
  if [ $rc = 0 ] && ! curl -sf --max-time 10 --connect-timeout 5 "$EB/feed" | jq -e '.posts | length == 20' >/dev/null; then echo "EXTRA_PORT /feed: want 20 posts"; rc=1; fi
  if [ $rc = 0 ] && ! BODY="$(curl -sf --max-time 10 --connect-timeout 5 "$EB/posts/500000")" || [ "$BODY" != "$(cat "$ROOT/test/golden/post-500000.json")" ]; then echo "EXTRA_PORT /posts/500000 != golden"; rc=1; fi
  # gzip or identity both accepted; the body must decode to valid JSON either way
  if [ $rc = 0 ] && ! curl -sf --max-time 10 --connect-timeout 5 -H 'Accept-Encoding: gzip' --compressed "http://$HOST:$PORT/feed" | jq -e '.posts | length == 20' >/dev/null; then echo "gzip /feed smoke failed"; rc=1; fi
fi
[ $rc = 0 ] || { echo "== server log (last 30 lines)"; tail -30 "$LOG"; }
trap - EXIT; cleanup
exit $rc
