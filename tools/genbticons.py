#!/usr/bin/env python3
"""Draw the Bluetooth icons Windows 2000 never had, into icons/win98/ (the
directory genicons.py reads drawn pairs from):

    cp_bluetooth  -- the blue badge with the rune: the applet, the radio
    bt_phone      -- a mobile phone of the time
    bt_headset    -- headphones
    bt_gamepad    -- a game controller

The 32-pixel art is drawn with antialiased shapes; the 16-pixel badge is
pixel art, since a rune that small blurs, and the rest are the 32-pixel
art box-filtered, as the Control Panel icons are. Pure Python: no PIL.
"""
import math, os, struct, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "icons", "win98")


def png(path, w, h, rgba):
    raw = b"".join(b"\0" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" +
                           chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


class Canvas:
    """RGBA, painted with shapes given as signed distance functions; each
    pixel is sampled 4x4 for the edges."""
    def __init__(self, n):
        self.n = n
        self.px = [0.0] * (n * n * 4)     # premultiplied

    def shape(self, sdf, colour, ss=4):
        r, g, b = (c / 255 for c in colour[:3])
        a0 = (colour[3] if len(colour) > 3 else 255) / 255
        n = self.n
        for y in range(n):
            for x in range(n):
                cov = 0
                for sy in range(ss):
                    for sx in range(ss):
                        if sdf(x + (sx + .5) / ss, y + (sy + .5) / ss) <= 0:
                            cov += 1
                if not cov:
                    continue
                a = a0 * cov / (ss * ss)
                i = (y * n + x) * 4
                p = self.px
                p[i] = r * a + p[i] * (1 - a)
                p[i + 1] = g * a + p[i + 1] * (1 - a)
                p[i + 2] = b * a + p[i + 2] * (1 - a)
                p[i + 3] = a + p[i + 3] * (1 - a)

    def gradient(self, sdf, top, bottom, ss=4):
        """The shape filled top to bottom (and a little left to right)."""
        n = self.n
        for y in range(n):
            for x in range(n):
                t = min(1, max(0, (y + x * .35) / (n * 1.2)))
                c = tuple(int(top[k] + (bottom[k] - top[k]) * t) for k in range(3))
                self.shape_px(sdf, x, y, c, ss)

    def shape_px(self, sdf, x, y, colour, ss=4):
        cov = sum(1 for sy in range(ss) for sx in range(ss)
                  if sdf(x + (sx + .5) / ss, y + (sy + .5) / ss) <= 0)
        if not cov:
            return
        a = cov / (ss * ss)
        i = (y * self.n + x) * 4
        p = self.px
        for k in range(3):
            p[i + k] = colour[k] / 255 * a + p[i + k] * (1 - a)
        p[i + 3] = a + p[i + 3] * (1 - a)

    def rgba(self):
        out = []
        for i in range(0, len(self.px), 4):
            a = self.px[i + 3]
            if a <= 0.004:
                out += [0, 0, 0, 0]
                continue
            out += [min(255, round(self.px[i + k] / a * 255)) for k in range(3)]
            out.append(min(255, round(a * 255)))
        return out


# ---- signed distance functions --------------------------------------
def rrect(x0, y0, x1, y1, r):
    cx, cy, hw, hh = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2
    def f(x, y):
        qx, qy = abs(x - cx) - hw + r, abs(y - cy) - hh + r
        return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r
    return f


def seg(ax, ay, bx, by, w):
    def f(x, y):
        dx, dy = bx - ax, by - ay
        t = max(0, min(1, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)))
        return math.hypot(x - ax - t * dx, y - ay - t * dy) - w / 2
    return f


def poly(points, w):
    segs = [seg(*points[i], *points[i + 1], w) for i in range(len(points) - 1)]
    return lambda x, y: min(s(x, y) for s in segs)


def circle(cx, cy, r):
    return lambda x, y: math.hypot(x - cx, y - cy) - r


def ring(cx, cy, r, w, upper_only=True):
    def f(x, y):
        d = abs(math.hypot(x - cx, y - cy) - r) - w / 2
        return d if not upper_only or y <= cy else max(d, y - cy)
    return f


def union(*fs):
    return lambda x, y: min(f(x, y) for f in fs)


def grow(f, d):
    return lambda x, y: f(x, y) - d


def downsample(big, n):
    """Box-filter 32 -> 16, premultiplied."""
    out = []
    for y in range(n):
        for x in range(n):
            acc = [0.0] * 4
            for sy in range(2):
                for sx in range(2):
                    i = ((y * 2 + sy) * n * 2 + x * 2 + sx) * 4
                    a = big[i + 3] / 255
                    for k in range(3):
                        acc[k] += big[i + k] * a
                    acc[3] += a
            a = acc[3] / 4
            if a < 0.02:
                out += [0, 0, 0, 0]
                continue
            out += [min(255, round(acc[k] / acc[3])) for k in range(3)]
            out.append(min(255, round(a * 255)))
    return out


NAVY = (0, 0, 96)
BLACK = (0, 0, 0)
WHITE = (255, 255, 255)


# ---- the badge -------------------------------------------------------
def bluetooth32():
    c = Canvas(32)
    body = rrect(6.5, 1.5, 25.5, 30.5, 9.5)
    c.shape(grow(body, 1), NAVY)
    c.gradient(body, (80, 150, 255), (0, 40, 170))
    # The rune: a staff with two arrowheads, drawn over a darker shadow.
    rune = [(10.5, 10.5), (21, 21), (16, 26.2), (16, 5.8), (21, 11), (10.5, 21.5)]
    c.shape(lambda x, y: poly(rune, 2.4)(x - .8, y - .8), (0, 20, 90, 150))
    c.shape(poly(rune, 2.4), WHITE)
    return c.rgba()


BADGE16 = [
    "................",
    ".....KKKKK......",
    "...KKLLLLBKK....",
    "..KLLLBWBBBDK...",
    "..KLLBBWWBBDK...",
    "..KLBWBWBWBDK...",
    "..KLBBWWWBBDK...",
    "..KLBBBWBBBDK...",
    "..KLBBWWWBBDK...",
    "..KLBWBWBWBDK...",
    "..KLBBBWWBBDK...",
    "..KBBBBWBBBDK...",
    "..KBBBBBBBDDK...",
    "...KKBBBBDKK....",
    ".....KKKKK......",
    "................",
]
PAL16 = {"K": NAVY, "L": (80, 150, 255), "B": (0, 80, 210), "D": (0, 40, 150), "W": WHITE}


def pixels(art, pal):
    out = []
    for row in art:
        for ch in row:
            out += list(pal[ch]) + [255] if ch in pal else [0, 0, 0, 0]
    return out


# ---- a phone ---------------------------------------------------------
def phone32():
    c = Canvas(32)
    c.shape(rrect(19.5, 1, 22.5, 8, 1.2), BLACK)                 # the aerial
    c.shape(rrect(20.3, 1.8, 21.7, 7, .6), (96, 96, 96))
    body = rrect(9.5, 4.5, 23.5, 30.5, 4)
    c.shape(grow(body, 1), BLACK)
    c.gradient(body, (150, 150, 160), (50, 50, 60))
    c.shape(rrect(11.5, 7, 21.5, 16, 1.5), (20, 20, 20))         # the screen
    c.shape(rrect(12.5, 8, 20.5, 15, 1), (160, 196, 150))
    c.shape(rrect(13.5, 9.5, 17, 10.5, .3), (60, 90, 60))
    c.shape(rrect(13.5, 12, 19.5, 13, .3), (60, 90, 60))
    for row in range(4):                                         # the keys
        for col in range(3):
            x, y = 12.5 + col * 3.3, 18.2 + row * 2.9
            c.shape(rrect(x, y, x + 2.4, y + 1.9, .7), (20, 20, 20))
            c.shape(rrect(x + .2, y + .1, x + 2.1, y + 1.5, .6), (225, 225, 230))
    return c.rgba()


# ---- headphones ------------------------------------------------------
def headset32():
    c = Canvas(32)
    band = ring(16, 17, 11, 3.2)
    c.shape(grow(band, 1), BLACK)
    c.shape(band, (110, 110, 120))
    c.shape(ring(16, 17, 11.6, 1), (190, 190, 200))
    for x0 in (2.5, 21.5):
        cup = rrect(x0, 15.5, x0 + 8, 29.5, 3)
        c.shape(grow(cup, 1), BLACK)
        c.gradient(cup, (120, 120, 130), (30, 30, 40))
        pad = rrect(x0 + (5.5 if x0 < 16 else 0), 17.5, x0 + (8 if x0 < 16 else 2.5), 27.5, 1)
        c.shape(pad, (0, 70, 190))
    return c.rgba()


# ---- a game controller -----------------------------------------------
def gamepad32():
    c = Canvas(32)
    body = union(rrect(3, 9.5, 29, 20.5, 5), circle(8.5, 21, 5.5), circle(23.5, 21, 5.5))
    c.shape(grow(body, 1), BLACK)
    c.gradient(body, (215, 215, 220), (110, 110, 120))
    pad = union(rrect(5.5, 13.5, 12.5, 16.5, .5), rrect(7.5, 11.5, 10.5, 18.5, .5))
    c.shape(grow(pad, .6), (40, 40, 40))
    c.shape(pad, (70, 70, 75))
    for (x, y, col) in ((23.5, 11.8, (0, 160, 0)), (26.5, 14.8, (210, 30, 30)),
                        (20.5, 14.8, (0, 80, 220)), (23.5, 17.8, (230, 180, 0))):
        c.shape(circle(x, y, 1.7), BLACK)
        c.shape(circle(x - .2, y - .2, 1.2), col)
    c.shape(rrect(14, 12.5, 18, 13.8, .6), (60, 60, 60))
    return c.rgba()


def main():
    os.makedirs(OUT, exist_ok=True)
    png(os.path.join(OUT, "cp_bluetooth_32.png"), 32, 32, bluetooth32())
    png(os.path.join(OUT, "cp_bluetooth_16.png"), 16, 16, pixels(BADGE16, PAL16))
    for name, fn in (("bt_phone", phone32), ("bt_headset", headset32), ("bt_gamepad", gamepad32)):
        big = fn()
        png(os.path.join(OUT, name + "_32.png"), 32, 32, big)
        png(os.path.join(OUT, name + "_16.png"), 16, 16, downsample(big, 16))
    print("wrote", OUT)


main()
