#!/bin/sh
# install.sh -- set up Linux 2000, the Windows 2000-like desktop, on any Linux.
#
#   ./install.sh                 asks what to install, then does it
#   ./install.sh --setup NAME    full, standard, light or custom -- no questions
#                                (or W2K_SETUP=name in the environment)
#   ./install.sh --looks | --no-looks        the XP, Vista, 7 and Modern looks
#   ./install.sh --all-apps | --basic-apps   every built-in program, or the few
#   ./install.sh --wireless | --no-wireless  Bluetooth Devices and Wi-Fi
#   ./install.sh --windows | --no-windows    Wine and Proton for Windows programs
#   ./install.sh --yes           take the answers as they stand; ask nothing
#   ./install.sh --prefix DIR    install under DIR (default /usr/local)
#   ./install.sh --no-deps       do not touch the package manager
#   ./install.sh --no-build      do not compile or install the binaries
#   ./install.sh --no-theme      skip Chicago95 and the XP, Vista and 7 themes
#                                for GTK and Qt programs (they need the network)
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
#      palette for Qt, B00merang's Windows XP, Vista and 7 themes and icons
#      and the Windows 7 Kvantum theme (which the looks switch to), and the
#      file manager as the folder handler; and for the machine: brightnessctl
#      with the backlight udev rule and the video group, and a PolicyKit
#      agent for pkexec.
# Four parts can be left out, and are asked about at a terminal: the looks
# besides the classic one, the programs beyond the basic ones, Bluetooth
# and Wi-Fi, and Windows programs. What was chosen is remembered in
# <prefix>/share/w2k/setup.conf and used again the next time, so an update
# asks nothing; --setup or the flags above change it. A part left out is
# also taken away, so a machine moved to the light setup keeps nothing of
# what it had.
# It is safe to run again; existing configuration files are backed up
# with a .pre-w2k suffix the first time they are replaced.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
PREFIX=/usr/local
DO_DEPS=1 DO_BUILD=1 DO_THEME=1 DO_XINITRC=0 DO_TAHOMA=0 USER_ONLY=0 DRY=0 FULL=0
TARGET_USER=''
# What to install. CHOSE is set by a flag or the environment: then nothing
# is asked and the last run's choice is not read back.
SETUP=full WANT_LOOKS=1 WANT_APPS=all WANT_WIRELESS=1 WANT_WINDOWS=1 CHOSE=0 ASK=0 NOASK=0

preset() {
    case "$1" in
        full)     WANT_LOOKS=1 WANT_APPS=all   WANT_WIRELESS=1 WANT_WINDOWS=1 ;;
        standard) WANT_LOOKS=1 WANT_APPS=all   WANT_WIRELESS=1 WANT_WINDOWS=0 ;;
        light)    WANT_LOOKS=0 WANT_APPS=basic WANT_WIRELESS=0 WANT_WINDOWS=0 ;;
        custom)   ASK=1 ;;
        *) echo "install.sh: unknown setup '$1' (full, standard, light, custom)" >&2; exit 2 ;;
    esac
    SETUP=$1
}
if [ -n "${W2K_SETUP:-}" ]; then preset "$W2K_SETUP"; CHOSE=1; fi

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
        --setup) preset "$2"; CHOSE=1; shift ;;
        --looks) WANT_LOOKS=1; SETUP=custom; CHOSE=1 ;;
        --no-looks) WANT_LOOKS=0; SETUP=custom; CHOSE=1 ;;
        --all-apps) WANT_APPS=all; SETUP=custom; CHOSE=1 ;;
        --basic-apps) WANT_APPS=basic; SETUP=custom; CHOSE=1 ;;
        --wireless) WANT_WIRELESS=1; SETUP=custom; CHOSE=1 ;;
        --no-wireless) WANT_WIRELESS=0; SETUP=custom; CHOSE=1 ;;
        --windows) WANT_WINDOWS=1; SETUP=custom; CHOSE=1 ;;
        --no-windows) WANT_WINDOWS=0; SETUP=custom; CHOSE=1 ;;
        --yes|-y) NOASK=1 ;;
        --user) TARGET_USER=$2; shift ;;
        --dry-run) DRY=1 ;;
        -h|--help) sed -n '2,${/^#/!q;p;}' "$0"; exit 0 ;;
        *) echo "install.sh: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done

# Run as root through sudo, the caller is the user to set up.
if [ "$(id -u)" = 0 ] && [ -z "$TARGET_USER" ] && [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
    TARGET_USER=$SUDO_USER
fi
# Run as root with nobody named -- a console login on a fresh machine --
# the desktop is meant for the person who uses it, not for root: take the
# first ordinary account, as the one-command install does, and say whose
# it is. --user root sets root's own up instead.
if [ "$(id -u)" = 0 ] && [ -z "$TARGET_USER" ] && [ "$USER_ONLY" != 1 ]; then
    TARGET_USER=$(getent passwd 2>/dev/null | awk -F: '$3 >= 1000 && $3 < 60000 && $7 !~ /nologin|false/ { print $1; exit }')
fi

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
run() { if [ "$DRY" = 1 ]; then echo "  + $*"; else "$@"; fi; }
as_root() {
    if [ "$(id -u)" = 0 ]; then run "$@"
    elif command -v sudo >/dev/null 2>&1; then run sudo "$@"
    elif command -v doas >/dev/null 2>&1; then run doas "$@"
    else echo "install.sh: need root for: $*" >&2; exit 1; fi
}
summary() {
    echo "  This setup: $SETUP"
    if [ "$WANT_LOOKS" = 1 ]; then
        echo "    Looks       classic, XP, Vista, Windows 7 (Aero) and Modern"
    else
        echo "    Looks       the classic colour schemes only  (--looks adds the rest)"
    fi
    if [ "$WANT_APPS" = all ]; then
        echo "    Programs    all of them"
    else
        echo "    Programs    Explorer, Notepad, Calculator, Task Manager, Control"
        echo "                Panel, Display, Windows Update  (--all-apps adds Paint,"
        echo "                Imaging, Character Map, Device Manager, Disk Management)."
        echo "                What is not installed is greyed in the Start menu and"
        echo "                left out of Control Panel."
    fi
    if [ "$WANT_WIRELESS" = 1 ]; then
        echo "    Wireless    Wi-Fi and Bluetooth Devices"
    else
        echo "    Wireless    none  (--wireless adds Wi-Fi and Bluetooth Devices)"
    fi
    if [ "$WANT_WINDOWS" = 1 ]; then
        echo "    Windows     Wine, and Proton Manager for games"
    else
        echo "    Windows     none  (--windows adds Wine and Proton Manager)"
    fi
    echo "  Run install.sh again with those to add a part, or --setup full for all of it."
    if [ "$FULL" != 1 ]; then
        echo "  The boot was left alone: no logon screen, and your display manager (if any)"
        echo "  still runs. install.sh --full installs l2kdm and boots into Log On to Windows."
    fi
}
# The window managers that are running, by name. BusyBox has no pgrep
# (Alpine), so /proc answers instead.
wm_pids() {
    if command -v pgrep >/dev/null 2>&1; then
        pgrep -x l2kwm 2>/dev/null
        pgrep -x w2kwm 2>/dev/null
    else
        for c in /proc/[0-9]*/comm; do
            read -r n < "$c" 2>/dev/null || continue
            case "$n" in
                l2kwm|w2kwm) p=${c#/proc/}; echo "${p%/comm}" ;;
            esac
        done
    fi
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

# Alpine keeps most of a desktop -- the X server and its tools, sound,
# notifications, Wine, NetworkManager, even xterm -- in the community
# repository, which a fresh install leaves switched off. apk then refuses
# the whole list at once ("no such package"), so it goes on first.
alpine_community() {
    f=/etc/apk/repositories
    if grep -rqs '^[^#]*/community' "$f" /etc/apk/repositories.d 2>/dev/null; then return 0; fi
    if [ -f "$f" ] && grep -q '^[[:space:]]*#.*/community' "$f"; then
        say "Turning on Alpine's community repository"
        as_root sed -i 's|^[[:space:]]*#[[:space:]]*\(.*/community\)|\1|' "$f"
    else
        # No line to uncomment: build one from the mirror main comes from,
        # or from this machine's own Alpine version.
        m=$(sed -n 's|^\(https*://[^#[:space:]]*\)/main[[:space:]]*$|\1|p' "$f" 2>/dev/null | head -1)
        if [ -z "$m" ]; then
            v=$(sed -n 's|^\([0-9][0-9]*\.[0-9][0-9]*\).*|\1|p' /etc/alpine-release 2>/dev/null | head -1)
            [ -n "$v" ] && m="https://dl-cdn.alpinelinux.org/alpine/v$v"
        fi
        if [ -z "$m" ]; then
            echo "  (could not work out the community repository; add it to $f by hand)" >&2
            return 0
        fi
        say "Adding Alpine's community repository"
        as_root sh -c "echo '$m/community' >> '$f'"
    fi
    as_root apk update
}

# ------------------------------------------------------------------
# 0. What to install
# ------------------------------------------------------------------
SETUP_FILE="$PREFIX/share/w2k/setup.conf"

# The programs. The basic ones are the shell and what a desktop is not a
# desktop without; the rest are asked about.
APPS_BASIC="l2kwm l2kexplorer l2knotepad l2kcalc l2ktaskmgr l2kcontrol l2kdisplay l2kupdate l2knotify linver"
APPS_EXTRA="l2kpaint l2kimage l2ksnip l2kcharmap l2kdevmgmt l2kdiskmgmt l2kpicker l2kportal l2kscaler l2kdm"
APPS_WIRELESS="l2knetwork l2kbluetooth"
APPS_WINDOWS="l2kproton"

# A terminal to ask at: /dev/tty is there but cannot be opened when the
# script runs with no controlling terminal (cloud-init, a container), and
# then nothing is asked.
# (in a subshell: a redirection that fails on a special built-in like :
# would end the script itself, silently.)
have_tty() { ( : < /dev/tty ) 2>/dev/null && ( : > /dev/tty ) 2>/dev/null; }

ask_yn() {      # question default(y|n): 0 for yes
    if [ "$2" = y ]; then _p='[Y/n]'; else _p='[y/N]'; fi
    printf '  %s %s ' "$1" "$_p" > /dev/tty
    read -r _a < /dev/tty || _a=''
    [ -n "$_a" ] || _a=$2
    case "$_a" in [Yy]*) return 0 ;; *) return 1 ;; esac
}

ask_setup() {
    cat > /dev/tty <<'MENU'

  What would you like installed?

    1. Everything  every look, every program, Bluetooth and Wi-Fi, and
                   Windows programs through Wine and Proton
    2. Standard    every look and program, without Wine and Proton
    3. Light       the shell and the basic programs -- Explorer, Notepad,
                   Calculator, Task Manager, Control Panel, Display
                   Properties, Windows Update -- and the classic look alone
    4. Choose      answer for each part

MENU
    printf '  Which one? [1] ' > /dev/tty
    read -r _n < /dev/tty || _n=''
    case "$_n" in
        2) preset standard ;;
        3) preset light ;;
        4) SETUP=custom
           ask_yn "The XP, Vista, Windows 7 (Aero) and Modern looks, with their wallpapers, sounds, icon sets and themes for other programs?" y || WANT_LOOKS=0
           ask_yn "Every built-in program -- Paint, Imaging, Snipping Tool, Character Map, Device Manager, Disk Management?" y || WANT_APPS=basic
           ask_yn "Bluetooth Devices and Wi-Fi?" y || WANT_WIRELESS=0
           ask_yn "Windows programs, through Wine and Proton Manager?" y || WANT_WINDOWS=0 ;;
        *) preset full ;;
    esac
}

# The user's own pass is told what to do by the root pass; otherwise the
# choice comes from the flags, the last run, or the question.
if [ "$USER_ONLY" != 1 ]; then
    if [ "$CHOSE" = 0 ] && [ -r "$SETUP_FILE" ]; then
        while IFS='=' read -r k v; do
            case "$k" in
                SETUP) SETUP=$v ;; LOOKS) WANT_LOOKS=$v ;; APPS) WANT_APPS=$v ;;
                WIRELESS) WANT_WIRELESS=$v ;; WINDOWS) WANT_WINDOWS=$v ;;
            esac
        done < "$SETUP_FILE"
        say "Keeping the $SETUP setup chosen last time (--setup changes it)"
    elif [ "$CHOSE" = 0 ] && [ "$NOASK" = 0 ] && have_tty; then
        ask_setup
    elif [ "$ASK" = 1 ]; then
        if have_tty; then ask_setup
        else echo "install.sh: --setup custom needs a terminal to ask at" >&2; exit 2; fi
    fi
fi

# ------------------------------------------------------------------
# 1. Packages
# ------------------------------------------------------------------
if [ "$DO_DEPS" = 1 ]; then
    . /etc/os-release 2>/dev/null || true
    fam="$ID $ID_LIKE"
    say "Installing packages for ${PRETTY_NAME:-this system}"
    # The first list is what the build needs and what every setup uses. The
    # four after it belong to the parts that can be left out, and are put in
    # further down with PM, which is how this system installs a package.
    PM='' PKG_LOOKS='' PKG_APPS='' PKG_WIRELESS='' PKG_WINDOWS=''
    case "$fam" in
    *debian*|*ubuntu*)
        as_root apt-get update
        apt_install build-essential libx11-dev libxext-dev libxrandr-dev \
            libxcursor-dev libxft-dev libfontconfig1-dev libfreetype-dev zlib1g-dev \
            libjpeg-dev libwebp-dev libxss-dev x11-xserver-utils x11-utils xdg-utils zip unzip tar p7zip-full \
            pulseaudio-utils alsa-utils xterm python3 git curl fonts-dejavu-core dbus-x11 \
            cabextract libpam0g-dev xauth libdbus-1-dev libnotify-bin lxpolkit brightnessctl \
            xserver-xephyr xvfb libgl1-mesa-dev libxtst-dev libxdamage-dev libxfixes-dev libxcomposite-dev libxi-dev
        PM=apt_install
        PKG_LOOKS="qt5ct qt6ct qt5-style-plugins qt-style-kvantum"
        PKG_APPS="udisks2 xdg-desktop-portal xdg-desktop-portal-gtk dosfstools exfatprogs ntfs-3g"
        PKG_WIRELESS="network-manager rfkill bluez"
        PKG_WINDOWS="wine icoutils" ;;
    *fedora*|*rhel*|*centos*|*rocky*|*alma*)
        # strict=0: a name this release no longer has is skipped, not fatal.
        as_root dnf install -y --setopt=strict=0 gcc make libX11-devel libXext-devel libXrandr-devel \
            libXcursor-devel libXft-devel fontconfig-devel freetype-devel zlib-devel \
            libjpeg-turbo-devel libwebp-devel libXScrnSaver-devel xrandr xset xsetroot xrdb xmessage xdg-utils zip unzip \
            tar p7zip p7zip-plugins pulseaudio-utils alsa-utils xterm python3 git curl \
            dejavu-sans-fonts dbus-x11 cabextract pam-devel xorg-x11-xauth dbus-devel libnotify \
            lxpolkit brightnessctl \
            xorg-x11-server-Xephyr xorg-x11-server-Xvfb mesa-libGL-devel libXtst-devel libXdamage-devel libXfixes-devel libXcomposite-devel libXi-devel
        PM="as_root dnf install -y --setopt=strict=0"
        PKG_LOOKS="qt5ct qt6ct qt5-qtstyleplugins kvantum kvantum-qt5"
        PKG_APPS="udisks2 xdg-desktop-portal xdg-desktop-portal-gtk dosfstools exfatprogs ntfsprogs"
        PKG_WIRELESS="NetworkManager bluez"
        PKG_WINDOWS="wine icoutils" ;;
    *arch*|*manjaro*|*endeavouros*)
        # -Syu, never -Sy: a refreshed database with an unrefreshed system
        # is the partial upgrade Arch warns about.
        as_root pacman -Syu --needed --noconfirm base-devel libx11 libxext libxrandr \
            libxcursor libxft fontconfig freetype2 zlib libjpeg-turbo libwebp libxss xorg-xrandr \
            xorg-xset xorg-xsetroot xorg-xrdb xorg-xmessage xdg-utils zip unzip tar \
            p7zip libpulse alsa-utils xterm python git curl ttf-dejavu dbus cabextract pam xorg-xauth libnotify \
            polkit-gnome brightnessctl \
            xorg-server-xephyr xorg-server-xvfb mesa libxtst libxdamage libxfixes libxcomposite libxi
        PM="as_root pacman -S --needed --noconfirm"
        PKG_LOOKS="qt5ct qt6ct kvantum kvantum-qt5"
        PKG_APPS="udisks2 xdg-desktop-portal xdg-desktop-portal-gtk dosfstools exfatprogs ntfs-3g"
        PKG_WIRELESS="networkmanager rfkill bluez bluez-utils"
        PKG_WINDOWS="wine icoutils" ;;
    *suse*)
        as_root zypper --non-interactive install gcc make libX11-devel libXext-devel \
            libXrandr-devel libXcursor-devel libXft-devel fontconfig-devel \
            freetype2-devel zlib-devel libjpeg8-devel libwebp-devel libXss-devel xrandr xset xsetroot xrdb xmessage \
            xdg-utils zip unzip tar p7zip-full pulseaudio-utils alsa-utils xterm python3 git curl \
            dejavu-fonts dbus-1-x11 cabextract pam-devel xauth dbus-1-devel libnotify-tools brightnessctl \
            libXcomposite-devel libXi-devel
        PM="as_root zypper --non-interactive install"
        PKG_LOOKS="qt5ct qt6ct"
        PKG_APPS="udisks2 xdg-desktop-portal xdg-desktop-portal-gtk dosfstools exfatprogs ntfs-3g ntfsprogs"
        PKG_WIRELESS="NetworkManager bluez"
        PKG_WINDOWS="wine icoutils" ;;
    *alpine*)
        alpine_community
        # linux-headers: build-base does not bring the kernel headers on
        # musl, and the device list reads udev over a netlink socket.
        as_root apk add build-base linux-headers libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
            libxft-dev fontconfig-dev freetype-dev zlib-dev libjpeg-turbo-dev libwebp-dev libxscrnsaver-dev xrandr \
            xset xsetroot xrdb xmessage xdg-utils zip unzip tar p7zip pulseaudio-utils alsa-utils \
            xterm python3 git curl font-dejavu dbus-x11 cabextract linux-pam-dev xauth dbus-dev libnotify \
            polkit-gnome brightnessctl \
            mesa-dev libxtst-dev libxdamage-dev libxfixes-dev libxcomposite-dev libxi-dev
        PM="as_root apk add"
        # Alpine has no qt5ct or qt6ct, so Qt programs keep their own
        # colours there; Kvantum carries the Windows 7 theme.
        PKG_LOOKS="kvantum"
        PKG_APPS="udisks2 xdg-desktop-portal xdg-desktop-portal-gtk dosfstools exfatprogs ntfs-3g-progs"
        PKG_WIRELESS="networkmanager bluez"
        # gcompat: Proton's own builds are glibc programs.
        PKG_WINDOWS="wine icoutils gcompat" ;;
    *void*)
        as_root xbps-install -Sy base-devel libX11-devel libXext-devel libXrandr-devel \
            libXcursor-devel libXft-devel fontconfig-devel freetype-devel zlib-devel \
            libjpeg-turbo-devel libwebp-devel libXScrnSaver-devel xrandr xset xsetroot xrdb xmessage xdg-utils zip unzip \
            tar p7zip pulseaudio-utils xterm python3 git curl dejavu-fonts-ttf dbus \
            cabextract pam-devel xauth dbus-devel libnotify brightnessctl \
            libXcomposite-devel libXi-devel
        PM="as_root xbps-install -Sy"
        PKG_LOOKS="qt5ct qt6ct"
        PKG_APPS="udisks2 xdg-desktop-portal xdg-desktop-portal-gtk dosfstools exfatprogs ntfs-3g"
        PKG_WIRELESS="NetworkManager bluez"
        PKG_WINDOWS="wine icoutils" ;;
    *)
        echo "install.sh: I do not know this distribution's package manager." >&2
        echo "  Install: a C compiler and make; the development packages for X11," >&2
        echo "  Xext, Xrandr, Xcursor, Xft, fontconfig, freetype, zlib and libjpeg;" >&2
        echo "  and xrandr, xrdb, xset, xsetroot, xmessage, xdg-utils, zip, unzip," >&2
        echo "  p7zip, pulseaudio-utils, python3, git, curl. Then rerun with --no-deps." >&2
        exit 1 ;;
    esac

    # The packages behind the parts that were asked for. A name this
    # distribution does not have is said and passed over, never fatal.
    opt_pkgs() {
        _what=$1; shift
        [ "$#" -gt 0 ] || return 0
        say "Installing what $_what needs"
        # shellcheck disable=SC2086
        $PM "$@" || echo "  (this system does not have all of these, carrying on)"
    }
    # shellcheck disable=SC2086
    if [ "$WANT_LOOKS" = 1 ]; then opt_pkgs "the other looks" $PKG_LOOKS; fi
    # shellcheck disable=SC2086
    if [ "$WANT_APPS" = all ]; then opt_pkgs "disks, formatting and the portal dialogs" $PKG_APPS; fi
    # shellcheck disable=SC2086
    if [ "$WANT_WIRELESS" = 1 ]; then opt_pkgs "Wi-Fi and Bluetooth" $PKG_WIRELESS; fi
    # shellcheck disable=SC2086
    if [ "$WANT_WINDOWS" = 1 ]; then opt_pkgs "Windows programs" $PKG_WINDOWS; fi
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
            xdg-user-dirs desktop-file-utils shared-mime-info \
            polkitd pkexec policykit-1 dbus-user-session
        if [ "$WANT_APPS" = all ]; then apt_install firefox-esr firefox; fi ;;
    *fedora*|*rhel*|*centos*|*rocky*|*alma*)
        as_root dnf install -y --setopt=strict=0 xorg-x11-server-Xorg xorg-x11-xinit xorg-x11-drivers \
            liberation-fonts pulseaudio-utils pavucontrol \
            spice-vdagent xdg-user-dirs desktop-file-utils shared-mime-info polkit
        if [ "$WANT_APPS" = all ]; then as_root dnf install -y --setopt=strict=0 firefox; fi ;;
    *arch*|*manjaro*|*endeavouros*)
        as_root pacman -Syu --needed --noconfirm xorg-server xorg-xinit xf86-video-vesa \
            xf86-video-vmware xf86-video-qxl \
            ttf-liberation pipewire pipewire-pulse pavucontrol spice-vdagent \
            xdg-user-dirs desktop-file-utils shared-mime-info polkit
        if [ "$WANT_APPS" = all ]; then as_root pacman -S --needed --noconfirm firefox; fi ;;
    *suse*)
        as_root zypper --non-interactive install xorg-x11-server xinit \
            liberation-fonts pulseaudio pavucontrol spice-vdagent \
            xdg-user-dirs desktop-file-utils shared-mime-info polkit
        if [ "$WANT_APPS" = all ]; then as_root zypper --non-interactive install MozillaFirefox; fi ;;
    *alpine*)
        # eudev in the same breath as the X server: the server finds its
        # keyboard and mouse through udev, and a fresh Alpine runs mdev
        # and answers with libudev-zero, which knows of no devices at all.
        as_root apk add eudev xorg-server xinit xf86-video-vesa xf86-input-libinput \
            font-liberation pulseaudio pavucontrol spice-vdagent \
            xdg-user-dirs desktop-file-utils shared-mime-info polkit
        if [ "$WANT_APPS" = all ]; then as_root apk add firefox; fi
        # And the machine has to be on udev, not mdev, or the desktop
        # comes up with nothing that types.
        if command -v setup-devd >/dev/null 2>&1 && [ ! -e /run/udev/control ]; then
            say "Moving this machine to udev, so the X server can find the keyboard and mouse"
            as_root setup-devd udev
        fi ;;
    *void*)
        as_root xbps-install -Sy xorg-server xinit xf86-video-vesa \
            liberation-fonts-ttf pulseaudio pavucontrol \
            spice-vdagent xdg-user-dirs desktop-file-utils shared-mime-info polkit
        if [ "$WANT_APPS" = all ]; then as_root xbps-install -Sy firefox; fi ;;
    esac
fi

# ------------------------------------------------------------------
# 2. Build and install
# ------------------------------------------------------------------
if [ "$DO_BUILD" = 1 ]; then
    say "Building"
    # A build that fails in a tree with older work in it is usually
    # something left behind -- a stale object, or a library half written
    # by a build that was interrupted or run twice at once, which reads as
    # a wall of undefined references -- rather than a real fault. Clear it
    # out and try once more before giving up.
    if ! run make -C "$HERE" -s; then
        say "That failed; building again from nothing"
        run make -C "$HERE" -s clean
        run make -C "$HERE" -s
    fi
    # A link that failed can leave the program behind with nothing in it.
    # make then counts it as built and never tries again, and the machine
    # gets a desktop of nought-byte programs that start and stop in
    # silence. One empty program means the tree cannot be trusted.
    empty=''
    for b in bin/l2kwm bin/l2kexplorer bin/l2kdm; do
        if [ -f "$HERE/$b" ] && [ ! -s "$HERE/$b" ]; then empty="$empty $b"; fi
    done
    if [ -n "$empty" ] && [ "$DRY" != 1 ]; then
        say "An earlier build left empty programs ($empty ); building again from nothing"
        run make -C "$HERE" -s clean
        run make -C "$HERE" -s
    fi
    # Everything builds; what goes in is what this setup asked for, of the
    # programs that did build.
    want=$APPS_BASIC
    if [ "$WANT_APPS" = all ]; then want="$want $APPS_EXTRA"; fi
    if [ "$WANT_WIRELESS" = 1 ]; then want="$want $APPS_WIRELESS"; fi
    if [ "$WANT_WINDOWS" = 1 ]; then want="$want $APPS_WINDOWS"; fi
    # The logon screen stays while a boot service starts it, --full or
    # not: a later run without --full took l2kdm away and left the machine
    # booting into a service with nothing to start.
    if [ "$FULL" = 1 ] || [ -e /etc/systemd/system/l2kdm.service ] || [ -e /etc/init.d/l2kdm ]; then
        case " $want " in *" l2kdm "*) ;; *) want="$want l2kdm" ;; esac
    fi
    bins='' missing=''
    for b in $want; do
        if [ -s "$HERE/bin/$b" ]; then bins="$bins bin/$b"; else missing="$missing $b"; fi
    done
    [ -z "$missing" ] || echo "  (did not build, left out:$missing)"

    # A part left out is taken away as well, so a machine moved to a
    # lighter setup keeps nothing of what it had. Before make install,
    # which puts the polkit action and the portal files in only for the
    # programs that are there.
    for b in $APPS_BASIC $APPS_EXTRA $APPS_WIRELESS $APPS_WINDOWS; do
        case " $want " in *" $b "*) continue ;; esac
        if [ -e "$PREFIX/bin/$b" ]; then
            as_root rm -f "$PREFIX/bin/$b" "$PREFIX/bin/w2k${b#l2k}"
            case "$b" in
                l2kdiskmgmt) as_root rm -f /usr/share/polkit-1/actions/org.linux2000.diskmgmt.policy ;;
                l2kportal) as_root rm -f /usr/share/xdg-desktop-portal/portals/w2k.portal \
                    /usr/share/xdg-desktop-portal/w2k-portals.conf \
                    /usr/share/dbus-1/services/org.freedesktop.impl.portal.desktop.w2k.service ;;
            esac
        fi
    done

    if [ "$WANT_LOOKS" = 1 ]; then sounds=all; else sounds=win2000; fi
    say "Installing under $PREFIX"
    as_root make -C "$HERE" -s install PREFIX="$PREFIX" \
        INSTALL_BINS="$bins" INSTALL_LOOKS="$WANT_LOOKS" INSTALL_SOUNDS="$sounds"
    # make install has put the cursor sets in already -- the folder's own
    # and every set folder inside it. Only the loose files again here:
    # install(1) refuses a folder, and under set -e that stopped the run.
    as_root install -d "$PREFIX/share/w2k/cursors"
    as_root sh -c "install -m644 '$HERE'/cursors/*.cur '$HERE'/cursors/*.crs '$PREFIX/share/w2k/cursors/'"
    # What was chosen, for the next run and for anyone wondering later.
    as_root sh -c "printf '%s\n' '# What install.sh put in. Run it again with --setup full,' \
        '# --setup light or --setup custom to change this.' \
        'SETUP=$SETUP' 'LOOKS=$WANT_LOOKS' 'APPS=$WANT_APPS' \
        'WIRELESS=$WANT_WIRELESS' 'WINDOWS=$WANT_WINDOWS' > '$SETUP_FILE'"
    # A desktop that is running picks the new build up in place: the
    # window manager restarts itself with every window kept (l2kwm
    # --restart), each session found by its l2kwm process.
    if [ "$DRY" != 1 ] && [ "$(id -u)" = 0 ]; then
        for pid in $(wm_pids); do
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
        # From the default set -- ReactOS's, where it is there.
        curset="$HERE/cursors"; [ -d "$HERE/cursors/reactos" ] && curset="$HERE/cursors/reactos"
        as_root python3 "$HERE/tools/gencursortheme.py" "$curset" /usr/share/icons/Windows2000
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
        # No PAM in the logon screen is usually an object left over from a
        # build made before the PAM headers were installed, not headers
        # that are missing now: build it again from nothing and look again.
        if [ "$DRY" != 1 ] && [ "$DO_BUILD" = 1 ] &&
           { [ ! -s "$PREFIX/bin/l2kdm" ] || ! "$PREFIX/bin/l2kdm" --check 2>/dev/null; }; then
            say "The logon screen came out without PAM; building again from nothing"
            run make -C "$HERE" -s clean
            run make -C "$HERE" -s
            as_root make -C "$HERE" -s install PREFIX="$PREFIX" \
                INSTALL_BINS="$bins" INSTALL_LOOKS="$WANT_LOOKS" INSTALL_SOUNDS="$sounds"
        fi
        if [ "$DRY" != 1 ] &&
           { [ ! -s "$PREFIX/bin/l2kdm" ] || ! "$PREFIX/bin/l2kdm" --check 2>/dev/null; }; then
            echo "  l2kdm did not build, or has no PAM (no PAM development headers); nobody could log on." >&2
            echo "  Install them (libpam0g-dev / pam-devel) and rerun." >&2
            exit 1
        fi
        # The PAM stack in the words this distribution's stacks use --
        # looked for in /usr/lib/pam.d as well as /etc/pam.d, since
        # Linux-PAM 1.5 and later keep a distribution's own stacks there
        # and Alpine ships its base-* ones nowhere else. Including a file
        # that is not there refuses every password, which is what a logon
        # screen saying the system cannot log you on usually means.
        pamd() { [ -f "/etc/pam.d/$1" ] || [ -f "/usr/lib/pam.d/$1" ]; }
        if pamd common-auth; then pamsrc=l2kdm.pam.debian
        elif pamd password-auth; then pamsrc=l2kdm.pam.fedora
        elif pamd base-auth; then pamsrc=l2kdm.pam.alpine
        elif pamd system-login; then pamsrc=l2kdm.pam.generic
        else pamsrc=l2kdm.pam.standalone; fi
        say "Logon screen: PAM through ${pamsrc#l2kdm.pam.}"
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
        elif command -v rc-update >/dev/null 2>&1; then
            # OpenRC (Alpine and friends): the same job in its own words.
            as_root sh -c "sed 's|@BINDIR@|$PREFIX/bin|' '$HERE/config/l2kdm.openrc' > /etc/init.d/l2kdm"
            as_root chmod +x /etc/init.d/l2kdm
            for dm in lightdm gdm sddm xdm lxdm slim greetd; do
                as_root sh -c "rc-update del $dm default >/dev/null 2>&1; true"
            done
            as_root rc-update add l2kdm default
            echo "  l2kdm takes over the console at the next boot (or now: rc-service l2kdm start)."
        else
            echo "  No systemd or OpenRC here: start '$PREFIX/bin/l2kdm' as root at boot from" >&2
            echo "  your init system (it runs in the foreground and puts the logon screen back)." >&2
        fi
    fi
fi

# The screen's backlight: a udev rule hands the brightness file to the
# video group and the user joins that group, so Power Options can set it
# straight from the file, with no password (brightnessctl's package does
# the same). The rule is applied now; the group takes effect at the
# user's next logon.
if [ "$USER_ONLY" != 1 ]; then
    tmp=$(mktemp)
    cat > "$tmp" <<'EOF'
# Linux 2000: the console user (video group) may set the screen brightness.
ACTION=="add", SUBSYSTEM=="backlight", RUN+="/bin/chgrp video /sys/class/backlight/%k/brightness", RUN+="/bin/chmod g+w /sys/class/backlight/%k/brightness"
EOF
    as_root install -m644 "$tmp" /etc/udev/rules.d/90-linux2000-backlight.rules
    rm -f "$tmp"
    as_root sh -c "udevadm control --reload 2>/dev/null; udevadm trigger -s backlight -c add 2>/dev/null; true"
    bl_user=${TARGET_USER:-$USER}
    if [ -n "$bl_user" ] && [ "$bl_user" != root ] && getent group video >/dev/null 2>&1; then
        if command -v usermod >/dev/null 2>&1; then
            as_root sh -c "usermod -aG video $bl_user 2>/dev/null; true"
        elif command -v addgroup >/dev/null 2>&1; then          # BusyBox
            # Alpine has no logind to hand the seat over, so the X server
            # startx runs wants its user in input as well as video.
            as_root sh -c "addgroup $bl_user video 2>/dev/null; addgroup $bl_user input 2>/dev/null; true"
        fi
    fi
fi

# Run as root for someone else (the bootstrap does this): the user part of
# the job is handed to them, and the rest of this file is skipped.
if [ "$(id -u)" = 0 ] && [ -n "$TARGET_USER" ] && [ "$TARGET_USER" != root ]; then
    say "Configuring for $TARGET_USER"
    opts="--user-only"
    [ "$DO_THEME" = 1 ] || opts="$opts --no-theme"
    [ "$WANT_LOOKS" = 1 ] || opts="$opts --no-looks"
    [ "$DO_TAHOMA" = 1 ] && opts="$opts --tahoma"
    [ "$DO_XINITRC" = 1 ] && opts="$opts --xinitrc"
    [ "$DRY" = 1 ] && opts="$opts --dry-run"
    run su -s /bin/sh "$TARGET_USER" -c "cd '$HERE' && ./install.sh $opts --prefix '$PREFIX'"
    run su -s /bin/sh "$TARGET_USER" -c "xdg-user-dirs-update >/dev/null 2>&1 || true"
    say "Done. Reboot, or: systemctl start l2kdm"
    summary
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
run sh -c "cp -rf '$HERE'/cursors/* '$HOME/.w2k/cursors/'"   # sets are folders
if command -v python3 >/dev/null 2>&1; then
    curset="$HOME/.w2k/cursors"; [ -d "$curset/reactos" ] && curset="$curset/reactos"
    run python3 "$HERE/tools/gencursortheme.py" "$curset" "$HOME/.icons/Windows2000"
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

# Chicago95 for the classic look, and the looks' own themes for other
# programs -- B00merang's Windows XP, Vista and 7 GTK themes and icons and
# the Windows 7 Kvantum theme -- come from tools/fetch-themes.sh, which
# fetches nothing twice and can be run on its own after a `make install`.
if [ "$DO_THEME" = 1 ]; then
    tflags=''
    [ "$WANT_LOOKS" = 1 ] || tflags="--classic-only"
    [ "$DRY" != 1 ] || tflags="$tflags --dry-run"
    # shellcheck disable=SC2086
    sh "$HERE/tools/fetch-themes.sh" $tflags
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
if [ "$USER_ONLY" != 1 ]; then summary; fi
echo "  Start it with:  startx $PREFIX/bin/l2k-session"
echo "  or pick \"Windows 2000\" in your display manager. Explorer becomes the"
echo "  folder handler for other programs the first time the shell runs."
echo "  To take it all off again: sh $PREFIX/share/w2k/uninstall.sh"
echo "  (or Windows Update > Remove Linux 2000)."
