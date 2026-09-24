#!/usr/bin/env python3
"""Build an Xcursor theme from a Windows .cur set.

The window manager and the toolkit load .cur files directly (lib/cursor.c),
but every other application on the desktop -- GTK, Qt, Firefox, xterm --
asks libXcursor for a *theme* by name. Without one they fall back to Adwaita
or the X core font, which is why the pointer changes shape and size the
moment it crosses into another program's window.

Each image is registered under every nominal size a toolkit is likely to ask
for, all of them the same 32x32 bitmap. libXcursor picks the nominal size
nearest the request and uses those pixels as they are, so the cursor is
identical at any DPI and is never resampled.

    tools/gencursortheme.py ~/.w2k/cursors ~/.icons/Windows2000
"""
import os, struct, sys

NOMINAL_SIZES = [8, 12, 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 96, 128]
CHUNK_IMAGE = 0xfffd0002

# Role -> the names applications actually ask for. The first is the real
# file; the rest become symlinks to it.
NAMES = {
    "Arrow":    ["left_ptr", "default", "arrow", "top_left_arrow", "left_arrow",
                 "right_ptr", "X_cursor", "copy", "alias", "link", "dnd-copy",
                 "dnd-link", "dnd-ask", "zoom-in", "zoom-out", "sb_left_arrow",
                 "sb_right_arrow", "sb_down_arrow", "based_arrow_down",
                 "based_arrow_up", "exchange", "draft_large", "draft_small"],
    "IBeam":    ["xterm", "ibeam", "text", "vertical-text"],
    "Wait":     ["watch", "wait", "clock", "pirate"],
    "AppStarting": ["left_ptr_watch", "progress", "half-busy"],
    "Hand":     ["hand2", "hand1", "hand", "pointer", "pointing_hand",
                 "grab", "openhand"],
    "SizeAll":  ["fleur", "move", "all-scroll", "size_all", "grabbing",
                 "closedhand", "dnd-move"],
    "SizeNS":   ["sb_v_double_arrow", "v_double_arrow", "ns-resize", "size_ver",
                 "double_arrow", "n-resize", "s-resize", "top_side",
                 "bottom_side", "row-resize", "split_v"],
    "SizeWE":   ["sb_h_double_arrow", "h_double_arrow", "ew-resize", "size_hor",
                 "e-resize", "w-resize", "left_side", "right_side",
                 "col-resize", "split_h"],
    "SizeNWSE": ["top_left_corner", "bottom_right_corner", "nwse-resize",
                 "size_fdiag", "nw-resize", "se-resize", "fd_double_arrow",
                 "sizing", "ul_angle", "lr_angle"],
    "SizeNESW": ["top_right_corner", "bottom_left_corner", "nesw-resize",
                 "size_bdiag", "ne-resize", "sw-resize", "bd_double_arrow",
                 "ur_angle", "ll_angle"],
    "No":       ["crossed_circle", "not-allowed", "no-drop", "forbidden",
                 "circle", "dnd-none"],
    "Help":     ["question_arrow", "help", "whats_this", "left_ptr_help",
                 "context-menu"],
    "Crosshair": ["crosshair", "cross", "tcross", "cell", "color-picker"],
    "UpArrow":  ["up_arrow", "center_ptr", "sb_up_arrow"],
    "NWPen":    ["pencil", "draft"],
}


def read_scheme(directory):
    """Role (lower case) -> filename, from the .crs file cursor packs ship
    with. user.crs -- the one Mouse Properties' Browse... writes -- first,
    then the set's own for the roles it leaves out: Browse... writes only the
    ten the desktop draws, and user.crs read alone left the busy, help,
    crosshair, up-arrow and pen pointers out of the theme.
    Role names are matched in any case, as lib/cursor.c matches them."""
    out = {}
    entries = sorted(os.listdir(directory),
                     key=lambda e: (e.lower() != "user.crs", e))
    for entry in entries:
        if not entry.lower().endswith(".crs"):
            continue
        role = None
        with open(os.path.join(directory, entry), "r",
                  errors="replace") as handle:
            for line in handle:
                line = line.strip().lstrip("﻿")
                if line.startswith("[") and line.endswith("]"):
                    role = line[1:-1].strip().lower()
                elif line.lower().startswith("path=") and role:
                    out.setdefault(role, line[5:].strip())
                    role = None
        if entry.lower() != "user.crs":
            break                       # the set's own scheme: one is enough
    return out


def decode_cur(path):
    """First image of a .cur as (width, height, xhot, yhot, [ARGB...]), or
    None. Checked as lib/cursor.c checks it: a file the desktop accepts --
    Browse... lets the user pick any of those -- must not stop the
    installer here with an IndexError."""
    data = open(path, "rb").read()
    n = len(data)
    if n < 22 or struct.unpack_from("<H", data, 4)[0] < 1:
        return None
    w, h, ncol, _, xhot, yhot, size, off = struct.unpack_from("<BBBBHHII", data, 6)
    w, h = w or 256, h or 256
    if size < 40 or off + size > n:
        return None
    hdr = struct.unpack_from("<I", data, off)[0]
    if hdr < 40:
        return None
    bw, bh, _planes, bpp = struct.unpack_from("<iihh", data, off + 4)
    ncol = struct.unpack_from("<I", data, off + 32)[0]
    w, h = bw, bh // 2 if bh == 2 * h else bh
    if not (0 < w <= 256 and 0 < h <= 256):
        return None
    if bpp not in (1, 4, 8, 24, 32):
        return None
    if not ncol and bpp <= 8:
        ncol = 1 << bpp
    if ncol > 256:
        return None
    pal = off + hdr
    xor = pal + ncol * 4
    xs = ((w * bpp + 31) // 32) * 4
    ms = ((w + 31) // 32) * 4
    and_ = xor + xs * h
    if and_ > n:
        return None
    have_mask = and_ + ms * h <= n
    if xhot >= w: xhot = 0
    if yhot >= h: yhot = 0

    px = [0] * (w * h)
    invert = [False] * (w * h)
    for y in range(h):
        row = xor + (h - 1 - y) * xs
        for x in range(w):
            if bpp == 1:
                idx = (data[row + (x >> 3)] >> (7 - (x & 7))) & 1
            elif bpp == 4:
                idx = (data[row + (x >> 1)] >> (0 if x & 1 else 4)) & 0xf
            elif bpp == 8:
                idx = data[row + x]
            else:
                idx = None
            if idx is None:
                step = 3 if bpp == 24 else 4
                b, g, r = data[row + x * step: row + x * step + 3]
                a = data[row + x * 4 + 3] if bpp == 32 else 255
            else:
                if idx >= ncol:
                    idx = 0
                b, g, r = data[pal + idx * 4: pal + idx * 4 + 3]
                a = 255
            mbit = have_mask and \
                (data[and_ + (h - 1 - y) * ms + (x >> 3)] >> (7 - (x & 7))) & 1
            if mbit:
                # Masked: transparent, or "invert the screen" when the colour
                # bit is set too -- which X cannot do, so it becomes black
                # with a white halo, exactly as lib/cursor.c does it.
                if bpp == 1 and idx:
                    invert[y * w + x] = True
                a = 0
            px[y * w + x] = 0 if not a else (0xff << 24) | (r << 16) | (g << 8) | b

    for i, inv in enumerate(invert):
        if inv:
            px[i] = 0xff000000
    for y in range(h):
        for x in range(w):
            if not invert[y * w + x]:
                continue
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    ny, nx = y + dy, x + dx
                    if 0 <= ny < h and 0 <= nx < w and not px[ny * w + nx] \
                            and not invert[ny * w + nx]:
                        px[ny * w + nx] = 0xffffffff
    return w, h, xhot, yhot, px


def write_xcursor(path, image):
    w, h, xhot, yhot, px = image
    ntoc = len(NOMINAL_SIZES)
    header = struct.pack("<4sIII", b"Xcur", 16, 0x10000, ntoc)
    toc, chunks = b"", b""
    pos = len(header) + 12 * ntoc
    body = struct.pack("<IIIIIIIII", 36, CHUNK_IMAGE, 0, 1, w, h, xhot, yhot, 0)
    body += b"".join(struct.pack("<I", p) for p in px)
    for size in NOMINAL_SIZES:
        toc += struct.pack("<III", CHUNK_IMAGE, size, pos)
        # subtype (the nominal size) appears in both the TOC and the chunk
        chunk = bytearray(body)
        struct.pack_into("<I", chunk, 8, size)
        struct.pack_into("<I", chunk, 12, 1)
        chunks += bytes(chunk)
        pos += len(chunk)
    open(path, "wb").write(header + toc + chunks)


def main():
    src = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1
                             else "~/.w2k/cursors")
    dst = os.path.expanduser(sys.argv[2] if len(sys.argv) > 2
                             else "~/.icons/Windows2000")
    name = os.path.basename(dst)
    cursors = os.path.join(dst, "cursors")
    os.makedirs(cursors, exist_ok=True)

    scheme = read_scheme(src)
    made = 0
    for role, names in NAMES.items():
        filename = scheme.get(role.lower())
        if not filename:
            continue
        path = os.path.join(src, filename)
        if not os.path.exists(path):
            continue
        try:
            image = decode_cur(path)
        except (IndexError, ValueError, struct.error, OSError):
            image = None
        if not image:
            print(f"  skipped {role}: cannot decode {filename}")
            continue
        real = os.path.join(cursors, names[0])
        write_xcursor(real, image)
        made += 1
        for alias in names[1:]:
            link = os.path.join(cursors, alias)
            if os.path.lexists(link):
                os.remove(link)
            os.symlink(names[0], link)

    with open(os.path.join(dst, "index.theme"), "w") as handle:
        handle.write("[Icon Theme]\n"
                     f"Name={name}\n"
                     "Comment=Windows 2000 cursors, at a fixed size\n"
                     "Inherits=Adwaita\n")
    with open(os.path.join(dst, "cursor.theme"), "w") as handle:
        handle.write(f"[Icon Theme]\nInherits={name}\n")
    print(f"{made} cursors -> {cursors}")


main()
