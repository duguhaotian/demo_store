# ublk-mmap-demo/Makefile
# Top-level build for ublk mmap demo

.PHONY: all clean ublksrv test

all: ublksrv test

ublksrv:
	$(MAKE) -C ublksrv

test:
	$(MAKE) -C test

clean:
	$(MAKE) -C ublksrv clean
	$(MAKE) -C test clean