#!/bin/sh
# shot.sh [display] [outfile] -- grab the nested display's root window as PNG
D="${1:-:9}"; OUT="${2:-/tmp/claude-1000/-home-jack-Linux-explorer/8b8b59b2-494e-4b7a-84c6-e182e20dccbc/scratchpad/shot.png}"
xwd -display "$D" -root -silent > /tmp/claude-1000/-home-jack-Linux-explorer/8b8b59b2-494e-4b7a-84c6-e182e20dccbc/scratchpad/.shot.xwd || exit 1
python3 /home/jack/Linux-explorer/tools/xwd2png.py /tmp/claude-1000/-home-jack-Linux-explorer/8b8b59b2-494e-4b7a-84c6-e182e20dccbc/scratchpad/.shot.xwd "$OUT"
