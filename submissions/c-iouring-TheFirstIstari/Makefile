CC=gcc
# -march=x86-64-v3 (AVX/AVX2/BMI/FMA): valid on the Ryzen build host and on the
# Xeon 8260-class bench box (Cascade Lake, Haswell+ ISA). Falls back to generic
# -O3 only if a future bench CPU predates it (it does not).
CFLAGS=-O3 -march=x86-64-v3 -flto -Wall -Wextra -D_GNU_SOURCE
LDLIBS=-luring -lpq -lcrypto -lz
TARGET=server
SRCS=server.c net.c http.c pg.c json.c jwt.c
OBJS=server.o net.o http.o pg.o json.o jwt.o
NPROC := $(shell nproc 2>/dev/null || echo 16)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

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
