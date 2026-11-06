#!/usr/bin/env bash
# The database box (16 vCPU, 32 GB, NVMe, Ubuntu 24.04): Postgres 18 with box/postgresql.conf, a role "feed", the
# seed database feed_seed and the app box allowed in. Run once as root from a checkout of this repo:
#
#   APP_BOX=10.0.0.2 FEED_PASSWORD=... bash box/setup-db-box.sh
set -euo pipefail
: "${APP_BOX:?APP_BOX=<app box IP>}" "${FEED_PASSWORD:?FEED_PASSWORD=<password for role feed>}"
PG_MAJOR=18
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
apt-get update -qq && apt-get install -y -qq postgresql-common curl ca-certificates nodejs
/usr/share/postgresql-common/pgdg/apt.postgresql.org.sh -y
apt-get install -y -qq postgresql-$PG_MAJOR
CONF=/etc/postgresql/$PG_MAJOR/main
install -m 644 "$ROOT/box/postgresql.conf" $CONF/conf.d/challenge.conf
grep -qF "host feed,feed_seed feed $APP_BOX/32 scram-sha-256" $CONF/pg_hba.conf \
  || echo "host feed,feed_seed feed $APP_BOX/32 scram-sha-256" >> $CONF/pg_hba.conf
systemctl restart postgresql@$PG_MAJOR-main
sudo -u postgres psql -qX -c "DO \$\$BEGIN CREATE ROLE feed LOGIN CREATEDB PASSWORD '$FEED_PASSWORD'; EXCEPTION WHEN duplicate_object THEN NULL; END\$\$"
# build feed_seed as the feed role over a local socket
cp -r "$ROOT/seed" /tmp/seed-build && cp "$ROOT/schema.sql" /tmp/ && chown -R postgres: /tmp/seed-build /tmp/schema.sql
sudo -u postgres PGUSER=postgres bash /tmp/seed-build/make-seed.sh
sudo -u postgres psql -qX -c "ALTER DATABASE feed_seed OWNER TO feed" -c "ALTER DATABASE feed_seed WITH ALLOW_CONNECTIONS false"
bash "$ROOT/box/reset-db.sh"
rm -rf /tmp/seed-build /tmp/schema.sql
