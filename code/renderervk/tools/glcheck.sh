#!/bin/sh
#
# glcheck.sh [bin dir]
#
# No OpenGL left (README.md, "No OpenGL"), as the metal branch checks it
# (port/glcheck.sh there):
# 1. grep over code/, cmake/, .github/, misc/ and CMakeLists.txt for qgl,
#    opengl, SDL_GL_, GL_<name> and glimp (any case), minus the documented
#    exceptions below; every other match is printed. Markdown files are left
#    out: the READMEs keep the history of the port, GL1 comparisons included.
# 2. the binaries of the bin dir (default $MOH_BINDIR, see env.sh; a CI
#    artifact or an extracted download works too): no renderer but
#    renderer_*vulkan* and test copies (no renderer_opengl*), no OpenGL,
#    GLX, EGL or GLES library needed (readelf -d / objdump -p for .dll
#    and .exe) and no gl*, glX*, egl*, wgl* or SDL_GL_* import (nm -D).
# Exit status 0 = clean. Run it after upstream merges: a merge can bring GL
# code back.

TOOLS=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$TOOLS/../../.." && pwd)
BINDIR=${1:-${MOH_BINDIR:-$REPO/build-vk/RelWithDebInfo}}
PATTERN='qgl|opengl|SDL_GL_|GL_[A-Z0-9_]+|glimp'
status=0

cd "$REPO" || exit 1
hits=$(grep -rniIE "$PATTERN" code cmake .github misc CMakeLists.txt --exclude='*.md' --exclude-dir=__pycache__ | python3 -I -c '
import re, sys
pattern = re.compile(sys.argv[1], re.I)
# (file regex, allowed text regex): the allowed text is removed from the line
# before it is tested again
allowed = [
	# this check
	(r"^code/renderervk/tools/glcheck\.sh$", r".*"),
	# vendored code: SDL 3s own API headers (SDL_video.h declares SDL_GL_*;
	# its GL, GLES and EGL headers are left out), miniaudio comments on
	# coordinate systems, a Qt guard in libjpeg
	(r"^code/thirdparty/SDL3-[^/]*/include/", r".*"),
	# update-libs.sh leaves those GL headers out by name
	(r"^misc/update-libs\.sh$", r"OpenGL( ES)?|opengl"),
	(r"^code/thirdparty/miniaudio/miniaudio\.h$", r"OpenGL"),
	(r"^code/thirdparty/jpeg-[^/]*/jmorecfg\.h$", r"QGLOBAL_H|qglobal\.h"),
	# shader script keywords of the retail .shader files (blendFunc GL_ONE
	# GL_ZERO ...), also in the test shaders of the view lists
	(r"^code/renderervk/tr_shader\.c$", r"\"GL_[A-Z_]+\"|substituting GL_ONE"),
	(r"^code/renderervk/tools/views/[^/]*\.shader$", r"\bGL_[A-Z_]+"),
	# cvar values kept for configs: r_textureMode GL_LINEAR_MIPMAP_NEAREST ...,
	# r_drawBuffer GL_BACK and GL_FRONT
	(r"^code/renderervk/tr_image\.c$", r"\{\"GL_[A-Z_]+\", TEXFILTER_"),
	(r"^code/renderervk/tr_init\.c$", r"\"gl_linear_mipmap_linear\"|\"GL_BACK\""),
	(r"^code/renderervk/tr_cmds\.c$", r"\"GL_FRONT\""),
	# GLSL built-ins of the Vulkan shaders (and comments naming them), and the
	# glslang extension behind their #include lines
	(r"^code/renderervk/", r"\bgl_(Position|ClipDistance|InstanceIndex|VertexIndex|FrontFacing|FragCoord|GlobalInvocationID|LocalInvocationID|LocalInvocationIndex|WorkGroupID)\b|\bGL_GOOGLE_include_directive\b"),
	# false positives: the "sgl" font header type, the glue flag of the game,
	# a UI event of the LAN game list
	(r"^code/", r"fontheader_sgl_[st]"),
	(r"^code/fgame/entity\.(h|cpp)$", r"GL_USEANGLES"),
	(r"^code/uilib/uilangamelist\.cpp$", r"EV_UILGL_"),
	# upstream code kept for small diffs: the refimport hooks (no-ops that
	# nothing calls, kept for the layout of refimport_t), a comment on
	# matrix order
	(r"^code/(renderercommon/tr_public\.h|client/cl_main\.cpp|sys/sys_local\.h|sys/sys_unix\.c|sys/sys_win32\.c)$",
	 r"Sys_GLimp(Safe)?Init"),
	(r"^code/qcommon/q_math\.c$", r"the OpenGL order"),
	# CL_InitRef resets an old configs cl_renderer opengl1 to vulkan
	(r"^code/client/cl_main\.cpp$", r"OpenGL renderers are gone: an old config.s opengl1|\"opengl\", 6|no OpenGL renderer any more"),
	# the downloads tell players the OpenGL renderers are gone
	(r"^misc/release/INSTALL-[^/]*\.txt$", r"There is no OpenGL renderer any more"),
]
allowed = [(re.compile(f), re.compile(t)) for f, t in allowed]
for line in sys.stdin:
	path, _, rest = line.partition(":")
	num, _, text = rest.partition(":")
	for f, t in allowed:
		if f.search(path):
			text = t.sub("", text)
	if pattern.search(text):
		sys.stdout.write(line)
' "$PATTERN")
if [ -n "$hits" ]; then
	echo "glcheck: GL names outside the documented exceptions:"
	echo "$hits"
	status=1
else
	echo "glcheck: source clean (only documented exceptions)"
fi

if [ ! -d "$BINDIR" ]; then
	echo "glcheck: no bin dir $BINDIR" >&2
	exit 1
fi
cd "$BINDIR" || exit 1
bins=$(ls | grep -E '^(openmohaa|omohaaded|launch_openmohaa_[a-z]+|cgame|game|renderer_[A-Za-z0-9_]+)(\.so|\.dll|\.exe)?$')
gl=$(ls | grep -iE '^renderer_.*(opengl|gl[12])')
if [ -n "$gl" ]; then
	echo "glcheck: OpenGL renderers in $BINDIR: $gl"
	status=1
fi
bad=0
for f in $bins; do
	case $f in
	*.dll|*.exe) libs=$(objdump -p "$f" 2>/dev/null | awk '/DLL Name:/ { print $3 }');;
	*) libs=$(readelf -d "$f" 2>/dev/null | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p');;
	esac
	if echo "$libs" | grep -iE '^(lib)?(GL|GLX|EGL|GLES|OpenGL|opengl32)[^a-z]' ; then
		echo "glcheck: $f needs an OpenGL library"
		bad=1
	fi
	case $f in
	*.dll|*.exe) syms=;;
	*) syms=$(nm -D --undefined-only "$f" 2>/dev/null | awk '{ print $NF }' | grep -E '^(gl[A-Z]|glX|egl[A-Z]|wgl[A-Z]|SDL_GL_)');;
	esac
	if [ -n "$syms" ]; then
		echo "glcheck: $f imports GL symbols:" $syms
		bad=1
	fi
done
[ $bad = 0 ] && echo "glcheck: binaries clean ($(echo $bins | wc -w) checked in $BINDIR: no OpenGL renderer, library or import)"
[ $bad = 0 ] || status=1
exit $status
