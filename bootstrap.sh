#!/bin/sh
# bootstrap.sh -- from a bare Debian (or any of the distributions install.sh
# knows) to the Windows 2000 desktop in one command, run as root:
#
#   curl -fsSL https://raw.githubusercontent.com/JackTulli/Linux-explorer/main/bootstrap.sh | sh
#
# It installs git, fetches the repository into /usr/local/src, and runs
# install.sh --full: the X server, LightDM logging straight into the
# session, sound, VM guest tools, Firefox, the shell itself, the cursor
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
case "$REPO" in *JackTulli*)
    echo "bootstrap.sh: set W2K_REPO to the repository's URL first, e.g." >&2
    echo "  W2K_REPO=https://github.com/you/Linux-explorer sh bootstrap.sh" >&2
    exit 1 ;;
esac

# Who gets the desktop: the caller's user under sudo, else the first
# ordinary account.
USERNAME=${W2K_USER:-${SUDO_USER:-}}
if [ -z "$USERNAME" ] || [ "$USERNAME" = root ]; then
    USERNAME=$(getent passwd | awk -F: '$3 >= 1000 && $3 < 60000 && $7 !~ /nologin|false/ { print $1; exit }')
fi
[ -n "$USERNAME" ] || { echo "bootstrap.sh: no ordinary user found; set W2K_USER" >&2; exit 1; }
echo "==> Setting up the Windows 2000 desktop for $USERNAME"

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
if [ -d "$SRC/.git" ]; then
    git -C "$SRC" pull -q --ff-only || true
else
    mkdir -p "$(dirname "$SRC")"
    git clone -q "$REPO" "$SRC"
fi
chown -R "$USERNAME" "$SRC"

cd "$SRC"
sh ./install.sh --full --tahoma --xinitrc --user "$USERNAME"
