#version 450
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_EXT_spirv_intrinsics : require
// Resolve feed from a 1x k_2_10_10_10_FLOAT render target (host
// R16G16B16A16_SFLOAT, float transfer view) resolved as k_2_10_10_10 (the 7e3
// bits reinterpreted: a bitwise fast copy of the EDRAM words) into the
// A2B10G10R10_UNORM_PACK32 texture of a k_2_10_10_10 texture, bit-exact with
// loading the resolved guest memory (a plain 32bpb copy): the color is packed
// to 7e3 / 2-bit alpha like the EDRAM dump and the direct resolve
// (resolve_fast_32bpp_host_rt.xesli, XE_RESOLVE_HOST_RT_7E3), and each field
// is written as UNORM (bits / 1023, alpha / 3), which the attachment converts
// back to the same bits.
// flags bit 0: swap the 10-bit fields 0 and 2 (copy_dest_swap, as
// XeResolveSwapRedBlue_2_10_10_10); bit 1: write a debug color
// (vulkan_resolve_feed_debug).
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

void main() {
  if ((xe_feed.flags & 2u) != 0u) {
    xe_feed_color = vec4(1.0, 0.0, 1.0, 1.0);
    return;
  }
  vec4 color = texelFetch(xe_feed_source, ivec2(gl_FragCoord.xy) + xe_feed.source_offset, 0);
  uvec4 bits = uvec4(XeFloat32To7e3(color.r), XeFloat32To7e3(color.g), XeFloat32To7e3(color.b),
                     uint(XeNClamp(color.a, 0.0, 1.0) * 3.0 + 0.5));
  if ((xe_feed.flags & 1u) != 0u) {
    bits = bits.bgra;
  }
  // (bits + 1/16) / (2^b - 1): the attachment's float -> UNORM conversion
  // (round(f * (2^b - 1)), clamped to [0, 1] first) gets back the bits whether
  // it rounds to nearest or truncates, also through an fp16 intermediate with
  // rounding (bits / (2^b - 1) alone is up to 3e-5 below the integer for half
  // of the codes).
  xe_feed_color =
      (vec4(bits) + 0.0625) * vec4(1.0 / 1023.0, 1.0 / 1023.0, 1.0 / 1023.0, 1.0 / 3.0);
}
