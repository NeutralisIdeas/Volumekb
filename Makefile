CC = gcc
CFLAGS = -O2 -Wall
LDFLAGS = -lX11 -lpthread
TARGET = volumekb
SRC = volumekb.c
PREFIX = /usr/local

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

install: $(TARGET)
	install -D -m 755 $(TARGET) $(PREFIX)/bin/$(TARGET)

uninstall:
	rm -f $(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all install uninstall clean
