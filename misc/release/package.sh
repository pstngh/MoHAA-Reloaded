#!/bin/bash
#
# The v1.0.0 release's Linux and Windows downloads from CI's build artifacts (out-linux-amd64, out-windows-x64):
# the build's files without the dedicated server, plus COPYING.txt, this folder's INSTALL-<platform>.txt as INSTALL.txt (with the commit) and
# this folder's licenses/ (licenses-linux/ too on Linux: wolfSSL is only in the Linux build). The download must hold
# exactly the files in files-<platform>.txt, so a build that adds or drops a file stops here instead of shipping.
#
#   misc/release/package.sh <linux-amd64|windows-x64> <artifact dir> <output dir> <commit>
#
# Prints the archive's path. .github/workflows/branches-build.yml runs it for each push to vulkan that changes code.
#
set -euo pipefail

die() { echo "package.sh: $*" >&2; exit 1; }

[ $# -eq 4 ] || die "usage: package.sh <linux-amd64|windows-x64> <artifact dir> <output dir> <commit>"
platform=$1
artifact=$2
out=$3
commit=$4

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
name="openmohaa-vulkan-1.0.0-$platform"
stage="$out/$name"

[[ $commit =~ ^[0-9a-f]{40}$ ]] || die "not a full commit ID: $commit"
[ -e "$artifact/openmohaa" ] || [ -e "$artifact/openmohaa.exe" ] || die "$artifact doesn't hold the game"

rm -rf "$stage"
mkdir -p "$stage"
cp -R "$artifact/." "$stage/"
# the downloads are the client only, as the release notes say: no dedicated server
rm -f "$stage/omohaaded" "$stage/omohaaded.exe"
cp "$root/COPYING.txt" "$stage/"
cp -R "$here/licenses" "$stage/"

case $platform in
linux-amd64)
	cp -R "$here/licenses-linux/." "$stage/licenses/"
	sed "s/@COMMIT@/$commit/" "$here/INSTALL-$platform.txt" > "$stage/INSTALL.txt"
	;;
windows-x64)
	# Notepad's line endings
	sed "s/@COMMIT@/$commit/; s/\$/\r/" "$here/INSTALL-$platform.txt" > "$stage/INSTALL.txt"
	;;
*)
	die "unknown platform: $platform"
	;;
esac

diff <(cd "$stage" && find . -mindepth 1 | sed 's#^\./##' | LC_ALL=C sort) \
	<(LC_ALL=C sort "$here/files-$platform.txt") \
	|| die "$name doesn't hold the files in files-$platform.txt (< the build's, > the list's)"

# The artifact's zip lost the permissions: folders and files readable, the programs executable
find "$stage" -type d -exec chmod 755 {} +
find "$stage" -type f -exec chmod 644 {} +
if [ "$platform" = linux-amd64 ]; then
	chmod 755 "$stage/openmohaa" "$stage/mohlauncher" "$stage"/launch_openmohaa_*
	archive="$out/$name.tar.gz"
	rm -f "$archive"
	tar --sort=name --owner=0 --group=0 --numeric-owner -czf "$archive" -C "$out" "$name"
else
	archive="$out/$name.zip"
	rm -f "$archive"
	(cd "$out" && zip -q -X -9 -r "$name.zip" "$name")
fi

echo "$archive"
