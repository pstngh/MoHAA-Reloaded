#!/bin/sh
# Renders the launchers' PNG icons from the SVGs beside this script. The PNGs are committed, so building a launcher
# needs no SVG renderer; run this again after an SVG changes. Needs rsvg-convert (librsvg).
#   game-aa.svg, game-sh.svg, game-bt.svg: the games' icons (the main branch's icons/)
#   app-icon.svg: the launcher's icon, the game's gold star (the metal branch's misc/macos/openmohaa-metal.svg)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
for game in aa sh bt; do
	for size in 32 64; do
		rsvg-convert -w "$size" -h "$size" -o "$here/game-$game-$size.png" "$here/game-$game.svg"
	done
done
rsvg-convert -w 128 -h 128 -o "$here/app-icon-128.png" "$here/app-icon.svg"
