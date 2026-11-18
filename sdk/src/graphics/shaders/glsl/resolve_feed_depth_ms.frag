#version 450
#extension GL_EXT_samplerless_texture_functions : require
// Resolve feed from a 4x depth render target (binding 0 of the depth / stencil
// transfer source set) for a 1x resolve of the same EDRAM, into an R32_SFLOAT
// texture, as k_24_8 textures are loaded (24-bit unorm depth as float, stencil
// dropped). A 1x resolve reads EDRAM samples as pixels: the source position is
// in the render target's sample grid, a host 4x pixel holds 2x2 of them
// (sample bit 0 horizontal, bit 1 vertical with the Vulkan standard locations,
// as the ownership transfers and the direct resolve map them).
layout(set = 0, binding = 0) uniform texture2DMS xe_feed_depth;
layout(push_constant) uniform XeFeedConstants {
  ivec2 source_offset;
  uint flags;
} xe_feed;
layout(location = 0) out vec4 xe_feed_color;
void main() {
  if ((xe_feed.flags & 2u) != 0u) {
    xe_feed_color = vec4(0.5, 0.0, 0.5, 1.0);
    return;
  }
  ivec2 sample_position = ivec2(gl_FragCoord.xy) + xe_feed.source_offset;
  float depth = texelFetch(xe_feed_depth, sample_position >> 1,
                           (sample_position.x & 1) | ((sample_position.y & 1) << 1)).r;
  xe_feed_color = vec4(depth, 0.0, 0.0, 1.0);
}
