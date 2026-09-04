# Windows 2000 desktop environment for X11 -- build

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
LIBEXEC ?= $(PREFIX)/lib/w2k

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c99 -Wall -Wextra -Wno-unused-parameter -Iinclude
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
# Where skins, cursors, icons and wallpapers are looked for at run time.
CPPFLAGS += -DW2K_PREFIX=\"$(PREFIX)\"
# The release number, from the VERSION file; a git checkout adds its hash.
W2K_VERSION := $(shell cat VERSION)$(shell git rev-parse --short HEAD 2>/dev/null | sed "s/^/+/")
CPPFLAGS += -DW2K_VERSION=\"$(W2K_VERSION)\"
CFLAGS  += -I/usr/include/freetype2
LDLIBS  := -lX11 -lXext -lXrandr -lXcursor -lXft -lfontconfig -lz -ljpeg -lm

LIB_SRC := $(wildcard lib/*.c)
LIB_OBJ := $(LIB_SRC:.c=.o)
LIB     := lib/libw2k.a

WM_SRC  := $(wildcard wm/*.c)
WM_OBJ  := $(WM_SRC:.c=.o)

# w2kswatch is a development scratch tool: buildable, never installed.
APPS    := $(filter-out bin/w2kswatch,$(patsubst apps/%.c,bin/%,$(wildcard apps/*.c)))
# The display manager needs PAM; without its header it still builds, as the
# picture alone (W2K_RENDER).
ifneq ($(wildcard /usr/include/security/pam_appl.h),)
apps/w2kdm.o: CFLAGS += -DHAVE_PAM
bin/w2kdm: LDLIBS += -lpam
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
# The version stamp carries the commit, so the file that prints it is
# rebuilt when the commit changes.
wm/wm.o: VERSION $(wildcard .git/HEAD .git/refs/heads/*)

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
	# The switchable icon sets (Display Properties > Appearance > Icons).
	for s in icons/sets/*/; do n=$$(basename $$s); \
	    install -d $(DESTDIR)$(PREFIX)/share/w2k/icons/sets/$$n; \
	    install -m644 $$s*.ico $(DESTDIR)$(PREFIX)/share/w2k/icons/sets/$$n; done
	# A session entry for any other display manager that may be around --
	# where that directory can be written (a user prefix cannot).
	@if install -d $(DESTDIR)/usr/share/xsessions 2>/dev/null; then \
	    sed 's|^Exec=.*|Exec=$(BINDIR)/w2k-session|; /^TryExec/d' config/w2k-session.desktop > $(DESTDIR)/usr/share/xsessions/w2k-session.desktop; \
	else echo "(no /usr/share/xsessions entry: not writable)"; fi

.PHONY: all clean install swatch
.PRECIOUS: apps/%.o
