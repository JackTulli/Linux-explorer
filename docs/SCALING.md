# Scaling

How Linux 2000 makes a desktop bigger on a dense panel, why it does it
the way it does, and how that compares with Windows and the other
desktops. Two mechanisms exist; both are set from Display Properties >
Settings.

## Screen scaling (the plain way)

xrandr builds a smaller virtual screen and stretches it over the panel
(`--scale`). Nothing in the desktop knows: every program draws at 96 dpi
into the small screen and the GPU enlarges the result. This is what a
compositor does for "legacy" programs, and what GNOME on X11 offers as
its only fractional option. At **200%** the nearest filter is used and
every pixel becomes an exact two-by-two block, which is pixel-perfect;
at 125, 150 and 175% no filter can be: nearest gives uneven pixels
(some doubled, some not) and bilinear gives a slight blur. The desktop
picks bilinear there, having tried the other.

## Sharp scaling (supersampled)

The third choice, "Sharp", is how GNOME's mutter and the Wayland
compositors produce a fractional scale: render at the next whole scale
and shrink. The desktop draws itself at 200%, where everything is an
exact pixel doubling and nothing is invented, and xrandr's transform
makes the panel show that virtual screen *smaller* by 200/wanted
(`--scale 1.3333` for 150%). Downscaling with bilinear averages real
pixels rather than inventing them, so edges and text stay crisp where
upscaling from 100% smears them. The cost is a virtual screen a third
larger than the panel for the GPU to render, and fonts hinted at 200%
then shrunk rather than hinted at 150%. It uses the desktop-scaling
machinery below, at its one exact setting. Each monitor keeps its own
scale: a second monitor wanting 100% gets `--scale 2` from the same
200% desktop.

## Desktop scaling (the experimental way)

The panel stays at its native size and the desktop renders larger. This
is how Windows draws a DPI-aware program, how macOS draws at 2x, and
what Wayland's `fractional-scale-v1` asks of a client.

### The model

Every program lays out in *logical* pixels -- the Windows 2000 metrics
the whole desktop was measured in: an 18-pixel caption, a 16 by 14
caption button, a 75-pixel icon cell, Tahoma 8 -- and the toolkit
multiplies on the way to the screen:

- `w2k_px(v)` maps a logical coordinate to the screen, `w2k_lp(v)` maps
  back. Windows, menus, tooltips and popups are created at the mapped
  size; pointer positions come back through `w2k_lp()` before a program
  sees them.
- A rectangle's span is mapped as the difference of its mapped ends
  (`w2k_cw`), never as a rounded width, so neighbouring rectangles stay
  neighbours at any scale.
- Fonts are opened at their pixel size times the scale; `w2k_font_height`
  and `w2k_text_width` report logical values, so layout code is unchanged.
- Icons are resampled from the 32-pixel art to the size on screen (the
  16-pixel icon at 200% *is* the 32-pixel art). The XP and Windows 7
  chrome is enlarged from its sheets by nearest neighbour into a copy
  whose columns are the exact inverse of `w2k_px()`, so a piece cut from
  the sheet lands on the pixels it names; a one-pixel column tiled across
  a caption stays one column.
- The mouse pointer is enlarged from the cursor art: blocks at 200%,
  bilinear at fractions.

### Lines

The hard part of a fractional scale is a line. A one-pixel line at 150%
is a pixel and a half; stretching the grid makes it one pixel here and
two there, which is exactly the uneven look screen scaling has. Windows
avoids it because a DPI-aware program lays out in *device* pixels with
scaled *metrics* (`GetSystemMetrics` at 120 dpi says the caption is 24
high) and draws its 3D edges one pixel thick whatever the DPI. Qt does
the same for its cosmetic pens; GTK 3 refused the problem and supports
whole-number scales only.

The desktop follows Windows:

- Every 3D edge, frame and button ring is `w2k_th(1)` thick: one pixel up
  to 199%, two from 200%.
- In a program the rings are anchored to where the control's *inside*
  begins (its rectangle inset by the number of rings), not to its outer
  corner. Two logical pixels can be three on the screen, and anchoring
  outward would leave a coloured gap between the edge and the white of an
  edit box; anchored inward, the spare pixel falls outside, on the
  parent's background, where it is invisible.
- A pushbutton fills its face over the whole rectangle first and draws
  its edge on top, so face and edge always meet.
- Hand-drawn pixel art -- arrows, check marks, the speaker -- keeps the
  stretched mapping, because a triangle built from one-pixel rows must
  stay solid.

### The window manager

The manager's chrome is laid out in screen pixels -- the frame has to fit
the client window it wraps, whose size is whatever the program asked for
-- in a *raw* mode of the primitives where coordinates pass through
unchanged and only thicknesses, fonts, icons and skins scale. Its metrics
(`FRAME_SIZE`, `CAPTION_H`, `CAPBTN_W`) are multiplied once, the way
`GetSystemMetrics` is. The caption glyphs are drawn from their art so
each art pixel covers its own share of the scaled button. The taskbar,
desktop icons, Start panel, balloons and the Alt+Tab box are laid out
logically over physical windows like a program.

### Other programs

Programs the desktop starts are told the scale the way their own
desktops would: `GDK_SCALE` and `GDK_DPI_SCALE` (GTK), `QT_SCALE_FACTOR`
(Qt), `XCURSOR_SIZE`, and `Xft.dpi` in the X resources. GTK 3 renders
whole scales only, so at 150% it draws at 1x with 1.5x fonts, as it does
on any other X11 desktop.

### What it does not do

- A change of scale takes a fresh logon: every program opened its fonts
  and sized its windows against the old one.
- Snipping Tool captures are in screen pixels.
- Bitmap wallpapers are not scaled; they are fitted to the monitor as
  before.
