CC      = gcc
CFLAGS  = -std=c11 -O2 -Wall -Wextra -Wpedantic
TARGET  = autoclicker
PREFIX  = /usr/local

all: $(TARGET)

$(TARGET): autoclicker.c
	$(CC) $(CFLAGS) -o $@ $<

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all install uninstall clean
