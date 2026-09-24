#!/bin/sh
# uninstall.sh -- take Linux 2000 off a machine, as completely as it went on.
#
#   ./uninstall.sh               everything install.sh put in, for this user
#   ./uninstall.sh --dry-run     say what would go; touch nothing
#   ./uninstall.sh --yes         do not ask first (Windows Update uses this)
#   ./uninstall.sh --prefix DIR  where it was installed (default /usr/local)
#   ./uninstall.sh --keep-config keep ~/.w2k: schemes, pinned items, settings
#   ./uninstall.sh --keep-themes keep the GTK, icon and Kvantum themes fetched
#                                for the looks (Chicago95, XP, Vista, 7)
#   ./uninstall.sh --sources     also the source tree it was built from
#   ./uninstall.sh --games       also the Wine and Proton prefixes, with
#                                every Windows program installed in them
#   ./uninstall.sh --user NAME   whose settings to take off (root, for someone else)
#   ./uninstall.sh --system-only | --user-only   one half of the job
#
# What goes:
#   the programs and their w2k* names under <prefix>/bin, everything under
#   <prefix>/share/w2k, the session entry, the portal and polkit files, the
#   Windows 2000 cursor theme, l2kdm with its PAM file and service, the
#   backlight rule and the virtual machine's cursor snippet; and for the
#   user, ~/.w2k, the cursor theme, the GTK and Qt settings, Explorer as
#   the folder handler, Tahoma, and .xinitrc. A file install.sh replaced
#   has its .pre-w2k copy put back rather than being deleted.
#
# What stays, unless asked for: the source tree, the Wine and Proton
# prefixes (a game lives in one), and the distribution's packages -- Wine,
# NetworkManager, bluez, qt5ct and the rest were installed by name and are
# yours to keep or remove with the package manager.
#
# The machine gets its display manager back: whichever one is installed is
# enabled again, since install.sh had stood it down for l2kdm.
set -e
# Run from Windows Update in a terminal on the desktop being removed: a
# hang-up when that desktop goes must not cut the job in half.
trap '' HUP

HERE=$(cd "$(dirname "$0")" && pwd)
SELF=$HERE/$(basename "$0")             # this file, wherever it was run from
PREFIX=/usr/local
DRY=0 YES=0 DO_SYSTEM=1 DO_USER=1 KEEP_CONFIG=0 KEEP_THEMES=0 DO_SOURCES=0 DO_GAMES=0
TARGET_USER=''

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX=$2; shift ;;
        --dry-run|-n) DRY=1 ;;
        --yes|-y) YES=1 ;;
        --keep-config) KEEP_CONFIG=1 ;;
        --keep-themes) KEEP_THEMES=1 ;;
        --sources) DO_SOURCES=1 ;;
        --games) DO_GAMES=1 ;;
        --system-only) DO_USER=0 ;;
        --user-only) DO_SYSTEM=0 ;;
        --user) TARGET_USER=$2; shift ;;
        -h|--help) sed -n '2,${/^#/!q;p;}' "$0"; exit 0 ;;
        *) echo "uninstall.sh: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done

# Run as root through sudo, the caller is the user to clean up after.
if [ "$(id -u)" = 0 ] && [ -z "$TARGET_USER" ] && [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
    TARGET_USER=$SUDO_USER
fi

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
run() { if [ "$DRY" = 1 ]; then echo "  + $*"; else "$@"; fi; }
as_root() {
    if [ "$(id -u)" = 0 ]; then run "$@"
    elif command -v sudo >/dev/null 2>&1; then run sudo "$@"
    elif command -v doas >/dev/null 2>&1; then run doas "$@"
    else echo "uninstall.sh: need root for: $*" >&2; exit 1; fi
}

# Nothing is deleted that is not there, and every path is said out loud, so
# the terminal is a list of exactly what went.
n_gone=0
del() {                                 # this user's own files
    for p in "$@"; do
        [ -e "$p" ] || [ -L "$p" ] || continue
        echo "  - $p"
        [ "$DRY" = 1 ] || rm -rf "$p"
        n_gone=$((n_gone + 1))
    done
}
sdel() {                                # the machine's, through root
    for p in "$@"; do
        [ -e "$p" ] || [ -L "$p" ] || continue
        echo "  - $p"
        as_root rm -rf "$p"
        n_gone=$((n_gone + 1))
    done
}
# A file install.sh replaced kept a copy: that copy goes back where it was.
restore() {
    for p in "$@"; do
        if [ -e "$p.pre-w2k" ]; then
            echo "  ~ $p (the copy from before)"
            run mv -f "$p.pre-w2k" "$p"
            n_gone=$((n_gone + 1))
        else
            del "$p"
        fi
    done
}

if [ "$YES" != 1 ] && [ "$DRY" != 1 ] && ( : < /dev/tty ) 2>/dev/null; then
    printf 'This removes Linux 2000 from %s. Type yes to go on: ' "$(hostname 2>/dev/null || echo this machine)" > /dev/tty
    read -r a < /dev/tty || a=''
    case "$a" in [Yy][Ee][Ss]|[Yy]) ;; *) echo "Nothing was touched."; exit 0 ;; esac
fi

# Root hands the user's half to them by running this file again, and the
# machine's half below can delete it (it lives in share/w2k, or in the
# sources): a copy, readable by them, is what gets handed over.
HANDOVER=$SELF
if [ "$(id -u)" = 0 ] && [ -n "$TARGET_USER" ] && [ "$TARGET_USER" != root ] &&
   [ "$DO_USER" = 1 ] && [ "$DRY" != 1 ]; then
    HANDOVER=$(mktemp /tmp/l2k-uninstall.XXXXXX)
    cp "$SELF" "$HANDOVER"
    chmod 644 "$HANDOVER"
    trap 'rm -f "$HANDOVER"' EXIT
fi

# ------------------------------------------------------------------
# 1. The machine
# ------------------------------------------------------------------
if [ "$DO_SYSTEM" = 1 ]; then
    say "Taking the desktop off the machine"

    # The logon screen is taken off the boot first, and the console goes
    # back to whichever display manager is installed, or the machine would
    # come up with none at all. It is not stopped: this may be running in
    # the session it started (Windows Update's Remove button does that),
    # and stopping it ended the session -- and this script with it -- with
    # nothing removed. It goes at the next boot.
    if [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
        for u in l2kdm w2kdm; do
            as_root sh -c "systemctl disable $u >/dev/null 2>&1; true"
        done
        sdel /etc/systemd/system/l2kdm.service /etc/systemd/system/w2kdm.service
        as_root sh -c "systemctl daemon-reload >/dev/null 2>&1; true"
        if [ ! -e /etc/systemd/system/display-manager.service ]; then
            for dm in lightdm gdm3 gdm sddm xdm lxdm slim ly greetd; do
                if [ -e "/lib/systemd/system/$dm.service" ] ||
                   [ -e "/usr/lib/systemd/system/$dm.service" ] ||
                   [ -e "/etc/systemd/system/$dm.service" ]; then
                    say "Giving the console back to $dm"
                    as_root sh -c "systemctl enable $dm >/dev/null 2>&1; true"
                    break
                fi
            done
        fi
    fi
    # OpenRC (Alpine and friends): the same, in its own words.
    if command -v rc-update >/dev/null 2>&1; then
        as_root sh -c "rc-update del l2kdm default >/dev/null 2>&1; true"
        sdel /etc/init.d/l2kdm
        for dm in lightdm gdm sddm xdm lxdm slim greetd; do
            if [ -x "/etc/init.d/$dm" ]; then
                say "Giving the console back to $dm"
                as_root sh -c "rc-update add $dm default >/dev/null 2>&1; true"
                break
            fi
        done
    fi
    # sysvinit (Devuan): the respawn line comes out of inittab for the next
    # boot. init is not told now -- it would end the logon screen, and the
    # session this may be running in -- and Debian's display manager
    # switch names the one there was before, or none.
    if [ -f /etc/inittab ] && grep -q '^l2k:' /etc/inittab 2>/dev/null; then
        say "Taking the logon screen out of /etc/inittab"
        as_root sh -c "sed -i '/^l2k:/d; /^# Linux 2000 logon screen/d' /etc/inittab"
    fi
    if [ -f /etc/X11/default-display-manager.l2k-was ]; then
        if [ "$(cat /etc/X11/default-display-manager.l2k-was)" = none ]; then
            sdel /etc/X11/default-display-manager
        else
            say "Giving the console back to $(cat /etc/X11/default-display-manager.l2k-was)"
            as_root cp /etc/X11/default-display-manager.l2k-was /etc/X11/default-display-manager
        fi
        sdel /etc/X11/default-display-manager.l2k-was
    fi
    sdel /etc/pam.d/l2kdm /etc/pam.d/w2kdm
    sdel /etc/X11/xorg.conf.d/20-w2k-vm-cursor.conf
    if [ -e /etc/udev/rules.d/90-linux2000-backlight.rules ]; then
        sdel /etc/udev/rules.d/90-linux2000-backlight.rules
        as_root sh -c "udevadm control --reload 2>/dev/null; true"
    fi

    # The programs, their w2k* names, and everything make install laid down.
    for p in "$PREFIX"/bin/l2k* "$PREFIX"/bin/w2k* "$PREFIX/bin/linver"; do sdel "$p"; done
    sdel "$PREFIX/share/w2k" "$PREFIX/share/systemd/user/l2k-session.target"
    sdel /usr/share/xsessions/l2k-session.desktop /usr/share/xsessions/w2k-session.desktop
    sdel /usr/share/polkit-1/actions/org.linux2000.diskmgmt.policy
    sdel /usr/share/xdg-desktop-portal/portals/w2k.portal \
         /usr/share/xdg-desktop-portal/w2k-portals.conf \
         /usr/share/dbus-1/services/org.freedesktop.impl.portal.desktop.w2k.service
    sdel /usr/share/icons/Windows2000
    # The system-wide default pointer, only where it is still ours.
    if [ -f /usr/share/icons/default/index.theme ] &&
       grep -q 'Inherits=Windows2000' /usr/share/icons/default/index.theme 2>/dev/null; then
        sdel /usr/share/icons/default/index.theme
    fi

    if [ "$DO_SOURCES" = 1 ]; then
        for d in /usr/local/src/Linux-explorer /usr/local/src/w2k; do sdel "$d"; done
    fi
fi

# Root doing it for someone else hands their half over to them, the way
# install.sh does, and stops here.
if [ "$(id -u)" = 0 ] && [ -n "$TARGET_USER" ] && [ "$TARGET_USER" != root ] && [ "$DO_USER" = 1 ]; then
    opts="--user-only --yes"
    [ "$DRY" = 1 ] && opts="$opts --dry-run"
    [ "$KEEP_CONFIG" = 1 ] && opts="$opts --keep-config"
    [ "$KEEP_THEMES" = 1 ] && opts="$opts --keep-themes"
    [ "$DO_GAMES" = 1 ] && opts="$opts --games"
    say "Taking $TARGET_USER's settings off"
    run su -s /bin/sh "$TARGET_USER" -c "sh '$HANDOVER' $opts --prefix '$PREFIX'"
    say "Done. $n_gone things went."
    exit 0
fi

# ------------------------------------------------------------------
# 2. This user
# ------------------------------------------------------------------
if [ "$DO_USER" = 1 ]; then
    [ -n "$HOME" ] || { echo "uninstall.sh: HOME is not set" >&2; exit 1; }
    say "Taking ${USER:-this user}'s settings off"

    # The pointer: ours goes, and anything it was put in front of comes back.
    del "$HOME/.icons/Windows2000" "$HOME/.local/share/icons/Windows2000"
    # install.sh made ~/.local/share/icons/default only as a link to
    # ~/.icons/default, and only where there was none: a folder of the
    # user's own there went too.
    if [ -L "$HOME/.local/share/icons/default" ] &&
       [ "$(readlink "$HOME/.local/share/icons/default")" = "$HOME/.icons/default" ]; then
        del "$HOME/.local/share/icons/default"
    fi
    restore "$HOME/.icons/default/index.theme"

    # GTK and Qt were pointed at the classic theme and the Windows 2000
    # palette; the files from before come back where install.sh kept one.
    restore "$HOME/.gtkrc-2.0" "$HOME/.config/gtk-3.0/settings.ini" \
            "$HOME/.config/gtk-4.0/settings.ini"
    for q in qt5ct qt6ct; do
        restore "$HOME/.config/$q/$q.conf"
        del "$HOME/.config/$q/colors/Windows2000.conf"
    done
    # The scheme's colours, which the desktop hands GTK 3 and 4 at every
    # logon: its own file, and the import it added to the user's gtk.css.
    # Left behind, every GTK program kept the Windows 2000 colours after
    # the uninstall. A gtk.css with nothing else in it was the desktop's.
    cfg=${XDG_CONFIG_HOME:-$HOME/.config}
    for g in gtk-3.0 gtk-4.0; do
        del "$cfg/$g/w2k-colors.css"
        css=$cfg/$g/gtk.css
        if [ -f "$css" ] && grep -q 'w2k-colors\.css' "$css" 2>/dev/null; then
            echo "  ~ $css (the desktop's colours taken out)"
            if [ "$DRY" != 1 ]; then
                tmp=$(mktemp)
                grep -v -x -F -e "/* Linux 2000: the desktop's colours, kept in w2k-colors.css. */" \
                    -e '@import url("w2k-colors.css");' "$css" > "$tmp" || true
                if grep -q '[^[:space:]]' "$tmp"; then cat "$tmp" > "$css"; else rm -f "$css"; fi
                rm -f "$tmp"
            fi
            n_gone=$((n_gone + 1))
        fi
    done

    # Explorer as the folder handler, and the entry it wrote for itself.
    del "$HOME/.local/share/applications/l2kexplorer.desktop"
    if [ -f "$HOME/.config/mimeapps.list" ] &&
       grep -q 'l2kexplorer.desktop' "$HOME/.config/mimeapps.list" 2>/dev/null; then
        echo "  ~ $HOME/.config/mimeapps.list (Explorer taken out)"
        if [ "$DRY" != 1 ]; then
            tmp=$(mktemp)
            grep -v 'l2kexplorer.desktop' "$HOME/.config/mimeapps.list" > "$tmp"
            cat "$tmp" > "$HOME/.config/mimeapps.list"
            rm -f "$tmp"
        fi
        n_gone=$((n_gone + 1))
    fi
    if [ "$DRY" != 1 ] && command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$HOME/.local/share/applications" >/dev/null 2>&1 || true
    fi

    # The shell's own typeface, and the startx session.
    for f in "$HOME"/.local/share/fonts/tahoma*.ttf "$HOME"/.local/share/fonts/tahoma*.TTF; do del "$f"; done
    if [ "$DRY" != 1 ] && command -v fc-cache >/dev/null 2>&1; then
        fc-cache -f "$HOME/.local/share/fonts" >/dev/null 2>&1 || true
    fi
    if [ -f "$HOME/.xinitrc" ] && grep -q 'l2k-session\|w2k-session' "$HOME/.xinitrc" 2>/dev/null; then
        restore "$HOME/.xinitrc"
    fi

    # The themes the looks switch other programs to.
    if [ "$KEEP_THEMES" != 1 ]; then
        del "$HOME/.themes/Chicago95" "$HOME/.themes/Windows-7" \
            "$HOME/.themes/Windows Vista"
        for d in "$HOME"/.themes/"Windows XP"*; do del "$d"; done
        # (Not ~/.icons/Chicago95: fetch-themes.sh never puts it there, so
        # one there is the user's own.)
        del "$HOME/.local/share/icons/Chicago95" "$HOME/.local/share/icons/Windows XP" \
            "$HOME/.local/share/icons/Windows-7"
        del "$HOME/.config/Kvantum/Windows7Kvantum"
        # And Kvantum's own setting, which the Windows 7 look pointed at it.
        kv=$cfg/Kvantum/kvantum.kvconfig
        if [ -f "$kv" ] && grep -qx 'theme=Windows7Kvantum' "$kv" 2>/dev/null; then
            echo "  ~ $kv (the Windows 7 theme taken out)"
            [ "$DRY" = 1 ] || sed -i '/^theme=Windows7Kvantum$/d' "$kv"
            n_gone=$((n_gone + 1))
        fi
    fi

    # Everything the desktop itself keeps: schemes, pinned items, cursors,
    # the compatibility list, the icon cache.
    if [ "$KEEP_CONFIG" != 1 ]; then del "$HOME/.w2k"; fi

    # A game lives in a prefix, so these go only when asked for by name.
    pfxdir=${XDG_DATA_HOME:-$HOME/.local/share}/l2k
    if [ "$DO_GAMES" = 1 ]; then
        del "$pfxdir"
    elif [ -d "$pfxdir" ]; then
        echo "  (kept: $pfxdir -- the Wine and Proton prefixes, and what is installed in them)"
    fi
fi

say "Done. $n_gone things went."
if [ "$DO_SYSTEM" = 1 ]; then
    echo "  The packages install.sh asked for -- Wine, NetworkManager, bluez, qt5ct and"
    echo "  the rest -- are still installed; the package manager takes those off."
    echo "  A desktop that is still running is running from deleted files: restart the"
    echo "  computer, and it comes up without Linux 2000's logon screen."
fi
