# Windows 2000 for X11

A window manager and desktop environment that recreates the Windows 2000
shell on X11 — the classic 3D look, the gradient title bars, the taskbar,
the Start menu, and the applications that came with it — written from
scratch in C against Xlib and nothing else.

    bin/w2kwm        window manager + desktop + taskbar + Start menu
    bin/w2kexplorer  Windows Explorer (Folders pane, four views, file ops)
    bin/w2knotepad   Notepad (word wrap, find, go to, single-level undo)
    bin/w2ktaskmgr   Task Manager (Applications / Processes / Performance)
    w2k-session      starts the whole desktop as an X session

## Build

From a bare system with no desktop at all (a fresh Debian, say), as root:

    W2K_REPO=https://github.com/JackTulli/Linux-explorer sh -c "$(curl -fsSL https://raw.githubusercontent.com/JackTulli/Linux-explorer/main/bootstrap.sh)"

That installs the X server, LightDM with the Windows 2000 logon screen (`w2klogon`, a LightDM greeter drawn by the shell's own toolkit), sound, guest tools, Firefox and everything below, and reboots into "Log On to Windows".

On a system that already has a desktop:

    ./install.sh              # packages, build, install, cursors, Chicago95, Qt
    ./install.sh --xinitrc    # ...and make it your startx session

(`./install.sh --help` lists the options.) By hand, Debian/Ubuntu: `apt install build-essential libx11-dev xfonts-75dpi xfonts-base`
(the bitmap Helvetica in `xfonts-75dpi` is what stands in for MS Sans Serif;
`xfonts-base` provides the Fixedsys-alike used by Notepad).

    make
    make install PREFIX=/usr/local      # optional

## Run

Try it nested first:

    Xephyr :2 -screen 1024x768 &
    DISPLAY=:2 ./w2k-session

Or as your session: put `exec /path/to/w2k-session` in `~/.xinitrc`.
Commands in `~/.w2k/autostart` (one per line) are launched at login.

Keys: **Ctrl+Esc** / **Win** Start menu · **Alt+Tab** switch windows ·
**Alt+F4** close · **Alt+Space** system menu · **Ctrl+Alt+Del** Task Manager ·
**Win+E** Explorer · **Win+R** Run · **Win+D** show desktop.

## Icons

The icons are the genuine Windows 2000 artwork (see `icons/README.md`),
baked into the binaries by `tools/genicons.py`. Any icon can be replaced
at run time by dropping `<slug>.ico` into `~/.w2k/icons`; `w2kwm --icons`
lists the slugs.

## Layout

    include/w2k.h    drawing primitives, system colours, fonts, icons, menus
    include/w2kui.h  windows, controls (edit, list, tree, scrollbar, ...), dialogs
    lib/             the toolkit ("libw2k")
    wm/              the window manager
    apps/            the applications
    tools/           icon baking (genicons.py) and development helpers
