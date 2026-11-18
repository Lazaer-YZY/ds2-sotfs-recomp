#version 450
#extension GL_EXT_samplerless_texture_functions : require
// Resolve feed from a render target with a bit-exact UINT transfer view (e.g.
// k_32_FLOAT as R32_UINT) into an R32_SFLOAT texture. flags bit 1: write a
// debug value (vulkan_resolve_feed_debug).
layout(set = 0, binding = 0) uniform utexture2D xe_feed_source;
layout(push_constant) uniform XeFeedConstants {
  ivec2 source_offset;
  uint flags;
} xe_feed;
layout(location = 0) out vec4 xe_feed_color;
void main() {
  if ((xe_feed.flags & 2u) != 0u) {
    xe_feed_color = vec4(1.0, 0.0, 1.0, 1.0);
    return;
  }
  uvec4 bits = texelFetch(xe_feed_source, ivec2(gl_FragCoord.xy) + xe_feed.source_offset, 0);
  xe_feed_color = vec4(uintBitsToFloat(bits.r), 0.0, 0.0, 1.0);
}
