#!/usr/bin/env python3
"""
convert_gifs.py - turn the robot's GIFs into small, ready-to-show face files
for the Pi. Run this on your laptop, not the Pi.

One-time setup on the laptop:
  pip install pygame pillow

Usage (gifface.py must be in the same folder as this script):
  python convert_gifs.py                    reads ./gifs, writes ./faces
  python convert_gifs.py --max-fps 15       cap every GIF at 15 fps
  python convert_gifs.py --canvas 800x480   full resolution (sharper, but more
                                            work for the Pi on every frame)
  python convert_gifs.py --rotate 180       if the Pi shows them upside down

Per-GIF framing, speed and fps caps come from gifs/settings.json, which the
editor writes (python gifface.py gifs, then E to edit and S to save).

Then copy the whole "faces" folder onto the SD card's bootfs.
"""

import argparse
import os
import sys
import time
import zlib
from pathlib import Path

os.environ["SDL_VIDEODRIVER"] = "dummy"          # no window needed
os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")

import pygame

import gifface as gf


def parse_canvas(text):
    try:
        w, h = (int(v) for v in text.lower().split("x"))
    except ValueError:
        raise argparse.ArgumentTypeError("use WIDTHxHEIGHT, for example 400x240")
    if w < 16 or h < 16 or w % 2 or h % 2:
        raise argparse.ArgumentTypeError("width and height must be even and at least 16")
    return (w, h)


def main():
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description="Convert GIFs into face files for the Pi")
    ap.add_argument("gifs", nargs="?", default=str(here / "gifs"), help="folder with the GIFs")
    ap.add_argument("out", nargs="?", default=str(here / "faces"), help="output folder")
    ap.add_argument("--canvas", type=parse_canvas, default=gf.CANVAS,
                    help="frame size in the screen's wide orientation (default 400x240)")
    ap.add_argument("--rotate", type=int, choices=[0, 90, 180, 270], default=gf.ROTATE,
                    help="0 wide (default), 180 wide upside down, 90 or 270 tall")
    ap.add_argument("--max-fps", type=float, default=None,
                    help="cap every GIF at this frame rate (overrides settings.json defaults)")
    args = ap.parse_args()

    src = Path(args.gifs)
    out = Path(args.out)
    gifs = sorted(f for f in src.iterdir() if f.suffix.lower() == ".gif") if src.is_dir() else []
    if not gifs:
        print(f"No .gif files found in {src}")
        return 1

    pygame.display.init()
    pygame.display.set_mode((1, 1))

    settings, _ = gf.load_settings(src)
    if args.max_fps is not None:
        settings.setdefault("defaults", {})["max_fps"] = args.max_fps

    out.mkdir(parents=True, exist_ok=True)
    old = list(out.glob("*" + gf.PACK_EXT))
    for f in old:
        f.unlink()
    print(f"Cleared {len(old)} old face files from {out}" if old else f"Writing to {out}")

    canvas, rotate = args.canvas, args.rotate
    raw_frame_kb = canvas[0] * canvas[1] * 2 / 1024
    print(f"Frame size {canvas[0]}x{canvas[1]}, rotate {rotate}, 16-bit color\n")
    print(f"{'GIF':<22}{'size':>10}{'frames':>14}{'fps':>7}{'file KB':>10}")

    t0 = time.time()
    total = 0
    biggest = []
    for f in gifs:
        st = gf.effective(settings, f.name)
        n_total, ms = gf.gif_info(f)
        step = gf.cap_step(ms, st["max_fps"])
        blobs, durs, size, pitch = [], [], None, 0
        try:
            for surf, d in gf.iter_gif(f, step):
                size = size or surf.get_size()
                frame = gf.compose_frame(surf, st, canvas, rotate, 16)
                pitch = frame.get_pitch()
                blobs.append(zlib.compress(frame.get_buffer().raw, 9))
                durs.append(d)
        except (OSError, ValueError) as err:
            print(f"{f.name:<22}  could not read: {err}")
            continue
        if not blobs:
            print(f"{f.name:<22}  no frames, skipped")
            continue
        durs = gf.bake_timing(durs, st)
        fps = 1000.0 * len(durs) / max(1, sum(durs))
        header = {
            "source": f.name,
            "canvas": list(canvas),
            "rotate": rotate,
            "bits": 16,
            "pitch": pitch,
            "durations": durs,
            "total_frames": n_total,
            "frame_ms": ms,
        }
        path = out / (f.stem + gf.PACK_EXT)
        gf.write_pack(path, header, blobs)
        kb = path.stat().st_size / 1024
        total += kb
        biggest.append((kb, f.name))
        frames = f"{n_total}->{len(blobs)}"
        print(f"{f.name:<22}{size[0]:>5}x{size[1]:<4}{frames:>14}{fps:>7.1f}{kb:>10.0f}")

    print(f"\nTotal: {total / 1024:.2f} MB on the Pi for {len(biggest)} faces "
          f"(uncompressed would be {raw_frame_kb:.0f} KB per frame). "
          f"Took {time.time() - t0:.0f} s.")
    if biggest:
        biggest.sort(reverse=True)
        print("Largest: " + ", ".join(f"{name} {kb:.0f} KB" for kb, name in biggest[:3]))
    print(f"\nNext: copy the '{out.name}' folder onto the SD card's bootfs.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
