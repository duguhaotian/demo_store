CC = gcc
CFLAGS = -Wall -O2

all: test_ioctl test_mmap

test_ioctl: test_ioctl.c snapshot_types.h
	$(CC) $(CFLAGS) -o $@ test_ioctl.c

test_mmap: test_mmap.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f test_ioctl test_mmap