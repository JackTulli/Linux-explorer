# Windows 2000 for X11

A window manager and desktop environment that recreates the Windows 2000
shell on X11: the classic 3D look, the gradient title bars, the taskbar
and Start menu, the logon screen, and the applications that came with it,
written from scratch in C against Xlib. Optional Windows XP and Windows 7
Basic looks are cropped pixel for pixel from the real thing.

![The desktop](docs/desktop.png)

## Get it running

**A fresh machine or VM with no desktop** (Debian, Ubuntu, Fedora, Arch,
openSUSE, Alpine or Void), as root:

    curl -sL jacktulli.github.io/w2k | sh

That is the whole install. It fetches this repository into
`/usr/local/src`, installs the X server, sound, VM guest tools and Firefox,
builds and installs the shell, and sets up the first ordinary user (or
`W2K_USER=name`). Reboot and the machine comes up in **Log On to Windows**,
served by `w2kdm`, the shell's own display manager: no LightDM, no GDM. Log
on and you are on the desktop above.

If `curl` is missing on a bare Debian: `apt install curl` first. If the
first run is interrupted, run the same command again; it carries on.

**Updating** is the same command: it pulls the latest source, rebuilds,
installs over the old copy and restarts a running desktop in place, with
every window kept. `w2kwm --version` says what is installed.

**A machine that already has a desktop:**

    git clone https://github.com/JackTulli/Linux-explorer
    cd Linux-explorer
    ./install.sh --xinitrc    # packages, build, install, cursors, Chicago95,
                              # Qt, and w2k-session as your startx session

Then `startx /usr/local/bin/w2k-session`, or pick "Windows 2000" in your
display manager. `./install.sh --help` lists the options: `--tahoma`
fetches the shell's typeface, `--user-only` touches only your own
configuration (run it again as another user), `--dry-run` shows what would
happen. `--full` adds the X server and w2kdm and *disables your current
display manager for the next boot*, so the machine then logs on through
Log On to Windows; leave it out to keep GDM, LightDM or SDDM and just pick
the session there.

**By hand:** the build needs a C compiler, make, and the development
packages for X11, Xext, Xrandr, Xcursor, Xft, fontconfig, freetype, zlib
and libjpeg (Debian: `build-essential libx11-dev libxext-dev libxrandr-dev
libxcursor-dev libxft-dev libfontconfig1-dev libfreetype-dev zlib1g-dev
libjpeg-dev`; add `libpam0g-dev` for the display manager). Then

    make
    make install PREFIX=/usr/local

Try it nested first, without logging out of anything:

    Xephyr :2 -screen 1024x768 &
    DISPLAY=:2 ./w2k-session

## What is in the box

    w2kdm          the display manager: Log On to Windows, PAM, the session
    w2k-session    starts the desktop as an X session
    w2kwm          window manager, desktop, taskbar, Start menu, dialogs
    w2kexplorer    Windows Explorer: Folders pane, four views, cut/copy/paste,
                   undo, drag and drop (XDND, with any other program), Recycle
                   Bin, Add to Archive / Extract with progress, Send To,
                   Open With, Properties, Search, drives in My Computer
    w2kcontrol     Control Panel: Display, Date/Time, Default Programs, Mouse,
                   Keyboard, Sounds, Fonts, Folder Options, Performance
                   Options, Taskbar and Start Menu
    w2kdisplay     Display Properties: wallpaper (centre, tile, stretch, fit,
                   fill, span), appearance schemes, themes, monitors
    w2kdevmgmt     Device Manager: the machine's hardware from sysfs, drivers,
                   enable/disable, DKMS driver install (contributed)
    w2knotepad, w2kcalc, w2kcharmap, w2kimage (Imaging), w2ktaskmgr,
    w2ksnip        Snipping Tool

Three looks, from Display Properties > Appearance: the Windows 2000
classic scheme (and its colour variants), Windows XP (Luna, with the
two-column Start menu), and Windows 7 Basic (with its Start menu, orb and
taskbar). The XP and 7 chrome is cropped from screenshots and checked by
diffing against them.

![Windows XP look: Luna windows, the two-column Start menu, Explorer, Task Manager and Display Properties](docs/windows-xp.png)

![Windows 7 Basic look: the orb, the Windows 7 Start menu and taskbar, with the Windows 7 icon set](docs/windows-7.png)

Icons come in five sets, from Display Properties > Appearance > Icons:
Windows 2000 (the built-in artwork), Windows 98, Windows XP, Windows 7 and
ReactOS. Every window, the desktop, Explorer and the Start menu follow the
choice at once.

Notifications from every program appear as the yellow balloon over the
notification area: the desktop provides the `org.freedesktop.Notifications`
service, so Firefox, mail, `notify-send` and anything using libnotify all
show the same balloon, one after another, with a close box and a click to
act on it. `w2knotify "Title" "Text"` sends one from a script without
any of that.

Other programs match: GTK 2/3/4 get the Chicago95 theme and icons, Qt gets
the Windows style with a Windows 2000 palette (qt5ct/qt6ct), every program
gets the Windows cursors through an Xcursor theme, and Explorer is the
folder handler for `xdg-open` and "show in folder".

Keys: **Ctrl+Esc** / **Win** Start menu · **Alt+Tab** switch windows ·
**Alt+F4** close · **Alt+Space** system menu · **Ctrl+Alt+Del** Task Manager ·
**Win+E** Explorer · **Win+R** Run · **Win+D** show desktop · typing at the
Start menu searches.

## Configuration

Everything the applets set lives in `~/.w2k/scheme` (colours, theme,
wallpaper, effects, taskbar, folder options, input settings, the monitor
arrangement) and is applied live to every running program. Commands in
`~/.w2k/autostart` (one per line) are launched at logon. Pinned programs,
favorites and the Recycle Bin are under `~/.w2k` too.

The icons are the genuine Windows 2000 artwork (see `icons/README.md`),
baked into the binaries by `tools/genicons.py`; any of them can be replaced
by dropping `<slug>.ico` into `~/.w2k/icons` (`w2kwm --icons` lists the
slugs). The cursor set in `cursors/` is installed to `~/.w2k/cursors` and
turned into the `Windows2000` Xcursor theme by `tools/gencursortheme.py`.

## Layout

    include/w2k.h    drawing primitives, system colours, fonts, icons, menus
    include/w2kui.h  windows, controls (edit, list, tree, scrollbar, ...), dialogs
    lib/             the toolkit ("libw2k"): drawing, controls, XDND, file ops,
                     images (BMP/PNG/JPEG), skins, themes
    wm/              the window manager, taskbar, desktop, Start menus, dialogs
    apps/            the applications and the display manager
    skins/           the XP and Windows 7 chrome, cropped from screenshots
    config/          GTK/Qt settings, the w2kdm service and PAM stacks
    tools/           icon and cursor-theme baking, development helpers
    install.sh       the installer; bootstrap.sh the one-command form
