#!/bin/sh
# fetch-themes.sh -- the third-party themes the looks switch other programs
# to, into the home directory:
#
#   Chicago95 (github.com/grassmunk/Chicago95): the GTK theme and icons the
#     classic look uses.
#   B00merang's Windows XP (all seven styles), Windows Vista and Windows-7
#     GTK themes (github.com/B00merang-Project), and B00merang-Artwork's
#     Windows XP and Windows-7 icon themes (github.com/B00merang-Artwork).
#   The Windows 7 Kvantum theme for Qt from the KDE Store (store.kde.org
#     1679903, by drgordbord).
#
# They go to ~/.themes, ~/.local/share/icons and ~/.config/Kvantum, and
# nothing is fetched twice: a theme already in place is left alone.
# Display Properties switches to them with the look. install.sh runs this;
# after a plain `make install`, run it yourself:
#
#   sh tools/fetch-themes.sh [--dry-run] [--classic-only]
#
# --classic-only fetches Chicago95 and stops: what a setup with the classic
# look alone needs.
set -u
DRY=0 CLASSIC=0
for a in "$@"; do
    case "$a" in
        --dry-run) DRY=1 ;;
        --classic-only) CLASSIC=1 ;;
    esac
done

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
run() { if [ "$DRY" = 1 ]; then echo "  + $*"; else "$@"; fi; }

# Chicago95: the Windows 95/2000 look for GTK programs, and its icons.
if [ ! -d "$HOME/.themes/Chicago95" ] || { [ ! -d "$HOME/.icons/Chicago95" ] && [ ! -d "$HOME/.local/share/icons/Chicago95" ]; }; then
    say "Fetching Chicago95 (github.com/grassmunk/Chicago95)"
    tmp=$(mktemp -d)
    if command -v git >/dev/null 2>&1; then
        run git clone -q --depth 1 https://github.com/grassmunk/Chicago95 "$tmp/c95"
    else
        run sh -c "curl -sL https://github.com/grassmunk/Chicago95/archive/refs/heads/master.tar.gz | tar xz -C '$tmp' && mv '$tmp'/Chicago95-* '$tmp/c95'"
    fi
    if [ "$DRY" != 1 ]; then
        mkdir -p "$HOME/.themes" "$HOME/.local/share/icons"
        [ -d "$HOME/.themes/Chicago95" ] || cp -r "$tmp/c95/Theme/Chicago95" "$HOME/.themes/"
        [ -d "$HOME/.local/share/icons/Chicago95" ] || [ -d "$HOME/.icons/Chicago95" ] || \
            cp -r "$tmp/c95/Icons/Chicago95" "$HOME/.local/share/icons/"
    fi
    rm -rf "$tmp"
else
    say "Chicago95 is already installed"
fi

if [ "$CLASSIC" = 1 ]; then
    echo "  (the classic look only: the XP, Vista and 7 themes are not fetched)"
    exit 0
fi

fetch_tgz() {   # url dest [folder]: the archive's top folder, or a folder
                # inside it, becomes dest
    _url=$1; _dest=$2; _sub=${3:-}
    [ -d "$_dest" ] && return 0
    _tmp=$(mktemp -d)
    if curl -fsL -m 300 "$_url" | tar xz -C "$_tmp" 2>/dev/null; then
        _top=$(find "$_tmp" -mindepth 1 -maxdepth 1 -type d | head -n 1)
        _src="$_top${_sub:+/$_sub}"
        if [ -d "$_src" ]; then
            mkdir -p "$(dirname "$_dest")" && cp -r "$_src" "$_dest"
        else
            echo "  (nothing called $_sub in $_url; skipped)" >&2
        fi
    else
        echo "  (could not fetch $_url; skipped)" >&2
    fi
    rm -rf "$_tmp"
}

say "Fetching the Windows XP, Vista and 7 themes for GTK and Qt programs"
if [ "$DRY" = 1 ]; then
    echo "  + B00merang Windows XP, Windows Vista, Windows-7 into ~/.themes"
    echo "  + B00merang-Artwork Windows XP, Windows-7 into ~/.local/share/icons"
    echo "  + Windows 7 Kvantum into ~/.config/Kvantum"
    exit 0
fi
B=https://github.com/B00merang-Project
A=https://github.com/B00merang-Artwork
mkdir -p "$HOME/.themes" "$HOME/.local/share/icons" "$HOME/.config/Kvantum"
if [ ! -d "$HOME/.themes/Windows XP Luna" ]; then
    tmp=$(mktemp -d)
    if curl -fsL -m 300 "$B/Windows-XP/archive/refs/tags/3.1.tar.gz" | tar xz -C "$tmp" 2>/dev/null; then
        for d in "$tmp"/Windows-XP-*/"Windows XP "*/; do
            [ -d "$d" ] && cp -r "$d" "$HOME/.themes/"
        done
    else
        echo "  (could not fetch the Windows XP themes; skipped)" >&2
    fi
    rm -rf "$tmp"
fi
fetch_tgz "$B/Windows-7/archive/refs/tags/2.1.tar.gz" "$HOME/.themes/Windows-7"
fetch_tgz "$B/Windows-Vista/archive/refs/tags/1.0.tar.gz" "$HOME/.themes/Windows Vista"
fetch_tgz "$A/Windows-XP/archive/refs/tags/3.1.tar.gz" "$HOME/.local/share/icons/Windows XP"
fetch_tgz "$A/Windows-7/archive/refs/tags/1.0.tar.gz" "$HOME/.local/share/icons/Windows-7"
for t in "Windows XP" "Windows-7"; do
    [ -d "$HOME/.local/share/icons/$t" ] && command -v gtk-update-icon-cache >/dev/null 2>&1 && \
        gtk-update-icon-cache -q -f "$HOME/.local/share/icons/$t" 2>/dev/null || true
done
# The KDE Store hands out a fresh download link through its OCS API.
if [ ! -d "$HOME/.config/Kvantum/Windows7Kvantum" ]; then
    link=$(curl -fsL -m 30 "https://api.pling.com/ocs/v1/content/data/1679903" 2>/dev/null | \
           tr -d '\r' | sed -n 's/.*<downloadlink1>\([^<]*\)<.*/\1/p' | head -n 1)
    if [ -n "$link" ]; then
        fetch_tgz "$link" "$HOME/.config/Kvantum/Windows7Kvantum"
    else
        echo "  (the KDE Store did not answer for the Windows 7 Kvantum theme; skipped)" >&2
    fi
fi
n=0; for t in "Windows XP Luna" "Windows-7" "Windows Vista"; do [ -d "$HOME/.themes/$t" ] && n=$((n + 1)); done
echo "  $n of 3 GTK themes, $(ls -d "$HOME/.local/share/icons/Windows XP" "$HOME/.local/share/icons/Windows-7" 2>/dev/null | wc -l) of 2 icon themes, Kvantum: $([ -d "$HOME/.config/Kvantum/Windows7Kvantum" ] && echo yes || echo no)"
