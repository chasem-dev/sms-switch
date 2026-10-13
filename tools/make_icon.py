#!/usr/bin/env python3
"""Make the Switch icon (256x256 JPEG) from the game's memory-card icon.
usage: tools/make_icon.py --disc GAME.iso --out icon.jpg

The 32x32 Mario head the game writes with its saves (card/mario_icon.bti in
/data/common.szs, read by sms-port's tools/extract_icon.py), scaled 6x
without smoothing and centred on a sky-blue gradient (JPEG has no
transparency). The art comes from the user's disc at build time; none of it
is kept in this repository, and builds without a disc keep libnx's default.
"""
import argparse
import os
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "sms-port", "tools"))
import extract_icon as ei  # noqa: E402
from PIL import Image  # noqa: E402

SIZE, SCALE = 256, 6
TOP, BOTTOM = (40, 150, 230), (150, 215, 245)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--disc", required=True, help="GMSE01 disc image (.iso/.gcm/.ciso)")
    ap.add_argument("--out", required=True, help="JPEG to write")
    a = ap.parse_args()
    arc = ei.yaz0(ei.disc_file(ei.Disc(a.disc), ei.ICON_PATH))
    frame = ei.decode_c8(ei.rarc_file(arc, ei.ARC_ENTRY))[: ei.FRAME]
    sprite = Image.new("RGBA", (ei.FRAME, ei.FRAME))
    sprite.putdata([tuple(p) for row in frame for p in row])
    sprite = sprite.resize((ei.FRAME * SCALE, ei.FRAME * SCALE), Image.NEAREST)
    icon = Image.new("RGB", (SIZE, SIZE))
    for y in range(SIZE):
        t = y / (SIZE - 1)
        icon.paste(tuple(round(TOP[c] + (BOTTOM[c] - TOP[c]) * t) for c in range(3)), (0, y, SIZE, y + 1))
    at = (SIZE - sprite.width) // 2
    icon.paste(sprite, (at, at), sprite)
    icon.save(a.out, "JPEG", quality=95)
    print("make_icon: wrote %s" % a.out)


if __name__ == "__main__":
    main()
