CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2 -pthread
LDFLAGS =
LDLIBS = -lX11 -pthread

TARGET = terminator
SOURCE = src/main.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGET)
