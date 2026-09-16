# Audit, September 2026

A pass over the whole tree (37,000 lines across the toolkit, the window
manager and the programs) looking for bugs, rendering faults and slow
paths, done on 5 September 2026 against 1.10.2. What was run, what was
found, and what changed. A [second pass](#second-pass-8-september-2026),
over the scaling code and over the speed and memory of the whole tree,
follows it.

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

# Second pass, 8 September 2026

A second audit against 1.17.2: the scaling code -- every mode, the
switching between them and the experimental desktop scaling -- read end
to end, and the whole tree measured for time and memory. The same tools
as the first pass, plus a comparison of 120 harness pictures (fifteen
looks and resampling settings, two scales, four desktop dialogs)
rendered before and after every change.

## Scaling

Read: `w2k_px`, `w2k_lp`, `w2k_cx`, `w2k_cw`, `w2k_th` and every caller;
the three modes (screen, sharp and desktop) and the scheme keys that
choose them; the logon path that applies a mode; and the Display
Properties page that sets one. The rounding policy holds everywhere it
was checked: logical to physical truncates, physical to logical rounds,
and a span is the difference of its mapped ends, so neighbouring spans
tile without a gap or an overlap at every whole and fractional scale.
The sanitizer build was run through the harnesses at 100%, 150% and
200% in five looks without a report. Two faults, both in switching:

| Where | What | Fix |
|---|---|---|
| `wm/wm.c` | Turning scaling off left `Xft.dpi` in the X resources at the scaled value, so GTK and Qt programs stayed large at the next logon. | The desktop remembers having set it (`~/.w2k/.xft-dpi-set`) and merges `Xft.dpi: 96` back when it logs on at 100%. |
| `apps/l2kdisplay.c` | A scale, method, mode, resolution or refresh-rate change was applied only if the Settings tab was still showing when OK or Apply was pressed; look at another tab first and it was dropped without a word. | The page keeps a Settings-tab flag and applies from whichever tab is up. |

## Performance

Every program was timed from start to first paint under the render
harness and its peak memory taken. Most start in 20-40 ms in 7-13 MB.
What did not, and what changed:

| Where | Before | After |
|---|---|---|
| Desktop paint, 3840x2160, a 1920x1200 wallpaper filled (cubic) | 0.79 s, 305 MB | 0.47 s, 86 MB |
| `w2k_rgba_resample`, 1920x1080 to 3840x2160, cubic | 0.38 s, 262 MB | 0.10 s, 43 MB |
| same, Lanczos | 1.59 s, 262 MB | 0.10 s, 43 MB |
| same, 3840x2160 down to 1920x1200, Lanczos | 1.27 s, 267 MB | 0.07 s, 43 MB |
| Device Manager start | 0.41 s | 0.05 s |

- **`lib/resample.c`** held three floating-point copies of the picture
  and evaluated the kernel -- two sines for Lanczos -- for every tap of
  every pixel. It now works out each axis's tap weights once and streams
  the picture through a ring of across-filtered rows, holding one output
  row's taps at a time. The pixels are the same, to a rounding tie: an
  exact half, which a hard edge sampled at phase 1/2 gives at 150%, now
  always rounds up instead of going with the order of a floating-point
  sum. (The desktop paint's remaining 86 MB is the 4K X image and the
  decoded picture, both freed after the paint.)
- **`lib/device.c`** ran `modinfo` for every device with a driver at
  start-up, though only the properties sheet shows the result, and
  `lspci` once per PCI slot. `modinfo` runs when a sheet opens;
  `lspci` runs once and each device finds its line by address.
- **`lib/draw.c`** decomposed the visual's colour masks -- shift, width,
  maximum for each channel -- inside `w2k_rgb()`, which is called for
  every pixel of every picture. Once per visual now, with an 8-bit fast
  path.
- **`wm/desktop.c`** stored the wallpaper with `XPutPixel`, a call and a
  branch ladder per pixel, eight million of them at 4K. A 32-bit image
  in the host's byte order is written straight through.

## Also fixed

- `lib/device.c`: PCI devices were named with their slot and class
  ("00:03.1 PCI bridge [0604]: ...") because the name was cut at the
  first colon of the domain-qualified address; the name is now what
  follows the class.
- `lib/win.c`: the timer table held eight, and the ninth timer -- the
  Control Panel's file-types page makes one per class for its caret --
  was dropped in silence. Thirty-two.
- `apps/l2kcontrol.c`: a null check the analyzer asked for;
  `apps/l2kswatch.c`: an icon loop bounded by its table;
  `lib/gtkcolors.c`, `include/w2k.h`: prototype and format warnings.

## What was run

- The warning set of the first pass: 17 warnings, all shadowed locals,
  missing prototypes and one non-literal format; the prototypes and the
  format fixed, the rest cosmetic.
- The analyzer: 51 reports. The stack-overflow claims at
  `apps/l2kcontrol.c:565` are bounded by `MAX_SLIDERS` and the fixed
  radio and check tables; the null-dereference claims in
  `lib/folderwin.c` are guarded a line above; the one in
  `apps/l2kcontrol.c` is now checked.
- AddressSanitizer and UndefinedBehaviorSanitizer through 191 harness
  renders -- thirteen programs and four desktop dialogs, in the classic,
  Windows XP, Windows 7, Windows Vista and Modern dark looks at 100%,
  150% and 200% -- before and after the changes: no reports.
- The 120-picture comparison: the only differences are the rounding
  ties in scaled icons at 150% (563 pixels of 92 million) and the clock.

## How to repeat it

    # the resampler, old against new
    gcc -O2 -Iinclude $(pkg-config --cflags xft freetype2) -o rsbench \
        tools/rsbench.c lib/resample.c -lm && ./rsbench 1920 1080 3840 2160 2
    # a desktop paint at 4K with the wallpaper in ~/.w2k/scheme
    W2K_RENDER=desk.ppm W2K_RENDER_DIALOG=desktop W2K_RENDER_W=3840 \
        W2K_RENDER_H=2160 bin/l2kwm

## Third pass, 9 September 2026

Against 1.25.0, over the whole tree (52,000 lines, 15,000 of them added
since the second pass: the Aero look, the live glass, the Start menu
search, the Windows names, the logon screen). What was run, what it
found, and what changed.

### What was run

- **The whole tree with a wide warning set** (`-Wshadow -Wpointer-arith
  -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Wvla
  -Wnull-dereference -Wduplicated-cond -Wlogical-op -Warray-bounds=2
  -Wstringop-overflow=4`): no bounds or overflow warnings; the rest are
  the same bounded `snprintf` truncations and shadowed locals as before.
- **GCC's static analyzer** over the newest and hottest twenty files: one
  report, `lib/aero.c`'s `w2k_rgb_put`, whose trace needs zero pixels on
  a path that returns early. A false positive.
- **AddressSanitizer and UndefinedBehaviorSanitizer** through some five
  hundred harness renders -- every desktop dialog, both Start panels, the
  classic menu, the taskbar at both button sizes and three orb frames,
  frames active, inactive, maximised and fixed-size, four Display
  Properties and four Task Manager tabs, eighteen Control Panel applets,
  Explorer, Disk Management, the updater and the logon screen with each
  of its three artworks -- in the classic, XP, Vista, Windows 7 Basic,
  Aero and Modern looks: **no reports**, before and after the changes.
- **A 264-picture comparison** of every harness in every look, before
  against after. Forty-three vary between two runs of the same build (a
  clock, Task Manager's live figures). Of the rest, four differ: the Aero
  frames, by one step in one channel, from reordering the reflection's
  arithmetic.
- **Benchmarks** of the paths the reading suggested were hot, and a
  before-and-after test of the two most serious bugs driven through
  XTest.

### Bugs found and fixed

| Where | What | Fix |
|---|---|---|
| `apps/l2kexplorer.c` | With the Search box in use the list showed fewer rows than `entries[]` held, but Delete, Rename, Open and drop all indexed `entries[]` by row: **acting on the wrong file**. Searching `zzz` in a folder of `aaa bbb zzz` and pressing Delete offered to bin `aaa.txt`. | A row-to-entry map, and every consumer goes through `entry_at_row()`. |
| `apps/l2kdm.c` | The logon screen runs as root before anyone logs on and opened `Wallpaper=` and `Picture=` verbatim out of a user-writable file: any user could have root render `/root/anything.png` full-screen, and every keystroke in the name box pointed root's image decoders at another user's file. | A picture is loaded only when it is a regular file, inside that user's own home, owned by them and not a symlink. |
| `apps/l2kpaint.c` | After twelve edits the undo ring's shift left two slots naming one buffer; the next push freed it, so a later undo read and freed it again. | The vacated slot is cleared after the shift. |
| `lib/menu.c` | `XLookupString` wrote into the caller's type-ahead buffer before the printable test could reject the key, so Tab or a Ctrl-letter left a character behind and the Start menu opened a search **instead of running the item that was clicked**. | Looked up into a local buffer and copied out only when accepted. |
| `apps/l2kdevmgmt.c` | The Properties sheet held a raw device pointer while the one-second rescan timer, which frees them all, kept running under the modal loop. | The timer is stopped around the sheet. |
| `lib/wine.c` | `rm -rf` and `wrestool -o` on an unquoted path built from `$HOME`. | The path is quoted, and the cleanup unlinks rather than shelling out. |
| `apps/l2kdiskmgmt.c` | `widths[MAX_PARTS + 2]` indexed by a disk's *region* count, which is up to two per partition. Runs as root. | Sized like `regions[]`. |
| `wm/taskbar.c` | The task-drag index was never re-checked against `ntasks`, so a window closing mid-drag left a write through freed memory. | Cleared in `layout()`. |
| `wm/taskbar.c` | The orb took its overhang from how much of the art shows rather than how far it stands above the bar: no wallpaper was fetched, and it copied 54 rows out of a 40-row buffer. | The overhang is the window's height less the bar's. |
| `wm/taskbar.c` | The orb's click was rewritten with a screen-pixel `y` into a hit test that works in logical pixels, so the Start button missed at 200%. | The logical thickness. |
| `wm/frame.c` | `btn_hot` was only ever set while a button was held, so the Aero caption buttons could never light under the pointer. | Set from the hit test on hover. |
| `wm/balloon.c` | The balloon destroyed its window without `w2k_font_forget()`, leaving an Xft surface bound to a dead drawable. | Forgotten first, as the tooltip already does. |
| `lib/menu.c`, `lib/dialogs.c` | Three modal loops ignored the shutdown flag, so a SIGTERM while a menu, a colour popup or a combo was open left the pointer and keyboard grabbed. | They unwind like their siblings. |
| `wm/tray.c` | Losing the tray selection leaked the manager window and left the docked icons reparented inside a bar that no longer answered for them. | The icons go back to the root and the window is destroyed. |
| `wm/client.c`, `apps/l2ktaskmgr.c` | `XGetTextProperty` allocates even for an empty property; both callers returned without freeing it, on paths that run per title change and per refresh tick. | Freed either way. |
| `lib/edit.c` | `ensure_cap` failed silently and its three callers wrote past the old capacity; the password mask fell back to the plaintext when its own allocation failed; growing the buffer left a typed password in the freed block. | It reports failure, the callers check it, the mask fails closed, and the old buffer is scrubbed. |
| `apps/l2ksnip.c` | `p = realloc(p, n)` unchecked, then dereferenced. | Checked into a temporary; a failed stroke is refused. |
| `apps/l2kdisplay.c` | The wallpaper preview stamped its cache before the path that could fail, so every later repaint copied a pixmap nothing had drawn into; and the wallpaper list leaked a path per row on every rebuild. | The stamp is undone on failure; the rows are freed. |
| `apps/l2kupdate.c` | The checkout path was spliced unquoted into a script that runs `make install` as root. | Quoted. |
| `wm/startsearch.c` | A result held a bare index into the program list, which All Programs re-sorts: clicking one afterwards could **launch a different program**. | A result carries its own command line. |
| `lib/icon.c` | Four reallocs in a row, any of which could leave a freed pointer in place. | Committed one at a time. |
| `apps/l2kpaint.c` | A failed allocation part way through a resize left the dimensions disagreeing with the layers. | All layers are built first and swapped in together. |
| `wm/glass.c` | A window's origin was taken as the inside of its border. | Corrected. |
| `wm/startpanel.c`, `wm/startsearch.c` | Unchecked grabs could leave a panel that nothing dismissed. | Checked; the panel closes instead. |
| `lib/aero.c` | The user tile's cache was keyed on a variable belonging to another function, so it could reload the artwork on every repaint. | Its own key. |
| `apps/l2kscaler.c` | A failed SHM fetch cleared the accumulated damage, leaving that region stale on screen. | Cleared only after the fetch works. |

### Speed and memory

| Where | Was | Now |
|---|---|---|
| `wm/volume.c` | A shell and two `pactl` processes every five seconds for the life of the session -- 6.7 ms and a wakeup, some seventeen thousand times a day | One long-lived `pactl subscribe`, read from the main loop; the level is asked for when something changes it. The timer stays only for bare ALSA |
| `lib/list.c` | Smooth scrolling slept 21 ms inside the button handler and forced three full repaints a notch | Driven by the timer; the event loop keeps running |
| `wm/pins.c` | Every taskbar repaint re-read the pinned-items file, nine times in a quarter second through the orb's glow | Kept until the file changes |
| `lib/win.c` | A radio button was about 226 X requests | Five, one per colour |
| `lib/list.c` | A tree connector dot was two requests | One per run |
| `lib/draw.c` | The Start menu's vertical banner was 559 single-pixel requests per hover | Batched |
| `lib/icon.c` | At any scale but 100% every icon built a pixmap and mask that nothing drew | Built only on the path that uses it |
| `lib/aero.c` | The reflection did two divisions and two smoothsteps per pixel; the caption buttons were recoloured on every repaint | Separated into a row term and a column table; the strip is kept per state |
| `wm/glass.c` | Each of a frame's four pieces re-walked the window stack | One walk per frame |
| `wm/input.c` | Show Desktop grabbed the server for a tenth of a second per window and relaid the bar twice each | Once for the set |
| `wm/frame.c` | The caption buffer was freed and remade on every motion event of a horizontal resize; the cursor was set on every motion event | Widened in steps; set only when it changes |
| `apps/l2ktaskmgr.c` | `/proc/<pid>/cmdline` re-read every tick for every process with a truncated name | Once, when the process is first seen |
| `apps/l2kscaler.c` | Around 25 uniform lookups by name per monitor per frame | Looked up once |
| `apps/l2kdevmgmt.c` | The Resources tab read sysfs on every expose, and Details ran a `modinfo` whose answer was discarded | Read once per device; the dead call is gone |
| `lib/list.c` | The view rectangle was recomputed for every item on every motion event of a rubber-band sweep | Once per sweep |

### Withdrawn

**Show shadows under menus.** The effect drew a second window offset down
and right, shaped to a half-tone so what lay under it showed through at
half strength -- which is how it looked on hardware without alpha. It was
wrong against every backdrop and worse under a compositor, so the option
is gone from Performance Options and the code with it. The slot in the
scheme file's `Effects` line is kept, since that line is positional, and
anything already set there is ignored.

## Fourth pass, 15 September 2026

Against 1.35.0, over the whole tree: 65,000 lines, some 13,000 of them new
since the third pass (USB setup in Disk Management, Explorer's mount and
eject, the portal dialogs, System Properties and the drive sheet, the
graphics chooser, Paint, the Calculator, the colour picker, the composited
scaler, Bluetooth Devices). It began from three reports: some property
panels -- Network Connections among them -- were slow to open, the XP-style
Start menu was broken in the Modern Light and Modern Dark looks, and the
slide and window animations were slow and glitchy.

### What was run

- **Eight read-only audits by area** -- the window manager, the Start menus
  and the shell's dialogs, the toolkit core, Explorer and Disk Management,
  the slow panels, Display Properties with the scaler and the logon screen,
  Paint with the Calculator and the picture tools, Bluetooth with the
  portal -- each finding traced to a way of triggering it.
- **AddressSanitizer and UndefinedBehaviorSanitizer** through all 62 render
  harnesses, and through the window manager driven with XTest on a private
  display (dialogs and their owners, five look changes, Show Desktop both
  ways, snaps, full screen, Alt+Tab, the Start menu, the Recycle Bin
  filling and emptying): **no reports**. A JPEG with a stray SOI marker
  after its scan data aborts 1.35.0's picture viewer with a double free;
  it is now refused.
- **Every harness rendered before and after.** What differs is what was
  meant to: the combo-box and submenu arrows (they pointed left), a 4Kn
  disk's picture, and the live figures that differ between any two runs.
- **XTest checks of the window manager**: a modal dialog stays above its
  window and takes the focus when the window is clicked, and is minimised
  and restored with it; frames re-lay out through Classic (4/22), XP
  (4/30) and Aero (8/36) with their extents published; Show Desktop brings
  windows back in their order; a full-screen window covers the bar only
  while it has the focus; a snapped window is no longer "maximised"; a drag
  lands where the button came up; the bin icon follows the Trash within
  0.3 s.

### Bugs found and fixed

| Where | What | Fix |
|---|---|---|
| `lib/image.c` | The JPEG error path freed its two buffers twice. A picture with a stray marker after its scan data crashed whatever opened it: the logon screen as root, the shell at every logon when it was the wallpaper. | Freed once. |
| `apps/l2kdm.c`, `lib/logon.c`, `lib/account.c` | The logon screen runs as root and read `~/.w2k/logon` and `~/.w2k/account` with a plain `fopen`: a FIFO there hung it for good, across restarts, and a link to `/dev/zero` was read without end. Pictures were checked with `lstat` and opened by path later, and root's decoders parsed them. | Settings are opened confined (no FIFO, no symlink, the home's owner, 64 KB). Pictures are decoded by a child running as that user, with a memory and time limit and no descriptor but its pipe. |
| `apps/l2kdm.c` | The session leader inherited the service's SIGTERM handler: stopping the service or shutting down killed the X server and left without closing the PAM session. | It passes the signal to the desktop, waits, and closes the session. |
| `apps/l2kdm.c` | `initgroups` after `pam_setcred` threw away the groups pam_group adds; the password stayed in the service's PAM handle until logoff; `DISPLAY` stayed set for the re-exec, so every logoff failed and waited for systemd; a shutdown asked for in the session never reached it. | Groups first; the handle is released at the fork; the variables are unset; `l2k-session` hands 10 and 11 back. |
| `l2k-session` | The nested server's cookie went on `xauth`'s command line, and without `xauth` the nested server ran with no cookie at all; a scaler that failed to start left the desktop invisible; the log grew for ever. | Through a pipe, and never without one; the start is checked and watched; the last session's log is kept. |
| `apps/l2kexplorer.c`, `lib/fileops.c` | Paste's Replace deleted the destination first -- a folder holding the source included -- and a copy that then failed lost both. Send To merged into folders unasked. | `w2k_fs_put`: the old item is set aside and put back on failure, the same file is known by device and inode, folders merge. |
| `apps/l2kexplorer.c` | The execute bit was trusted on exFAT and NTFS sticks, where every file has it: a document double-clicked ran as a shell script, and a `.desktop` file on a stick ran its `Exec=`. | Only ELF files and `#!` scripts run, never from a filesystem without permissions; a shortcut is trusted when it is the user's own on one with them. |
| `apps/l2kexplorer.c` | Ctrl+Z typed in the Search or Address box undid file operations; undoing New deleted a document written since; undoing a rename or move replaced whatever had the name. | Keys stay in the boxes; a New item with content goes to the bin; undo never replaces. |
| `apps/l2kexplorer.c`, `lib/fileprops.c`, `wm/desktop.c` | Rename, the Properties name box and the Hidden box replaced an existing file without a word. | `renameat2` with `RENAME_NOREPLACE`, and a message. |
| `lib/fileprops.c` | For a symlink the mode shown was the link's 0777, and an edit was applied to the target: Read-only on a link to a private key made it world-readable. | The target's mode is shown and never changed through the link. |
| `lib/trash.c` | `Path=` was written raw and the last one won on restore, so a crafted name restored anywhere; the bin was 0755. | Percent-encoded, the first one in the group, `.trashinfo` claimed with `O_EXCL`, 0700. |
| `apps/l2kexplorer.c` | Create Shortcut wrote raw names into `Name=` and `Exec=`; a drag across drives moved; tar with "delete files after adding" deleted the new archive. | Escaped; copies (Shift moves); `--remove-files`. |
| `lib/driveprops.c` | Re-reading the sheet after Disk Cleanup wrote over its own input and could relabel the root filesystem; "temporary files" deleted week-old folders holding today's files. | Its inputs are copied; only files a week old go, never sockets. |
| `apps/l2kdiskmgmt.c` | Device names are handed out again as disks come and go: after a stick was swapped for a backup drive, Initialize on the stale row wiped the drive. Partition numbers came from kernel names that lag behind, and failures were swallowed. | Every root script checks the disk's `diskseq` and the partition's start first; the number is looked up by start in the table on the disk; failures are reported. |
| `apps/l2kdiskmgmt.c` | "Busy" was a mount at `/`, `/boot` or `/usr`: the EFI System Partition, a RAID or ZFS member and a btrfs subvolume root could be deleted in use. | Every mount, swap, `/etc/fstab` and an exclusive open are asked; boot and recovery partitions on fixed disks are protected, and the reason is shown. |
| `apps/l2kdiskmgmt.c` | lsblk counts `START` in 512-byte sectors; multiplied by a 4Kn disk's 4096, partitions were drawn eight times too far along. The selection stayed at its index across a rescan. | 512; the selection is kept by what it is, and prompts name the partition. |
| `apps/l2kportal.c` | A sandboxed program's "save" could name dotfiles and paths. | Refused before any dialog; replacing asks. |
| `apps/l2kbluetooth.c` | BlueZ's signals were taken from any sender; Return accepted a pairing the moment the prompt appeared; trusted devices were accepted unasked. | The sender and signature are checked; input waits 750 ms; nothing is accepted unasked. |
| `apps/l2knetwork.c` | The Wi-Fi key went on `nmcli`'s command line. | A masked prompt, and `--ask` on standard input. |
| `wm/notifyd.c`, `wm/startdir.c`, `apps/l2kupdate.c` | SIGTERM to any owner of the notification name; Startup programs run unquoted; the installer fetched over plain HTTP. | Only our own daemons of the same user; quoted; `curl -fsSL --proto '=https'`. |
| `lib/dialogs.c`, `apps/l2knotepad.c` | Save As replaced a file without asking; Notepad's save turned a symlinked file into a plain one and split hard links. | It asks; the save goes through the link, in place for hard links, keeping the owner. |
| `wm/startpanel.c` | The XP-style Start menu in Modern Light and Dark: the footer's Log Off and Turn Off Computer white on white, the name's shadow smeared, the rules invisible, the search box in the wrong colours. | Colours from the scheme with a contrast guard. |
| `lib/menu.c`, `lib/dialogs.c`, `wm/startpanel.c` | Submenu arrows and combo-box arrows pointed left. | Right and down. |
| `lib/font.c` | Text in a colour outside the palette (the Calculator's keys, the looks' panel text) stopped being drawn once eight such colours had been asked for in a session: the cache was full and answered nothing. | Its slots are reused in turn. |
| `lib/anim.c`, `lib/menu.c`, `lib/dialogs.c`, `lib/bars.c` | Menu and combo slides flashed the window's background at every step and grew an empty box. 1.36.0's first answer moved a full-size window in from outside its place, which left the taskbar and whatever else it crossed blank until the slide was over. A menu-bar title, and an item with a submenu, lit only after the menu had finished sliding. | 1.36.1: what is on the screen under the menu is copied first, and the picture slides in over it inside the menu's own rectangle -- nothing around it is touched, and nothing is cleared to a plain colour. Titles and items light first. (Under a compositor the menu just appears.) |
| `wm/input.c` | 1.36.0 replaced the minimise and restore wire frame with a caption-coloured bar. | 1.36.1 puts the wire frame back: it looked better. |
| `wm/client.c` | Nothing kept a dialog above its owner: clicking a program's window buried its modal dialog, which has no task button, and the program looked hung. | Dialogs rise with their owner, a modal one takes the focus, and they are minimised and restored with it. |
| `wm/wm.c`, `wm/client.c` | A look change left every frame at the old size (a new caption half under the client), the work area and maximised windows unchanged, and `_NET_FRAME_EXTENTS` stale. | Re-laid out from the frame's corner, refitted, published. |
| `wm/client.c`, `wm/input.c` | A window mapped full screen was cascaded, frameless, and vanished when it left; full screen from maximised lost the restore size; a full-screen window stayed over everything unfocused; Win+Left on a maximised window left it stuck "maximised"; a drag landed short of the pointer; with outlines, a snap was overridden. | Each fixed; full screen covers the bar only while focused. |
| `wm/wm.c`, `wm/client.c` | The in-place restart (updates) ran the Startup folder again, moved and unminimised windows, and framed tray icons; windows on a monitor switched off stayed there; `_NET_WORKAREA` was the primary monitor only. | Fixed; windows come onto the nearest monitor; the screen less the bar. |
| `wm/taskbar.c`, `wm/desktop.c` | Task buttons never lit on hover in the themed looks; the orb's glow never animated; the clock ignored a new time zone; Refresh on the desktop did not re-read it, and the selection stayed at its index. | Fixed. |
| `wm/startmenu.c`, `wm/programs.c` | Recent documents were decoded as pictures to draw their icons, freezing the menu on big files; Wine's entries never launched (their escapes were not undone); a hidden user entry did not hide the system one. | Icons by type; the escapes undone; the user's entry wins. |
| `apps/l2kdisplay.c`, `lib/app.c`, `lib/win.c` | Choices not yet applied were lost to any other program's Apply, and every save undid what other programs had applied since. | Reloads wait while a dialog has changes; a save writes its own changes over the file as it is now. |
| `apps/l2kdisplay.c` | OK closed the dialog after a refused Apply; Primary and Extend were lost on a tab switch; the lists were stale after Apply; Cancel left the new scaler filter on screen. | Fixed. |
| `apps/l2ktaskmgr.c`, `apps/l2kdevmgmt.c` | The lists jumped to the top every second, CPU read 375% after returning to Processes; Device Manager's right-click acted on the old selection. | Fixed. |
| `apps/l2kpaint.c`, `lib/dialogs.c` | Paint: undo across layer changes, a flood-fill hole, Pick reading the bottom layer, a gradient writing past the canvas, a crash opening pictures over 8192 px, quitting without asking; a colour typed in hex was dropped on OK. | Fixed. |

### Speed and memory

| Where | Was | Now |
|---|---|---|
| `wm/programs.c`, `lib/xdgicon.c` | The Start menu rescanned every program folder and probed the icon theme file by file at every open: 620 ms cold | The scan kept until a folder changes, icon lookups indexed and cached: 60 ms |
| `apps/l2kdisplay.c`, `lib/monitors.c` | `xrandr --query` re-probed every monitor before the window opened, and the whole wallpaper was decoded for a 160-pixel preview: 300 ms with a 24-megapixel photograph | The server's current information; the picture decoded at 1/8 by libjpeg after the window is up: 94 ms |
| `apps/l2knetwork.c`, `lib/driveprops.c`, `apps/l2kdevmgmt.c` | Scans, connects, the Disk Cleanup walk and `modinfo` ran before the window could paint | In the background, or after the first paint |
| `wm/desktop.c` | Every Apply anywhere decoded and resampled the wallpaper again, 0.5-1 s of frozen shell on a wide desktop | Rebuilt only when its inputs change, and decoded no bigger than the monitors need |
| `wm/volume.c` | With no sound server, `pactl` was started again on every pass of the loop: 780 wakeups a second | A backoff from 5 s to a minute: under one a second |
| `wm/desktop.c`, `wm/wm.c` | The shell woke every 2 s for the Recycle Bin, and asked the server for the idle time at every wakeup | The bin's folder is watched; the idle time every 30 s |
| `wm/frame.c`, `lib/draw.c`, `wm/client.c`, `wm/wm.c` | Every step of a drag rebuilt the frame's shape as a mask; a caption gradient was two requests per column; every click in every program waited on a restack; a title change repainted twice | The shape kept until its size changes, as rectangles; one request per shade; the click goes on first and a window already on top is not restacked; unchanged titles ignored |
| `lib/win.c`, `lib/app.c` | The event loop woke every second; exposures repainted; colour conversion looked the visual's masks up for every pixel | It sleeps until something is due; exposures are copied from the buffer; the masks are kept |
| `apps/l2kdm.c`, `lib/skin.c` | The logon wallpaper was built one `XPutPixel` per pixel, eight million at 4K | A row of words at a time |
| `apps/l2ktaskmgr.c`, `apps/l2kpaint.c` | Command lines re-read every tick; Paint composited every screen pixel over all layers at each motion | Once per process; a kept canvas, updated where it changed |

### Not tested here

A PAM login and anything run as root (the logon screen's switch to the
user, Disk Management's scripts); Wi-Fi (there is no card); real monitors
and their EDID reads; a real 4Kn disk.

### Still open

Reported and left for later: the scaler's damage merging, pointer polling
and mixed-scale layouts; `_NET_MOVERESIZE_WINDOW` taking the client's
origin; the live glass's cost; Explorer re-reading a folder for every
sort, drag-and-drop with no timeout, the 64-item limits and copies without
progress; the Calculator's operator semantics; PNG size limits; the
updater's blocking checks; Wine icon extraction on the listing path.

### Corrected since

| Where | Was | Now |
|---|---|---|
| `wm/client.c` | The shortcut above for a window already on top also skipped the restack when a window came back from minimised, and minimised frames were left out of the stacking: the only window open, minimised and restored, came back under the desktop, unseen | Coming back from minimised always restacks, and minimised frames keep a place above the desktop |
| `lib/win.c` | A message box paragraph that wrapped before a line break lost the rest of itself (since the first release) | Past the line break only once the paragraph is used up |
