# Windows 2000 desktop environment for X11 -- build

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
LIBEXEC ?= $(PREFIX)/lib/w2k

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c99 -Wall -Wextra -Wno-unused-parameter -Iinclude
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS  += -I/usr/include/freetype2
LDLIBS  := -lX11 -lXext -lXrandr -lXcursor -lXft -lfontconfig -lz -ljpeg -lm

LIB_SRC := $(wildcard lib/*.c)
LIB_OBJ := $(LIB_SRC:.c=.o)
LIB     := lib/libw2k.a

WM_SRC  := $(wildcard wm/*.c)
WM_OBJ  := $(WM_SRC:.c=.o)

# w2kswatch is a development scratch tool: buildable, never installed.
APPS    := $(filter-out bin/w2kswatch,$(patsubst apps/%.c,bin/%,$(wildcard apps/*.c)))
# The logon screen talks to LightDM when its library is about; without it
# the greeter still builds, as the picture alone.
LIGHTDM_CFLAGS := $(shell pkg-config --cflags liblightdm-gobject-1 2>/dev/null)
LIGHTDM_LIBS   := $(shell pkg-config --libs liblightdm-gobject-1 2>/dev/null)
ifneq ($(LIGHTDM_LIBS),)
apps/w2klogon.o: CFLAGS += -DHAVE_LIGHTDM $(LIGHTDM_CFLAGS)
bin/w2klogon: LDLIBS += $(LIGHTDM_LIBS)
endif
BINS    := bin/w2kwm $(APPS)

all: $(BINS)

swatch: bin/w2kswatch

$(LIB): $(LIB_OBJ)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

bin/w2kwm: $(WM_OBJ) $(LIB)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(WM_OBJ) $(LIB) $(LDLIBS)

bin/%: apps/%.o $(LIB)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB) $(LDLIBS)

# Every object depends on the public headers; the WM also on its own.
HDRS := include/w2k.h include/w2kui.h
$(LIB_OBJ) $(patsubst apps/%.c,apps/%.o,$(wildcard apps/*.c)): $(HDRS)
$(WM_OBJ): $(HDRS) wm/wm.h
lib/icon.o: lib/icon_data.inc

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

clean:
	rm -f lib/*.o wm/*.o apps/*.o $(LIB)
	rm -rf bin

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m755 $(BINS) $(DESTDIR)$(BINDIR)
	install -m755 w2k-session $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(PREFIX)/share/w2k/skins
	install -m644 skins/*.png $(DESTDIR)$(PREFIX)/share/w2k/skins
	install -d $(DESTDIR)$(PREFIX)/share/w2k/cursors
	install -m644 cursors/* $(DESTDIR)$(PREFIX)/share/w2k/cursors
	install -d $(DESTDIR)/usr/share/xgreeters
	install -m644 config/w2klogon.desktop $(DESTDIR)/usr/share/xgreeters/w2klogon.desktop

.PHONY: all clean install swatch
.PRECIOUS: apps/%.o
