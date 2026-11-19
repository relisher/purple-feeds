
PIDGIN_TREE_TOP ?= ../pidgin-2.x.y
WIN32_DEV_TOP ?= $(PIDGIN_TREE_TOP)/../win32-dev
VERSION ?= v0.2.0

PKG_CONFIG ?= pkg-config

COMMIT_ID = $(shell git log -1 --pretty=format:"%h" 2>/dev/null)
ifneq ($(COMMIT_ID),)
PLUGIN_VERSION ?= $(shell echo $(VERSION) | sed 's/^.//').git.$(COMMIT_ID)
else
PLUGIN_VERSION ?= $(shell echo $(VERSION) | sed 's/^.//')
endif

CFLAGS ?= -O2 -g -pipe -Wall
CFLAGS += -DFEEDS_PLUGIN_VERSION='"$(PLUGIN_VERSION)"'

ifeq ($(OS),Windows_NT)
  FEEDS_TARGET = libfeeds.dll
  FEEDS_DEST = "$(PROGRAMFILES)/Pidgin/plugins"
  FEEDS_ICONS_DEST = "$(PROGRAMFILES)/Pidgin/pixmaps/pidgin/protocols"
  WIN32_CC ?= $(WIN32_DEV_TOP)/mingw-4.7.2/bin/gcc
else
  WIN32_CC ?= /usr/bin/i686-w64-mingw32-gcc
  CC ?= gcc

  ifeq ($(shell $(PKG_CONFIG) --exists purple glib-2.0 2>/dev/null && echo "true"),)
    FEEDS_TARGET = FAILNOPURPLE
    FEEDS_DEST =
    FEEDS_ICONS_DEST =
  else
    FEEDS_TARGET = libfeeds.so
    FEEDS_DEST = $(DESTDIR)`$(PKG_CONFIG) --variable=plugindir purple`
    FEEDS_ICONS_DEST = $(DESTDIR)`$(PKG_CONFIG) --variable=datadir purple`/pixmaps/pidgin/protocols
  endif
endif

USER_PLUGIN_DIR ?= $(HOME)/.purple/plugins

WIN32_CFLAGS = -I$(WIN32_DEV_TOP)/glib-2.28.8/include -I$(WIN32_DEV_TOP)/glib-2.28.8/include/glib-2.0 -I$(WIN32_DEV_TOP)/glib-2.28.8/lib/glib-2.0/include -I$(PIDGIN_TREE_TOP)/libpurple -I$(PIDGIN_TREE_TOP) -DFEEDS_PLUGIN_VERSION='"$(PLUGIN_VERSION)"' -Wall -Wno-deprecated-declarations -fno-strict-aliasing
WIN32_LDFLAGS = -L$(WIN32_DEV_TOP)/glib-2.28.8/lib -L$(PIDGIN_TREE_TOP)/libpurple -lpurple -lintl -lglib-2.0 -lgobject-2.0 -static-libgcc

C_FILES := libfeeds.c

.PHONY: all install install-user install-icons uninstall check clean FAILNOPURPLE

all: $(FEEDS_TARGET)

libfeeds.so: $(C_FILES)
	$(CC) -fPIC $(CFLAGS) -shared -o $@ $^ $(LDFLAGS) `$(PKG_CONFIG) purple glib-2.0 --libs --cflags`

libfeeds.dll: $(C_FILES)
	$(WIN32_CC) -O2 -g -shared -o $@ $^ $(WIN32_CFLAGS) $(WIN32_LDFLAGS)

install: $(FEEDS_TARGET) install-icons
	mkdir -p $(FEEDS_DEST)
	install -p -m644 $(FEEDS_TARGET) $(FEEDS_DEST)

install-icons: feeds16.png feeds22.png feeds48.png
	mkdir -p $(FEEDS_ICONS_DEST)/16
	mkdir -p $(FEEDS_ICONS_DEST)/22
	mkdir -p $(FEEDS_ICONS_DEST)/48
	install -m644 feeds16.png $(FEEDS_ICONS_DEST)/16/feeds.png
	install -m644 feeds22.png $(FEEDS_ICONS_DEST)/22/feeds.png
	install -m644 feeds48.png $(FEEDS_ICONS_DEST)/48/feeds.png

# Installs for the current user only; Pidgin finds the icons only in the
# system location, so run install-icons as well if you want them.
install-user: $(FEEDS_TARGET)
	mkdir -p $(USER_PLUGIN_DIR)
	install -p -m644 $(FEEDS_TARGET) $(USER_PLUGIN_DIR)

uninstall:
	rm -f $(FEEDS_DEST)/$(FEEDS_TARGET)
	rm -f $(FEEDS_ICONS_DEST)/16/feeds.png $(FEEDS_ICONS_DEST)/22/feeds.png $(FEEDS_ICONS_DEST)/48/feeds.png

tests/parse_test: tests/parse_test.c $(C_FILES)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) `$(PKG_CONFIG) purple glib-2.0 --libs --cflags`

check: tests/parse_test
	./tests/parse_test tests/fixtures/atom.xml tests/fixtures/rss1.xml tests/fixtures/rss2.xml | diff -u tests/expected.txt -

FAILNOPURPLE:
	echo "You need libpurple 2.x development headers installed to be able to compile this plugin"

clean:
	rm -f libfeeds.so libfeeds.dll tests/parse_test
	rm -rf libfeeds.so.dSYM tests/parse_test.dSYM
