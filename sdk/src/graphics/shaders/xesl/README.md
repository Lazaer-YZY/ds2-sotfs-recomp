# xesl shader sources (ReXGlue additions)

ReXGlue ships Xenia's GPU shaders as precompiled bytecode (`../vulkan_spirv`, `../bytecode`).
This folder holds the xesl sources of shaders that ReXGlue adds or changes, plus the Xenia
headers they include, taken from xenia-canary commit `65d42843b6` (the last one whose resolve
constants match `draw_util::ResolveEdramInfo` here: `fill_half_pixel_offset` in the EDRAM info).
Xenia is BSD-licensed; the original headers are kept.

- `gpu/shaders/resolve_fast_32bpp_host_rt.xesli`: the fast 32bpp resolve
  (`resolve_fast_32bpp_1x2xmsaa`) reading the host render target directly instead of the EDRAM
  buffer (`VulkanRenderTargetCache::PrepareDirectHostResolve`). Variants: UNORM 8888, 32-bit
  integer view, D24S8 depth+stencil; 1x and 4x MSAA.
- `gpu/shaders/resolve.xesli`: Xenia's, plus three push constants under `XE_RESOLVE_HOST_RT`.

Build (writes `../vulkan_spirv/resolve_fast_32bpp_host_rt_*_cs.h`; glslc and spirv-opt from the
Android NDK's shader-tools): `bash scripts/shaders/build_host_rt_resolve_shaders.sh`.
