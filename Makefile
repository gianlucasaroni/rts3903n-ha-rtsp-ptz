CC ?= mips-linux-uclibc-gcc
CFLAGS ?= -std=gnu99 -O2 -Wall -s

all: fshare2fifo motor

fshare2fifo: src/fshare2fifo.c
	$(CC) $(CFLAGS) $< -o $@

motor: src/motor.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f fshare2fifo motor
