#version 450
#extension GL_EXT_samplerless_texture_functions : require
// Resolve feed from a 1x kD24FS8 depth render target (binding 0 of the depth /
// stencil transfer source set) into the R32_SFLOAT texture of a k_24_8_FLOAT
// texture, bit-exact with loading the resolved guest memory: the host depth
// (guest 20e4 depth * 0.5) is converted to 20e4 like the EDRAM dump and the
// direct resolve do (SpirvShaderTranslator::PreClampedDepthTo20e4 with
// remap_from_0_to_0_5), then to float32 like the k_24_8_FLOAT texture load
// (texture_load_depth_float: XeFloat20e4To32 of the word >> 8, no remap).
// Depth resolves never swap red and blue (draw_util: copy_dest_swap = false),
// so the stencil byte doesn't affect the loaded value.
// flags bit 1: write a debug color (vulkan_resolve_feed_debug); bit 2: round
// to the nearest even (depth_float24_round), otherwise truncate.
layout(set = 0, binding = 0) uniform texture2D xe_feed_depth;
layout(push_constant) uniform XeFeedConstants {
  ivec2 source_offset;
  uint flags;
} xe_feed;
layout(location = 0) out vec4 xe_feed_color;

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
  if ((xe_feed.flags & 2u) != 0u) {
    xe_feed_color = vec4(0.25, 0.0, 0.0, 1.0);
    return;
  }
  float host_depth =
      texelFetch(xe_feed_depth, ivec2(gl_FragCoord.xy) + xe_feed.source_offset, 0).r;
  uint f24 = XeHostDepthTo20e4(host_depth, (xe_feed.flags & 4u) != 0u);
  xe_feed_color = vec4(uintBitsToFloat(XeFloat20e4To32(f24)), 0.0, 0.0, 1.0);
}
