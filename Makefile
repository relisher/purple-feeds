CC ?= cc
PKG_CONFIG ?= pkg-config

CFLAGS += -fPIC -Wall -O2 $(shell $(PKG_CONFIG) --cflags purple glib-2.0)
LDLIBS += $(shell $(PKG_CONFIG) --libs purple glib-2.0)

TARGET = libfeeds.so
USER_PLUGIN_DIR ?= $(HOME)/.purple/plugins
SYSTEM_PLUGIN_DIR = $(shell $(PKG_CONFIG) --variable=plugindir purple)

all: $(TARGET)

$(TARGET): feeds.c
	$(CC) $(CFLAGS) -shared $< $(LDFLAGS) $(LDLIBS) -o $@

install: $(TARGET)
	mkdir -p $(DESTDIR)$(USER_PLUGIN_DIR)
	install -m 644 $(TARGET) $(DESTDIR)$(USER_PLUGIN_DIR)

install-system: $(TARGET)
	install -m 644 $(TARGET) $(DESTDIR)$(SYSTEM_PLUGIN_DIR)

# Prints what the parser finds in feed files: make check FEEDS="a.xml b.xml"
tests/parse_test: tests/parse_test.c feeds.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@

check: tests/parse_test
	./tests/parse_test $(FEEDS)

clean:
	rm -f $(TARGET) tests/parse_test

.PHONY: all install install-system check clean
