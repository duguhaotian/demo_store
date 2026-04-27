CC = gcc
CFLAGS = -Wall -Wextra -g -O0 -D_GNU_SOURCE
LDFLAGS = -pthread

TARGET = uffd-cow
SRCS = main.c shmem.c uffd_handler.c page_meta.c test_data.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean