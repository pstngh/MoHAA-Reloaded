#!/bin/sh
# Renders the launcher's PNG icons from the repository's SVGs (icons/). The PNGs are committed, so building a
# launcher needs no SVG renderer; run this again after an SVG changes. Needs rsvg-convert (librsvg).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
icons="$here/../../icons"
for size in 32 64 128; do
	rsvg-convert -w "$size" -h "$size" -o "$here/game-aa-$size.png" "$icons/mohaa.svg"
	rsvg-convert -w "$size" -h "$size" -o "$here/game-sh-$size.png" "$icons/mohaa-spearhead.svg"
	rsvg-convert -w "$size" -h "$size" -o "$here/game-bt-$size.png" "$icons/mohaa-breakthrough.svg"
done
# The app icon: the game's gold star (the metal branch's misc/macos/openmohaa-metal.svg)
for size in 64 128; do
	rsvg-convert -w "$size" -h "$size" -o "$here/app-icon-$size.png" "$here/app-icon.svg"
done
