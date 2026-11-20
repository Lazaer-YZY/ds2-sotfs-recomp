#version 450
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_EXT_spirv_intrinsics : require
// Resolve feed from a 1x k_2_10_10_10_FLOAT render target (host R16G16B16A16_SFLOAT, float transfer view) resolved
// to k_8_8_8_8 (a format-converting resolve) into the R8G8B8A8_UNORM texture of a k_8_8_8_8 texture, bit-exact with
// loading the resolved guest memory: the color is packed to 7e3 / 2-bit alpha like the EDRAM dump
// (resolve_feed_7e3.frag), then converted as the full resolve does (XeResolveLoad2RGBAColors and
// XePackR8G8B8A8UNorm): unpacked to float (XeUnpackR10G10B10A2Float), multiplied by 2^exp_bias, red/blue swapped,
// UNORM8 with rounding. Each byte n is written as (n + 1/16) / 255, which the attachment converts back to n.
// flags bit 0: red/blue swap (copy_dest_swap); bit 1: debug color (vulkan_resolve_feed_debug); bits 8-13: the
// signed 6-bit copy_dest_exp_bias.
layout(set = 0, binding = 0) uniform texture2D xe_feed_source;
layout(push_constant) uniform XeFeedConstants {
  ivec2 source_offset;
  uint flags;
} xe_feed;
layout(location = 0) out vec4 xe_feed_color;

// GLSL.std.450 NClamp, as the EDRAM dump uses (NaN -> the lower bound).
spirv_instruction(set = "GLSL.std.450", id = 81)
float XeNClamp(float x, float min_value, float max_value);

// SpirvShaderTranslator::UnclampedFloat32To7e3 step by step.
uint XeFloat32To7e3(float f) {
  uint f32 = floatBitsToUint(XeNClamp(f, 0.0, 31.875));
  uint denormal_biased = bitfieldInsert(f32, 1u, 23, 9) >> min(125u - (f32 >> 23u), 24u);
  uint normal_biased = f32 - (124u << 23u);
  uint biased = (f32 < 0x3E800000u) ? denormal_biased : normal_biased;
  return bitfieldExtract(biased + 0x7FFFu + bitfieldExtract(biased, 16, 1), 16, 10);
}

// pixel_formats.xesli XeUnpackR10G10B10A2Float.
vec4 XeUnpackR10G10B10A2Float(uint p) {
  uvec3 rgb_f10u32 = (uvec3(p) >> uvec3(0u, 10u, 20u)) & 0x3FFu;
  uvec3 mantissa = rgb_f10u32 & 0x7Fu;
  uvec3 exponent = rgb_f10u32 >> 7u;
  bvec3 is_denormalized = equal(exponent, uvec3(0u));
  uvec3 mantissa_lzcnt = uvec3(7u) - uvec3(findMSB(mantissa));
  exponent = mix(exponent, uvec3(1u) - mantissa_lzcnt, is_denormalized);
  mantissa = mix(mantissa, (mantissa << mantissa_lzcnt) & 0x7Fu, is_denormalized);
  uvec3 rgb_f32u32 =
      mix(((exponent + 124u) << 23u) | (mantissa << 16u), uvec3(0u), equal(rgb_f10u32, uvec3(0u)));
  return vec4(uintBitsToFloat(rgb_f32u32), float(p >> 30u) * (1.0 / 3.0));
}

void main() {
  if ((xe_feed.flags & 2u) != 0u) {
    xe_feed_color = vec4(1.0, 0.0, 1.0, 1.0);
    return;
  }
  vec4 color = texelFetch(xe_feed_source, ivec2(gl_FragCoord.xy) + xe_feed.source_offset, 0);
  uint packed = XeFloat32To7e3(color.r) | (XeFloat32To7e3(color.g) << 10u) |
                (XeFloat32To7e3(color.b) << 20u) | (uint(XeNClamp(color.a, 0.0, 1.0) * 3.0 + 0.5) << 30u);
  // The resolve's exponent bias factor (draw_util: 2^bias as float bits; exact for -32..31).
  int exp_bias = bitfieldExtract(int(xe_feed.flags), 8, 6);
  vec4 rgba = XeUnpackR10G10B10A2Float(packed) * ldexp(1.0, exp_bias);
  if ((xe_feed.flags & 1u) != 0u) {
    rgba = rgba.bgra;
  }
  uvec4 n = uvec4(clamp(rgba, 0.0, 1.0) * 255.0 + 0.5);
  xe_feed_color = (vec4(n) + 0.0625) * (1.0 / 255.0);
}
