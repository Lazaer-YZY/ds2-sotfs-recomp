#!/bin/bash
# Compiles the ReXGlue shaders under src/graphics/shaders/xesl (xesl, GLSL mode) and
# src/graphics/shaders/glsl (plain GLSL) to SPIR-V headers in src/graphics/shaders/vulkan_spirv
# (glslc + spirv-opt + spirv-dis from the Android NDK's shader-tools, or SHADER_TOOLS).
# Usage: build_host_rt_resolve_shaders.sh [header names...] - with names (without .h), only those
# headers are regenerated.
set -euo pipefail
ONLY=" $* "
SDK=$(cd "$(dirname "$0")/../.." && pwd)
TOOLS=${SHADER_TOOLS:-$(ls -d "${ANDROID_NDK_HOME:-/c/dev/android-sdk/ndk/28.2.13676358}"/shader-tools/*/ 2>/dev/null | head -1)}
GLSLC="${TOOLS}glslc"; OPT="${TOOLS}spirv-opt"; DIS="${TOOLS}spirv-dis"
SRC="$SDK/src/graphics/shaders/xesl/gpu/shaders"
GLSL="$SDK/src/graphics/shaders/glsl"
OUT="$SDK/src/graphics/shaders/vulkan_spirv"
HEADER="$SDK/scripts/shaders/spv_to_header.py"
TMP=$(mktemp -d)
finish() {  # name
  "$OPT" -O "$TMP/$1.glslc.spv" -o "$TMP/$1.spv"
  "$DIS" "$TMP/$1.spv" -o "$TMP/$1.txt"
  python -I "$(cygpath -m "$HEADER" 2>/dev/null || echo "$HEADER")" "$TMP/$1.spv" "$TMP/$1.txt" "$OUT/$1.h" "$1"
  echo "$1: $(stat -c %s "$TMP/$1.spv") bytes"
}
selected() {  # name
  [ "$ONLY" = "  " ] || [[ "$ONLY" == *" $1 "* ]]
}
build() {  # name entry defines...
  local name=$1 entry=$2; shift 2
  selected "$name" || return 0
  printf '#version 460\n#extension GL_EXT_control_flow_attributes : require\n#extension GL_EXT_samplerless_texture_functions : require\n#extension GL_GOOGLE_include_directive : require\n#include "%s"\n' "$entry" > "$TMP/$name.comp"
  "$GLSLC" -fshader-stage=compute --target-env=vulkan1.0 -DSHADING_LANGUAGE_GLSL_XE=1 "$@" \
    -I"$SRC" "$TMP/$name.comp" -o "$TMP/$name.glslc.spv"
  finish "$name"
}
build_glsl() {  # name file stage
  selected "$1" || return 0
  "$GLSLC" -fshader-stage=$3 --target-env=vulkan1.0 "$GLSL/$2" -o "$TMP/$1.glslc.spv"
  finish "$1"
}
E=resolve_fast_32bpp_host_rt.xesli
build resolve_fast_32bpp_host_rt_1x_unorm8_cs $E
build resolve_fast_32bpp_host_rt_1x_uint_cs $E -DXE_RESOLVE_HOST_RT_UINT
build resolve_fast_32bpp_host_rt_4x_unorm8_cs $E -DXE_RESOLVE_HOST_RT_MSAA
build resolve_fast_32bpp_host_rt_4x_uint_cs $E -DXE_RESOLVE_HOST_RT_UINT -DXE_RESOLVE_HOST_RT_MSAA
build resolve_fast_32bpp_host_rt_1x_d24s8_cs $E -DXE_RESOLVE_HOST_RT_DEPTH
build resolve_fast_32bpp_host_rt_4x_d24s8_cs $E -DXE_RESOLVE_HOST_RT_DEPTH -DXE_RESOLVE_HOST_RT_MSAA
F24="-DXE_RESOLVE_HOST_RT_DEPTH -DXE_RESOLVE_HOST_RT_DEPTH_FLOAT24"
build resolve_fast_32bpp_host_rt_1x_d24fs8_cs $E $F24
build resolve_fast_32bpp_host_rt_1x_d24fs8_round_cs $E $F24 -DXE_RESOLVE_HOST_RT_DEPTH_FLOAT24_ROUND
build resolve_fast_32bpp_host_rt_4x_d24fs8_cs $E $F24 -DXE_RESOLVE_HOST_RT_MSAA
build resolve_fast_32bpp_host_rt_4x_d24fs8_round_cs $E $F24 -DXE_RESOLVE_HOST_RT_DEPTH_FLOAT24_ROUND   -DXE_RESOLVE_HOST_RT_MSAA
build resolve_fast_32bpp_host_rt_1x_7e3_cs $E -DXE_RESOLVE_HOST_RT_7E3
build resolve_fast_32bpp_host_rt_4x_7e3_cs $E -DXE_RESOLVE_HOST_RT_7E3 -DXE_RESOLVE_HOST_RT_MSAA
build resolve_fast_32bpp_host_rt_1x_unorm2101010_cs $E -DXE_RESOLVE_HOST_RT_UNORM_2101010
build resolve_fast_32bpp_host_rt_1x_7e3_to_8888_cs $E -DXE_RESOLVE_HOST_RT_7E3 -DXE_RESOLVE_HOST_RT_7E3_TO_8888
build resolve_fast_32bpp_host_rt_1x_d24fs8_fed_cs $E $F24 -DXE_RESOLVE_HOST_RT_DEPTH_FLOAT24_FED
build_glsl resolve_feed_vs resolve_feed.vert vertex
build_glsl resolve_feed_ps resolve_feed.frag fragment
build_glsl resolve_feed_uint_ps resolve_feed_uint.frag fragment
build_glsl resolve_feed_depth_ps resolve_feed_depth.frag fragment
build_glsl resolve_feed_depth_ms_ps resolve_feed_depth_ms.frag fragment
build_glsl resolve_feed_depth_8888_ps resolve_feed_depth_8888.frag fragment
build_glsl resolve_feed_depth_float24_ps resolve_feed_depth_float24.frag fragment
build_glsl resolve_feed_depth_float24_stencil_ps resolve_feed_depth_float24_stencil.frag fragment
build_glsl resolve_feed_7e3_ps resolve_feed_7e3.frag fragment
build_glsl resolve_feed_7e3_8888_ps resolve_feed_7e3_8888.frag fragment
build_glsl resolve_feed_fxaa_ps resolve_feed_fxaa.frag fragment
rm -rf "$TMP"
