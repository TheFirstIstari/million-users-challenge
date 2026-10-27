CC=gcc
# -march=x86-64-v3 (AVX/AVX2/BMI/FMA): valid on the Ryzen build host and on the
# Xeon 8260-class bench box (Cascade Lake, Haswell+ ISA). Falls back to generic
# -O3 only if a future bench CPU predates it (it does not).
CFLAGS=-O3 -march=x86-64-v3 -flto -Wall -Wextra -D_GNU_SOURCE
LDLIBS=-luring -lpq -lcrypto
TARGET=server
SRCS=server.c net.c http.c pg.c json.c jwt.c
OBJS=server.o net.o http.o pg.o json.o jwt.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
