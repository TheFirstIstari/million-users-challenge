CC=gcc
# Pass 15: gzip via vendored libdeflate (static). No system zlib (-lz) anymore.
LIBDEFLATE_VER=1.26
LIBDEFLATE_DIR=vendor/libdeflate-$(LIBDEFLATE_VER)
LIBDEFLATE_A=$(LIBDEFLATE_DIR)/libdeflate.a
# -march=x86-64-v3 (AVX/AVX2/BMI/FMA): valid on the Ryzen build host and on the
# Xeon 8260-class bench box (Cascade Lake, Haswell+ ISA). Falls back to generic
# -O3 only if a future bench CPU predates it (it does not).
# Portable libpq flags: Ubuntu 24.04 puts libpq-fe.h in /usr/include/postgresql
# (needs -I/usr/include/postgresql); CachyOS/Arch puts it in /usr/include.
# pkg-config covers both; the hardcoded fallback covers a missing pkg-config.
PG_CFLAGS := $(shell pkg-config --cflags libpq 2>/dev/null || echo -I/usr/include/postgresql)
PG_LDLIBS := $(shell pkg-config --libs libpq 2>/dev/null || echo -lpq)
CFLAGS=-O3 -march=x86-64-v3 -flto -Wall -Wextra -D_GNU_SOURCE -I$(LIBDEFLATE_DIR) $(PG_CFLAGS)
LDLIBS=-luring $(PG_LDLIBS) -lcrypto
TARGET=server
SRCS=server.c net.c http.c pg.c json.c jwt.c
OBJS=server.o net.o http.o pg.o json.o jwt.o
NPROC := $(shell nproc 2>/dev/null || echo 16)

all: $(TARGET)

$(TARGET): $(OBJS) $(LIBDEFLATE_A)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBDEFLATE_A) $(LDLIBS)

# Vendor the static lib on demand (fetch + sha256 verify + extract + build);
# a no-op when libdeflate.a already exists. `make clean` keeps vendor/.
$(LIBDEFLATE_A):
	bash ./build-vendor.sh

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Pass 12: optional PGO build (NOT default; CI and build.sh keep the plain
# target above). Instruments, runs the 5k mload profile pass
# (pgo-profile.sh: scratch DB on feed18, SIGTERM dump via the BUILD_PGO_GEN
# hook in server.c), drops the instrumented objects (keeping .gcda/.gcno),
# then rebuilds with -fprofile-use -fprofile-correction -DBUILD_PGO_GEN.
# -fprofile-correction tolerates torn counters from the 16 racing workers
# (documented for multithreaded profiles); -DBUILD_PGO_GEN is kept so main
# keeps an identical CFG. -Wl,-u,__gcov_dump pulls the dump routine out of
# the static libgcov.a (weak refs alone never link it). Validate with 42/42 +
# golden-body diff vs plain.
pgo:
	$(MAKE) clean
	$(MAKE) -j$(NPROC) CFLAGS="$(CFLAGS) -fprofile-generate -DBUILD_PGO_GEN -Wl,-u,__gcov_dump" $(TARGET)
	bash ./pgo-profile.sh
	rm -f $(OBJS)
	$(MAKE) -j$(NPROC) CFLAGS="$(CFLAGS) -fprofile-use -fprofile-correction -DBUILD_PGO_GEN" $(TARGET)

clean:
	rm -f $(TARGET) $(OBJS) *.gcda *.gcno

.PHONY: all clean pgo
