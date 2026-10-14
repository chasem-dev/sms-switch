#!/usr/bin/env python3
"""Draw switch/prompts/switch.png: the Switch's buttons for the game's button
glyphs (sms-port gx_prompts.cpp: eight 64x64 cells, A B X Y Z L R C-stick),
as sms-switch maps them: A B X Y, Z = R, L = ZL, R = ZR, C-stick = right stick.

    python3 tools/make_prompts.py [FONT.ttf] [OUT.png]
"""
import sys
from PIL import Image, ImageDraw, ImageFont

FONT = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
OUT = sys.argv[2] if len(sys.argv) > 2 else "switch/prompts/switch.png"
CELL, SS = 64, 4  # drawn at 4x, then reduced: smooth edges
S = CELL * SS
FILL, INK = (245, 245, 245, 255), (40, 40, 40, 255)


def text(d, label, size, box):
    font = ImageFont.truetype(FONT, size * SS)
    l, t, r, b = d.textbbox((0, 0), label, font=font)
    x0, y0, x1, y1 = box
    d.text(((x0 + x1 - (r - l)) / 2 - l, (y0 + y1 - (b - t)) / 2 - t), label, font=font, fill=INK)


def face(label):  # a round face button
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    m = 3 * SS
    d.ellipse((m, m, S - m, S - m), fill=FILL)
    text(d, label, 36, (0, 0, S, S))
    return im


def shoulder(label):  # a shoulder button: a rounded bar
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    box = (2 * SS, 13 * SS, S - 2 * SS, S - 13 * SS)
    d.rounded_rectangle(box, radius=12 * SS, fill=FILL)
    text(d, label, 30 if len(label) == 1 else 26, box)
    return im


def stick(label):  # a stick: a ring around its cap, with its letter
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    m = 2 * SS
    d.ellipse((m, m, S - m, S - m), fill=FILL)
    d.ellipse((10 * SS, 10 * SS, S - 10 * SS, S - 10 * SS), outline=INK, width=4 * SS)
    text(d, label, 26, (0, 0, S, S))
    return im


cells = [face("A"), face("B"), face("X"), face("Y"), shoulder("R"), shoulder("ZL"), shoulder("ZR"), stick("R")]
atlas = Image.new("RGBA", (CELL * len(cells), CELL), (0, 0, 0, 0))
for i, c in enumerate(cells):
    atlas.paste(c.resize((CELL, CELL), Image.LANCZOS), (i * CELL, 0))
atlas.save(OUT, optimize=True)
print(f"wrote {OUT} ({atlas.width}x{atlas.height})")
