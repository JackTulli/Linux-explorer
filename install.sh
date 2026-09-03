#!/bin/sh
# install.sh -- set up the Windows 2000 desktop on any Linux.
#
#   ./install.sh                 everything: packages, build, install, theme
#   ./install.sh --prefix DIR    install under DIR (default /usr/local)
#   ./install.sh --no-deps       do not touch the package manager
#   ./install.sh --no-build      do not compile or install the binaries
#   ./install.sh --no-theme      skip Chicago95 (needs the network)
#   ./install.sh --tahoma        fetch Tahoma from the corefonts project
#   ./install.sh --xinitrc       make w2k-session your startx session
#   ./install.sh --user-only     only this user's configuration
#   ./install.sh --full          a bare system too: X server, login manager,
#                                sound, guest tools, a browser
#   ./install.sh --user NAME     the user to configure (when run as root)
#   ./install.sh --dry-run       say what would be done
#
# What it does, in order:
#   1. installs the build and runtime packages for your distribution
#      (Debian/Ubuntu, Fedora/RHEL, Arch, openSUSE, Alpine, Void);
#   2. builds and installs the shell (make install), the cursor set and a
#      session entry for display managers;
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

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
run() { if [ "$DRY" = 1 ]; then echo "  + $*"; else "$@"; fi; }
as_root() {
    if [ "$(id -u)" = 0 ]; then run "$@"
    elif command -v sudo >/dev/null 2>&1; then run sudo "$@"
    elif command -v doas >/dev/null 2>&1; then run doas "$@"
    else echo "install.sh: need root for: $*" >&2; exit 1; fi
}
backup() { [ -e "$1" ] && [ ! -e "$1.pre-w2k" ] && run cp -a "$1" "$1.pre-w2k" || true; }

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
        as_root apt-get install -y build-essential libx11-dev libxext-dev libxrandr-dev \
            libxcursor-dev libxft-dev libfontconfig1-dev libfreetype-dev zlib1g-dev \
            libjpeg-dev x11-xserver-utils x11-utils xdg-utils zip unzip tar p7zip-full \
            pulseaudio-utils xterm python3 git curl fonts-dejavu-core dbus-x11 \
            cabextract qt5ct qt6ct ;;
    *fedora*|*rhel*|*centos*|*rocky*|*alma*)
        as_root dnf install -y gcc make libX11-devel libXext-devel libXrandr-devel \
            libXcursor-devel libXft-devel fontconfig-devel freetype-devel zlib-devel \
            libjpeg-turbo-devel xrandr xset xsetroot xrdb xmessage xdg-utils zip unzip \
            tar p7zip p7zip-plugins pulseaudio-utils xterm python3 git curl \
            dejavu-sans-fonts dbus-x11 cabextract qt5ct qt6ct ;;
    *arch*|*manjaro*|*endeavouros*)
        as_root pacman -Sy --needed --noconfirm base-devel libx11 libxext libxrandr \
            libxcursor libxft fontconfig freetype2 zlib libjpeg-turbo xorg-xrandr \
            xorg-xset xorg-xsetroot xorg-xrdb xorg-xmessage xdg-utils zip unzip tar \
            p7zip libpulse xterm python git curl ttf-dejavu dbus cabextract qt5ct qt6ct ;;
    *suse*)
        as_root zypper --non-interactive install gcc make libX11-devel libXext-devel \
            libXrandr-devel libXcursor-devel libXft-devel fontconfig-devel \
            freetype2-devel zlib-devel libjpeg8-devel xrandr xset xsetroot xrdb xmessage \
            xdg-utils zip unzip tar p7zip-full pulseaudio-utils xterm python3 git curl \
            dejavu-fonts dbus-1-x11 cabextract qt5ct qt6ct ;;
    *alpine*)
        as_root apk add build-base libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
            libxft-dev fontconfig-dev freetype-dev zlib-dev libjpeg-turbo-dev xrandr \
            xset xsetroot xrdb xmessage xdg-utils zip unzip tar p7zip pulseaudio-utils \
            xterm python3 git curl font-dejavu dbus-x11 cabextract ;;
    *void*)
        as_root xbps-install -Sy base-devel libX11-devel libXext-devel libXrandr-devel \
            libXcursor-devel libXft-devel fontconfig-devel freetype-devel zlib-devel \
            libjpeg-turbo-devel xrandr xset xsetroot xrdb xmessage xdg-utils zip unzip \
            tar p7zip pulseaudio-utils xterm python3 git curl dejavu-fonts-ttf dbus \
            cabextract qt5ct qt6ct ;;
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
    say "Installing the X server, login manager and desktop essentials"
    case "$fam" in
    *debian*|*ubuntu*)
        as_root apt-get install -y xserver-xorg xinit xserver-xorg-video-all \
            xserver-xorg-input-all lightdm lightdm-gtk-greeter xfonts-base \
            fonts-liberation pulseaudio pavucontrol alsa-utils spice-vdagent \
            xdg-user-dirs desktop-file-utils shared-mime-info firefox-esr \
            policykit-1 dbus-user-session ;;
    *fedora*|*rhel*|*centos*|*rocky*|*alma*)
        as_root dnf install -y xorg-x11-server-Xorg xorg-x11-xinit xorg-x11-drivers \
            lightdm lightdm-gtk liberation-fonts pulseaudio-utils pavucontrol \
            spice-vdagent xdg-user-dirs desktop-file-utils shared-mime-info firefox \
            polkit ;;
    *arch*|*manjaro*|*endeavouros*)
        as_root pacman -S --needed --noconfirm xorg-server xorg-xinit xf86-video-vesa \
            xf86-video-vmware xf86-video-qxl lightdm lightdm-gtk-greeter \
            ttf-liberation pipewire pipewire-pulse pavucontrol spice-vdagent \
            xdg-user-dirs desktop-file-utils shared-mime-info firefox polkit ;;
    *suse*)
        as_root zypper --non-interactive install xorg-x11-server xinit lightdm \
            lightdm-gtk-greeter liberation-fonts pulseaudio pavucontrol spice-vdagent \
            xdg-user-dirs desktop-file-utils shared-mime-info MozillaFirefox polkit ;;
    *alpine*)
        as_root apk add xorg-server xinit xf86-video-vesa xf86-input-libinput lightdm \
            lightdm-gtk-greeter font-liberation pulseaudio pavucontrol spice-vdagent \
            xdg-user-dirs desktop-file-utils shared-mime-info firefox polkit ;;
    *void*)
        as_root xbps-install -Sy xorg-server xinit xf86-video-vesa lightdm \
            lightdm-gtk-greeter liberation-fonts-ttf pulseaudio pavucontrol \
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
    # A session entry, so display managers list "Windows 2000".
    if [ -d /usr/share/xsessions ] || [ "$DRY" = 1 ]; then
        as_root install -d /usr/share/xsessions
        as_root install -m644 "$HERE/config/w2k-session.desktop" /usr/share/xsessions/w2k-session.desktop
    fi
    if [ "$FULL" = 1 ]; then
        # LightDM logs straight into Windows 2000; the greeter looks the part.
        as_root install -d /etc/lightdm/lightdm.conf.d
        as_root sh -c "printf '[Seat:*]\nuser-session=w2k-session\ngreeter-session=lightdm-gtk-greeter\n' > /etc/lightdm/lightdm.conf.d/50-w2k.conf"
        as_root sh -c "printf '[greeter]\nbackground=#3a6ea5\ntheme-name=Chicago95\nicon-theme-name=Chicago95\nfont-name=Tahoma 8\ncursor-theme-name=Windows2000\n' > /etc/lightdm/lightdm-gtk-greeter.conf"
        if command -v systemctl >/dev/null 2>&1; then
            as_root systemctl enable lightdm 2>/dev/null || true
            as_root systemctl set-default graphical.target 2>/dev/null || true
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
    say "Done. Reboot, or: systemctl start lightdm"
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
        run sh -c "curl -sL -o '$tmp/IELPKTH.CAB' https://downloads.sourceforge.net/corefonts/IELPKTH.CAB && cd '$tmp' && cabextract -q -F 'tahoma*.ttf' IELPKTH.CAB && cp -f tahoma*.ttf '$HOME/.local/share/fonts/' && fc-cache -f '$HOME/.local/share/fonts'"
        rm -rf "$tmp"
    else
        echo "  cabextract is needed for --tahoma; skipped." >&2
    fi
fi

# startx: the session as this user's X session.
if [ "$DO_XINITRC" = 1 ]; then
    backup "$HOME/.xinitrc"
    run sh -c "printf '#!/bin/sh\nexec %s/bin/w2k-session\n' '$PREFIX' > '$HOME/.xinitrc'"
    run chmod +x "$HOME/.xinitrc"
fi

say "Done."
echo "  Start it with:  startx $PREFIX/bin/w2k-session"
echo "  or pick \"Windows 2000\" in your display manager. Explorer becomes the"
echo "  folder handler for other programs the first time the shell runs."
