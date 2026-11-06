#!/bin/sh
# Bundles "mohlauncher.app" (arm64, signed ad hoc) into the folder given, launcher/macos/build by default.
#   launcher/macos/bundle.sh [out-folder]
# MOHL_SCRATCH, if set, is the folder for Swift's build files (by default launcher/macos/.build).
# MOHL_TEST_HOOKS=1 builds in the updater's test hooks (Updating.swift); port/release.sh refuses such a launcher.
# Needs Xcode (or its command line tools) on macOS 14 or later. `swift test` here checks the logic.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-"$here/build"}
assets="$here/../assets"

cd "$here"
set -- -c release --arch arm64
if [ -n "${MOHL_SCRATCH:-}" ]; then
	set -- "$@" --scratch-path "$MOHL_SCRATCH"
fi
if [ "${MOHL_TEST_HOOKS:-}" = 1 ]; then
	set -- "$@" -Xswiftc -DMOHL_TEST_HOOKS
fi
swift build "$@"
bin=$(swift build "$@" --show-bin-path)

app="$out/mohlauncher.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$bin/mohlauncher" "$app/Contents/MacOS/"
cp "$here/Resources/Info.plist" "$app/Contents/"
# The git tree of launcher/, when no file in it has changed: the updater leaves a launcher of the same source alone
# (CI's builds differ byte for byte even from the same source). No stamp is never "the same".
root=$(git -C "$here" rev-parse --show-toplevel 2>/dev/null || true)
if [ -n "$root" ] && [ -z "$(git -C "$root" status --porcelain -- launcher)" ]; then
	source=$(git -C "$root" rev-parse HEAD:launcher)
	/usr/libexec/PlistBuddy -c "Add MOHLauncherSource string $source" "$app/Contents/Info.plist"
fi
cp "$here/Resources/AppIcon.icns" "$app/Contents/Resources/"
# the GitHub and Discord logos
cp "$assets/github.svg" "$assets/discord.svg" "$app/Contents/Resources/"
# the games' icons at 1x and 2x, for NSImage(named:)
for game in aa sh bt; do
	cp "$assets/game-$game-32.png" "$app/Contents/Resources/game-$game.png"
	cp "$assets/game-$game-64.png" "$app/Contents/Resources/game-$game@2x.png"
done

codesign --force --sign - "$app"
codesign --verify --deep --strict "$app"
echo "$app"
