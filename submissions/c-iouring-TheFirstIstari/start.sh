#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
ulimit -n 1048576 2>/dev/null || true
exec ./server
