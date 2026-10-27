#!/usr/bin/env bash
set -euo pipefail
# Install build dependencies. Works as root (CI) or via sudo; never reads passwords.
SUDO=""
if [ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1; then
  SUDO="sudo"
fi
if command -v pacman >/dev/null 2>&1; then
  # CachyOS/Arch. Pinned to the validated versions; fall back to latest on drift.
  $SUDO pacman -Sy --noconfirm 2>&1 | tail -2 || true
  $SUDO pacman -S --noconfirm 'liburing=2.15-1.1' 'postgresql-libs=18.6-4.1' 'openssl=3.6.4-1.1' zlib 2>&1 | tail -3 \
    || $SUDO pacman -S --noconfirm gcc liburing postgresql-libs openssl zlib 2>&1 | tail -3 || true
elif command -v apt-get >/dev/null 2>&1; then
  # Ubuntu 24.04 equivalents of the CachyOS pins above.
  $SUDO apt-get update
  $SUDO apt-get install -y build-essential liburing-dev libpq-dev libssl-dev zlib1g-dev
else
  echo "no supported package manager (pacman/apt) found" >&2
  exit 1
fi
echo "versions:"
gcc --version | head -1 || true
(pacman -Q liburing postgresql-libs openssl zlib 2>/dev/null || true)
(pkg-config --modversion liburing 2>/dev/null || true)
