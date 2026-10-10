#!/usr/bin/env python3
"""
connect4.py - menu shell for the Connect 4 robot (800x480 screen)

Needs gifface.py in the same folder (it shares the display setup and the
rotation setting).

Run:
  python3 connect4.py
  python3 connect4.py --rotate 180     if the screen is upside down
  (the layout also works tall: --rotate 90 or 270)

Keys (for now, GPIO buttons later; see read_actions()):
  Up / Down          move through the menu
  Enter or Space     select
  Esc or Backspace   back
  Q                  quit

Statistics are saved to ~/connect4_stats.json. On the Statistics screen,
T adds a random test game and C (pressed twice) clears everything, so the
layout can be checked before real games exist.

For the real game code later, record results with:
  record_roger_game(stats, "pro", "win")     # from R.O.G.E.R's point of view
  record_pvp_game(stats, "p1")               # "p1", "p2" or "draw"
"""

import argparse
import json
import os
import random
import subprocess
from pathlib import Path

import pygame

from gifface import ROTATE, FacePlayer, open_display, set_canvas

# ---------------------------------------------------------------------
# CONFIG
# ---------------------------------------------------------------------
SCREEN = (800, 480)          # physical panel
TITLE = "CONNECT 4"          # set to "" to remove the title
STATS_FILE = Path.home() / "connect4_stats.json"
DIFFICULTIES = ["noob", "pro", "hacker"]

BLACK = (0, 0, 0)
WHITE = (255, 255, 255)
GRAY = (150, 150, 150)
DIM = (100, 100, 100)
RED = (230, 40, 40)
BLUE = (50, 120, 255)

# Menu layout: labels are not selectable, items are.
MENU = [
    ("label", "Player vs Player"),
    ("item", "1v1", "pvp"),
    ("gap",),
    ("label", "VS R.O.G.E.R"),
    ("item", "Noob", "roger_noob"),
    ("item", "Pro", "roger_pro"),
    ("item", "Hacker", "roger_hacker"),
    ("gap",),
    ("item", "Options", "options"),
    ("item", "Statistics", "stats"),
]

# Options submenu: (label, target)
OPTIONS = [
    ("GIF test", "gif_test"),
    ("Shut down", "shutdown"),
]

KEYMAP = {
    pygame.K_UP: "up",
    pygame.K_DOWN: "down",
    pygame.K_LEFT: "left",
    pygame.K_RIGHT: "right",
    pygame.K_RETURN: "select",
    pygame.K_KP_ENTER: "select",
    pygame.K_SPACE: "select",
    pygame.K_ESCAPE: "back",
    pygame.K_BACKSPACE: "back",
    pygame.K_t: "test",
    pygame.K_c: "clear",
    pygame.K_q: "quit",
}


# ---------------------------------------------------------------------
# STATISTICS
# ---------------------------------------------------------------------
def empty_stats():
    return {
        "roger": {d: {"wins": 0, "losses": 0, "draws": 0} for d in DIFFICULTIES},
        "pvp": {"games": 0, "p1_wins": 0, "p2_wins": 0, "draws": 0},
    }


def load_stats():
    stats = empty_stats()
    if STATS_FILE.is_file():
        try:
            saved = json.loads(STATS_FILE.read_text())
            for d in DIFFICULTIES:
                stats["roger"][d].update(saved.get("roger", {}).get(d, {}))
            stats["pvp"].update(saved.get("pvp", {}))
        except (OSError, ValueError) as err:
            print(f"Could not read {STATS_FILE}, starting fresh: {err}")
    return stats


def save_stats(stats):
    tmp = STATS_FILE.with_suffix(".tmp")
    tmp.write_text(json.dumps(stats, indent=2) + "\n")
    os.replace(tmp, STATS_FILE)


def record_roger_game(stats, difficulty, result):
    """difficulty: noob / pro / hacker. result from R.O.G.E.R's view: win / loss / draw."""
    key = {"win": "wins", "loss": "losses", "draw": "draws"}[result]
    stats["roger"][difficulty][key] += 1
    save_stats(stats)


def record_pvp_game(stats, result):
    """result: p1 / p2 / draw"""
    stats["pvp"]["games"] += 1
    stats["pvp"][{"p1": "p1_wins", "p2": "p2_wins", "draw": "draws"}[result]] += 1
    save_stats(stats)


def roger_totals(stats):
    t = {"wins": 0, "losses": 0, "draws": 0}
    for d in DIFFICULTIES:
        for k in t:
            t[k] += stats["roger"][d][k]
    return t


# ---------------------------------------------------------------------
# APP
# ---------------------------------------------------------------------
class App:
    def __init__(self, rotate):
        self.rotate = rotate % 360
        self.screen, driver, _ = open_display(SCREEN)
        pygame.font.init()
        pygame.mouse.set_visible(False)
        self.pw, self.ph = (SCREEN[1], SCREEN[0]) if self.rotate % 180 else SCREEN
        self.wide = self.pw > self.ph
        self.canvas = pygame.Surface((self.pw, self.ph))
        if self.wide:
            # Title on the left, menu column on the right
            self.L = dict(menu_cx=int(self.pw * 0.66), title_cx=int(self.pw * 0.26),
                          label_step=34, item_step=48, box_h=42, box_w=300, gap=14)
        else:
            self.L = dict(menu_cx=self.pw // 2, title_cx=self.pw // 2,
                          label_step=44, item_step=60, box_h=52, box_w=320, gap=30)
        self.fonts = {
            "title": pygame.font.Font(None, 76),
            "item": pygame.font.Font(None, 50),
            "label": pygame.font.Font(None, 32),
            "big": pygame.font.Font(None, 72),
            "huge": pygame.font.Font(None, 140),
            "table": pygame.font.Font(None, 34),
            "small": pygame.font.Font(None, 26),
        }
        print(f"Video driver: {driver}, layout {self.pw}x{self.ph}")

        self.stats = load_stats()
        self.items = [i for i, e in enumerate(MENU) if e[0] == "item"]
        self.sel = 0
        self.state = "menu"
        self.player = 1
        self.tbd = ("", "", "")
        self.opt_sel = 0
        self.face = None   # GIF player, loaded the first time GIF test opens
        self.confirm_clear = False
        self.shutting_down = False
        self.dirty = True

    # ---- input -------------------------------------------------------------
    def actions_from(self, events):
        actions = []
        for e in events:
            if e.type == pygame.QUIT:
                actions.append("quit")
            elif e.type == pygame.KEYDOWN and e.key in KEYMAP:
                actions.append(KEYMAP[e.key])
        # GPIO HOOK: read your button pins here and append the same action
        # names ("up", "down", "select", "back", ...).
        return actions

    def read_actions(self):
        return self.actions_from(pygame.event.get())

    def handle(self, a):
        self.dirty = True
        if self.state == "menu":
            if a == "up":
                self.sel = (self.sel - 1) % len(self.items)
            elif a == "down":
                self.sel = (self.sel + 1) % len(self.items)
            elif a == "select":
                self.open(MENU[self.items[self.sel]][2])
        elif self.state == "pvp":
            if a == "select":
                self.player = 2 if self.player == 1 else 1
            elif a == "back":
                self.state = "menu"
        elif self.state == "tbd":
            if a in ("select", "back"):
                self.state = "menu"
        elif self.state == "options":
            if a == "up":
                self.opt_sel = (self.opt_sel - 1) % len(OPTIONS)
            elif a == "down":
                self.opt_sel = (self.opt_sel + 1) % len(OPTIONS)
            elif a == "select":
                self.state = OPTIONS[self.opt_sel][1]
            elif a == "back":
                self.state = "menu"
        elif self.state == "shutdown":
            if a == "select":
                self.shutting_down = True
            elif a == "back":
                self.state = "options"
        elif self.state == "stats":
            if a == "clear":
                if self.confirm_clear:
                    self.stats = empty_stats()
                    save_stats(self.stats)
                    self.confirm_clear = False
                else:
                    self.confirm_clear = True
                return
            self.confirm_clear = False
            if a in ("select", "back"):
                self.state = "menu"
            elif a == "test":
                self.add_test_game()

    def open(self, target):
        if target == "pvp":
            self.state, self.player = "pvp", 1
        elif target.startswith("roger_"):
            self.state, self.tbd = "tbd", ("VS R.O.G.E.R", target[6:].capitalize(), "TBD")
        elif target == "options":
            self.state, self.opt_sel = "options", 0
        elif target == "stats":
            self.state, self.confirm_clear = "stats", False

    def add_test_game(self):
        if random.random() < 0.75:
            result = random.choices(["win", "loss", "draw"], [45, 45, 10])[0]
            record_roger_game(self.stats, random.choice(DIFFICULTIES), result)
        else:
            record_pvp_game(self.stats, random.choices(["p1", "p2", "draw"], [45, 45, 10])[0])

    # ---- drawing -------------------------------------------------------------
    def text(self, s, font, color, y, center=None, left=None, right=None):
        surf = self.fonts[font].render(s, True, color)
        if center is not None:
            x = center - surf.get_width() // 2
        elif right is not None:
            x = right - surf.get_width()
        else:
            x = left
        self.canvas.blit(surf, (x, y))
        return surf.get_height()

    def hint(self, s):
        self.text(s, "small", DIM, self.ph - 40, center=self.pw // 2)

    def draw(self):
        self.canvas.fill(BLACK)
        getattr(self, "draw_" + self.state)()
        out = pygame.transform.rotate(self.canvas, self.rotate) if self.rotate else self.canvas
        self.screen.blit(out, (0, 0))
        pygame.display.flip()

    def menu_height(self):
        L, h = self.L, 0
        for entry in MENU:
            h += {"gap": L["gap"], "label": L["label_step"]}.get(entry[0], L["item_step"])
        return h

    def draw_menu(self):
        L = self.L
        cx = L["menu_cx"]
        if self.wide:
            if TITLE:
                th = self.fonts["title"].size(TITLE)[1]
                self.text(TITLE, "title", WHITE, (self.ph - 40 - th) // 2, center=L["title_cx"])
            y = max(10, (self.ph - 40 - self.menu_height()) // 2)
        else:
            y = 60
            if TITLE:
                self.text(TITLE, "title", WHITE, y, center=cx)
                y += 120
        selected = self.items[self.sel]
        for i, entry in enumerate(MENU):
            kind = entry[0]
            if kind == "gap":
                y += L["gap"]
            elif kind == "label":
                self.text(entry[1], "label", GRAY, y, center=cx)
                y += L["label_step"]
            else:
                self.draw_item(y, entry[1], i == selected, cx)
                y += L["item_step"]
        self.hint("Up/Down: move    Enter: select")

    def draw_item(self, y, label, selected, cx=None):
        cx = self.pw // 2 if cx is None else cx
        box_h, box_w = self.L["box_h"], self.L["box_w"]
        color = WHITE
        if selected:
            pygame.draw.rect(self.canvas, WHITE, (cx - box_w // 2, y, box_w, box_h), border_radius=10)
            color = BLACK
        th = self.fonts["item"].size(label)[1]
        self.text(label, "item", color, y + (box_h - th) // 2, center=cx)

    def draw_options(self):
        cx = self.pw // 2
        self.text("Options", "big", WHITE, 40 if self.wide else 60, center=cx)
        y = 140 if self.wide else 200
        for i, (label, _) in enumerate(OPTIONS):
            self.draw_item(y, label, i == self.opt_sel)
            y += self.L["item_step"]
        self.text("More options TBD", "label", DIM, y + 20, center=cx)
        self.hint("Up/Down: move    Enter: select    Esc: back")

    def draw_shutdown(self):
        cx = self.pw // 2
        top = 110 if self.wide else 260
        if self.shutting_down:
            self.text("Shutting down...", "big", WHITE, top + 40, center=cx)
            self.text("Wait for the Pi's green light to stop", "label", GRAY, top + 130, center=cx)
            self.text("blinking before unplugging.", "label", GRAY, top + 165, center=cx)
            return
        self.text("Shut down?", "big", WHITE, top + 40, center=cx)
        self.text("Always shut down before unplugging,", "label", GRAY, top + 130, center=cx)
        self.text("or the SD card can get corrupted.", "label", GRAY, top + 165, center=cx)
        self.hint("Enter: shut down    Esc: cancel")

    def do_shutdown(self):
        """Show the message, close the screen, and power the Pi off."""
        self.draw()
        pygame.time.wait(1500)
        pygame.quit()
        try:
            subprocess.run(["sudo", "-n", "shutdown", "-h", "now"], check=True)
        except (OSError, subprocess.CalledProcessError) as err:
            print(f"Could not shut down ({err}). Run: sudo shutdown now")
        raise SystemExit(0)
    def draw_pvp(self):
        cx = self.pw // 2
        color = RED if self.player == 1 else BLUE
        name = "Player One's" if self.player == 1 else "Player Two's"
        if self.wide:
            pygame.draw.circle(self.canvas, color, (self.pw // 4 + 20, self.ph // 2 - 20), 110)
            tx = self.pw * 5 // 8 + 20
            self.text(name, "big", color, self.ph // 2 - 75, center=tx)
            self.text("turn", "big", color, self.ph // 2 - 5, center=tx)
        else:
            pygame.draw.circle(self.canvas, color, (cx, 250), 90)
            self.text(name, "big", color, 400, center=cx)
            self.text("turn", "big", color, 470, center=cx)
        self.hint("Enter: next turn    Esc: menu")

    def draw_tbd(self):
        cx = self.pw // 2
        sub, title, big = self.tbd
        top = 100 if self.wide else 230
        if sub:
            self.text(sub, "label", GRAY, top, center=cx)
        self.text(title, "big", WHITE, top + 40, center=cx)
        if big:
            self.text(big, "huge", WHITE, top + 150, center=cx)
        self.hint("Esc: back")

    def stat_cols(self):
        """Column positions for the R.O.G.E.R table: label x, W, L, D, Win% right edge."""
        if self.wide:
            return 30, 185, 230, 275, self.pw // 2 - 20
        return 30, 250, 310, 370, self.pw - 25

    def stat_row(self, y, label, rec, color=WHITE):
        lx, wx, lsx, dx, rr = self.stat_cols()
        g = rec["wins"] + rec["losses"] + rec["draws"]
        self.text(label, "table", color, y, left=lx)
        for key, x in (("wins", wx), ("losses", lsx), ("draws", dx)):
            self.text(str(rec[key]), "table", color, y, center=x)
        rate = f"{100 * rec['wins'] / g:.0f}%" if g else "-"
        self.text(rate, "table", color, y, right=rr)

    def value_row(self, y, label, value, left=30, right=None):
        self.text(label, "table", WHITE, y, left=left)
        self.text(str(value), "table", WHITE, y, right=self.pw - 25 if right is None else right)

    def draw_stats(self):
        cx = self.pw // 2
        s = self.stats
        wide = self.wide
        self.text("Statistics", "big", WHITE, 20 if wide else 40, center=cx)
        lx, wx, lsx, dx, rr = self.stat_cols()

        # R.O.G.E.R record (left half when wide)
        y = 100 if wide else 130
        self.text("R.O.G.E.R record", "label", GRAY, y, center=self.pw // 4 if wide else cx)
        y += 45
        for h, x in (("W", wx), ("L", lsx), ("D", dx)):
            self.text(h, "table", GRAY, y, center=x)
        self.text("Win %", "table", GRAY, y, right=rr)
        y += 40
        for d in DIFFICULTIES:
            self.stat_row(y, d.capitalize(), s["roger"][d])
            y += 42
        pygame.draw.line(self.canvas, DIM, (25, y + 2), (rr, y + 2), 1)
        y += 12
        totals = roger_totals(s)
        self.stat_row(y, "Overall", totals)

        # Player vs Player (right half when wide, underneath when tall)
        p = s["pvp"]
        if wide:
            pygame.draw.line(self.canvas, DIM, (cx, 100), (cx, self.ph - 70), 1)
            left, right, y = cx + 30, self.pw - 30, 100
            self.text("Player vs Player", "label", GRAY, y, center=self.pw * 3 // 4)
            y += 45
        else:
            left, right = 30, self.pw - 25
            y += 60
            self.text("Player vs Player", "label", GRAY, y, center=cx)
            y += 44
        for label, value in (("Games", p["games"]), ("Player One wins", p["p1_wins"]),
                             ("Player Two wins", p["p2_wins"]), ("Draws", p["draws"])):
            self.value_row(y, label, value, left, right)
            y += 40
        y += 10
        roger_games = totals["wins"] + totals["losses"] + totals["draws"]
        self.value_row(y, "Total games played", roger_games + p["games"], left, right)

        if self.confirm_clear:
            self.text("Press C again to clear all stats", "small", RED, self.ph - 75, center=cx)
        self.hint("Esc: back    T: test game    C: clear")

    # ---- GIF test ------------------------------------------------------------
    def run_gif_test(self):
        """Hand the screen to the GIF player until Esc, then take it back."""
        try:
            if self.face is None:
                self.face = FacePlayer(rotate=self.rotate)  # loads every face once
                self.face.show_name = True
                self.face.show_fps = True
                self.face.hold_on_edit_exit = True
                self.face.help_line = "Enter: next   Esc: back"
            else:
                self.face.attach()
        except SystemExit as err:
            print(err)
            self.screen, _ = set_canvas(SCREEN)
            self.state, self.tbd = "tbd", ("GIF test", "No GIFs found", "")
            self.dirty = True
            return

        face = self.face
        face.play_clip(0)
        while face.update():          # Esc or Q returns False -> back to Options
            if face.editing:
                continue              # arrows and Enter belong to the editor
            for a in self.actions_from(face.events):
                i = face.clips.index(face.clip)
                if a in ("select", "down", "right"):
                    face.play_clip(i + 1)
                elif a in ("up", "left"):
                    face.play_clip(i - 1)

        self.screen, _ = set_canvas(SCREEN)
        self.state = "options"
        self.dirty = True

    # ---- main loop -----------------------------------------------------------
    def run(self):
        clock = pygame.time.Clock()
        try:
            while True:
                if self.state == "gif_test":
                    self.run_gif_test()
                    continue
                for a in self.read_actions():
                    if a == "quit":
                        return
                    self.handle(a)
                if self.state == "gif_test":
                    continue  # hand the screen straight to the GIF player
                if self.shutting_down:
                    self.do_shutdown()
                if self.dirty:
                    self.draw()
                    self.dirty = False
                clock.tick(30)
        except KeyboardInterrupt:
            pass
        finally:
            pygame.quit()


def main():
    ap = argparse.ArgumentParser(description="Connect 4 menu shell")
    ap.add_argument("--rotate", type=int, default=ROTATE, choices=[0, 90, 180, 270],
                    help="0 wide (default), 180 wide upside down, 90 or 270 tall")
    args = ap.parse_args()
    App(args.rotate).run()


if __name__ == "__main__":
    main()
