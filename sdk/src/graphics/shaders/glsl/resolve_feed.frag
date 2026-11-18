#version 450
#extension GL_EXT_samplerless_texture_functions : require
// Resolve feed: copies a render target region into the cached texture that
// samples the resolve destination. flags bit 0: swap red and blue
// (copy_dest_swap); bit 1: write solid magenta (vulkan_resolve_feed_debug).
layout(set = 0, binding = 0) uniform texture2D xe_feed_source;
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
  vec4 color = texelFetch(xe_feed_source, ivec2(gl_FragCoord.xy) + xe_feed.source_offset, 0);
  xe_feed_color = (xe_feed.flags & 1u) != 0u ? color.bgra : color;
}
