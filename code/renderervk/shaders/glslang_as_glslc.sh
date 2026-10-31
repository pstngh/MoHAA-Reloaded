#!/bin/sh
#
# glslang standing in for glslc: compile_shaders.sh calls
#   glslc --target-env=vulkan1.1 -O [-DNAME[=v] ...] -mfmt=c <src> -o -
# and reads the SPIR-V as a C initializer from stdout. glslang (the Khronos
# reference compiler, a single download from its GitHub releases) has the
# same -O (SPIRV-Tools' performance passes), -D and --target-env, so this
# maps the options and writes the words out as glslc's -mfmt=c does.
#
# Usage: GLSLC=code/renderervk/shaders/glslang_as_glslc.sh code/renderervk/shaders/compile_shaders.sh
#        (GLSLANG=/path/to/glslang to name the compiler; default: glslang in PATH)

GLSLANG=${GLSLANG:-glslang}
src=""
out="-"
fmtc=0
set -- "$@"
args=""
while [ $# -gt 0 ]; do
	case "$1" in
		--target-env=*) args="$args --target-env ${1#--target-env=}";;
		-O) args="$args -O";;
		-D*) args="$args $1";;
		-mfmt=c) fmtc=1;;
		-o) out="$2"; shift;;
		-*) echo "glslang_as_glslc.sh: unknown option $1" >&2; exit 2;;
		*) src="$1";;
	esac
	shift
done
tmp="$(mktemp)" || exit 1
# shellcheck disable=SC2086
"$GLSLANG" -V $args -o "$tmp" "$src" >/dev/null || { rm -f "$tmp"; exit 1; }
emit() {
	if [ "$fmtc" = 1 ]; then
		python -c '
import sys, struct
data = open(sys.argv[1], "rb").read()
words = struct.unpack("<%dI" % (len(data) // 4), data)
out = ["{"]
line = []
for i, w in enumerate(words):
    line.append("0x%08x" % w)
    if len(line) == 8:
        out.append(",".join(line) + ("," if i + 1 < len(words) else ""))
        line = []
if line:
    out.append(",".join(line))
out.append("}")
sys.stdout.write("\n".join(out))
' "$tmp"
	else
		cat "$tmp"
	fi
}
# not "> /dev/stdout": that reopens, and truncates, the file stdout is
if [ "$out" = "-" ]; then
	emit
else
	emit > "$out"
fi
rm -f "$tmp"
