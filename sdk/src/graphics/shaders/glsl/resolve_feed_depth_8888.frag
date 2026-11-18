#version 450
#extension GL_EXT_samplerless_texture_functions : require
// Resolve feed from a 1x kD24S8 depth render target (bindings 0 and 1 of the
// depth / stencil transfer source set) into an R8G8B8A8_UNORM texture holding
// the raw 32-bit word the resolve writes to memory (24-bit depth << 8 |
// stencil), as k_8_8_8_8 textures of resolved depth are loaded from it.
// flags bit 0: swap bytes 0 and 2 (copy_dest_swap, which the resolve applies to
// kD24S8 like to k_8_8_8_8); bit 1: write a debug color
// (vulkan_resolve_feed_debug).
layout(set = 0, binding = 0) uniform texture2D xe_feed_depth;
layout(set = 0, binding = 1) uniform utexture2D xe_feed_stencil;
layout(push_constant) uniform XeFeedConstants {
  ivec2 source_offset;
  uint flags;
} xe_feed;
layout(location = 0) out vec4 xe_feed_color;
void main() {
  if ((xe_feed.flags & 2u) != 0u) {
    xe_feed_color = vec4(0.0, 1.0, 1.0, 1.0);
    return;
  }
  ivec2 pixel = ivec2(gl_FragCoord.xy) + xe_feed.source_offset;
  // Round to the nearest even like the dump.
  uint depth24 = uint(roundEven(texelFetch(xe_feed_depth, pixel, 0).r * 16777215.0));
  uint stencil = texelFetch(xe_feed_stencil, pixel, 0).r & 0xFFu;
  uint word = (depth24 << 8u) | stencil;
  if ((xe_feed.flags & 1u) != 0u) {
    word = (word & 0xFF00FF00u) | ((word >> 16u) & 0xFFu) | ((word & 0xFFu) << 16u);
  }
  xe_feed_color = unpackUnorm4x8(word);
}
