#!/usr/bin/env python3
"""Generate the fuckgram app icon: SVG source, PNGs and .ico.

The icon is a red asterisk (the one from "f*ck") on a dark-to-red
diagonal gradient circle. Everything is drawn from the numbers below,
so tweak them and re-run:

    python tools/brand/make_icon.py            # write into the repo
    python tools/brand/make_icon.py --preview  # only tools/brand/out/

Needs Pillow and numpy (development only, not used by the build).
"""
import io
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageChops, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ART = os.path.join(ROOT, "Telegram", "Resources", "art")
OUT = os.path.join(ROOT, "tools", "brand", "out")

# Geometry in a 100x100 box.
CIRCLE_R = 48.0
BAR = dict(x=44.5, y=20.0, w=11.0, h=60.0)        # one arm of the asterisk
BAR_SMALL = dict(x=43.0, y=21.5, w=14.0, h=57.0)  # thicker for <= 32 px
ANGLES = (0, 60, -60)

# Gradients: (start point, end point, [(offset, hex)]).
BACKGROUND = ((14, 14), (86, 86), [(0.0, "#3a0006"), (0.5, "#9c000a"), (1.0, "#ff1f1f")])
GLYPH = ((30, 22), (72, 82), [(0.0, "#a00000"), (0.65, "#ff1010"), (1.0, "#ff2a2a")])

SUPER = 2048  # supersampled canvas


def hex_rgb(value):
    value = value.lstrip("#")
    return [int(value[i:i + 2], 16) for i in (0, 2, 4)]


def gradient(size, spec):
    (x1, y1), (x2, y2), stops = spec
    scale = size / 100.0
    ys, xs = np.mgrid[0:size, 0:size].astype(np.float32) / scale
    dx, dy = x2 - x1, y2 - y1
    t = ((xs - x1) * dx + (ys - y1) * dy) / (dx * dx + dy * dy)
    t = np.clip(t, 0.0, 1.0)
    offsets = np.array([s[0] for s in stops], dtype=np.float32)
    colors = np.array([hex_rgb(s[1]) for s in stops], dtype=np.float32)
    rgb = np.stack([np.interp(t, offsets, colors[:, c]) for c in range(3)], axis=-1)
    return Image.fromarray(rgb.astype(np.uint8), "RGB")


def asterisk_mask(size, bar):
    scale = size / 100.0
    base = Image.new("L", (size, size), 0)
    draw = ImageDraw.Draw(base)
    box = [bar["x"] * scale, bar["y"] * scale,
           (bar["x"] + bar["w"]) * scale, (bar["y"] + bar["h"]) * scale]
    draw.rounded_rectangle(box, radius=bar["w"] * scale / 2, fill=255)
    result = Image.new("L", (size, size), 0)
    for angle in ANGLES:
        rotated = base.rotate(angle, resample=Image.BICUBIC, center=(size / 2, size / 2))
        result = ImageChops.lighter(result, rotated)
    return result


def circle_mask(size, radius, center=50.0):
    scale = size / 100.0
    mask = Image.new("L", (size, size), 0)
    r = radius * scale
    c = center * scale
    ImageDraw.Draw(mask).ellipse([c - r, c - r, c + r, c + r], fill=255)
    return mask


def master(bar, circle_r=CIRCLE_R):
    size = SUPER
    bg = gradient(size, BACKGROUND).convert("RGBA")
    bg.putalpha(circle_mask(size, circle_r))
    glyph = gradient(size, GLYPH).convert("RGBA")
    glyph.putalpha(ImageChops.multiply(asterisk_mask(size, bar), circle_mask(size, circle_r)))
    return Image.alpha_composite(bg, glyph)


def render(pixels, margin=0.0):
    """Icon at `pixels` size; margin is the transparent border in % of size."""
    image = master(BAR_SMALL if pixels <= 32 else BAR)
    if margin:
        inner = round(SUPER * (1 - 2 * margin / 100.0))
        canvas = Image.new("RGBA", (SUPER, SUPER), (0, 0, 0, 0))
        offset = (SUPER - inner) // 2
        canvas.alpha_composite(image.resize((inner, inner), Image.LANCZOS), (offset, offset))
        image = canvas
    return image.resize((pixels, pixels), Image.LANCZOS)


def write_ico(path, sizes):
    """ICO with a separately rendered PNG frame for every size."""
    frames = []
    for s in sizes:
        buffer = io.BytesIO()
        render(s).save(buffer, "PNG", optimize=True)
        frames.append((s, buffer.getvalue()))
    header = struct.pack("<HHH", 0, 1, len(frames))
    offset = 6 + 16 * len(frames)
    entries, blobs = b"", b""
    for s, data in frames:
        dim = 0 if s >= 256 else s
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        blobs += data
        offset += len(data)
    with open(path, "wb") as f:
        f.write(header + entries + blobs)


def svg():
    def stops(spec):
        return "".join(
            '<stop offset="%g" stop-color="%s"/>' % (o, c) for o, c in spec[2])
    (bx1, by1), (bx2, by2), _ = BACKGROUND
    (gx1, gy1), (gx2, gy2), _ = GLYPH
    bars = "".join(
        '<rect x="%g" y="%g" width="%g" height="%g" rx="%g"%s/>' % (
            BAR["x"], BAR["y"], BAR["w"], BAR["h"], BAR["w"] / 2,
            ' transform="rotate(%d 50 50)"' % a if a else "")
        for a in ANGLES)
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="1200" viewBox="0 0 100 100">\n'
        '<defs>\n'
        '<linearGradient id="bg" x1="%g" y1="%g" x2="%g" y2="%g" gradientUnits="userSpaceOnUse">%s</linearGradient>\n'
        '<linearGradient id="glyph" x1="%g" y1="%g" x2="%g" y2="%g" gradientUnits="userSpaceOnUse">%s</linearGradient>\n'
        '</defs>\n'
        '<circle cx="50" cy="50" r="%g" fill="url(#bg)"/>\n'
        '<g fill="url(#glyph)">%s</g>\n'
        '</svg>\n') % (bx1, by1, bx2, by2, stops(BACKGROUND),
                       gx1, gy1, gx2, gy2, stops(GLYPH), CIRCLE_R, bars)


def main():
    preview = "--preview" in sys.argv
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "icon.svg"), "w", encoding="utf-8") as f:
        f.write(svg())
    render(1024).save(os.path.join(OUT, "icon1024.png"))
    sheet = Image.new("RGBA", (16 + 24 + 32 + 48 + 64 + 128 + 70, 140), (32, 32, 32, 255))
    x = 10
    for s in (16, 24, 32, 48, 64, 128):
        sheet.alpha_composite(render(s), (x, 140 - s - 6))
        x += s + 10
    sheet.save(os.path.join(OUT, "sizes_dark.png"))
    if preview:
        print("preview written to", OUT)
        return

    # Windows executable icon and the default in-app icon.
    sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
    write_ico(os.path.join(ART, "icon256.ico"), sizes)
    write_ico(os.path.join(ART, "ayu", "default", "app_icon.ico"), sizes)
    with open(os.path.join(ART, "ayu", "default", "app.svg"), "w", encoding="utf-8") as f:
        f.write(svg())
    # Window / tray / Linux icons.
    for s in (16, 32, 48, 64, 128, 256, 512):
        render(s).save(os.path.join(ART, "icon%d.png" % s), optimize=True)
        render(s * 2).save(os.path.join(ART, "icon%d@2x.png" % s), optimize=True)
    render(256, margin=3.0).save(os.path.join(ART, "logo_256.png"), optimize=True)
    render(256).save(os.path.join(ART, "logo_256_no_margin.png"), optimize=True)
    render(1024).save(os.path.join(ART, "icon_round512@2x.png"), optimize=True)
    print("icons written to", ART)


if __name__ == "__main__":
    main()
