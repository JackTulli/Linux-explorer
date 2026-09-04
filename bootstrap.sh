#!/bin/sh
# bootstrap.sh -- from a bare Debian (or any of the distributions install.sh
# knows) to the Windows 2000 desktop in one command, run as root:
#
#   curl -sL jacktulli.github.io/w2k | sh
#
# (which serves a copy of this file; the long form is
#   sh -c "$(curl -fsSL https://raw.githubusercontent.com/JackTulli/Linux-explorer/main/bootstrap.sh)")
#
# It installs git, fetches the repository into /usr/local/src, and runs
# install.sh --full: the X server, w2kdm (our own logon screen, no LightDM),
# sound, VM guest tools, Firefox, the shell itself, the cursor
# set, Chicago95 for GTK, the Windows 2000 palette for Qt and Tahoma --
# all of it for the first ordinary user on the machine (or W2K_USER).
#
#   W2K_REPO=https://github.com/JackTulli/Linux-explorer   where to fetch from
#   W2K_USER=name                                          who to set up
set -e
REPO=${W2K_REPO:-https://github.com/JackTulli/Linux-explorer}
SRC=/usr/local/src/Linux-explorer

if [ "$(id -u)" != 0 ]; then
    echo "bootstrap.sh: run this as root (su -, or sudo sh)" >&2
    exit 1
fi

# Who gets the desktop: the caller's user under sudo, else the first
# ordinary account.
USERNAME=${W2K_USER:-${SUDO_USER:-}}
if [ -z "$USERNAME" ] || [ "$USERNAME" = root ]; then
    USERNAME=$(getent passwd | awk -F: '$3 >= 1000 && $3 < 60000 && $7 !~ /nologin|false/ { print $1; exit }')
fi
[ -n "$USERNAME" ] || { echo "bootstrap.sh: no ordinary user found; set W2K_USER" >&2; exit 1; }
# Run again on a machine that has it: the source is updated, the shell
# rebuilt and installed over the old one, and a desktop that is running
# restarts in place with its windows. Nothing of the user's is touched
# twice (configuration files are only written when missing).
if [ -x /usr/local/bin/w2kwm ]; then
    echo "==> Updating the Windows 2000 desktop ($(/usr/local/bin/w2kwm --version 2>/dev/null | cut -d' ' -f2)) for $USERNAME"
else
    echo "==> Setting up the Windows 2000 desktop for $USERNAME"
fi

# git and curl, then the source.
if command -v apt-get >/dev/null 2>&1; then
    apt-get update
    apt-get install -y git ca-certificates curl
elif command -v dnf >/dev/null 2>&1; then dnf install -y git curl
elif command -v pacman >/dev/null 2>&1; then pacman -Sy --needed --noconfirm git curl
elif command -v zypper >/dev/null 2>&1; then zypper --non-interactive install git curl
elif command -v apk >/dev/null 2>&1; then apk add git curl
elif command -v xbps-install >/dev/null 2>&1; then xbps-install -Sy git curl
fi
# The checkout stays root's: root builds and installs from it, and a tree
# the desktop user could edit would be a way to become root at the next
# run. (Earlier versions handed it to the user; take it back.)
if [ -d "$SRC/.git" ]; then
    chown -R root:root "$SRC"
    if ! git -c safe.directory="$SRC" -C "$SRC" pull -q --ff-only; then
        echo "==> Could not update $SRC; fetching it afresh"
        rm -rf "$SRC"
        git clone -q "$REPO" "$SRC"
    fi
else
    mkdir -p "$(dirname "$SRC")"
    git clone -q "$REPO" "$SRC"
fi
echo "==> Source at $SRC, $(git -C "$SRC" log -1 --format='%h %s')"

cd "$SRC"
sh ./install.sh --full --tahoma --xinitrc --user "$USERNAME"
echo "==> $(/usr/local/bin/w2kwm --version 2>/dev/null || echo 'w2kwm') is installed. Run this command again any time to update."
