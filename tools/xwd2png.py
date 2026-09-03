#!/usr/bin/env python3
"""Convert an X11 window dump (xwd) on stdin/argv to PNG. No X deps needed."""
import struct, sys
from PIL import Image

def convert(data, outpath):
    if len(data) < 100:
        raise SystemExit("xwd data too short")
    h = struct.unpack(">25I", data[:100])
    (hdrsize, ver, fmt, depth, width, height, xoff, byteorder, bmunit,
     bmbitorder, bmpad, bpp, bpl, vclass, rmask, gmask, bmask, bitsrgb,
     cmapent, ncolors) = h[:20]
    if ver != 7:
        raise SystemExit(f"unsupported xwd version {ver}")
    off = hdrsize + ncolors * 12
    pix = data[off:]
    need = bpl * height
    if len(pix) < need:
        raise SystemExit(f"short pixel data: {len(pix)} < {need}")

    if bpp in (24, 32):
        img = Image.new("RGB", (width, height))
        px = img.load()
        step = bpp // 8
        # figure channel byte offsets from the masks
        def shift(m):
            s = 0
            while m and not (m & 1):
                m >>= 1; s += 1
            return s
        rs, gs, bs = shift(rmask), shift(gmask), shift(bmask)
        for y in range(height):
            row = pix[y * bpl: y * bpl + width * step]
            for x in range(width):
                chunk = row[x * step:x * step + step]
                if byteorder == 0:      # LSBFirst
                    v = int.from_bytes(chunk, "little")
                else:
                    v = int.from_bytes(chunk, "big")
                px[x, y] = ((v & rmask) >> rs, (v & gmask) >> gs, (v & bmask) >> bs)
        img.save(outpath)
    else:
        raise SystemExit(f"unsupported bpp {bpp}")
    print(f"{outpath} {width}x{height} depth={depth} bpp={bpp}")

if __name__ == "__main__":
    src = open(sys.argv[1], "rb").read() if len(sys.argv) > 2 else sys.stdin.buffer.read()
    convert(src, sys.argv[-1])
