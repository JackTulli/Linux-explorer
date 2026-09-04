#!/bin/sh
# shot.sh [display] [outfile] -- grab a (nested) display's root window as PNG
here=$(cd "$(dirname "$0")" && pwd)
D="${1:-:9}"; OUT="${2:-${TMPDIR:-/tmp}/shot.png}"
tmp=$(mktemp "${TMPDIR:-/tmp}/shot.XXXXXX.xwd")
xwd -display "$D" -root -silent > "$tmp" || { rm -f "$tmp"; exit 1; }
python3 "$here/xwd2png.py" "$tmp" "$OUT"
rm -f "$tmp"
