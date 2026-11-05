#!/usr/bin/env python3
"""Writes a deterministic screenshot script for the game.

  shots.py <view list> [cfg name]

The list file has one directive per line ('#' starts a comment):

  map <name>                     map to load with devmap (first line)
  spmap <name>                   same with spdevmap: single player game type
                                 and one client, which the `script` console
                                 command needs (e.g. to give an entity a light)
  set <console command>          console command run before the next shot
  shot <name> x y z pitch yaw roll [wait]
  menu <name> [<menu> [wait]]    screenshot of a UI menu (see below)
  mask <name> x,y,w,h            for compare.sh: a rectangle of the 640x480
                                 UI screen left out of the comparison of
                                 shot <name> (scaled to the frame size)

Each shot teleports the player (noclip, god mode, no HUD, no view weapon) to
the position, sets the view angles, waits 2 s of game time (or [wait]
milliseconds, e.g. to catch a fade), pauses the game and takes the
screenshot <name> (`screenshot screenshots/<name>.tga`: the game writes
TGAs for a path, PNGs for a bare name). The game runs with `fixedtime 16`: every
frame advances the game by 16 ms whatever the real frame time, and `wait`
counts game milliseconds, so every run reaches each screenshot after the
same number of frames at the same game time (sway, animated shaders, sky,
camera). The pause matters too: the screenshot command reads the frame
before it is drawn, the last presented one; while paused all frames are
the same.

A menu shot pushes the menu (`pushmenu <menu>`; none, or `-`, for the
screen as it is), waits 1 s (or [wait] milliseconds), takes the
screenshot <name> and pops the menu. Before the first map it is taken over
the main menu (the first one waits 6 s for the game to start); after a
map, over the view of the previous shot, with the game paused like a shot.
Menus are 2D and drawn every frame the same way, but for what they animate
with the wall clock (the ceiling fan of the main menu's room): mask it.

r_showcluster 1 prints the view cluster whenever it changes and every shot
is announced with `echo shot <name>`, so compare.sh can warn about shots
taken from outside the world (cluster -1): there, parts of the frame are
covered by no surface and keep whatever the buffer held before (the
previous frame), so they differ between builds and runs without meaning
anything.

The script goes to $MOH_HOMEPATH/main/<cfg name>.cfg (default homepath
/tmp/moh-vktest, default cfg name: the list file name without extension).
Don't use the name of a cfg the game runs itself, like menu.cfg.
"""

import os
import sys

HOMEPATH = os.environ.get("MOH_HOMEPATH", "/tmp/moh-vktest")


def main():
    src = sys.argv[1]
    name = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(os.path.basename(src))[0]
    # thereisnomonkey: without it the game turns cheats off on the first
    # cheat command (tele, noclip, ...). The screenshot command centerprints
    # "Wrote ..." on the screen; a blank centerprint replaces it.
    out = ["set thereisnomonkey 1", "set fixedtime 16"]
    started = False
    menus = 0
    for raw in open(src):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        cmd, _, rest = line.partition(" ")
        if cmd in ("map", "spmap"):
            out += [f"{'devmap' if cmd == 'map' else 'spdevmap'} {rest}", "wait 3000", "finishloadingscreen", "wait 1000",
                    "god", "notarget", "noclip", "ui_hud 0", "cg_drawviewmodel 0", "r_showcluster 1"]
            started = True
        elif cmd == "set":
            out.append(rest)
        elif cmd == "shot":
            if not started:
                sys.exit("shot before map")
            parts = rest.split()
            shot, pos, ang = parts[0], parts[1:4], parts[4:7]
            wait = parts[7] if len(parts) > 7 else "2000"
            out += [f"tele {' '.join(pos)}", f"face {' '.join(ang)}", f"wait {wait}",
                    "pause", "wait 300", f"echo shot {shot}", f"screenshot screenshots/{shot}.tga", "wait 100", 'centerprint " "',
                    "pause", "wait 100"]
        elif cmd == "menu":
            parts = rest.split()
            shot = parts[0]
            menu = parts[1] if len(parts) > 1 and parts[1] != "-" else None
            wait = parts[2] if len(parts) > 2 else "1000"
            push = [f"pushmenu {menu}"] if menu else []
            pop = ["popmenu 0"] if menu else []
            if started:
                out += ["pause", "wait 300"] + push + [f"wait {wait}", f"echo shot {shot}", f"screenshot screenshots/{shot}.tga",
                        "wait 100"] + pop + ['centerprint " "', "pause", "wait 100"]
            else:
                out += ([] if menus else ["wait 6000"]) + push + [f"wait {wait}", f"echo shot {shot}",
                        f"screenshot screenshots/{shot}.tga", "wait 300"] + pop + ["wait 300"]
            menus += 1
        elif cmd == "mask":
            pass
        else:
            sys.exit(f"unknown directive: {line}")
    out.append("quit")
    os.makedirs(os.path.join(HOMEPATH, "main"), exist_ok=True)
    path = os.path.join(HOMEPATH, "main", name + ".cfg")
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")
    print(path)


if __name__ == "__main__":
    main()
