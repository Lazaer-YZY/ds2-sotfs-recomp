#version 450
#extension GL_EXT_samplerless_texture_functions : require
// Resolve feed with FXAA (ds2 Android, vulkan_scene_fxaa): copies a k_8_8_8_8
// render target region into the cached texture like resolve_feed.frag, smoothing
// edges with FXAA 3.11 (quality variant, Timothy Lottes, public domain) on the way.
// Used for the full-screen resolve of the finished 3D scene, before the game draws
// its 2D layer (HUD, menus, text) over it, so that text stays sharp.
// The source has no sampler: bilinear taps are four texel fetches.
// flags bit 0: swap red and blue; bit 1: solid magenta (vulkan_resolve_feed_debug);
// bits 4-5: preset (1 normal, 2 strong).
layout(set = 0, binding = 0) uniform texture2D xe_feed_source;
layout(push_constant) uniform XeFeedFxaaConstants {
  ivec2 source_offset;  // source texel = destination pixel + source_offset
  uint flags;
  ivec2 dest_min;  // the fed rectangle in destination pixels (inclusive)
  ivec2 dest_max;
} xe_feed;
layout(location = 0) out vec4 xe_feed_color;

vec4 FetchDest(ivec2 dest_pixel) {
  ivec2 p = clamp(dest_pixel, xe_feed.dest_min, xe_feed.dest_max) + xe_feed.source_offset;
  vec4 color = texelFetch(xe_feed_source, p, 0);
  return (xe_feed.flags & 1u) != 0u ? color.bgra : color;
}

// Bilinear at a position in destination pixel units (pixel centers at +0.5).
vec3 SampleDest(vec2 position) {
  vec2 p = position - 0.5;
  vec2 cell = floor(p);
  vec2 f = p - cell;
  ivec2 i = ivec2(cell);
  vec3 a = FetchDest(i).rgb;
  vec3 b = FetchDest(i + ivec2(1, 0)).rgb;
  vec3 c = FetchDest(i + ivec2(0, 1)).rgb;
  vec3 d = FetchDest(i + ivec2(1, 1)).rgb;
  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float Luma(vec3 color) { return dot(color, vec3(0.299, 0.587, 0.114)); }

void main() {
  if ((xe_feed.flags & 2u) != 0u) {
    xe_feed_color = vec4(1.0, 0.0, 1.0, 1.0);
    return;
  }
  bool strong = ((xe_feed.flags >> 4u) & 3u) == 2u;
  // Normal: FXAA quality preset 12 defaults; strong: preset 39 with more subpixel smoothing.
  float subpix = strong ? 1.0 : 0.75;
  float edge_threshold = strong ? 0.125 : 0.166;
  float edge_threshold_min = strong ? 0.0625 : 0.0833;
  const float kStepsNormal[5] = float[5](1.0, 1.5, 2.0, 4.0, 12.0);
  const float kStepsStrong[12] =
      float[12](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);
  int step_count = strong ? 12 : 5;

  ivec2 ip = ivec2(gl_FragCoord.xy);
  vec2 pos_m = vec2(ip) + 0.5;
  vec4 rgba_m = FetchDest(ip);
  float luma_m = Luma(rgba_m.rgb);
  float luma_s = Luma(FetchDest(ip + ivec2(0, 1)).rgb);
  float luma_e = Luma(FetchDest(ip + ivec2(1, 0)).rgb);
  float luma_n = Luma(FetchDest(ip + ivec2(0, -1)).rgb);
  float luma_w = Luma(FetchDest(ip + ivec2(-1, 0)).rgb);
  float range_max = max(max(max(luma_s, luma_m), luma_e), max(luma_n, luma_w));
  float range_min = min(min(min(luma_s, luma_m), luma_e), min(luma_n, luma_w));
  float range = range_max - range_min;
  if (range < max(edge_threshold_min, range_max * edge_threshold)) {
    xe_feed_color = rgba_m;
    return;
  }
  float luma_nw = Luma(FetchDest(ip + ivec2(-1, -1)).rgb);
  float luma_se = Luma(FetchDest(ip + ivec2(1, 1)).rgb);
  float luma_ne = Luma(FetchDest(ip + ivec2(1, -1)).rgb);
  float luma_sw = Luma(FetchDest(ip + ivec2(-1, 1)).rgb);

  float luma_ns = luma_n + luma_s;
  float luma_we = luma_w + luma_e;
  float subpix_rcp_range = 1.0 / range;
  float subpix_nswe = luma_ns + luma_we;
  float edge_horz1 = -2.0 * luma_m + luma_ns;
  float edge_vert1 = -2.0 * luma_m + luma_we;
  float luma_nese = luma_ne + luma_se;
  float luma_nwne = luma_nw + luma_ne;
  float edge_horz2 = -2.0 * luma_e + luma_nese;
  float edge_vert2 = -2.0 * luma_n + luma_nwne;
  float luma_nwsw = luma_nw + luma_sw;
  float luma_swse = luma_sw + luma_se;
  float edge_horz4 = abs(edge_horz1) * 2.0 + abs(edge_horz2);
  float edge_vert4 = abs(edge_vert1) * 2.0 + abs(edge_vert2);
  float edge_horz3 = -2.0 * luma_w + luma_nwsw;
  float edge_vert3 = -2.0 * luma_s + luma_swse;
  float edge_horz = abs(edge_horz3) + edge_horz4;
  float edge_vert = abs(edge_vert3) + edge_vert4;
  float subpix_nwswnese = luma_nwsw + luma_nese;
  bool horz_span = edge_horz >= edge_vert;
  float subpix_a = subpix_nswe * 2.0 + subpix_nwswnese;
  if (!horz_span) {
    luma_n = luma_w;
    luma_s = luma_e;
  }
  float subpix_b = subpix_a * (1.0 / 12.0) - luma_m;
  float gradient_n = luma_n - luma_m;
  float gradient_s = luma_s - luma_m;
  float luma_nn = luma_n + luma_m;
  float luma_ss = luma_s + luma_m;
  bool pair_n = abs(gradient_n) >= abs(gradient_s);
  float gradient = max(abs(gradient_n), abs(gradient_s));
  float length_sign = pair_n ? -1.0 : 1.0;
  float subpix_c = clamp(abs(subpix_b) * subpix_rcp_range, 0.0, 1.0);

  vec2 pos_b = pos_m;
  vec2 off_np = horz_span ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
  if (horz_span) {
    pos_b.y += length_sign * 0.5;
  } else {
    pos_b.x += length_sign * 0.5;
  }
  float first_step = strong ? kStepsStrong[0] : kStepsNormal[0];
  vec2 pos_n = pos_b - off_np * first_step;
  vec2 pos_p = pos_b + off_np * first_step;
  float subpix_d = -2.0 * subpix_c + 3.0;
  float luma_end_n = Luma(SampleDest(pos_n));
  float subpix_e = subpix_c * subpix_c;
  float luma_end_p = Luma(SampleDest(pos_p));
  if (!pair_n) {
    luma_nn = luma_ss;
  }
  float gradient_scaled = gradient * (1.0 / 4.0);
  float luma_mm = luma_m - luma_nn * 0.5;
  float subpix_f = subpix_d * subpix_e;
  bool luma_m_lt_zero = luma_mm < 0.0;
  luma_end_n -= luma_nn * 0.5;
  luma_end_p -= luma_nn * 0.5;
  bool done_n = abs(luma_end_n) >= gradient_scaled;
  bool done_p = abs(luma_end_p) >= gradient_scaled;
  for (int i = 1; i < step_count; ++i) {
    float step_length = strong ? kStepsStrong[i] : kStepsNormal[i];
    if (!done_n) {
      pos_n -= off_np * step_length;
    }
    if (!done_p) {
      pos_p += off_np * step_length;
    }
    if (done_n && done_p) {
      break;
    }
    if (!done_n) {
      luma_end_n = Luma(SampleDest(pos_n)) - luma_nn * 0.5;
      done_n = abs(luma_end_n) >= gradient_scaled;
    }
    if (!done_p) {
      luma_end_p = Luma(SampleDest(pos_p)) - luma_nn * 0.5;
      done_p = abs(luma_end_p) >= gradient_scaled;
    }
  }

  float dst_n = horz_span ? pos_m.x - pos_n.x : pos_m.y - pos_n.y;
  float dst_p = horz_span ? pos_p.x - pos_m.x : pos_p.y - pos_m.y;
  bool good_span_n = (luma_end_n < 0.0) != luma_m_lt_zero;
  bool good_span_p = (luma_end_p < 0.0) != luma_m_lt_zero;
  float span_length = dst_p + dst_n;
  bool direction_n = dst_n < dst_p;
  float dst = min(dst_n, dst_p);
  bool good_span = direction_n ? good_span_n : good_span_p;
  float subpix_g = subpix_f * subpix_f;
  float pixel_offset = dst * (-1.0 / span_length) + 0.5;
  float subpix_h = subpix_g * subpix;
  float pixel_offset_good = good_span ? pixel_offset : 0.0;
  float pixel_offset_subpix = max(pixel_offset_good, subpix_h);
  vec2 pos_f = pos_m;
  if (horz_span) {
    pos_f.y += pixel_offset_subpix * length_sign;
  } else {
    pos_f.x += pixel_offset_subpix * length_sign;
  }
  xe_feed_color = vec4(SampleDest(pos_f), rgba_m.a);
}
