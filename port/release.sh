#!/bin/bash
# The release zip, ad-hoc signed (no Developer ID, no notarization): openmohaa-metal-<version>-macos-arm64.zip from
# an installed build, with the sealed openmohaa.app (game.dylib and cgame.dylib inside it), MoHAA Launcher's
# mohlauncher.app (launcher/macos, built here), COPYING.txt, the licenses of the libraries built into it, the install
# text (port/release/INSTALL.txt) and the source commit. Never game data.
#   port/release.sh [install dir] [output dir]
# The install dir is the one that holds openmohaa.app: `cmake --install`'s <prefix>/lib/openmohaa (CI,
# .github/workflows/macos-metal.yml), default build.noindex/metal-install/lib/openmohaa (port/build.sh metal). The
# output dir (default build.noindex/release) gets the zip and its .sha256. The version is the app's
# CFBundleShortVersionString (PORT_RELEASE_VERSION, cmake/identity.cmake). The build must be of HEAD in a clean tree.
# Needs no port/env.sh, so CI runs it as is.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INSTALL="${1:-$ROOT/build.noindex/metal-install/lib/openmohaa}"
OUT="${2:-$ROOT/build.noindex/release}"
APP="$INSTALL/openmohaa.app"
# The libraries built into the binaries and their license texts in code/thirdparty: <name in the zip>:<file>
LICENSES=(
	libmad:libmad/COPYING libmad:libmad/COPYRIGHT
	jpeg:jpeg-9f/README
	libogg:libogg-1.3.6/COPYING
	libvorbis:libvorbis-1.3.7/COPYING
	opus:opus-1.5.2/COPYING
	opusfile:opusfile-0.12/COPYING
	zlib:zlib-1.3.1/LICENSE
	recastnavigation:recastnavigation/License.txt
)

die() { echo "release.sh: $*" >&2; exit 1; }

for f in "$APP" "$APP/Contents/MacOS/game.dylib" "$APP/Contents/MacOS/cgame.dylib"; do
	[ -e "$f" ] || die "$f is missing"
done
codesign --verify --deep --strict "$APP" || die "openmohaa.app's seal doesn't verify"

# The source commit: the tree must be clean, and the binaries must say they were built from it (version.cmake puts
# the abbreviated hash into the version string at configure time).
git -C "$ROOT" diff --quiet HEAD -- || die "the work tree has changes: commit them and rebuild"
COMMIT="$(git -C "$ROOT" rev-parse HEAD)"
BUILT="$(strings "$APP/Contents/MacOS/openmohaa" | sed -n 's/^OpenMoHAA [^ ]*+[0-9]*\.\([0-9a-f]*\) .*/\1/p')"
BUILT="${BUILT%%$'\n'*}"
[ -n "$BUILT" ] && [ "${COMMIT#"$BUILT"}" != "$COMMIT" ] || die "openmohaa.app was built from '${BUILT:-?}', not HEAD ($COMMIT): rebuild"

VERSION="$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$APP/Contents/Info.plist")"
NAME="openmohaa-metal-$VERSION-macos-arm64"
STAGE="$OUT/$NAME"
ZIP="$OUT/$NAME.zip"

rm -rf "$STAGE" "$ZIP" "$ZIP.sha256"
mkdir -p "$STAGE/licenses"
ditto "$APP" "$STAGE/openmohaa.app"
# MoHAA Launcher, beside the game: built and signed ad hoc here (Swift's build files in the output dir)
MOHL_SCRATCH="$OUT/mohlauncher-build" "$ROOT/launcher/macos/bundle.sh" "$STAGE" > /dev/null
cp "$ROOT/COPYING.txt" "$STAGE/"
for l in "${LICENSES[@]}"; do
	mkdir -p "$STAGE/licenses/${l%%:*}"
	cp "$ROOT/code/thirdparty/${l#*:}" "$STAGE/licenses/${l%%:*}/"
done
sed -e "s/@VERSION@/$VERSION/g" -e "s/@COMMIT@/$COMMIT/g" "$ROOT/port/release/INSTALL.txt" > "$STAGE/INSTALL.txt"
! grep -q '@[A-Z]*@' "$STAGE/INSTALL.txt" || die "INSTALL.txt has an unfilled placeholder"
codesign --verify --deep --strict "$STAGE/openmohaa.app" || die "the staged openmohaa.app's seal doesn't verify"
codesign --verify --deep --strict "$STAGE/mohlauncher.app" || die "the staged mohlauncher.app's seal doesn't verify"

# --norsrc: no extended attributes in the zip (ditto would store them as ._ files, such as this machine's
# com.apple.provenance, which xattr can't remove); the code signatures don't use any
ditto -c -k --norsrc --keepParent "$STAGE" "$ZIP"
! zipinfo -1 "$ZIP" | grep '/\._' || die "the zip holds AppleDouble ._ files"
(cd "$OUT" && shasum -a 256 "$NAME.zip" > "$NAME.zip.sha256")

# Unzip copies as players would (Archive Utility's ditto, and unzip) and check them: the seal, exactly the
# expected files, and the executables still executable
CHECK="$OUT/check-$NAME"
rm -rf "$CHECK"
mkdir -p "$CHECK/ditto" "$CHECK/unzip"
ditto -x -k "$ZIP" "$CHECK/ditto"
unzip -q "$ZIP" -d "$CHECK/unzip"
for c in ditto unzip; do
	codesign --verify --deep --strict "$CHECK/$c/$NAME/openmohaa.app" || die "openmohaa.app unzipped by $c: its seal doesn't verify"
	codesign --verify --deep --strict "$CHECK/$c/$NAME/mohlauncher.app" || die "mohlauncher.app unzipped by $c: its seal doesn't verify"
	diff <(cd "$STAGE" && find . | sort) <(cd "$CHECK/$c/$NAME" && find . | sort) || die "unzipped by $c: the files differ"
	[ -x "$CHECK/$c/$NAME/openmohaa.app/Contents/MacOS/openmohaa" ] || die "unzipped by $c: not executable"
	[ -x "$CHECK/$c/$NAME/mohlauncher.app/Contents/MacOS/mohlauncher" ] || die "unzipped by $c: the launcher isn't executable"
done
rm -rf "$CHECK" "$STAGE"

echo "release.sh: $ZIP"
echo "release.sh: $(cut -d' ' -f1 "$ZIP.sha256")  version $VERSION, commit $COMMIT"
