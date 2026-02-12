CC      = gcc
CFLAGS  = -O3 -Wall -Wextra -Wpedantic -std=c99
LDFLAGS = -lm

PREFIX  = /usr/local

# Use pkg-config to find libevdev headers and linker flags.
# Falls back to a manual search if pkg-config is unavailable.
PKG_CONFIG ?= pkg-config

EVDEV_CFLAGS := $(shell $(PKG_CONFIG) --cflags libevdev 2>/dev/null)
EVDEV_LIBS   := $(shell $(PKG_CONFIG) --libs   libevdev 2>/dev/null)

# Fallback: search common paths if pkg-config didn't find it.
ifeq ($(EVDEV_LIBS),)
  EVDEV_HEADER := $(shell find /usr/include /usr/local/include -name libevdev.h -print -quit 2>/dev/null)
  ifneq ($(EVDEV_HEADER),)
    EVDEV_CFLAGS := -I$(dir $(patsubst %/libevdev/libevdev.h,%,$(EVDEV_HEADER)))
  endif
  EVDEV_LIBS := -levdev
endif

CFLAGS  += $(EVDEV_CFLAGS)
LDFLAGS += $(EVDEV_LIBS)

all: smooth-scroll

smooth-scroll: smooth-scroll.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: smooth-scroll
	install -Dm755 smooth-scroll $(PREFIX)/bin/smooth-scroll
	install -Dm644 smooth-scroll.service /etc/systemd/system/smooth-scroll.service

uninstall:
	rm -f $(PREFIX)/bin/smooth-scroll
	rm -f /etc/systemd/system/smooth-scroll.service

clean:
	rm -f smooth-scroll

.PHONY: all install uninstall clean
