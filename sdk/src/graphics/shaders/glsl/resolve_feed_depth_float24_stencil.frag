#version 450
#extension GL_EXT_samplerless_texture_functions : require
// resolve_feed_depth_float24.frag plus the stencil byte in a second attachment (an R8_UINT side image of the fed
// texture), so the resolve's guest memory write can be deferred (vulkan_resolve_feed_lazy_dump_float24): the lazy
// dump packs (20e4 depth << 8) | stencil from the two images, the words the direct resolve writes.
// flags bit 1: write a debug color (vulkan_resolve_feed_debug); bit 2: round to the nearest even
// (depth_float24_round), otherwise truncate.
layout(set = 0, binding = 0) uniform texture2D xe_feed_depth;
layout(set = 0, binding = 1) uniform utexture2D xe_feed_stencil;
layout(push_constant) uniform XeFeedConstants {
  ivec2 source_offset;
  uint flags;
} xe_feed;
layout(location = 0) out vec4 xe_feed_color;
layout(location = 1) out uvec4 xe_feed_stencil_out;

uint XeHostDepthTo20e4(float host_depth, bool round_to_nearest_even) {
  uint f32 = floatBitsToUint(host_depth);
  uint denormal_biased = bitfieldInsert(f32, 1u, 23, 9) >> min(112u - (f32 >> 23u), 24u);
  uint normal_biased = f32 - (111u << 23u);
  uint biased = (f32 < 0x38000000u) ? denormal_biased : normal_biased;
  if (round_to_nearest_even) {
    biased += 3u + bitfieldExtract(biased, 3, 1);
  }
  return bitfieldExtract(biased, 3, 24);
}

uint XeFloat20e4To32(uint f24) {
  uint mantissa = f24 & 0xFFFFFu;
  uint exponent = f24 >> 20u;
  bool is_denormalized = exponent == 0u;
  uint mantissa_lzcnt = 20u - uint(findMSB(mantissa));
  exponent = is_denormalized ? (1u - mantissa_lzcnt) : exponent;
  mantissa = is_denormalized ? ((mantissa << mantissa_lzcnt) & 0xFFFFFu) : mantissa;
  return (f24 == 0u) ? 0u : (((exponent + 112u) << 23u) | (mantissa << 3u));
}

void main() {
  ivec2 source = ivec2(gl_FragCoord.xy) + xe_feed.source_offset;
  xe_feed_stencil_out = uvec4(texelFetch(xe_feed_stencil, source, 0).x & 0xFFu, 0u, 0u, 0u);
  if ((xe_feed.flags & 2u) != 0u) {
    xe_feed_color = vec4(0.25, 0.0, 0.0, 1.0);
    return;
  }
  float host_depth = texelFetch(xe_feed_depth, source, 0).r;
  uint f24 = XeHostDepthTo20e4(host_depth, (xe_feed.flags & 4u) != 0u);
  xe_feed_color = vec4(uintBitsToFloat(XeFloat20e4To32(f24)), 0.0, 0.0, 1.0);
}
