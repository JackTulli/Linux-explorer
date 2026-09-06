#!/bin/sh
# install.sh -- set up Linux 2000, the Windows 2000-like desktop, on any Linux.
#
#   ./install.sh                 everything: packages, build, install, theme
#   ./install.sh --prefix DIR    install under DIR (default /usr/local)
#   ./install.sh --no-deps       do not touch the package manager
#   ./install.sh --no-build      do not compile or install the binaries
#   ./install.sh --no-theme      skip Chicago95 (needs the network)
#   ./install.sh --tahoma        fetch Tahoma from the corefonts project
#   ./install.sh --xinitrc       make l2k-session your startx session
#   ./install.sh --user-only     only this user's configuration
#   ./install.sh --full          a bare system too: X server, the logon screen,
#                                sound, guest tools, a browser
#   ./install.sh --user NAME     the user to configure (when run as root)
#   ./install.sh --dry-run       say what would be done
#
# What it does, in order:
#   1. installs the build and runtime packages for your distribution
#      (Debian/Ubuntu, Fedora/RHEL, Arch, openSUSE, Alpine, Void);
#   2. builds and installs the shell (make install), the cursor set, and
#      with --full the l2kdm service that boots into Log On to Windows;
#   3. for the user running it: the Windows cursor set, the Xcursor theme
#      other programs use, Chicago95 for GTK, the Windows style and 2000
#      palette for Qt, and the file manager as the folder handler.
# It is safe to run again; existing configuration files are backed up
# with a .pre-w2k suffix the first time they are replaced.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
PREFIX=/usr/local
DO_DEPS=1 DO_BUILD=1 DO_THEME=1 DO_XINITRC=0 DO_TAHOMA=0 USER_ONLY=0 DRY=0 FULL=0
TARGET_USER=''

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX=$2; shift ;;
        --no-deps) DO_DEPS=0 ;;
        --no-build) DO_BUILD=0 ;;
        --no-theme) DO_THEME=0 ;;
        --tahoma) DO_TAHOMA=1 ;;
        --xinitrc) DO_XINITRC=1 ;;
        --user-only) USER_ONLY=1; DO_DEPS=0; DO_BUILD=0 ;;
        --full) FULL=1 ;;
        --user) TARGET_USER=$2; shift ;;
        --dry-run) DRY=1 ;;
        -h|--help) sed -n 2,26p "$0"; exit 0 ;;
        *) echo "install.sh: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done

# Run as root through sudo, the caller is the user to set up.
if [ "$(id -u)" = 0 ] && [ -z "$TARGET_USER" ] && [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
    TARGET_USER=$SUDO_USER
fi

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
run() { if [ "$DRY" = 1 ]; then echo "  + $*"; else "$@"; fi; }
as_root() {
    if [ "$(id -u)" = 0 ]; then run "$@"
    elif command -v sudo >/dev/null 2>&1; then run sudo "$@"
    elif command -v doas >/dev/null 2>&1; then run doas "$@"
    else echo "install.sh: need root for: $*" >&2; exit 1; fi
}
backup() { [ -e "$1" ] && [ ! -e "$1.pre-w2k" ] && run cp -a "$1" "$1.pre-w2k" || true; }
# apt refuses the whole list over one unknown name, and names come and go
# between releases (policykit-1 became polkitd); install what the archive
# actually has and say what it lacked.
apt_install() {
    have='' miss=''
    for p in "$@"; do
        # A record can exist without a candidate (an obsoleted name), so ask
        # for the candidate, which is what apt would install.
        if apt-cache policy "$p" 2>/dev/null | grep -q '^ *Candidate: [0-9]'; then have="$have $p"; else miss="$miss $p"; fi
    done
    [ -z "$miss" ] || echo "  (not in this release, skipped:$miss)"
    # shellcheck disable=SC2086
    as_root apt-get install -y $have
}

# ------------------------------------------------------------------
# 1. Packages
# ------------------------------------------------------------------
if [ "$DO_DEPS" = 1 ]; then
    . /etc/os-release 2>/dev/null || true
    fam="$ID $ID_LIKE"
    say "Installing packages for ${PRETTY_NAME:-this system}"
    case "$fam" in
    *debian*|*ubuntu*)
        as_root apt-get update
        apt_install build-essential libx11-dev libxext-dev libxrandr-dev \
            libxcursor-dev libxft-dev libfontconfig1-dev libfreetype-dev zlib1g-dev \
            libjpeg-dev libwebp-dev libxss-dev x11-xserver-utils x11-utils xdg-utils zip unzip tar p7zip-full \
            pulseaudio-utils alsa-utils xterm python3 git curl fonts-dejavu-core dbus-x11 \
            cabextract qt5ct qt6ct libpam0g-dev xauth libdbus-1-dev libnotify-bin ;;
    *fedora*|*rhel*|*centos*|*rocky*|*alma*)
        # strict=0: a name this release no longer has is skipped, not fatal.
        as_root dnf install -y --setopt=strict=0 gcc make libX11-devel libXext-devel libXrandr-devel \
            libXcursor-devel libXft-devel fontconfig-devel freetype-devel zlib-devel \
            libjpeg-turbo-devel libwebp-devel libXScrnSaver-devel xrandr xset xsetroot xrdb xmessage xdg-utils zip unzip \
            tar p7zip p7zip-plugins pulseaudio-utils alsa-utils xterm python3 git curl \
            dejavu-sans-fonts dbus-x11 cabextract qt5ct qt6ct pam-devel xorg-x11-xauth dbus-devel libnotify ;;
    *arch*|*manjaro*|*endeavouros*)
        # -Syu, never -Sy: a refreshed database with an unrefreshed system
        # is the partial upgrade Arch warns about.
        as_root pacman -Syu --needed --noconfirm base-devel libx11 libxext libxrandr \
            libxcursor libxft fontconfig freetype2 zlib libjpeg-turbo libwebp libxss xorg-xrandr \
            xorg-xset xorg-xsetroot xorg-xrdb xorg-xmessage xdg-utils zip unzip tar \
            p7zip libpulse alsa-utils xterm python git curl ttf-dejavu dbus cabextract qt5ct qt6ct pam xorg-xauth libnotify ;;
    *suse*)
        as_root zypper --non-interactive install gcc make libX11-devel libXext-devel \
            libXrandr-devel libXcursor-devel libXft-devel fontconfig-devel \
            freetype2-devel zlib-devel libjpeg8-devel libwebp-devel libXss-devel xrandr xset xsetroot xrdb xmessage \
            xdg-utils zip unzip tar p7zip-full pulseaudio-utils alsa-utils xterm python3 git curl \
            dejavu-fonts dbus-1-x11 cabextract qt5ct qt6ct pam-devel xauth dbus-1-devel libnotify-tools ;;
    *alpine*)
        as_root apk add build-base libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
            libxft-dev fontconfig-dev freetype-dev zlib-dev libjpeg-turbo-dev libwebp-dev libxscrnsaver-dev xrandr \
            xset xsetroot xrdb xmessage xdg-utils zip unzip tar p7zip pulseaudio-utils \
            xterm python3 git curl font-dejavu dbus-x11 cabextract linux-pam-dev xauth dbus-dev libnotify ;;
    *void*)
        as_root xbps-install -Sy base-devel libX11-devel libXext-devel libXrandr-devel \
            libXcursor-devel libXft-devel fontconfig-devel freetype-devel zlib-devel \
            libjpeg-turbo-devel libwebp-devel libXScrnSaver-devel xrandr xset xsetroot xrdb xmessage xdg-utils zip unzip \
            tar p7zip pulseaudio-utils xterm python3 git curl dejavu-fonts-ttf dbus \
            cabextract qt5ct qt6ct pam-devel xauth dbus-devel libnotify ;;
    *)
        echo "install.sh: I do not know this distribution's package manager." >&2
        echo "  Install: a C compiler and make; the development packages for X11," >&2
        echo "  Xext, Xrandr, Xcursor, Xft, fontconfig, freetype, zlib and libjpeg;" >&2
        echo "  and xrandr, xrdb, xset, xsetroot, xmessage, xdg-utils, zip, unzip," >&2
        echo "  p7zip, pulseaudio-utils, python3, git, curl. Then rerun with --no-deps." >&2
        exit 1 ;;
    esac
fi

# --full: what a system with no desktop at all still needs -- the X server,
# a login manager that offers the session, sound, the guest agents a VM
# wants, user folders, and a browser to pin.
if [ "$DO_DEPS" = 1 ] && [ "$FULL" = 1 ]; then
    say "Installing the X server and desktop essentials"
    case "$fam" in
    *debian*|*ubuntu*)
        apt_install xserver-xorg xinit xserver-xorg-video-all \
            xserver-xorg-input-all xfonts-base \
            fonts-liberation pulseaudio pavucontrol alsa-utils spice-vdagent \
            xdg-user-dirs desktop-file-utils shared-mime-info firefox-esr firefox \
            polkitd pkexec policykit-1 dbus-user-session ;;
    *fedora*|*rhel*|*centos*|*rocky*|*alma*)
        as_root dnf install -y --setopt=strict=0 xorg-x11-server-Xorg xorg-x11-xinit xorg-x11-drivers \
            liberation-fonts pulseaudio-utils pavucontrol \
            spice-vdagent xdg-user-dirs desktop-file-utils shared-mime-info firefox \
            polkit ;;
    *arch*|*manjaro*|*endeavouros*)
        as_root pacman -Syu --needed --noconfirm xorg-server xorg-xinit xf86-video-vesa \
            xf86-video-vmware xf86-video-qxl \
            ttf-liberation pipewire pipewire-pulse pavucontrol spice-vdagent \
            xdg-user-dirs desktop-file-utils shared-mime-info firefox polkit ;;
    *suse*)
        as_root zypper --non-interactive install xorg-x11-server xinit \
            liberation-fonts pulseaudio pavucontrol spice-vdagent \
            xdg-user-dirs desktop-file-utils shared-mime-info MozillaFirefox polkit ;;
    *alpine*)
        as_root apk add xorg-server xinit xf86-video-vesa xf86-input-libinput \
            font-liberation pulseaudio pavucontrol spice-vdagent \
            xdg-user-dirs desktop-file-utils shared-mime-info firefox polkit ;;
    *void*)
        as_root xbps-install -Sy xorg-server xinit xf86-video-vesa \
            liberation-fonts-ttf pulseaudio pavucontrol \
            spice-vdagent xdg-user-dirs desktop-file-utils shared-mime-info firefox polkit ;;
    esac
fi

# ------------------------------------------------------------------
# 2. Build and install
# ------------------------------------------------------------------
if [ "$DO_BUILD" = 1 ]; then
    say "Building"
    run make -C "$HERE" -s
    say "Installing under $PREFIX"
    as_root make -C "$HERE" -s install PREFIX="$PREFIX"
    as_root install -d "$PREFIX/share/w2k/cursors"
    as_root sh -c "install -m644 '$HERE'/cursors/* '$PREFIX/share/w2k/cursors/'"
    # A desktop that is running picks the new build up in place: the
    # window manager restarts itself with every window kept (l2kwm
    # --restart), each session found by its l2kwm process.
    if [ "$DRY" != 1 ] && [ "$(id -u)" = 0 ]; then
        for pid in $(pgrep -x l2kwm 2>/dev/null; pgrep -x w2kwm 2>/dev/null); do
            u=$(stat -c %U "/proc/$pid" 2>/dev/null) || continue
            disp=$(tr '\0' '\n' < "/proc/$pid/environ" 2>/dev/null | sed -n 's/^DISPLAY=//p' | head -1)
            xauth=$(tr '\0' '\n' < "/proc/$pid/environ" 2>/dev/null | sed -n 's/^XAUTHORITY=//p' | head -1)
            [ -n "$disp" ] || continue
            say "Restarting the desktop of $u on $disp"
            su -s /bin/sh "$u" -c "DISPLAY='$disp' XAUTHORITY='${xauth:-$(getent passwd "$u" | cut -d: -f6)/.Xauthority}' '$PREFIX/bin/l2kwm' --restart" || true
        done
    fi
    # The Xcursor theme for every other program, system-wide too (Xcursor
    # looks in /usr/share/icons but not under an arbitrary prefix), and as
    # the system default when no other default is set.
    if command -v python3 >/dev/null 2>&1; then
        as_root python3 "$HERE/tools/gencursortheme.py" "$HERE/cursors" /usr/share/icons/Windows2000
        if [ ! -e /usr/share/icons/default/index.theme ] || [ "$DRY" = 1 ]; then
            as_root install -d /usr/share/icons/default
            as_root sh -c "printf '[Icon Theme]\nName=Default\nInherits=Windows2000\n' > /usr/share/icons/default/index.theme"
        fi
    fi
    # A session entry, so display managers list "Windows 2000".
    # (make install wrote the session and greeter entries, with full paths.)
    if [ "$FULL" = 1 ]; then
        # Our own display manager: the machine boots into "Log On to
        # Windows". LightDM and friends, if any, stand down.
        if [ "$DRY" != 1 ] && ! "$PREFIX/bin/l2kdm" --check 2>/dev/null; then
            echo "  l2kdm was built without PAM (no PAM development headers); nobody could log on." >&2
            echo "  Install them (libpam0g-dev / pam-devel) and rerun." >&2
            exit 1
        fi
        # The PAM stack in the words this distribution's stacks use.
        if [ -f /etc/pam.d/common-auth ]; then pamsrc=l2kdm.pam.debian
        elif [ -f /etc/pam.d/password-auth ]; then pamsrc=l2kdm.pam.fedora
        elif [ -f /etc/pam.d/base-auth ]; then pamsrc=l2kdm.pam.alpine
        else pamsrc=l2kdm.pam.generic; fi
        as_root install -m644 "$HERE/config/$pamsrc" /etc/pam.d/l2kdm
        as_root rm -f /etc/pam.d/w2kdm          # the name before 1.7
        # Inside a virtual machine the X server's hardware cursor is often
        # never drawn by the hypervisor (virtio-gpu, QXL, VirtualBox and
        # VMware with the modesetting driver): an invisible pointer. A
        # software cursor is drawn into the framebuffer and always shows.
        if command -v systemd-detect-virt >/dev/null 2>&1 && systemd-detect-virt --vm -q 2>/dev/null; then
            say "Virtual machine: software cursor for the X server"
            as_root install -d /etc/X11/xorg.conf.d
            as_root install -m644 "$HERE/config/20-w2k-vm-cursor.conf" /etc/X11/xorg.conf.d/20-w2k-vm-cursor.conf
        fi
        if [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
            as_root sh -c "sed 's|^ExecStart=.*|ExecStart=$PREFIX/bin/l2kdm|' '$HERE/config/l2kdm.service' > /etc/systemd/system/l2kdm.service"
            # Another display manager stands down at the next boot; it is
            # not stopped now, because this may well be running under it.
            for dm in lightdm gdm gdm3 sddm xdm lxdm slim ly greetd; do
                as_root systemctl disable "$dm" >/dev/null 2>&1 || true
            done
            # Whoever held the display-manager alias last leaves it, or
            # enable refuses.
            as_root rm -f /etc/systemd/system/display-manager.service
            # The unit was w2kdm before 1.7: hand over without two managers.
            if [ -f /etc/systemd/system/w2kdm.service ]; then
                as_root systemctl disable w2kdm >/dev/null 2>&1 || true
                as_root rm -f /etc/systemd/system/w2kdm.service
            fi
            as_root systemctl daemon-reload
            as_root systemctl enable l2kdm
            as_root systemctl set-default graphical.target >/dev/null 2>&1 || true
            echo "  l2kdm takes over the console at the next boot (or now: systemctl start l2kdm)."
        else
            echo "  No systemd here: start '$PREFIX/bin/l2kdm' as root at boot from your" >&2
            echo "  init system (it runs in the foreground and restarts the logon screen itself)." >&2
        fi
    fi
fi

# Run as root for someone else (the bootstrap does this): the user part of
# the job is handed to them, and the rest of this file is skipped.
if [ "$(id -u)" = 0 ] && [ -n "$TARGET_USER" ] && [ "$TARGET_USER" != root ]; then
    say "Configuring for $TARGET_USER"
    opts="--user-only"
    [ "$DO_THEME" = 1 ] || opts="$opts --no-theme"
    [ "$DO_TAHOMA" = 1 ] && opts="$opts --tahoma"
    [ "$DO_XINITRC" = 1 ] && opts="$opts --xinitrc"
    [ "$DRY" = 1 ] && opts="$opts --dry-run"
    run su -s /bin/sh "$TARGET_USER" -c "cd '$HERE' && ./install.sh $opts --prefix '$PREFIX'"
    run su -s /bin/sh "$TARGET_USER" -c "xdg-user-dirs-update >/dev/null 2>&1 || true"
    say "Done. Reboot, or: systemctl start l2kdm"
    exit 0
fi

# ------------------------------------------------------------------
# 3. This user's configuration
# ------------------------------------------------------------------
[ -n "$HOME" ] || { echo "install.sh: HOME is not set" >&2; exit 1; }
say "Configuring for $USER"
run mkdir -p "$HOME/.w2k/cursors" "$HOME/.icons/default" "$HOME/.themes" \
    "$HOME/.config/gtk-3.0" "$HOME/.config/gtk-4.0" "$HOME/.config/qt5ct/colors" \
    "$HOME/.config/qt6ct/colors" "$HOME/.local/share/fonts" "$HOME/.local/share/applications"

# The Windows cursor set, and the Xcursor theme every other program uses.
run sh -c "cp -f '$HERE'/cursors/* '$HOME/.w2k/cursors/'"
if command -v python3 >/dev/null 2>&1; then
    run python3 "$HERE/tools/gencursortheme.py" "$HOME/.w2k/cursors" "$HOME/.icons/Windows2000"
    backup "$HOME/.icons/default/index.theme"
    run sh -c "printf '[Icon Theme]\nName=Default\nInherits=Windows2000\n' > '$HOME/.icons/default/index.theme'"
    # ~/.local/share/icons comes first on Xcursor's path; a theme there of
    # the same name would win, so point that at ours as well.
    run mkdir -p "$HOME/.local/share/icons"
    [ -e "$HOME/.local/share/icons/Windows2000" ] || run ln -s "$HOME/.icons/Windows2000" "$HOME/.local/share/icons/Windows2000"
    [ -e "$HOME/.local/share/icons/default" ] || run ln -s "$HOME/.icons/default" "$HOME/.local/share/icons/default"
fi
# Say what the pointer setup came to, so a wrong pointer is not a mystery.
if [ "$DRY" != 1 ]; then
    ncur=$(ls "$HOME/.w2k/cursors"/*.cur 2>/dev/null | wc -l)
    nth=$(ls "$HOME/.icons/Windows2000/cursors" 2>/dev/null | wc -l)
    if [ "$ncur" -gt 0 ] && [ "$nth" -gt 0 ]; then
        echo "  Cursors: $ncur Windows cursors in ~/.w2k/cursors, Xcursor theme with $nth names"
    else
        echo "  WARNING: the cursor set did not install ($ncur .cur files, theme with $nth names)" >&2
    fi
fi

# Chicago95: the Windows 95/2000 look for GTK programs, and its icons.
if [ "$DO_THEME" = 1 ]; then
    if [ ! -d "$HOME/.themes/Chicago95" ] || { [ ! -d "$HOME/.icons/Chicago95" ] && [ ! -d "$HOME/.local/share/icons/Chicago95" ]; }; then
        say "Fetching Chicago95 (github.com/grassmunk/Chicago95)"
        tmp=$(mktemp -d)
        if command -v git >/dev/null 2>&1; then
            run git clone -q --depth 1 https://github.com/grassmunk/Chicago95 "$tmp/c95"
        else
            run sh -c "curl -sL https://github.com/grassmunk/Chicago95/archive/refs/heads/master.tar.gz | tar xz -C '$tmp' && mv '$tmp'/Chicago95-* '$tmp/c95'"
        fi
        if [ "$DRY" != 1 ]; then
            [ -d "$HOME/.themes/Chicago95" ] || cp -r "$tmp/c95/Theme/Chicago95" "$HOME/.themes/"
            mkdir -p "$HOME/.local/share/icons"
            [ -d "$HOME/.local/share/icons/Chicago95" ] || [ -d "$HOME/.icons/Chicago95" ] || \
                cp -r "$tmp/c95/Icons/Chicago95" "$HOME/.local/share/icons/"
            rm -rf "$tmp"
        fi
    else
        say "Chicago95 is already installed"
    fi
fi

# GTK 2, 3 and 4: the theme, the icons, the cursor, the font.
backup "$HOME/.gtkrc-2.0";              run cp -f "$HERE/config/gtk/gtkrc-2.0" "$HOME/.gtkrc-2.0"
backup "$HOME/.config/gtk-3.0/settings.ini"; run cp -f "$HERE/config/gtk/settings.ini" "$HOME/.config/gtk-3.0/settings.ini"
backup "$HOME/.config/gtk-4.0/settings.ini"; run cp -f "$HERE/config/gtk/settings.ini" "$HOME/.config/gtk-4.0/settings.ini"

# Qt: the Windows style with the Windows 2000 palette, through qt5ct/qt6ct.
for q in qt5ct qt6ct; do
    backup "$HOME/.config/$q/$q.conf"
    run cp -f "$HERE/config/$q/colors/Windows2000.conf" "$HOME/.config/$q/colors/"
    run sh -c "sed 's|~/.config|$HOME/.config|' '$HERE/config/$q/$q.conf' > '$HOME/.config/$q/$q.conf'"
done

# Tahoma, the shell's typeface, from the corefonts project's IE font pack.
if [ "$DO_TAHOMA" = 1 ] && ! fc-list 2>/dev/null | grep -qi tahoma; then
    if command -v cabextract >/dev/null 2>&1; then
        say "Fetching Tahoma"
        tmp=$(mktemp -d)
        # A mirror's error page is not a font: -f, and a miss is not fatal.
        run sh -c "curl -fsSL -o '$tmp/IELPKTH.CAB' https://downloads.sourceforge.net/corefonts/IELPKTH.CAB && cd '$tmp' && cabextract -q -F 'tahoma*.ttf' IELPKTH.CAB && cp -f tahoma*.ttf '$HOME/.local/share/fonts/' && fc-cache -f '$HOME/.local/share/fonts'" \
            || echo "  Tahoma could not be fetched; the shell falls back to DejaVu Sans." >&2
        rm -rf "$tmp"
    else
        echo "  cabextract is needed for --tahoma; skipped." >&2
    fi
fi

# startx: the session as this user's X session.
if [ "$DO_XINITRC" = 1 ]; then
    backup "$HOME/.xinitrc"
    run sh -c "printf '#!/bin/sh\nexec %s/bin/l2k-session\n' '$PREFIX' > '$HOME/.xinitrc'"
    run chmod +x "$HOME/.xinitrc"
fi

say "Done: $("$PREFIX/bin/l2kwm" --version 2>/dev/null || echo "l2kwm installed")"
echo "  Start it with:  startx $PREFIX/bin/l2k-session"
echo "  or pick \"Windows 2000\" in your display manager. Explorer becomes the"
echo "  folder handler for other programs the first time the shell runs."
