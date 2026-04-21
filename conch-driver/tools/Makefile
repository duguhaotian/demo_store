CC = gcc
CFLAGS = -Wall -O2
LDFLAGS = -lcrypto

all: split_snapshot

split_snapshot: split_snapshot.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f split_snapshot

install:
	cp split_snapshot /usr/local/bin/