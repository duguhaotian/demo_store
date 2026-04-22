CC = gcc
CFLAGS = -Wall -O2
LDFLAGS = -lcrypto

all: split_snapshot query_snapshot

split_snapshot: split_snapshot.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

query_snapshot: query_snapshot.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f split_snapshot query_snapshot

install:
	cp split_snapshot query_snapshot /usr/local/bin/