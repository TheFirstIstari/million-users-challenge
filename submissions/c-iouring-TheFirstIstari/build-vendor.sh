#!/usr/bin/env bash
# Pass 15: vendor libdeflate as a static archive. No system install, no -lz.
#
#   build-vendor.sh fetch   download the pinned tarball + sha256 verify only
#                           (used by install.sh; safe as root, no compiler)
#   build-vendor.sh         fetch + extract + build vendor/libdeflate-<ver>/
#                           libdeflate.a (used by the Makefile; skips work
#                           that is already done)
set -euo pipefail
cd "$(dirname "$0")"
VER=1.26
SHA=125856d4656e0feab660f94842f835923410c9281fedbcee64598c918da42b5a
TARBALL="vendor/libdeflate-${VER}.tar.gz"
URL="https://github.com/ebiggers/libdeflate/releases/download/v${VER}/libdeflate-${VER}.tar.gz"
DIR="vendor/libdeflate-${VER}"
LIB="${DIR}/libdeflate.a"
MODE="${1:-build}"

mkdir -p vendor
if [ ! -f "$TARBALL" ]; then
  curl -fsSL --retry 3 -o "$TARBALL" "$URL"
fi
echo "$SHA  $TARBALL" | sha256sum -c -
if [ "$MODE" = fetch ]; then
  echo "libdeflate $VER tarball ready: $TARBALL"
  exit 0
fi
if [ ! -f "$LIB" ]; then
  rm -rf "$DIR"
  tar xzf "$TARBALL" -C vendor
  # Upstream 1.26 ships CMake only (no plain Makefile), and we want no new
  # build dependency: compile the static archive directly with gcc+ar.
  # -fPIC so it links cleanly into our (PIE) binary.
  mkdir -p "$DIR/build"
  for src in "$DIR"/lib/*.c "$DIR"/lib/x86/cpu_features.c; do
    gcc -O3 -fPIC -I"$DIR" -c "$src" -o "$DIR/build/$(basename "$src" .c).o"
  done
  ar rcs "$LIB" "$DIR"/build/*.o
  rm -f "$DIR"/build/*.o
fi
echo "libdeflate $VER vendored: $LIB"
