#!/usr/bin/env python3
"""Make the Switch icon (256x256 JPEG, for the homebrew menu and the HOME menu).
usage: tools/make_icon.py --out icon.jpg [--steamgriddb] [--disc GAME.iso]

--steamgriddb: Super Mario Sunshine's box art from SteamGridDB
(https://www.steamgriddb.com/game/34899, the library capsule SMS Launcher's
Add to Steam uses), cropped square around Mario and the logo.
--disc: the 32x32 Mario head the game writes with its saves (card/mario_icon.bti
in /data/common.szs, read by sms-port's tools/extract_icon.py), scaled 6x
without smoothing on a sky-blue gradient.
With both, SteamGridDB is tried first and the disc is the fallback. The art is
fetched or read at build time; none of it is kept in this repository.
"""
import argparse
import io
import os
import sys
import urllib.request

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "sms-port", "tools"))
import extract_icon as ei  # noqa: E402
from PIL import Image  # noqa: E402

SIZE, SCALE = 256, 6
TOP, BOTTOM = (40, 150, 230), (150, 215, 245)
# 600x900 library capsule; the square from 200 px down holds Mario and the logo.
GRID_URL = "https://cdn2.steamgriddb.com/grid/f5e97c0193978d636807084bf658fc1c.png"
GRID_TOP = 200 / 900


def from_steamgriddb():
    req = urllib.request.Request(GRID_URL, headers={"User-Agent": "sms-switch make_icon.py"})
    with urllib.request.urlopen(req, timeout=60) as r:
        grid = Image.open(io.BytesIO(r.read())).convert("RGB")
    w, h = grid.size
    top = min(round(h * GRID_TOP), h - w)
    return grid.crop((0, top, w, top + w)).resize((SIZE, SIZE), Image.LANCZOS)


def from_disc(path):
    arc = ei.yaz0(ei.disc_file(ei.Disc(path), ei.ICON_PATH))
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
    return icon


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", required=True, help="JPEG to write")
    ap.add_argument("--steamgriddb", action="store_true", help="the box art from SteamGridDB")
    ap.add_argument("--disc", help="GMSE01 disc image (.iso/.gcm/.ciso) for the memory-card Mario")
    a = ap.parse_args()
    if not (a.steamgriddb or a.disc):
        ap.error("pass --steamgriddb and/or --disc")
    icon, source = None, None
    if a.steamgriddb:
        try:
            icon, source = from_steamgriddb(), "SteamGridDB box art"
        except Exception as e:  # offline, moved: fall back
            print("make_icon: SteamGridDB: %s" % e, file=sys.stderr)
    if icon is None and a.disc:
        icon, source = from_disc(a.disc), "the disc's memory-card icon"
    if icon is None:
        sys.exit("make_icon: no icon")
    icon.save(a.out, "JPEG", quality=95)
    print("make_icon: wrote %s from %s" % (a.out, source))


if __name__ == "__main__":
    main()
