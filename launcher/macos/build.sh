#!/bin/sh
# Builds "MoHAA Launcher.app" (arm64, signed ad hoc) into the folder given, launcher/macos/build by default.
#   launcher/macos/build.sh [out-folder]
# Needs Xcode (or its command line tools) on macOS 14 or later. `swift test` here checks the logic.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-"$here/build"}
assets="$here/../assets"

cd "$here"
swift build -c release --arch arm64
bin=$(swift build -c release --arch arm64 --show-bin-path)

app="$out/MoHAA Launcher.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$bin/MoHAALauncher" "$app/Contents/MacOS/"
cp "$here/Resources/Info.plist" "$app/Contents/"
cp "$here/Resources/AppIcon.icns" "$app/Contents/Resources/"
# the games' icons at 1x and 2x, for NSImage(named:)
for game in aa sh bt; do
	cp "$assets/game-$game-32.png" "$app/Contents/Resources/game-$game.png"
	cp "$assets/game-$game-64.png" "$app/Contents/Resources/game-$game@2x.png"
done

codesign --force --sign - "$app"
codesign --verify --deep --strict "$app"
echo "$app"
