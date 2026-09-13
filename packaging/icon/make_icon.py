#!/usr/bin/env python3
"""Generate the Audient Console production icon (A + vertical fader).

Renders every required size independently (with 4x supersampling for clean
anti-aliasing) so small sizes stay readable, then assembles a multi-size .ico
with 32bpp BMP frames (universally supported, no PNG-frame ambiguity).

Output:
  src/app/assets/audient_console.ico
Usage:
  python packaging/icon/make_icon.py
"""
import os
import struct
from PIL import Image, ImageDraw

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT_ICO = os.path.join(REPO, "src", "app", "assets", "audient_console.ico")
SIZES = [16, 20, 24, 32, 48, 64, 128, 256]

BG = (23, 26, 32, 255)       # dark charcoal
BORDER = (42, 47, 55, 255)
AMBER = (240, 165, 44, 255)  # brand amber
CAP = (255, 224, 160, 255)   # fader cap highlight
SS = 4  # supersample factor


def render(size: int) -> Image.Image:
    c = size * SS
    img = Image.new("RGBA", (c, c), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # Rounded-square background.
    m = 0.045 * c
    r = 0.22 * (c - 2 * m)
    d.rounded_rectangle([m, m, c - m, c - m], radius=r, fill=BG,
                        outline=BORDER, width=max(1, int(0.012 * c)))

    # Stylized bold "A".
    apex = (0.50 * c, 0.20 * c)
    left_foot = (0.27 * c, 0.82 * c)
    right_foot = (0.73 * c, 0.82 * c)
    leg_w = max(2, int(0.135 * c))
    d.line([left_foot, apex], fill=AMBER, width=leg_w)
    d.line([apex, right_foot], fill=AMBER, width=leg_w)
    bar_w = max(2, int(0.105 * c))
    d.line([(0.355 * c, 0.60 * c), (0.645 * c, 0.60 * c)], fill=AMBER, width=bar_w)

    # Vertical fader integrated in the A: a dark track slot + amber cap.
    slot_w = int(0.085 * c)
    d.rounded_rectangle([0.50 * c - slot_w / 2, 0.32 * c, 0.50 * c + slot_w / 2, 0.75 * c],
                        radius=slot_w / 2, fill=BG)
    cap_w = int(0.22 * c)
    cap_h = max(2, int(0.06 * c))
    d.rounded_rectangle([0.50 * c - cap_w / 2, 0.51 * c - cap_h / 2,
                         0.50 * c + cap_w / 2, 0.51 * c + cap_h / 2],
                        radius=cap_h / 2, fill=CAP)

    return img.resize((size, size), Image.LANCZOS)


def bmp_frame(img: Image.Image) -> bytes:
    w = h = img.width
    bih = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    px = img.tobytes()  # RGBA, top-down
    rows = []
    for y in range(h - 1, -1, -1):  # bottom-up
        row = bytearray(px[y * w * 4:(y + 1) * w * 4])
        for i in range(0, len(row), 4):  # RGBA -> BGRA
            row[i], row[i + 2] = row[i + 2], row[i]
        rows.append(bytes(row))
    xor = b"".join(rows)
    and_stride = ((w + 31) // 32) * 4
    and_mask = bytes(and_stride * h)  # opaque; alpha carries transparency
    return bih + xor + and_mask


def main() -> None:
    os.makedirs(os.path.dirname(OUT_ICO), exist_ok=True)
    frames = {s: bmp_frame(render(s)) for s in SIZES}

    parts = []
    offset = 6 + 16 * len(SIZES)
    for s in SIZES:
        data = frames[s]
        parts.append(struct.pack("<BBBBHHII", s if s < 256 else 0, s if s < 256 else 0,
                                 0, 0, 1, 32, len(data), offset))
        offset += len(data)
    header = struct.pack("<HHH", 0, 1, len(SIZES))
    with open(OUT_ICO, "wb") as f:
        f.write(header)
        for p in parts:
            f.write(p)
        for s in SIZES:
            f.write(frames[s])

    # Review previews.
    tmp = os.environ.get("TEMP", ".")
    for s in (256, 48, 32, 16):
        render(s).save(os.path.join(tmp, f"icon-preview-{s}.png"))
    print("wrote", OUT_ICO, os.path.getsize(OUT_ICO), "bytes; sizes", SIZES)


if __name__ == "__main__":
    main()
