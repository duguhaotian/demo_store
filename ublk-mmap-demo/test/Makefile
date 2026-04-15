# ublk-mmap-demo/test/Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g

TARGET = test_mmap

SRCS = test_mmap.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS)

clean:
	rm -f $(TARGET)