CC := gcc
CFLAGS := -O2 -Wall -Wextra -pthread
TARGET := uffd-cow-demo

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $@ $<

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) sample.bin
