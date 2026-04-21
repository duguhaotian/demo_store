CC = gcc
CFLAGS = -Wall -O2

all: test_ioctl test_mmap

test_ioctl: test_ioctl.c
	$(CC) $(CFLAGS) -I../driver -o $@ $<

test_mmap: test_mmap.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f test_ioctl test_mmap