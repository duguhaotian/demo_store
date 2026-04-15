# ublk-mmap-demo/ublksrv/Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
LDFLAGS = -luring

TARGET = ublk_loop_srv

SRCS = ublk_loop_srv.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)