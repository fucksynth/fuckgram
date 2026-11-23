#!/usr/bin/env python3
"""Generate the fuckgram app icon: SVG source, PNGs and .ico.

The icon is a light red asterisk (the one from "f*ck") on a dark-to-red
diagonal gradient circle; VARIANTS are the other colors offered in
Settings > fuckgram > Appearance > App icon. Everything is drawn from the
numbers below, so tweak them and re-run:

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
def bg(*colors):
    step = 1.0 / (len(colors) - 1)
    return ((14, 14), (86, 86), [(i * step, c) for i, c in enumerate(colors)])


def glyph(top, bottom):
    return ((30, 22), (72, 82), [(0.0, top), (1.0, bottom)])


RED = bg("#3a0006", "#9c000a", "#ff1f1f")
BACKGROUND = RED
# Light red: readable from afar, still red.
GLYPH = glyph("#ff8a8a", "#ff5c5c")

# Folder in art/ayu -> (background, glyph). "default" is the main icon.
VARIANTS = {
    "default": (RED, GLYPH),
    "fg_white": (RED, glyph("#ffffff", "#ffe9e9")),
    "fg_ink": (bg("#ff4d4d", "#e00012", "#9c000a"), glyph("#2a0004", "#120002")),
    "fg_dark": (bg("#3a3a3a", "#1c1c1c", "#0a0a0a"), glyph("#ff6b6b", "#ff2a2a")),
    "fg_light": (bg("#ffffff", "#f1f1f1", "#d9d9d9"), glyph("#ff3b3b", "#d0000c")),
    "fg_violet": (bg("#21003f", "#6a1fd6", "#b06bff"), glyph("#ffffff", "#f1e3ff")),
    "fg_blue": (bg("#001634", "#0a56e0", "#43a2ff"), glyph("#ffffff", "#e3f0ff")),
    "fg_cyan": (bg("#002a33", "#008ea3", "#33e0f0"), glyph("#ffffff", "#e0fbff")),
    "fg_green": (bg("#002411", "#0a8f37", "#33e070"), glyph("#ffffff", "#e3ffe9")),
    "fg_gold": (bg("#3a2200", "#c07f00", "#ffd04a"), glyph("#ffffff", "#fff3d6")),
    "fg_pink": (bg("#40002a", "#d6267f", "#ff7ac0"), glyph("#ffffff", "#ffe6f3")),
    "fg_mono": (bg("#1e1e1e", "#555555", "#9a9a9a"), glyph("#ffffff", "#ececec")),
}

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


_MASTERS = {}


def master(bar, colors, circle_r=CIRCLE_R):
    key = (tuple(bar.items()), repr(colors), circle_r)
    if key in _MASTERS:
        return _MASTERS[key]
    background, glyph_spec = colors
    size = SUPER
    back = gradient(size, background).convert("RGBA")
    back.putalpha(circle_mask(size, circle_r))
    front = gradient(size, glyph_spec).convert("RGBA")
    front.putalpha(ImageChops.multiply(asterisk_mask(size, bar), circle_mask(size, circle_r)))
    result = _MASTERS[key] = Image.alpha_composite(back, front)
    return result


def render(pixels, margin=0.0, colors=None):
    """Icon at `pixels` size; margin is the transparent border in % of size."""
    colors = colors or (BACKGROUND, GLYPH)
    image = master(BAR_SMALL if pixels <= 32 else BAR, colors)
    if margin:
        inner = round(SUPER * (1 - 2 * margin / 100.0))
        canvas = Image.new("RGBA", (SUPER, SUPER), (0, 0, 0, 0))
        offset = (SUPER - inner) // 2
        canvas.alpha_composite(image.resize((inner, inner), Image.LANCZOS), (offset, offset))
        image = canvas
    return image.resize((pixels, pixels), Image.LANCZOS)


def write_ico(path, sizes, colors=None):
    """ICO with a separately rendered PNG frame for every size."""
    frames = []
    for s in sizes:
        buffer = io.BytesIO()
        render(s, colors=colors).save(buffer, "PNG", optimize=True)
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


def svg(colors=None):
    background, glyph_spec = colors or (BACKGROUND, GLYPH)

    def stops(spec):
        return "".join(
            '<stop offset="%g" stop-color="%s"/>' % (o, c) for o, c in spec[2])
    (bx1, by1), (bx2, by2), _ = background
    (gx1, gy1), (gx2, gy2), _ = glyph_spec
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
    # All the variants, as the icon picker shows them.
    names = list(VARIANTS)
    columns = 4
    cell = 140
    rows = (len(names) + columns - 1) // columns
    picker = Image.new("RGBA", (columns * cell, rows * cell), (24, 30, 38, 255))
    for i, name in enumerate(names):
        x, y = (i % columns) * cell, (i // columns) * cell
        picker.alpha_composite(render(112, colors=VARIANTS[name]), (x + 14, y + 14))
    picker.save(os.path.join(OUT, "variants.png"))
    if preview:
        print("preview written to", OUT)
        return

    # Windows executable icon and the default in-app icon.
    sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
    write_ico(os.path.join(ART, "icon256.ico"), sizes)
    for name, colors in VARIANTS.items():
        folder = os.path.join(ART, "ayu", name)
        os.makedirs(folder, exist_ok=True)
        write_ico(os.path.join(folder, "app_icon.ico"), sizes, colors)
        with open(os.path.join(folder, "app.svg"), "w", encoding="utf-8") as f:
            f.write(svg(colors))
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
