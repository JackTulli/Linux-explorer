# Audit, September 2026

A pass over the whole tree (37,000 lines across the toolkit, the window
manager and the programs) looking for bugs, rendering faults and slow
paths, done on 5 September 2026 against 1.10.2. What was run, what was
found, and what changed.

## What was run

- **Every source file compiled with a wide warning set** (`-Wall -Wextra
  -Wshadow -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes
  -Wformat=2 -Wvla -Wnull-dereference -Wduplicated-cond -Wlogical-op`):
  16 warnings, all shadowed locals, unused parameters and missing
  prototypes; none a defect.
- **GCC 14's static analyzer** (`-fanalyzer`) over every file: 120 reports,
  60 of them possible `snprintf` truncation (bounded, benign), and the
  rest listed below.
- **AddressSanitizer and UndefinedBehaviorSanitizer** builds of the whole
  tree, driven through every render harness (window frame, taskbar, both
  Start menus, the balloon, Explorer, Control Panel and each of its
  applets, Network and Dial-up Connections, all four Display Properties
  pages, Notepad, Calculator, Task Manager, Character Map, Device Manager,
  Imaging, Snipping Tool, About Linux 2000 and Linux 2000 Update) in the
  classic, Windows XP and Windows Classic Dark looks: no reports.
- **Fuzzing** of the PNG, JPEG, BMP and ICO decoders (1,200 mutations of
  each sample) and the .cur decoder (1,500 mutations of two cursors)
  under the sanitizers: no reports.
- **Leak checking** of the window manager, Explorer, Control Panel and
  Notepad harness runs: the only leaks are fontconfig's own one-time
  allocations; nothing of ours leaks per event.
- **A visual sweep**: contact sheets of all 26 harness renders in each
  of the three looks, read for mispaints.
- **Live tests** of the Snipping Tool's save paths with synthetic input
  (done for 1.10.2 and repeated here).

## Bugs found and fixed

| Where | What | Fix |
|---|---|---|
| `lib/png.c` | A palette PNG whose PLTE chunk is missing or short read colours from an uninitialised palette table. | The table starts zeroed, so such pixels come out black rather than as stack garbage. |
| `wm/startpanel.c` | The Luna Start panel wrote through `push()`'s return value without checking it; a full row table would have been a null write. | Every use checks the row. |
| `lib/cursor.c` | `w2k_cursors_init()` dereferenced the slot for every cursor role; a role without a slot would crash. | The slot is checked. |
| `lib/list.c` | `tree_next_visible()` dereferenced its argument before its own null check. | Null is handled first. |
| `apps/l2kcontrol.c` | Default Programs focused its first edit box even with no boxes. | Guarded. |
| `lib/assoc.c` | The `.desktop` name handed to `xdg-mime` was placed inside single quotes by hand; a quote in the name would have broken out. | It goes through `w2k_shell_quote()`. |
| `lib/edit.c`, `lib/list.c` | The growable text and item buffers assigned `realloc()`'s result straight back, so a failed allocation lost the old block and then dereferenced null. | The new block is checked and the old one kept on failure. |
| `apps/l2kdm.c` | The typed password stayed in the logon box's memory for the whole session after PAM had used it. | `w2k_edit_wipe()` scrubs the box as soon as authentication returns. |
| `wm/wm.c` | If the notification service's D-Bus descriptor went bad, `select()` failed with `EBADF` and the main loop treated it as fatal: the desktop would have logged off. | `EBADF` closes the service and carries on; only other errors end the loop. |
| `apps/l2kupdate.c` | Links and the "updates only when you ask" note were dark blue and dark grey on the Windows Classic Dark window colour, so they vanished. | Links lighten on a dark window, as the folder windows' do; the note uses the scheme's grey. |

## Performance

| Where | What | Change |
|---|---|---|
| `lib/list.c` | Icon-view labels were wrapped by measuring every prefix of the name, so a folder of a hundred long names cost thousands of text measurements on every repaint and scroll. | Measured at the spaces only, with a bisection for a single long word. |
| `lib/draw.c` | `w2k_ellipsis()` shortened a string one character at a time, measuring each time; a report view of long names paid for it on every row it drew. | The longest fitting prefix is found by bisection. |

Checked and left alone: the window manager sleeps in `select()` until X
or a due timer (clock, tooltip, balloon) needs it, so an idle desktop
wakes once a minute; the list views draw only the visible rows; Explorer
does one `lstat` per entry and looks icons up through a cache; the
Snipping Tool dims a 4K screen with a word-at-a-time pass rather than
per-pixel calls; the Xft draw surfaces and text faces are cached; the
taskbar keeps its back buffer between repaints; the wallpaper is rendered
once per change.

## Rendering

The sweep found one fault (the update page on the dark scheme, above).
Everything else — frames, menus, both Start menus, dialogs, the folder
windows, the balloon — painted correctly in all three looks.

## Not changed

- 60 `-Wformat-truncation` notes: every one is a bounded `snprintf` into
  a buffer sized for the normal case, where truncation loses nothing that
  matters (a display label, a path far beyond `PATH_MAX`).
- The shadowed-variable and unused-parameter warnings: cosmetic.
- The `system()` and `popen()` calls: each runs a fixed command or one
  built from numbers, an enumerated name, or a value quoted with
  `w2k_shell_quote()`.

## How to repeat it

    # warnings
    for f in lib/*.c wm/*.c apps/*.c; do gcc -std=gnu11 -Wall -Wextra -Wshadow \
        -Iinclude $(pkg-config --cflags xft freetype2 dbus-1) -DHAVE_DBUS \
        -DW2K_VERSION='"x"' -fsyntax-only "$f"; done
    # the analyzer
    ... -fanalyzer -c -o /dev/null "$f"
    # sanitizers: build a copy of the tree with
    make CFLAGS="-std=gnu11 -O1 -g -fsanitize=address,undefined -Iinclude \
        $(pkg-config --cflags xft freetype2 dbus-1)" LDFLAGS="-fsanitize=address,undefined"
    # then run the harnesses with W2K_RENDER=<file.ppm> [W2K_RENDER_DIALOG=...]
