# Draws src/capview.ico: a monitor with a blue screen and a yellow lightning bolt.
# Each size is drawn on its own grid, so the small sizes keep sharp edges, at 8x and box-filtered down.
# Sizes below 256 are stored as 32-bit BMPs and 256 as a PNG, which every Windows version reads.
# Run: python tools/make-icon.py  (needs Pillow and numpy)
import io
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw

SS = 8
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

BEZEL_TOP, BEZEL_BOTTOM = (0x64, 0x6F, 0x8C), (0x3A, 0x42, 0x58)
GLASS_A, GLASS_B = (0x2E, 0x8B, 0xFF), (0x7A, 0x3C, 0xF2)
BOLT_TOP, BOLT_BOTTOM = (0xFF, 0xE0, 0x4A), (0xFF, 0xA8, 0x1C)
STAND = (0x7A, 0x84, 0x9E)

# Bolt points around (0, 0), for a bolt 96 units tall.
BOLT = [(-10, -48), (22, -48), (6, -8), (30, -8), (-16, 48), (-4, 10), (-30, 10)]

# Per size, in pixels: body, glass and base as (x0, y0, x1, y1, radius), neck as (x0, y0, x1, y1),
# and the bolt as (center x, center y, height). Sizes not listed scale the 256 layout.
LAYOUT_256 = dict(body=(12, 30, 244, 172, 22), glass=(24, 42, 232, 160, 12), neck=(112, 172, 144, 200),
                  base=(72, 198, 184, 216, 9), bolt=(128, 101, 96))
LAYOUTS = {
    16: dict(body=(0, 1, 16, 12, 2), glass=(1, 2, 15, 11, 1), neck=(7, 12, 9, 13),
             base=(4, 13, 12, 15, 1), bolt=(8, 6.5, 9)),
    20: dict(body=(0, 2, 20, 15, 2.5), glass=(1, 3, 19, 14, 1), neck=(9, 15, 11, 17),
             base=(5, 17, 15, 19, 1), bolt=(10, 8.5, 10)),
    24: dict(body=(1, 2, 23, 17, 2.5), glass=(2, 3, 22, 16, 1), neck=(10, 17, 14, 20),
             base=(6, 20, 18, 22, 1), bolt=(12, 9.5, 11)),
    32: dict(body=(1, 3, 31, 22, 3), glass=(3, 5, 29, 20, 1.5), neck=(14, 22, 18, 26),
             base=(9, 26, 23, 28, 1), bolt=(16, 12.5, 13)),
}


def layout(size):
    if size in LAYOUTS:
        return LAYOUTS[size]
    k = size / 256
    out = {}
    for name, v in LAYOUT_256.items():
        out[name] = tuple(round(c * k * 2) / 2 for c in v)
    return out


def gradient(n, a, b, diagonal):
    y, x = np.mgrid[0:n, 0:n].astype(np.float32) / max(1, n - 1)
    t = (x + y) / 2 if diagonal else y
    a, b = np.array(a, np.float32), np.array(b, np.float32)
    return a + (b - a) * t[..., None]


def layer(canvas, mask, rgb):
    m = np.asarray(mask, np.float32)[..., None] / 255
    canvas[..., :3] = canvas[..., :3] * (1 - m) + rgb * m
    canvas[..., 3:] = canvas[..., 3:] * (1 - m) + m


def draw(size):
    n = size * SS
    L = layout(size)
    canvas = np.zeros((n, n, 4), np.float32)

    def rrect(r):
        x0, y0, x1, y1, rad = (c * SS for c in r)
        m = Image.new("L", (n, n), 0)
        ImageDraw.Draw(m).rounded_rectangle((x0, y0, x1 - 1, y1 - 1), radius=rad, fill=255)
        return m

    def rect(r):
        x0, y0, x1, y1 = (c * SS for c in r)
        m = Image.new("L", (n, n), 0)
        ImageDraw.Draw(m).rectangle((x0, y0, x1 - 1, y1 - 1), fill=255)
        return m

    stand = np.array(STAND, np.float32)
    layer(canvas, rect(L["neck"]), stand)
    layer(canvas, rrect(L["base"]), stand)
    layer(canvas, rrect(L["body"]), gradient(n, BEZEL_TOP, BEZEL_BOTTOM, False))
    layer(canvas, rrect(L["glass"]), gradient(n, GLASS_A, GLASS_B, True))

    cx, cy, h = L["bolt"]
    k = h / 96 * SS
    pts = [(cx * SS + x * k, cy * SS + y * k) for x, y in BOLT]
    m = Image.new("L", (n, n), 0)
    ImageDraw.Draw(m).polygon(pts, fill=255)
    layer(canvas, m, gradient(n, BOLT_TOP, BOLT_BOTTOM, False))

    # Box-filter down with premultiplied alpha, so edges don't pick up a dark fringe.
    pre = canvas.copy()
    pre[..., :3] *= pre[..., 3:]
    small = pre.reshape(size, SS, size, SS, 4).mean(axis=(1, 3))
    a = small[..., 3:]
    small[..., :3] = np.where(a > 0, small[..., :3] / np.maximum(a, 1e-6), 0)
    small[..., 3:] *= 255
    return Image.fromarray(np.clip(small + 0.5, 0, 255).astype(np.uint8), "RGBA")


def bmp_entry(img):
    w, h = img.size
    bgra = np.asarray(img)[::-1, :, [2, 1, 0, 3]].tobytes()
    mask_row = ((w + 31) // 32) * 4
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, len(bgra), 0, 0, 0, 0)
    return header + bgra + bytes(mask_row * h)


def write_ico(path, images):
    blobs = []
    for img in images:
        if img.size[0] >= 256:
            buf = io.BytesIO()
            img.save(buf, "PNG", optimize=True)
            blobs.append(buf.getvalue())
        else:
            blobs.append(bmp_entry(img))
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for img, blob in zip(images, blobs):
        w = img.size[0]
        out += struct.pack("<BBBBHHII", w % 256, w % 256, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    with open(path, "wb") as f:
        f.write(out + b"".join(blobs))


if __name__ == "__main__":
    sizes = [16, 20, 24, 32, 40, 48, 64, 256]
    images = [draw(s) for s in sizes]
    write_ico(os.path.join(ROOT, "src", "capview.ico"), images)
    if len(sys.argv) > 1:
        # A preview sheet: every size at 1x and 4x on a light and a dark strip.
        sheet = Image.new("RGBA", (1400, 720), (0, 0, 0, 0))
        for row, bg in enumerate([(243, 243, 243, 255), (32, 32, 32, 255)]):
            Image.Image.paste(sheet, Image.new("RGBA", (1400, 360), bg), (0, row * 360))
            x = 16
            for img in images[:-1]:
                s = img.size[0]
                sheet.alpha_composite(img, (x, row * 360 + 16))
                big = img.resize((s * 4, s * 4), Image.Resampling.NEAREST)
                sheet.alpha_composite(big, (x, row * 360 + 90))
                x += s * 4 + 16
            sheet.alpha_composite(images[-1], (1400 - 272, row * 360 + 52))
        sheet.save(sys.argv[1])
