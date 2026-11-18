#version 450
#extension GL_EXT_samplerless_texture_functions : require
// Resolve feed from a 1x depth render target (binding 0 of the depth / stencil
// transfer source set) into an R32_SFLOAT texture, as k_24_8 textures are
// loaded (24-bit unorm depth as float, stencil dropped).
layout(set = 0, binding = 0) uniform texture2D xe_feed_depth;
layout(push_constant) uniform XeFeedConstants {
  ivec2 source_offset;
  uint flags;
} xe_feed;
layout(location = 0) out vec4 xe_feed_color;
void main() {
  if ((xe_feed.flags & 2u) != 0u) {
    xe_feed_color = vec4(0.5, 0.0, 0.0, 1.0);
    return;
  }
  float depth = texelFetch(xe_feed_depth, ivec2(gl_FragCoord.xy) + xe_feed.source_offset, 0).r;
  xe_feed_color = vec4(depth, 0.0, 0.0, 1.0);
}
