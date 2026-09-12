# Linux 2000, a Windows 2000-like desktop environment for X11 -- build

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
LDLIBS  := -lX11 -lXext -lXrandr -lXcursor -lXft -lXrender -lfontconfig -lz -ljpeg -lm

LIB_SRC := $(wildcard lib/*.c)
LIB_OBJ := $(LIB_SRC:%.c=build/%.o)
LIB     := lib/libw2k.a

WM_SRC  := $(wildcard wm/*.c)
WM_OBJ  := $(WM_SRC:%.c=build/%.o)

# l2kswatch is a development scratch tool: buildable, never installed.
APPS    := $(filter-out bin/l2kswatch,$(patsubst apps/%.c,bin/%,$(wildcard apps/*.c)))
# The nested compositor draws with OpenGL and needs the GLX, XTest, Damage
# and Fixes headers; without them it is left out and the option is absent.
ifeq ($(wildcard /usr/include/GL/glx.h),)
APPS    := $(filter-out bin/l2kscaler,$(APPS))
endif
bin/l2kscaler: LDLIBS += -lGL -lXtst -lXdamage -lXfixes -lXcomposite
# The display manager needs PAM; without its header it still builds, as the
# picture alone (W2K_RENDER).
ifneq ($(wildcard /usr/include/security/pam_appl.h),)
build/apps/l2kdm.o: CFLAGS += -DHAVE_PAM
bin/l2kdm: LDLIBS += -lpam
endif
# The notification service needs libdbus; without it the shell shows only
# its own balloons.
ifneq ($(shell pkg-config --exists libwebp 2>/dev/null && echo y),)
build/lib/image.o: CFLAGS += -DHAVE_WEBP $(shell pkg-config --cflags libwebp)
LDLIBS += $(shell pkg-config --libs libwebp)
endif
ifneq ($(shell pkg-config --exists xscrnsaver 2>/dev/null && echo y),)
build/wm/wm.o: CFLAGS += -DHAVE_XSS
bin/l2kwm: LDLIBS += -lXss -lXcomposite
endif
ifneq ($(shell pkg-config --exists dbus-1 2>/dev/null && echo y),)
build/wm/notifyd.o: CFLAGS += -DHAVE_DBUS $(shell pkg-config --cflags dbus-1)
bin/l2kwm: LDLIBS += $(shell pkg-config --libs dbus-1)
# The file chooser portal is a D-Bus service or nothing.
build/apps/l2kportal.o: CFLAGS += $(shell pkg-config --cflags dbus-1)
bin/l2kportal: LDLIBS += $(shell pkg-config --libs dbus-1)
else
APPS    := $(filter-out bin/l2kportal,$(APPS))
endif
BINS    := bin/l2kwm $(APPS)

all: $(BINS)

swatch: bin/l2kswatch

$(LIB): $(LIB_OBJ)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

bin/l2kwm: $(WM_OBJ) $(LIB)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(WM_OBJ) $(LIB) $(LDLIBS)

bin/%: build/apps/%.o $(LIB)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB) $(LDLIBS)

# Every object depends on the public headers; the WM also on its own.
HDRS := include/w2k.h include/w2kui.h
$(LIB_OBJ) $(patsubst apps/%.c,build/apps/%.o,$(wildcard apps/*.c)): $(HDRS)
$(WM_OBJ): $(HDRS) wm/wm.h
build/lib/icon.o: lib/icon_data.inc
# The version stamp carries the commit, so the file that prints it is
# rebuilt when the commit changes.
build/wm/wm.o build/lib/sysprops.o: VERSION $(wildcard .git/HEAD .git/refs/heads/*)

build/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

clean:
	rm -rf build bin
	rm -f $(LIB)

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m755 $(BINS) $(DESTDIR)$(BINDIR)
	install -m755 l2k-session $(DESTDIR)$(BINDIR)
	# The programs were called w2k* until 1.7; the old names keep working
	# (saved associations, scripts, a running w2kwm's --restart).
	for b in $(notdir $(BINS)) l2k-session; do \
	    ln -sfn $$b $(DESTDIR)$(BINDIR)/w2k$${b#l2k}; done
	install -d $(DESTDIR)$(PREFIX)/share/w2k/skins
	install -m644 skins/*.png $(DESTDIR)$(PREFIX)/share/w2k/skins
	# The sound packs (Sounds and Multimedia in Control Panel).
	for s in sounds/*/; do n=$$(basename $$s); \
	    install -d $(DESTDIR)$(PREFIX)/share/w2k/sounds/$$n; \
	    install -m644 $$s*.wav $(DESTDIR)$(PREFIX)/share/w2k/sounds/$$n; done
	install -d "$(DESTDIR)$(PREFIX)/share/w2k/wallpapers"
	install -m644 wallpapers/* "$(DESTDIR)$(PREFIX)/share/w2k/wallpapers"
	install -d $(DESTDIR)$(PREFIX)/share/w2k/cursors
	install -m644 cursors/* $(DESTDIR)$(PREFIX)/share/w2k/cursors
	# The switchable icon sets (Display Properties > Appearance > Icons).
	for s in icons/sets/*/; do n=$$(basename $$s); \
	    install -d $(DESTDIR)$(PREFIX)/share/w2k/icons/sets/$$n; \
	    install -m644 $$s*.ico $(DESTDIR)$(PREFIX)/share/w2k/icons/sets/$$n; done
	# A session entry for any other display manager that may be around --
	# where that directory can be written (a user prefix cannot).
	@if install -d $(DESTDIR)/usr/share/xsessions 2>/dev/null; then \
	    sed 's|^Exec=.*|Exec=$(BINDIR)/l2k-session|; /^TryExec/d' config/l2k-session.desktop > $(DESTDIR)/usr/share/xsessions/l2k-session.desktop; \
	    rm -f $(DESTDIR)/usr/share/xsessions/w2k-session.desktop; \
	else echo "(no /usr/share/xsessions entry: not writable)"; fi
	# Disk Management asks for the administrator through pkexec: its polkit
	# action carries the prompt and lets the display through.
	@if install -d $(DESTDIR)/usr/share/polkit-1/actions 2>/dev/null; then \
	    sed 's|@BINDIR@|$(BINDIR)|' config/org.linux2000.diskmgmt.policy.in > $(DESTDIR)/usr/share/polkit-1/actions/org.linux2000.diskmgmt.policy; \
	else echo "(no polkit action: /usr/share/polkit-1/actions not writable)"; fi
	# The session target that lets graphical-session.target, and with it
	# xdg-desktop-portal, run under this desktop.
	install -d $(DESTDIR)$(PREFIX)/share/systemd/user
	install -m644 config/l2k-session.target $(DESTDIR)$(PREFIX)/share/systemd/user
	# File choosing for Flatpak programs in the shell's own dialogs: the
	# portal back end, its D-Bus activation and which portals this desktop
	# uses. xdg-desktop-portal reads its back ends from /usr/share only.
	@if [ -x $(DESTDIR)$(BINDIR)/l2kportal ] && install -d $(DESTDIR)/usr/share/xdg-desktop-portal/portals 2>/dev/null && \
	    install -d $(DESTDIR)/usr/share/dbus-1/services 2>/dev/null; then \
	    install -m644 config/w2k.portal $(DESTDIR)/usr/share/xdg-desktop-portal/portals/w2k.portal; \
	    install -m644 config/w2k-portals.conf $(DESTDIR)/usr/share/xdg-desktop-portal/w2k-portals.conf; \
	    sed 's|@BINDIR@|$(BINDIR)|' config/org.freedesktop.impl.portal.desktop.w2k.service.in \
	        > $(DESTDIR)/usr/share/dbus-1/services/org.freedesktop.impl.portal.desktop.w2k.service; \
	else echo "(no file chooser portal: no l2kportal, or /usr/share not writable)"; fi

.PHONY: all clean install swatch
.PRECIOUS: build/apps/%.o