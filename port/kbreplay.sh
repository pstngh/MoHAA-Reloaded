#!/bin/bash
# The keyboard and mouse replay checks (Phase 6c, 6d): the same key or mouse events, posted into the game's own event
# queue (in-process NSEvents: nothing system-wide, no synthetic HID input), must give the same SE_KEY/SE_CHAR/
# SE_MOUSE stream through SDL and through the native AppKit layer. Two scratch flavors plus port/kbreplay_patch.py
# (the shipped flavors never have the replay): kbnat from the work tree (exported into build.noindex/kbreplay-src),
# kbsdl from the last commit with SDL (port/build.sh's SDL_REF, the sdlref flavor's code; exported into
# build.noindex/kbsdl-src), so the native layer stays checked against SDL after SDL left the tree (Phase 6g):
#   port/kbreplay.sh build             kbsdl (SDL, the sdlref flavor's input) and kbnat (native, the metal flavor's)
#   port/kbreplay.sh run               the keys: both in dm/mohdm6 in a 1280x720 window, one after the other, then
#                                      the diff
#   port/kbreplay.sh mouse <mode>      the mouse (msreplay), the same way; <mode>: win (a 1280x720 window on the
#                                      main display), full (exclusive fullscreen at 1920x1080), hidpi (a window on
#                                      r_displayIndex 1, a 2x display: the UI's cursor is in pixels on the native
#                                      layer, in points on SDL, so those lines differ)
#   port/kbreplay.sh dialog <mode>     Sys_Dialog (dlgtest, Phase 6e) with the game up and the mouse captured: what
#                                      covers the dialog, the cursor, the display; the logs side by side, no verdict
#                                      (the layers differ on purpose). <mode>: win (a 1280x720 window), full
#                                      (exclusive fullscreen at the desktop's 1920x1080), full720 (exclusive at
#                                      1280x720: a mode switch until session 80, now scaled to the screen), space
#                                      (borderless fullscreen, a Space)
# The runs set ui_console 1: without it MOHAA's console key opens nothing. Each run opens the game for ~15 s and
# takes the keyboard focus: don't type meanwhile. The mouse runs capture the real cursor (warped to the window's
# center and hidden; fullscreen takes the screen): don't touch the mouse either. The logs and the diff are in
# build.noindex/kbreplay/ (kbreplay-<flavor>[-mouse-<mode>].log: the steps and the events each one queued).
set -euo pipefail
source "$(dirname "$0")/env.sh"

OUT="$OMM_BUILD/kbreplay"
mkdir -p "$OUT"

build() {
	local src="$OMM_BUILD/kbreplay-src" sdlsrc="$OMM_BUILD/kbsdl-src"
	local sdlref
	sdlref="$(sed -n 's/^SDL_REF=//p' "$OMM_PORT/build.sh")"
	rm -rf "$src" "$sdlsrc"
	mkdir -p "$src" "$sdlsrc"
	git -C "$OMM_ROOT" archive HEAD | tar -x -C "$src"
	git -C "$OMM_ROOT" diff --name-only HEAD | while read -r f; do
		if [ -f "$OMM_ROOT/$f" ]; then cp "$OMM_ROOT/$f" "$src/$f"; else rm -f "$src/$f"; fi
	done
	git -C "$OMM_ROOT" archive "$sdlref" | tar -x -C "$sdlsrc"
	python3 -I "$OMM_PORT/kbreplay_patch.py" "$src"
	python3 -I "$OMM_PORT/kbreplay_patch.py" "$sdlsrc"

	build_flavor kbsdl "$sdlsrc" -DUSE_SDL=ON
	build_flavor kbnat "$src"
}

# build_flavor <flavor> <tree> <cmake args>: one scratch flavor from one of build's trees
build_flavor() {
	local flavor=$1 tree=$2
	shift 2
	[ -d "$tree" ] || { echo "kbreplay.sh: run port/kbreplay.sh build first" >&2; exit 1; }
	# a build tree configured from another source tree (kbsdl before 6g, from kbreplay-src) can't be reused
	if [ -f "$OMM_BUILD/$flavor/CMakeCache.txt" ] &&
		! grep -qxF "CMAKE_HOME_DIRECTORY:INTERNAL=$tree" "$OMM_BUILD/$flavor/CMakeCache.txt"; then
		rm -rf "$OMM_BUILD/$flavor"
	fi
	export GIT_CEILING_DIRECTORIES="$OMM_BUILD"
	omm_mount_metal
	"$OMM_CMAKE" -S "$tree" -B "$OMM_BUILD/$flavor" -G Ninja \
		-DCMAKE_MAKE_PROGRAM="$OMM_NINJA" \
		-DBISON_EXECUTABLE="$OMM_BISON" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$OMM_BUILD/$flavor-install" \
		-DCMAKE_DISABLE_FIND_PACKAGE_OpenAL=ON \
		${1+"$@"} >/dev/null
	"$OMM_CMAKE" --build "$OMM_BUILD/$flavor" -j"$(sysctl -n hw.ncpu)"
	"$OMM_CMAKE" --install "$OMM_BUILD/$flavor" >/dev/null
	echo "kbreplay.sh: $flavor installed in $OMM_BUILD/$flavor-install"
}

# game <flavor> <command> <name> [engine args]: one run of `<command> <name>` in dm/mohdm6 in a 1280x720 window
# (unless the engine args say otherwise); its log goes to $OUT/kbreplay-<name>.log
game() {
	local flavor=$1 command=$2 name=$3 log="kbreplay-$3.log"
	shift 3
	# the same start for every run: no saved config, the run's name in the cfg
	rm -rf "$OMM_HOME/main/configs"
	printf 'wait 3000\nfinishloadingscreen\nwait 500\n%s %s\n' "$command" "$name" > "$OMM_HOME/main/kb_replay.cfg"
	rm -f "$OMM_HOME/$log"
	perl -e 'alarm shift; exec @ARGV' 120 "$OMM_PORT/run.sh" "$flavor" \
		+set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 +set r_fullscreen 0 \
		+set com_maxfps 250 +set com_maxfpsUnfocused 0 +set s_volume 0 +set s_musicvolume 0 \
		+set g_gametype 0 +set thereisnomonkey 1 +set ui_console 1 "$@" +devmap dm/mohdm6 +exec kb_replay.cfg \
		> "$OUT/$name-game.log" 2>&1 || echo "kbreplay.sh: $flavor exited with $?"
	rm -f "$OMM_HOME/main/kb_replay.cfg"
	cp "$OMM_HOME/$log" "$OUT/$log"
}

# replay <command> <label> [engine args]: runs `<command> <flavor><label>` in both flavors, then the diff
replay() {
	local command=$1 label=$2 flavor log
	shift 2
	for flavor in kbsdl kbnat; do
		log="kbreplay-$flavor$label.log"
		game "$flavor" "$command" "$flavor$label" "$@"
		echo "kbreplay.sh: $flavor$label: $(grep -c '^step' "$OUT/$log") steps," \
			"$(grep -c 'SE_' "$OUT/$log") events, quit: $(grep -c 'Closed window' "$OUT/$flavor$label-game.log")" \
			"'Closed window' line(s)"
	done
	# The header line names the flavor and the wait for the game depends on the load: the steps must match up to
	# the input shutdown at quit. After it, the native layer queues the release of the keys and buttons still
	# down (Cmd, Q, a mouse button), which SDL lost: shown, not compared.
	if diff <(sed -e '1d' -e '/^input shutdown/,$d' "$OUT/kbreplay-kbsdl$label.log" | grep -v '^waiting') \
		<(sed -e '1d' -e '/^input shutdown/,$d' "$OUT/kbreplay-kbnat$label.log" | grep -v '^waiting') \
		> "$OUT/diff$label.txt"; then
		echo "kbreplay.sh: the streams are identical"
	else
		echo "kbreplay.sh: the streams differ ($OUT/diff$label.txt):"
		head -60 "$OUT/diff$label.txt"
	fi
	for flavor in kbsdl kbnat; do
		echo "kbreplay.sh: $flavor$label after the input shutdown:" \
			$(sed -n '/^input shutdown/,$p' "$OUT/kbreplay-$flavor$label.log" | tail -n +2)
	done
}

run() {
	replay kbreplay "" +set in_mouse 0 "$@"
}

mouse() {
	local mode=${1:-}
	shift || true
	case "$mode" in
		win) replay msreplay -mouse-win +set in_mouse 1 +set in_mouseRaw 0 "$@" ;;
		full) replay msreplay -mouse-full +set in_mouse 1 +set in_mouseRaw 0 +set r_customwidth 1920 \
			+set r_customheight 1080 +set r_fullscreen 1 "$@" ;;
		hidpi) replay msreplay -mouse-hidpi +set in_mouse 1 +set in_mouseRaw 0 +set r_displayIndex 1 "$@" ;;
		*) echo "usage: port/kbreplay.sh mouse win|full|hidpi [engine args]" >&2; exit 1 ;;
	esac
}

dialog() {
	local mode=${1:-} flavor args
	shift || true
	case "$mode" in
		win) args=() ;;
		full) args=(+set r_customwidth 1920 +set r_customheight 1080 +set r_fullscreen 1) ;;
		full720) args=(+set r_fullscreen 1) ;;
		space) args=(+set r_customwidth 1920 +set r_customheight 1080 +set r_fullscreen 1 +set r_noborder 1) ;;
		*) echo "usage: port/kbreplay.sh dialog win|full|full720|space [engine args]" >&2; exit 1 ;;
	esac
	for flavor in kbsdl kbnat; do
		game "$flavor" dlgtest "$flavor-dialog-$mode" +set in_mouse 1 +set in_mouseRaw 0 ${args[@]+"${args[@]}"} "$@"
		echo "kbreplay.sh: $flavor-dialog-$mode:"
		cat "$OUT/kbreplay-$flavor-dialog-$mode.log"
	done
}

case "${1:-}" in
	build) build ;;
	run) shift; run "$@" ;;
	mouse) shift; mouse "$@" ;;
	dialog) shift; dialog "$@" ;;
	*) echo "usage: port/kbreplay.sh build | run [engine args] | mouse win|full|hidpi [engine args] |" \
		"dialog win|full|full720|space [engine args]" >&2; exit 1 ;;
esac
