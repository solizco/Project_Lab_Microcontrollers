#!/usr/bin/env python3
"""
gifface.py - face animation player for the Connect 4 robot (Pi Zero + 5" HDMI)

HOW IT'S MEANT TO BE USED
  1. Laptop: put the GIFs in a "gifs" folder next to these scripts.
     Optional: preview and adjust framing/speed with the editor:
         python gifface.py gifs          (press E to edit, S to save)
     Settings are saved to gifs/settings.json.
  2. Laptop: run  python convert_gifs.py  (or double-click convert.bat).
     It turns every GIF into a small, ready-to-show file in a "faces"
     folder. All the slow work (decoding, scaling, rotating) happens here.
  3. Copy the "faces" folder onto the SD card's bootfs. The Pi keeps the
     frames compressed and unpacks one frame at a time while playing, so it
     uses very little memory and loads in a second or two.

The Pi looks for (first one found):
  ~/faces, /boot/firmware/faces, /boot/faces     converted faces (fast)
  ~/gifs,  /boot/firmware/gifs,  /boot/gifs      raw GIFs (slow on a Pi Zero)

GIF naming: variants share a name with a number on the end. bad_move.gif and
bad_move1.gif are both "bad_move"; play("bad_move") picks one at random.

Run the demo:
  python3 gifface.py                       auto-find faces (or gifs)
  python3 gifface.py /boot/firmware/faces  specific folder
  python3 gifface.py --rotate 180          if the picture is upside down
Demo keys: 1-9 play a reaction, A auto-cycle on/off, F FPS counter,
E edit mode (raw GIFs only), Q or ESC quit.

Edit mode keys (raw GIFs only, normally on the laptop):
  Arrows   move the picture (hold Shift for bigger steps)
  + / -    zoom in / out
  Z        cycle fit / fill / stretch
  [ / ]    slower / faster
  , / .    lower / raise this GIF's frame rate cap
  Tab      next GIF (Shift+Tab previous)
  R        reset this GIF   S  save   E  leave edit mode

settings.json can also be edited by hand:
  "max_fps": 12   drop frames above 12 fps (less memory, same speed).
                  Put it under "defaults" to cap every GIF.
  "fps": 12       play every frame at exactly 12 fps (changes the speed).

Use it from your game code:
  from gifface import FacePlayer
  face = FacePlayer()                  # loads everything
  face.play("good_move")               # plays once, then back to idle
  face.play("roger_win", loops=0)      # loops until the next play() call
  while face.update():                 # call every pass of your game loop
      ...game logic...                 # keyboard events are in face.events
  face.close()
"""

import argparse
import json
import math
import os
import random
import re
import sys
import time
import zlib
from pathlib import Path

os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")
import pygame
from PIL import Image, ImageSequence

# ---------------------------------------------------------------------
# CONFIG
# ---------------------------------------------------------------------
# Frame size in the screen's own (wide) orientation. The graphics chip scales
# it to the full 800x480. 400x240 is exactly half, so it scales cleanly.
# For converted faces, the size chosen in convert_gifs.py is used instead.
CANVAS = (400, 240)

# 0 = normal wide screen, 180 = wide but mounted upside down,
# 90 or 270 = tall screen.
ROTATE = 0

SMOOTH = True          # smooth scaling; False = blocky (pixel art)

# Raw GIF mode only: memory the frames may use in MB (0 = automatic),
# color depth of stored frames, and a frame rate cap for every GIF.
MEM_BUDGET_MB = 0
FRAME_BITS = 16
MAX_FPS = 0

BG = (0, 0, 0)
WHITE = (255, 255, 255)

DEFAULTS = {"mode": "fit", "zoom": 1.0, "x": 0, "y": 0, "speed": 1.0, "fps": 0,
            "max_fps": MAX_FPS}
MODES = ["fit", "fill", "stretch"]
FPS_CAPS = [0, 30, 24, 20, 15, 12, 10, 8, 6, 5]   # editor steps, 0 = no cap
SETTINGS_NAME = "settings.json"
HOME_SETTINGS = Path.home() / "gifface_settings.json"

PACK_EXT = ".face"
PACK_MAGIC = b"FACE1\n"

# Windows and Mac get a normal window (for previewing/editing on a laptop);
# Linux (the Pi) goes full screen.
DESKTOP_OS = os.name == "nt" or sys.platform == "darwin"


def now_ms():
    return time.monotonic() * 1000


# ---------------------------------------------------------------------
# DISPLAY
# ---------------------------------------------------------------------
def _driver_candidates():
    if DESKTOP_OS or os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"):
        return [("desktop", {})]
    c = []
    runtime = f"/run/user/{os.getuid()}"
    if os.path.exists(f"{runtime}/wayland-0"):
        c.append(("wayland", {
            "XDG_RUNTIME_DIR": runtime,
            "WAYLAND_DISPLAY": "wayland-0",
            "SDL_VIDEODRIVER": "wayland",
        }))
    if os.path.exists("/tmp/.X11-unix/X0"):
        c.append(("x11", {"DISPLAY": ":0", "SDL_VIDEODRIVER": "x11"}))
    c.append(("kmsdrm", {"SDL_VIDEODRIVER": "kmsdrm"}))
    return c


def _flags():
    return pygame.SCALED if DESKTOP_OS else pygame.FULLSCREEN | pygame.SCALED


def open_display(canvas):
    """Full screen at canvas size, scaled to the screen by the GPU."""
    for label, env in _driver_candidates():
        saved = {k: os.environ.get(k) for k in env}
        os.environ.update(env)
        last_err = None
        for vsync in (1, 0):
            try:
                pygame.display.init()
                screen = pygame.display.set_mode(canvas, _flags(), vsync=vsync)
                return screen, label, bool(vsync)
            except pygame.error as err:
                last_err = err
                pygame.display.quit()
        print(f"  video driver {label} failed: {last_err}")
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
    raise SystemExit("Could not open the display. If you used sudo, try without it.")


def set_canvas(canvas):
    """Switch an already-open display to a new canvas size (GPU scaled).
    SDL can't always resize a GPU-scaled screen in place ("failed to create
    renderer"), so the display is closed and reopened. Loaded images and
    fonts are not affected; the screen just blinks black for a moment."""
    pygame.display.quit()
    pygame.display.init()
    last_err = None
    for vsync in (1, 0):
        try:
            screen = pygame.display.set_mode(canvas, _flags(), vsync=vsync)
            pygame.mouse.set_visible(False)
            return screen, bool(vsync)
        except pygame.error as err:
            last_err = err
    raise SystemExit(f"Could not set display mode: {last_err}")


def text_box(font, lines):
    surfs = [font.render(t, True, WHITE) for t in lines]
    w = max(s.get_width() for s in surfs) + 8
    h = sum(s.get_height() for s in surfs) + 6
    box = pygame.Surface((w, h))
    box.fill(BG)
    y = 3
    for s in surfs:
        box.blit(s, (4, y))
        y += s.get_height()
    return box


# ---------------------------------------------------------------------
# FRAMING (shared by the player and convert_gifs.py)
# ---------------------------------------------------------------------
def portrait_size(canvas, rotate):
    """The canvas as you see it (tall when rotated)."""
    return (canvas[1], canvas[0]) if rotate % 180 else canvas


def to_screen(px, py, w, h, canvas, rotate):
    """Where a w x h box at (px, py) in the upright layout lands on the real screen."""
    pw, ph = portrait_size(canvas, rotate)
    if rotate == 90:
        return (py, pw - px - w)
    if rotate == 270:
        return (ph - py - h, px)
    if rotate == 180:
        return (pw - px - w, ph - py - h)
    return (px, py)


def blit_portrait(target, surf, px, py, canvas, rotate):
    w, h = surf.get_size()
    if rotate:
        surf = pygame.transform.rotate(surf, rotate)
    target.blit(surf, to_screen(px, py, w, h, canvas, rotate))


def compose_frame(src, st, canvas, rotate, bits=16):
    """Frame, zoom, position and rotate one source frame into a screen-ready frame."""
    pw, ph = portrait_size(canvas, rotate)
    sw, sh = src.get_size()
    if st["mode"] == "stretch":
        sx, sy = pw / sw, ph / sh
    else:
        s = max(pw / sw, ph / sh) if st["mode"] == "fill" else min(pw / sw, ph / sh)
        sx = sy = s
    z = min(5.0, max(0.2, float(st["zoom"])))
    w, h = max(1, round(sw * sx * z)), max(1, round(sh * sy * z))
    img = src
    if (w, h) != (sw, sh):
        smooth = SMOOTH and src.get_bitsize() in (24, 32)
        img = (pygame.transform.smoothscale if smooth else pygame.transform.scale)(src, (w, h))
    px = (pw - w) // 2 + int(st["x"])
    py = (ph - h) // 2 + int(st["y"])
    out = pygame.Surface(canvas, 0, 16) if bits == 16 else pygame.Surface(canvas).convert()
    out.fill(BG)
    blit_portrait(out, img, px, py, canvas, rotate)
    return out


def bake_timing(durations, st):
    """Apply a GIF's speed / fixed fps settings to its frame times."""
    if st.get("fps"):
        return [1000.0 / max(0.5, float(st["fps"]))] * len(durations)
    speed = max(0.1, float(st.get("speed", 1.0)))
    return [d / speed for d in durations]


# ---------------------------------------------------------------------
# SETTINGS
# ---------------------------------------------------------------------
def load_settings(folder):
    """Returns (settings dict, path to save to)."""
    folder_file = folder / SETTINGS_NAME
    writable = os.access(folder, os.W_OK)
    save_path = folder_file if writable else HOME_SETTINGS
    sources = [folder_file] if writable else [folder_file, HOME_SETTINGS]
    data = {}
    for p in sources:
        if not p.is_file():
            continue
        try:
            d = json.loads(p.read_text())
        except (OSError, ValueError) as err:
            print(f"Could not read {p}: {err}")
            continue
        for k, v in d.items():
            if isinstance(v, dict):
                data.setdefault(k, {}).update(v)
        print(f"Loaded settings from {p}")
    return data, save_path


def effective(settings, name):
    st = dict(DEFAULTS)
    st.update(settings.get("defaults", {}))
    st.update(settings.get(name, {}))
    if st["mode"] not in MODES:
        st["mode"] = "fit"
    return st


# ---------------------------------------------------------------------
# GIF READING
# ---------------------------------------------------------------------
def _frame_ms_of(fr):
    d = fr.info.get("duration", 100) or 100
    return 100 if d <= 10 else d  # browsers treat 0 to 10 ms as 100 ms, so do the same


def iter_gif(path, step=1):
    """Yield (pygame surface at original size, duration ms) one frame at a time.
    Keeps every `step`-th frame, and merges a frame that is identical to the one
    before it (common in face GIFs that hold still). Skipped and merged frames
    add their time to the frame shown, so the overall timing stays the same."""
    with Image.open(path) as im:
        pending = None
        last_data = None
        for i, fr in enumerate(ImageSequence.Iterator(im)):
            d = _frame_ms_of(fr)
            if i % step:
                if pending is not None:
                    pending[1] += d
                continue
            if fr.mode in ("RGBA", "LA") or "transparency" in fr.info:
                rgba = fr.convert("RGBA")
                bg = Image.new("RGBA", rgba.size, BG + (255,))
                bg.alpha_composite(rgba)
                rgb = bg.convert("RGB")
            else:
                rgb = fr.convert("RGB")
            data = rgb.tobytes()
            if pending is not None and data == last_data:
                pending[1] += d          # same picture as before: just show it longer
                continue
            if pending is not None:
                yield pending[0], pending[1]
            surf = pygame.image.fromstring(data, rgb.size, "RGB").convert()
            pending = [surf, d]
            last_data = data
        if pending is not None:
            yield pending[0], pending[1]


def decode_gif(path, step=1):
    """Return (frames as pygame surfaces at original size, durations in ms)."""
    frames, durations = [], []
    for surf, d in iter_gif(path, step):
        frames.append(surf)
        durations.append(d)
    return frames, durations


def gif_info(path):
    """(frame count, first frame's duration in ms) without decoding the whole GIF."""
    try:
        with Image.open(path) as im:
            return getattr(im, "n_frames", 1), _frame_ms_of(im)
    except OSError:
        return 1, 100


def cap_step(frame_ms, max_fps):
    """How many frames to step over so the GIF plays at or below max_fps."""
    if not max_fps:
        return 1
    native_fps = 1000.0 / max(1, frame_ms)
    return max(1, math.ceil(native_fps / float(max_fps) - 1e-6))


def mem_available_mb():
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) / 1024
    except OSError:
        pass
    return None


# ---------------------------------------------------------------------
# CONVERTED FACE FILES (.face)
# ---------------------------------------------------------------------
def write_pack(path, header, blobs):
    header = dict(header, sizes=[len(b) for b in blobs])
    h = json.dumps(header).encode()
    with open(path, "wb") as f:
        f.write(PACK_MAGIC)
        f.write(len(h).to_bytes(4, "little"))
        f.write(h)
        for b in blobs:
            f.write(b)


def read_pack(path):
    data = Path(path).read_bytes()
    if not data.startswith(PACK_MAGIC):
        raise ValueError("not a converted face file")
    p = len(PACK_MAGIC)
    n = int.from_bytes(data[p:p + 4], "little")
    p += 4
    hdr = json.loads(data[p:p + n])
    p += n
    blobs = []
    for s in hdr["sizes"]:
        blobs.append(data[p:p + s])
        p += s
    if len(blobs) != len(hdr["durations"]) or p != len(data):
        raise ValueError("file is damaged or incomplete, convert it again")
    return hdr, blobs


# ---------------------------------------------------------------------
# FOLDERS
# ---------------------------------------------------------------------
def _group_name(path):
    """bad_move1.gif -> bad_move, idle.gif -> idle, idle_2.gif -> idle"""
    return re.sub(r"\d+$", "", Path(path).stem).rstrip("_-") or Path(path).stem


def _has(p, ext):
    return p.is_dir() and any(f.suffix.lower() == ext for f in p.iterdir())


def _default_folder():
    roots = (Path.home(), Path("/boot/firmware"), Path("/boot"))
    for name, ext in (("faces", PACK_EXT), ("gifs", ".gif")):
        for root in roots:
            if _has(root / name, ext):
                return root / name
    return Path.home() / "gifs"


class Clip:
    def __init__(self, name, durations, total_frames=0, frame_ms=100, group=None, path=None):
        self.path = path
        self.name = name
        self.group = group or _group_name(name)
        self.total_frames = total_frames   # frames in the original GIF
        self.frame_ms = frame_ms           # GIF's own frame time (first frame)
        self.set_durations(durations)
        self.frames = []     # surfaces (raw GIF mode) or compressed bytes (faces)
        self.packed = False
        self.pitch = 0
        self.src = None      # original frames, only kept while editing
        self.step = 1
        self.st = dict(DEFAULTS)

    def set_durations(self, durations):
        self.durations = durations
        self.gif_fps = 1000.0 * len(durations) / max(1, sum(durations))   # as stored

    def memory_mb(self):
        if self.packed:
            return sum(len(b) for b in self.frames) / 1e6
        return sum(f.get_width() * f.get_height() * f.get_bytesize() for f in self.frames) / 1e6


# ---------------------------------------------------------------------
# PLAYER
# ---------------------------------------------------------------------
class FacePlayer:
    def __init__(self, folder=None, idle="idle", rotate=ROTATE, canvas=CANVAS,
                 show_fps=False, allow_edit=True):
        folder = _default_folder() if folder is None else Path(folder).expanduser()
        listing = sorted(folder.iterdir()) if folder.is_dir() else []
        packs = [f for f in listing if f.suffix.lower() == PACK_EXT]
        gifs = [f for f in listing if f.suffix.lower() == ".gif"]
        if not packs and not gifs:
            raise SystemExit(f"No .face or .gif files found in {folder}")
        self.folder = folder
        self.packed = bool(packs)
        self.rotate = rotate % 360
        if self.rotate not in (0, 90, 180, 270):
            raise SystemExit("rotate must be 0, 90, 180 or 270")
        self._flip = 0

        # Converted faces: read the files first (no display needed), and take
        # the canvas size and rotation they were made with.
        loaded = []
        if self.packed:
            t0 = now_ms()
            for p in packs:
                try:
                    loaded.append((p,) + read_pack(p))
                except (OSError, ValueError, KeyError) as err:
                    print(f"  {p.name}: skipped ({err})")
            if not loaded:
                raise SystemExit(f"None of the face files in {folder} could be read. Convert them again.")
            canvas = tuple(loaded[0][1]["canvas"])
            baked = loaded[0][1]["rotate"] % 360
            if any(tuple(h["canvas"]) != canvas or h["rotate"] % 360 != baked for _, h, _ in loaded):
                raise SystemExit("The face files were converted with different settings. "
                                 "Run convert_gifs.py again so they all match.")
            self._flip = (self.rotate - baked) % 360
            if self._flip not in (0, 180):
                raise SystemExit(f"These faces were converted for rotate {baked}. "
                                 f"Convert again with --rotate {self.rotate}.")
            if self._flip:
                print(f"Note: faces were converted for rotate {baked}, flipping them on the fly. "
                      f"Convert with --rotate {self.rotate} for best speed.")
            self.rotate = baked
            read_s = (now_ms() - t0) / 1000

        self.canvas = canvas
        self.pcanvas = portrait_size(canvas, self.rotate)

        if pygame.display.get_init() and pygame.display.get_surface() is not None:
            # Another screen (like the menu) already opened the display
            self.screen, self.vsync = set_canvas(canvas)
            self.driver = pygame.display.get_driver()
        else:
            self.screen, self.driver, self.vsync = open_display(canvas)
        pygame.font.init()
        pygame.mouse.set_visible(False)
        self.font = pygame.font.Font(None, 18)
        self.small = pygame.font.Font(None, 16)
        pw, ph = self.pcanvas
        print(f"Video driver: {self.driver}, vsync: {self.vsync}, "
              f"layout {pw}x{ph} (rotate {self.rotate}), from {folder}")

        self.show_fps = show_fps
        self.show_name = False          # show "3/9  idle.gif" in the corner
        self.help_line = ""             # extra hint line under the name
        self.hold_on_edit_exit = False  # stay on the edited GIF instead of going to idle
        self.allow_edit = allow_edit
        self.editing = False
        self.events = []
        self.fps = 0.0
        self._flips = 0
        self._fps_start = now_ms()
        self._last_variant = {}
        self._preview = None
        self._rebuild_at = 0
        self._flash_msg = ""
        self._flash_until = 0
        self._quit_armed = False
        self._ov_key = None
        self._ov = None
        self._decode_key = None
        self.clip = None
        self.settings, self.settings_path = {}, HOME_SETTINGS
        self.unsaved = False
        self._mem_extra = 1

        self.clips = []
        self.groups = {}
        if self.packed:
            self._init_packed(loaded, read_s)
        else:
            self._init_gifs(gifs)
        print("Reactions: " + ", ".join(
            f"{n} ({len(v)})" if len(v) > 1 else n for n, v in self.groups.items()))

        self.idle = idle if idle in self.groups else next(iter(self.groups))
        self.play(self.idle, loops=0)

    # ---- loading ---------------------------------------------------------
    def _init_packed(self, loaded, read_s):
        self._decode_surf = pygame.Surface(self.canvas, 0, 16)
        for p, hdr, blobs in loaded:
            clip = Clip(hdr.get("source", p.stem + ".gif"), hdr["durations"],
                        hdr.get("total_frames", len(blobs)), hdr.get("frame_ms", 100),
                        group=_group_name(hdr.get("source", p.name)), path=p)
            clip.frames = blobs
            clip.packed = True
            clip.pitch = hdr["pitch"]
            self.clips.append(clip)
            self.groups.setdefault(clip.group, []).append(clip)
        mb = sum(c.memory_mb() for c in self.clips)
        print(f"Loaded {len(self.clips)} faces in {read_s:.1f} s, {mb:.1f} MB in memory")

    def _init_gifs(self, files):
        self.settings, self.settings_path = load_settings(self.folder)
        self._message_screen("Checking GIFs...")
        infos = [gif_info(f) for f in files]
        sts = [effective(self.settings, f.name) for f in files]
        steps = [cap_step(ms, st["max_fps"]) for (_, ms), st in zip(infos, sts)]
        frame_mb = self.canvas[0] * self.canvas[1] * (FRAME_BITS // 8) / 1e6
        need_mb = sum(math.ceil(n / s) for (n, _), s in zip(infos, steps)) * frame_mb
        avail = mem_available_mb()
        if MEM_BUDGET_MB:
            budget = MEM_BUDGET_MB
        elif avail:
            budget = max(30.0, avail * 0.6 - 40)   # leave room for everything else
        else:
            budget = 400.0
        self._mem_extra = max(1, math.ceil(need_mb / budget))
        print(f"Memory: {avail:.0f} MB free, " if avail else "Memory: ", end="")
        print(f"budget {budget:.0f} MB, frames need up to {need_mb:.0f} MB")
        if self._mem_extra > 1:
            print(f"  Short on memory: every GIF drops to 1 in {self._mem_extra} frames. "
                  f"On the Pi, use convert_gifs.py instead of raw GIFs.")

        t0 = now_ms()
        for i, f in enumerate(files):
            self._message_screen(f"Loading {i + 1}/{len(files)}", f.name, i / len(files))
            pygame.event.pump()
            st = sts[i]
            n_total, ms = infos[i]
            step = steps[i] * self._mem_extra
            frames, durs = [], []
            for surf, d in iter_gif(f, step):
                frames.append(self._compose(surf, st))
                durs.append(d)
            if not frames:
                print(f"  {f.name}: no frames, skipped")
                continue
            clip = Clip(f.name, durs, n_total, ms, path=f)
            clip.st = st
            clip.step = step
            clip.frames = frames
            self.clips.append(clip)
            self.groups.setdefault(clip.group, []).append(clip)
            print(f"  {f.name}: {n_total} frames -> {len(frames)} stored, {clip.memory_mb():.1f} MB")
        if not self.clips:
            raise SystemExit(f"None of the GIFs in {self.folder} could be loaded")
        mb = sum(c.memory_mb() for c in self.clips)
        print(f"Loaded {len(self.clips)} GIFs in {(now_ms() - t0) / 1000:.1f} s, about {mb:.0f} MB of memory")

    # ---- public API ----------------------------------------------------
    def names(self):
        return list(self.groups)

    def play(self, name, loops=1, then=None):
        """Switch to a reaction right away.
        loops: how many times to play it (0 = forever).
        then:  what to switch to afterwards (default: idle, looping)."""
        if name not in self.groups:
            print(f"No GIF named {name!r}. Available: {', '.join(self.groups)}")
            return
        variants = self.groups[name]
        last = self._last_variant.get(name)
        choices = [i for i in range(len(variants)) if i != last] or [0]
        vi = random.choice(choices)
        self._last_variant[name] = vi
        self._start(variants[vi], loops, then)

    def play_clip(self, i, loops=0, then=None):
        """Play one specific GIF file by its position in self.clips (wraps around)."""
        self._start(self.clips[i % len(self.clips)], loops, then)

    def attach(self):
        """Take the screen back after another screen (like the menu) used it."""
        self.screen, self.vsync = set_canvas(self.canvas)
        self._decode_key = None
        self.dirty = True

    def update(self):
        """Call every pass of your loop. Returns False when the user quits."""
        self.events = pygame.event.get()
        for e in self.events:
            if e.type == pygame.QUIT:
                return False
            if e.type == pygame.KEYDOWN:
                if e.key in (pygame.K_ESCAPE, pygame.K_q):
                    if self._quit_ok():
                        return False
                elif self.allow_edit and e.key == pygame.K_e:
                    if self.packed:
                        self._flash("Edit on the laptop, then convert")
                    else:
                        self._toggle_edit()
                elif self.editing:
                    self._edit_key(e)

        now = now_ms()
        if self._rebuild_at:
            if now >= self._rebuild_at:
                self._rebuild()
                self.next_due = now_ms() + self._frame_ms(self.fi)
            return self._draw_or_wait()

        if now >= self.next_due:
            nxt = self.fi + 1
            if nxt >= len(self.clip.frames):
                nxt = 0
                if self.loops_left > 0 and not self.editing:
                    self.loops_left -= 1
                    if self.loops_left == 0:
                        self.play(self.then, loops=0)
                        return self._draw_or_wait()
            self.fi = nxt
            self.next_due += self._frame_ms(nxt)
            if self.next_due < now - 250:  # fell way behind, resync
                self.next_due = now + self._frame_ms(nxt)
            self.dirty = True
        return self._draw_or_wait()

    def save_settings(self):
        base = dict(DEFAULTS)
        base.update(self.settings.get("defaults", {}))
        out = {}
        if self.settings.get("defaults"):
            out["defaults"] = self.settings["defaults"]
        for k, v in sorted(self.settings.items()):
            if k == "defaults":
                continue
            diff = {kk: vv for kk, vv in v.items() if base.get(kk) != vv}
            if diff:
                out[k] = diff
        try:
            self.settings_path.write_text(json.dumps(out, indent=2) + "\n")
        except OSError as err:
            print(f"Could not save settings: {err}")
            self._flash("Save failed, see terminal")
            return
        self.unsaved = False
        print(f"Saved settings to {self.settings_path}")
        self._flash("Saved")

    def close(self):
        pygame.quit()

    # ---- frames ------------------------------------------------------------
    def _compose(self, src, st):
        return compose_frame(src, st, self.canvas, self.rotate, FRAME_BITS)

    def _frame_surface(self, clip, i):
        """The surface to show for frame i. Converted faces are unpacked here,
        one frame at a time, into a single reusable surface."""
        if not clip.packed:
            return clip.frames[i]
        key = (id(clip), i)
        if key != self._decode_key:
            data = zlib.decompress(clip.frames[i])
            surf = self._decode_surf
            pitch = surf.get_pitch()
            buf = surf.get_buffer()
            if pitch == clip.pitch:
                buf.write(data, 0)
            else:  # different row padding on this machine: copy row by row
                row = self.canvas[0] * 2
                for y in range(self.canvas[1]):
                    buf.write(data[y * clip.pitch:y * clip.pitch + row], y * pitch)
            del buf
            self._decode_key = key
        return self._decode_surf

    def _frame_ms(self, i):
        st = self.clip.st
        if st.get("fps"):
            return 1000.0 / max(0.5, float(st["fps"]))
        return self.clip.durations[i] / max(0.1, float(st["speed"]))

    # ---- playback internals -----------------------------------------------
    def _start(self, clip, loops, then):
        if self._rebuild_at:
            self._rebuild()
        if self.clip is not None and self.clip is not clip:
            self.clip.src = None
        self.clip = clip
        self.current = clip.group
        self.loops_left = loops
        self.then = self.idle if then is None else then
        self.fi = 0
        self.next_due = now_ms() + self._frame_ms(0)
        self.dirty = True

    def _overlay(self, lines):
        """Text box in the corner. Only re-rendered when its text changes."""
        key = tuple(lines)
        if key != self._ov_key:
            box = text_box(self.small, lines)
            w, h = box.get_size()
            if self.rotate:
                box = pygame.transform.rotate(box, self.rotate)
            self._ov = (box, to_screen(3, 3, w, h, self.canvas, self.rotate))
            self._ov_key = key
        box, pos = self._ov
        if self._flip:
            pos = (self.canvas[0] - pos[0] - box.get_width(), self.canvas[1] - pos[1] - box.get_height())
            box = pygame.transform.rotate(box, 180)
        self.screen.blit(box, pos)

    def _draw_or_wait(self):
        now = now_ms()
        if self.dirty:
            self.dirty = False
            frame = self._preview if self._preview is not None else self._frame_surface(self.clip, self.fi)
            if self._flip:
                frame = pygame.transform.rotate(frame, 180)
            self.screen.blit(frame, (0, 0))
            lines = []
            if self.editing:
                lines = self._edit_lines()
            else:
                if self.show_fps:
                    lines.append(f"{self.fps:.1f} fps")
                if self.show_name:
                    idx = self.clips.index(self.clip)
                    lines.append(f"{idx + 1}/{len(self.clips)}  {self.clip.name}")
                    if self.help_line:
                        lines.append(self.help_line)
            if self._flash_msg and now < self._flash_until:
                lines.append(self._flash_msg)
            if lines:
                self._overlay(lines)
            pygame.display.flip()
            self._flips += 1
            t = now_ms()
            if t - self._fps_start >= 1000:
                self.fps = self._flips * 1000 / (t - self._fps_start)
                self._flips = 0
                self._fps_start = t
        else:
            target = self._rebuild_at or self.next_due
            wait = int(target - now)
            if wait > 1:
                pygame.time.wait(min(wait, 5))
        return True

    def _message_screen(self, line1, line2="", progress=None):
        pw, ph = self.pcanvas
        surf = pygame.Surface((pw, ph))
        surf.fill(BG)
        y = ph // 2 - 30
        for text in (line1, line2):
            if text:
                t = self.small.render(text, True, WHITE)
                surf.blit(t, ((pw - t.get_width()) // 2, y))
                y += t.get_height() + 4
        if progress is not None:
            bar_w = pw - 40
            pygame.draw.rect(surf, WHITE, (20, y + 6, bar_w, 8), 1)
            pygame.draw.rect(surf, WHITE, (20, y + 6, int(bar_w * progress), 8))
        self.screen.fill(BG)
        blit_portrait(self.screen, surf, 0, 0, self.canvas, self.rotate)
        pygame.display.flip()

    def _flash(self, msg):
        self._flash_msg = msg
        self._flash_until = now_ms() + 2500
        self.dirty = True

    def _quit_ok(self):
        if self.unsaved and not self._quit_armed:
            self._quit_armed = True
            print("Unsaved edits. Press S to save, or Q again to quit without saving.")
            self._flash("Unsaved! S save, Q again quit")
            return False
        return True

    # ---- edit mode (raw GIFs only) ------------------------------------------
    def _toggle_edit(self):
        if self.editing:
            if self._rebuild_at:
                self._rebuild()
            for c in self.clips:
                c.src = None
            self.editing = False
            print("Edit mode off")
            if self.hold_on_edit_exit:
                self._start(self.clip, 0, None)
            else:
                self.play(self.idle, loops=0)
        else:
            self.editing = True
            print("Edit mode on. Editing: " + self.clip.name)
        self.dirty = True

    def _edit_lines(self):
        c, st = self.clip, self.clip.st
        if st.get("fps"):
            speed = f"fixed {float(st['fps']):.1f} fps (set in file)"
        else:
            speed = f"speed {st['speed']:.1f}x = {c.gif_fps * st['speed']:.1f} fps"
        cap = f"cap {st['max_fps']} fps" if st["max_fps"] else "no fps cap"
        lines = [
            f"EDIT {c.name}",
            f"{st['mode']}  zoom {st['zoom']:.2f}  x {st['x']}  y {st['y']}",
            speed,
            f"{cap}  {len(c.frames)}/{c.total_frames} frames",
            "arrows move   +/- zoom",
            "Z fit/fill/stretch   [ ] speed",
            ", . fps cap down/up   Tab next",
            "R reset   S save   E exit",
        ]
        if self.unsaved:
            lines.append("UNSAVED")
        return lines

    def _edit_key(self, e):
        shift = pygame.key.get_mods() & pygame.KMOD_SHIFT
        k = e.key
        name = self.clip.name
        st = self.clip.st
        ov = self.settings.setdefault(name, {})
        step = 10 if shift else 2
        framing = True
        changed = True
        reload = False

        if k in (pygame.K_COMMA, pygame.K_PERIOD):
            cur = st["max_fps"]
            idx = FPS_CAPS.index(cur) if cur in FPS_CAPS else \
                next((i for i, c in enumerate(FPS_CAPS) if c and c < cur), len(FPS_CAPS) - 1)
            idx = min(len(FPS_CAPS) - 1, idx + 1) if k == pygame.K_COMMA else max(0, idx - 1)
            ov["max_fps"] = FPS_CAPS[idx]
            framing = False
            reload = True
        elif k == pygame.K_LEFT:
            ov["x"] = st["x"] - step
        elif k == pygame.K_RIGHT:
            ov["x"] = st["x"] + step
        elif k == pygame.K_UP:
            ov["y"] = st["y"] - step
        elif k == pygame.K_DOWN:
            ov["y"] = st["y"] + step
        elif k in (pygame.K_EQUALS, pygame.K_PLUS, pygame.K_KP_PLUS):
            ov["zoom"] = round(min(5.0, st["zoom"] * 1.05), 3)
        elif k in (pygame.K_MINUS, pygame.K_KP_MINUS):
            ov["zoom"] = round(max(0.2, st["zoom"] / 1.05), 3)
        elif k == pygame.K_z:
            ov["mode"] = MODES[(MODES.index(st["mode"]) + 1) % len(MODES)]
        elif k == pygame.K_RIGHTBRACKET:
            ov["speed"] = round(min(5.0, st["speed"] + 0.1), 2)
            framing = False
        elif k == pygame.K_LEFTBRACKET:
            ov["speed"] = round(max(0.1, st["speed"] - 0.1), 2)
            framing = False
        elif k == pygame.K_r:
            self.settings.pop(name, None)
            reload = True
        elif k == pygame.K_s:
            self.save_settings()
            changed = False
        elif k == pygame.K_TAB:
            i = self.clips.index(self.clip)
            self._start(self.clips[(i + (-1 if shift else 1)) % len(self.clips)], 0, None)
            print("Editing: " + self.clip.name)
            changed = False
        else:
            changed = False

        if changed:
            self.unsaved = True
            self._quit_armed = False
            self.clip.st = effective(self.settings, name)
            if reload:
                self._reload_clip()
            elif framing:
                self._schedule_rebuild()
            self.dirty = True

    def _reload_clip(self):
        """Re-read the current GIF from the file with its frame rate cap applied."""
        c = self.clip
        self._message_screen("Reloading", c.name)
        c.frames = []          # let the old frames go before loading new ones
        c.src = None
        self._preview = None
        self._rebuild_at = 0
        c.step = cap_step(c.frame_ms, c.st["max_fps"]) * self._mem_extra
        c.src, durs = decode_gif(c.path, c.step)
        c.set_durations(durs)
        c.frames = [self._compose(s, c.st) for s in c.src]
        print(f"  {c.name}: {len(c.frames)} frames stored, {c.memory_mb():.1f} MB")
        self.fi = 0
        self.next_due = now_ms() + self._frame_ms(0)
        self.dirty = True

    def _schedule_rebuild(self):
        """Show a quick preview of one frame now; rebuild all frames once keys settle."""
        c = self.clip
        if c.src is None:
            self._message_screen("Loading for edit", c.name)
            c.src, _ = decode_gif(c.path, c.step)
        self._preview = self._compose(c.src[self.fi], c.st)
        self._rebuild_at = now_ms() + 400

    def _rebuild(self):
        c = self.clip
        if c.src is not None:
            c.frames = [self._compose(s, c.st) for s in c.src]
        self._preview = None
        self._rebuild_at = 0
        self.dirty = True


# ---------------------------------------------------------------------
# DEMO
# ---------------------------------------------------------------------
def demo():
    ap = argparse.ArgumentParser(description="Face animation demo / GIF editor")
    ap.add_argument("folder", nargs="?", default=None, help="folder with faces or GIFs")
    ap.add_argument("--rotate", type=int, default=None,
                    help="0 wide, 180 wide upside down, 90 or 270 tall")
    args = ap.parse_args()

    if args.rotate is None and DESKTOP_OS:
        # On a laptop, show the layout upright in a window. Framing
        # settings made here match what convert_gifs.py makes for the Pi.
        face = FacePlayer(args.folder, rotate=0, canvas=portrait_size(CANVAS, ROTATE), show_fps=True)
    else:
        rotate = ROTATE if args.rotate is None else args.rotate
        face = FacePlayer(args.folder, rotate=rotate, show_fps=True)
    names = face.names()
    print("\nKeys:")
    for i, n in enumerate(names[:9], 1):
        print(f"  {i}  {n}")
    print("  A  auto-cycle on/off   F  FPS counter   E  edit mode   Q  quit\n")

    reactions = [n for n in names if n != face.idle] or names
    auto = True
    was_idle = True
    next_auto = now_ms() + 2000
    try:
        while face.update():
            for e in face.events:
                if e.type != pygame.KEYDOWN:
                    continue
                if pygame.K_1 <= e.key <= pygame.K_9:
                    i = e.key - pygame.K_1
                    if i < len(names):
                        face.play(names[i])
                        print(f"Playing {names[i]}")
                elif e.key == pygame.K_a and not face.editing:
                    auto = not auto
                    print(f"Auto-cycle: {auto}")
                elif e.key == pygame.K_f and not face.editing:
                    face.show_fps = not face.show_fps
                    face.dirty = True

            if face.editing:
                continue
            idle_now = face.current == face.idle
            if idle_now and not was_idle:
                next_auto = now_ms() + 2000  # sit on idle for 2 s between reactions
            was_idle = idle_now
            if auto and idle_now and now_ms() >= next_auto:
                n = random.choice(reactions)
                face.play(n)
                print(f"Playing {n}")
    except KeyboardInterrupt:
        pass
    face.close()


if __name__ == "__main__":
    demo()
