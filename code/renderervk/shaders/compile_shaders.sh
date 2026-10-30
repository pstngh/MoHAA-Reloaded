#!/bin/sh
#
# Compiles the GLSL shaders in this directory to SPIR-V and writes them as
# C arrays to ../vk_shaders.c (the renderer's shaders),
# ../vk_shaders_temporal.c (those of the temporal path, r_temporal) and
# ../vk_shaders_hdr.c (HDR output, r_hdrOutput), so that normal builds don't
# need glslc. Run it after changing a shader and commit the regenerated
# files. Another glslc (its spirv-opt) can make other SPIR-V from the same
# source: with an argument only that file is written, main, temporal or
# hdr, and the others' arrays stay as they are.
#
# Usage: code/renderervk/shaders/compile_shaders.sh [main|temporal|hdr]   (GLSLC=/path/to/glslc to override)

set -eu

cd "$(dirname "$0")"

GLSLC=${GLSLC:-glslc}
# name of the array, source file, extra glslc options (separated by commas)
SHADERS="generic_vert:generic.vert:
generic_clip_vert:generic.vert:-DCLIP_DISTANCE
generic_frag:generic.frag:
skinned_vert:skinned.vert:
skinned_clip_vert:skinned.vert:-DCLIP_DISTANCE
effects_vert:effects.vert:
effects_clip_vert:effects.vert:-DCLIP_DISTANCE"
TEMPORAL_SHADERS="generic_temporal_vert:generic.vert:-DTEMPORAL
generic_temporal_clip_vert:generic.vert:-DTEMPORAL,-DCLIP_DISTANCE
generic_temporal_frag:generic.frag:-DTEMPORAL
skinned_temporal_vert:skinned.vert:-DTEMPORAL
skinned_temporal_clip_vert:skinned.vert:-DTEMPORAL,-DCLIP_DISTANCE
effects_temporal_vert:effects.vert:-DTEMPORAL
effects_temporal_clip_vert:effects.vert:-DTEMPORAL,-DCLIP_DISTANCE
temporal_composite_vert:temporal_composite.vert:
temporal_composite_frag:temporal_composite.frag:
hiz_prepare_comp:hiz_build.comp:-DPREPARE
hiz_build_comp:hiz_build.comp:
hiz_tail_comp:hiz_build.comp:-DTAIL
taa_resolve_comp:taa_resolve.comp:
ao_prefilter_comp:ao_prefilter.comp:
gtao_main_comp:gtao_main.comp:
ao_temporal_comp:ao_temporal.comp:
ao_denoise_comp:ao_denoise.comp:
ao_apply_frag:ao_apply.frag:
motion_blur_tiles_comp:motion_blur.comp:
motion_blur_neighbor_comp:motion_blur.comp:-DNEIGHBOR
dof_focus_comp:dof.comp:-DFOCUS
dof_setup_comp:dof.comp:-DSETUP
dof_neighbor_comp:dof.comp:-DNEIGHBOR
dof_gather_comp:dof.comp:-DGATHER
dof_apply_comp:dof.comp:-DAPPLY"
HDR_SHADERS="output_frag:output.frag:"

# write <output file> <what it holds> <shader list>
write() {
	OUT=$1
	TMP=$OUT.tmp
	{
		cat <<'EOF'
/*
===========================================================================
Copyright (C) 2026 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

EOF
		echo "// $(basename "$OUT") -- SPIR-V of $2 in shaders/"
		echo "//"
		echo "// GENERATED FILE, DO NOT EDIT: run shaders/compile_shaders.sh instead."
		echo
		echo "#include <stddef.h>"
		echo "#include <stdint.h>"
		echo
		for entry in $3; do
			name=vk_spirv_$(echo "$entry" | cut -d: -f1)
			src=$(echo "$entry" | cut -d: -f2)
			opts=$(echo "$entry" | cut -d: -f3 | tr ',' ' ')
			echo "// $src${opts:+ $opts}"
			echo "const uint32_t ${name}[] ="
			"$GLSLC" --target-env=vulkan1.1 -O $opts -mfmt=c "$src" -o -
			echo ";"
			echo "const size_t ${name}_size = sizeof( ${name} );"
			echo
		done
	} > "$TMP"

	mv "$TMP" "$OUT"
	echo "wrote $(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"
}

ONLY=${1:-}
if [ -z "$ONLY" ] || [ "$ONLY" = main ]; then
	write ../vk_shaders.c "the shaders" "$SHADERS"
fi
if [ -z "$ONLY" ] || [ "$ONLY" = temporal ]; then
	write ../vk_shaders_temporal.c "the temporal path's shaders (r_temporal)" "$TEMPORAL_SHADERS"
fi
if [ -z "$ONLY" ] || [ "$ONLY" = hdr ]; then
	write ../vk_shaders_hdr.c "the shaders of HDR output (r_hdrOutput)" "$HDR_SHADERS"
fi
