# Icons

`win2k/` holds the original Windows 2000 shell icons (275 `.ico` files),
taken unmodified from https://github.com/trapd00r/win95-winxp_icons .
The artwork is Microsoft's; it is used here for a free, non-commercial
hobby Windows 2000-like desktop, Linux 2000.

`tools/genicons.py` selects the icons the desktop needs, extracts the 16x16
and 32x32 images and writes them as C arrays to `lib/icon_data.inc`, which
is compiled into `libw2k` so the binaries stay self-contained.

Any icon can also be overridden at runtime without rebuilding: drop a file
named `<slug>.ico` into `~/.w2k/icons/` (or the directory named by
`$W2K_ICON_DIR`). Run `l2kwm --icons` to list the slugs.

`win98/` holds a handful of PNGs from https://github.com/alexh/vintage-icons
(Windows 98 system icons). They are used only for artwork that Windows 2000
shipped unchanged from 98 and that the `win2k/` set lacks: the user32
message-box icons (error, question), the Calculator icon and the Task
Manager icon.

`win95/` holds the Explorer toolbar buttons from the Chicago95 theme
(https://github.com/grassmunk/Chicago95, Icons/Chicago95-tux/actions/16):
Cut, Copy, Paste, Delete and Properties are the comctl32 standard toolbar
bitmaps, unchanged from Windows 95 through 2000; Back, Forward and Up are the
Windows 98 / IE4 shell arrows, used until the Windows 2000 (IE5) strips are
extracted from an install CD. `view-list-details.png` is shell32's
list-window icon standing in for the Views button.

## Switchable icon sets (`sets/`)

Display Properties > Appearance > Icons lets the user pick whose icons the
desktop draws. Each directory under `sets/` holds `<slug>.ico` files (16 and
32 pixel images) for the slots `l2kwm --icons` lists; a slot a set lacks
falls back to the built-in Windows 2000 artwork. The sets are produced by
`tools/geniconsets.py`, which records where each icon comes from:

- `winxp/` -- Windows XP's shell32, user32, shimgvw and the applications'
  own icons, from a resource dump of an XP installation (not in the
  repository: `icons/winxp/` is ignored).
- `win7/` -- Windows 7's imageres and shell32 icons and the applications'
  own, likewise from a dump (`icons/win7/`, ignored).
- `reactos/` -- ReactOS's shell32, user32 and application icons from
  https://github.com/reactos/reactos (GPL-2.0-or-later); the pixel-art
  reimplementation of the same slots.
- `win98/` -- Windows 98 icons from https://github.com/alexh/vintage-icons.

The XP and 7 artwork is Microsoft's, used as the Windows 2000 set above is.
A set of your own goes in `~/.w2k/iconsets/<name>/` and appears in the
list; it needs at least `folder.ico`.
