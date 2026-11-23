/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <tuple>
#include <unordered_set>
#include <utility>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/render_target/cache.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>

REXCVAR_DEFINE_UINT32(gpu_skip_overwritten_transfers, 2, "GPU",
                      "Skip ownership transfers into the part of the render targets that "
                      "the draw needing them overwrites completely (a single rectangle, or a "
                      "triangle containing the scissor; depth written with ALWAYS, colors "
                      "written without blending). 1 = only if the stencil is overwritten too, "
                      "2 = also if the stencil isn't written (its old bits are then not "
                      "transferred).");
REXCVAR_DEFINE_BOOL(gpu_skip_overwritten_color_transfers, true, "GPU",
                    "gpu_skip_overwritten_transfers also for draws writing color render "
                    "targets (otherwise only depth-only draws).");
REXCVAR_DEFINE_BOOL(gpu_constant_transfers_as_clears, true, "GPU",
                    "When a depth ownership transfer's source only holds the constant written "
                    "by a clear-like rectangle (depth ALWAYS, same z at every vertex), clear "
                    "that area of the destination instead of transferring it.");
REXCVAR_DEFINE_BOOL(gpu_constant_transfers_as_color_clears, false, "GPU",
                    "gpu_constant_transfers_as_clears also for draws with color render targets: a "
                    "32bpp color (or depth) render target of the draw whose only foreign owner is a "
                    "depth render target of the same base and pitch holding a recorded constant "
                    "(depth and stencil both written by the clear rectangle) is cleared to that "
                    "constant's bits instead of being transferred; the draw's other render targets "
                    "are transferred as usual. The constant of a clear rectangle that also writes "
                    "color is recorded for its depth render target too. Dark Souls II clears its "
                    "light buffer, HDR target and half-size targets with a 4x depth rectangle and "
                    "then draws geometry at 1x (two full-screen 4x depth -> color transfers a frame).");
REXCVAR_DEFINE_BOOL(gpu_skip_slack_transfers, false, "GPU",
                    "A 1x draw proven to overwrite a rectangle (gpu_skip_overwritten_transfers) "
                    "doesn't transfer the rest of the EDRAM tiles the rectangle covers partially "
                    "either, if every previous owner of the draw's range has a different surface "
                    "layout (pitch, sample size, or a base not a whole number of rows away): those "
                    "tiles are damaged for the previous owner anyway, and for the new owner the "
                    "remainder is reinterpreted garbage. Dark Souls II's post-processing pool reuses "
                    "tiles for targets of different sizes (320x180 in 12 tile rows = 192 pixel rows: "
                    "the 12 spare rows were a transfer pass per target).");
REXCVAR_DEFINE_BOOL(gpu_redirect_msaa_depth_clears, true, "GPU",
                    "A depth-only clear rectangle (proven to overwrite its rectangle, depth ALWAYS "
                    "with one z) drawn on an MSAA alias of EDRAM that a 1x depth render target of "
                    "the same base, pitch and format also holds is done as a clear of that 1x "
                    "render target, and the draw is skipped: the tiles never change hands, so "
                    "neither the alias nor the 1x target has to be transferred. 4x samples and 1x "
                    "pixels are the same EDRAM bytes. Dark Souls II clears every point-light shadow "
                    "map of its atlas this way (2 x 277 tiles transferred per map, ~2-5 ms a frame "
                    "in the hut cutscene).");
REXCVAR_DEFINE_BOOL(gpu_ownership_from_first_row, true, "GPU",
                    "A 1x draw takes ownership of its render targets' EDRAM only from the first tile row it can "
                    "write (the host scissor's top, or the exact top of a rectangle draw) instead of from row 0. "
                    "Dark Souls II clears the last 8 rows of a 640x360 depth buffer with a rectangle while an 8888 "
                    "target shares its top rows: the whole-range claim moved 48 tiles to the depth buffer and the "
                    "next 8888 draw moved them back, 9 transfers a frame at Majula.");
REXCVAR_DEFINE_BOOL(gpu_log_transfer_draws, false, "GPU",
                    "Diagnostic: log each distinct ownership transfer situation once with "
                    "the state of the draw that needs it.");
REXCVAR_DEFINE_UINT32(gpu_edram_tiles, 2048, "GPU",
                      "EDRAM tile address space of the render target cache: 2048 (the console's 10 MiB), 4096 or "
                      "8192. Above 2048 (host render targets path only), surfaces a title places beyond the "
                      "console's EDRAM (a 1920x1080 screen: colour + depth are 3264 tiles) don't alias each other. "
                      "Bases come from RB_COLOR_INFO / RB_DEPTH_INFO bits 0-11 (4096) or 0-12 (8192: bit 12 is "
                      "padding on the console, the title must be patched to write it). One render target and one "
                      "resolve still span at most 2048 tiles.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(mrt_edram_used_range_clamp_to_min, true, "GPU",
                    "Clamp MRT EDRAM used range to minimum");

REXCVAR_DEFINE_BOOL(execute_unclipped_draw_vs_on_cpu_for_psi_render_backend, true, "GPU",
                    "Execute unclipped draw VS on CPU for PSI render backend");

REXCVAR_DEFINE_BOOL(snorm16_render_target_full_range, true, "GPU",
                    "Use full range for SNORM16 render targets");

REXCVAR_DEFINE_INT32(render_target_max_height, 0, "GPU",
                     "If above 0, host render targets are at most this many pixels tall (rounded "
                     "up to whole EDRAM tiles) instead of covering the EDRAM down to the next "
                     "addressing period. Experimental: content drawn below is lost.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(direct_host_resolve, true, "GPU",
                    "Resolve from host render targets directly to shared memory when possible")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics::xenos {
uint32_t edram_tile_count_configured = kEdramTileCount;
}  // namespace rex::graphics::xenos

namespace rex::graphics {

void RenderTargetCache::GetPSIColorFormatInfo(xenos::ColorRenderTargetFormat format,
                                              uint32_t write_mask, float& clamp_rgb_low,
                                              float& clamp_alpha_low, float& clamp_rgb_high,
                                              float& clamp_alpha_high, uint32_t& keep_mask_low,
                                              uint32_t& keep_mask_high) {
  keep_mask_low = keep_mask_high = 0;
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
      clamp_rgb_low = clamp_alpha_low = 0.0f;
      clamp_rgb_high = clamp_alpha_high = 1.0f;
      for (uint32_t i = 0; i < 4; ++i) {
        if (!(write_mask & (1 << i))) {
          keep_mask_low |= uint32_t(0xFF) << (i * 8);
        }
      }
    } break;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
      clamp_rgb_low = clamp_alpha_low = 0.0f;
      clamp_rgb_high = clamp_alpha_high = 1.0f;
      for (uint32_t i = 0; i < 3; ++i) {
        if (!(write_mask & (1 << i))) {
          keep_mask_low |= uint32_t(0x3FF) << (i * 10);
        }
      }
      if (!(write_mask & 0b1000)) {
        keep_mask_low |= uint32_t(3) << 30;
      }
    } break;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
      clamp_rgb_low = clamp_alpha_low = 0.0f;
      clamp_rgb_high = 31.875f;
      clamp_alpha_high = 1.0f;
      for (uint32_t i = 0; i < 3; ++i) {
        if (!(write_mask & (1 << i))) {
          keep_mask_low |= uint32_t(0x3FF) << (i * 10);
        }
      }
      if (!(write_mask & 0b1000)) {
        keep_mask_low |= uint32_t(3) << 30;
      }
    } break;
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      // Alpha clamping affects blending source, so it's non-zero for alpha for
      // k_16_16 (the render target is fixed-point). There's one deviation from
      // how Direct3D 11.3 functional specification defines SNorm conversion
      // (NaN should be 0, not the lowest negative number), and that needs to be
      // handled separately.
      clamp_rgb_low = clamp_alpha_low = -32.0f;
      clamp_rgb_high = clamp_alpha_high = 32.0f;
      if (!(write_mask & 0b0001)) {
        keep_mask_low |= 0xFFFFu;
      }
      if (!(write_mask & 0b0010)) {
        keep_mask_low |= 0xFFFF0000u;
      }
      if (format == xenos::ColorRenderTargetFormat::k_16_16_16_16) {
        if (!(write_mask & 0b0100)) {
          keep_mask_high |= 0xFFFFu;
        }
        if (!(write_mask & 0b1000)) {
          keep_mask_high |= 0xFFFF0000u;
        }
      } else {
        write_mask &= 0b0011;
      }
      break;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      // No NaNs on the Xbox 360 GPU, though can't use the extended range with
      // Direct3D and Vulkan conversions.
      // TODO(Triang3l): Use the extended-range encoding in all implementations.
      clamp_rgb_low = clamp_alpha_low = -65504.0f;
      clamp_rgb_high = clamp_alpha_high = 65504.0f;
      if (!(write_mask & 0b0001)) {
        keep_mask_low |= 0xFFFFu;
      }
      if (!(write_mask & 0b0010)) {
        keep_mask_low |= 0xFFFF0000u;
      }
      if (format == xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT) {
        if (!(write_mask & 0b0100)) {
          keep_mask_high |= 0xFFFFu;
        }
        if (!(write_mask & 0b1000)) {
          keep_mask_high |= 0xFFFF0000u;
        }
      } else {
        write_mask &= 0b0011;
      }
      break;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      // No clamping - let min/max always pick the original value.
      clamp_rgb_low = clamp_alpha_low = clamp_rgb_high = clamp_alpha_high = std::nanf("");
      write_mask &= 0b0001;
      if (!(write_mask & 0b0001)) {
        keep_mask_low = ~uint32_t(0);
      }
      break;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      // No clamping - let min/max always pick the original value.
      clamp_rgb_low = clamp_alpha_low = clamp_rgb_high = clamp_alpha_high = std::nanf("");
      write_mask &= 0b0011;
      if (!(write_mask & 0b0001)) {
        keep_mask_low = ~uint32_t(0);
      }
      if (!(write_mask & 0b0010)) {
        keep_mask_high = ~uint32_t(0);
      }
      break;
    default:
      assert_unhandled_case(format);
      // Disable invalid render targets.
      write_mask = 0;
      break;
  }
  // Special case handled in the shaders for empty write mask to completely skip
  // a disabled render target: all keep bits are set.
  if (!write_mask) {
    keep_mask_low = keep_mask_high = ~uint32_t(0);
  }
}

uint32_t RenderTargetCache::Transfer::GetRangeRectangles(uint32_t start_tiles, uint32_t end_tiles,
                                                         uint32_t base_tiles, uint32_t pitch_tiles,
                                                         xenos::MsaaSamples msaa_samples,
                                                         bool is_64bpp, Rectangle* rectangles_out,
                                                         const Rectangle* cutout) {
  // EDRAM addressing wrapping must be handled by doing GetRangeRectangles for
  // two clamped ranges, in this case start_tiles == end_tiles will also
  // unambiguously mean an empty range rather than the entire EDRAM.
  assert_true(start_tiles < xenos::EdramTileCount());
  assert_true(end_tiles <= xenos::EdramTileCount());
  assert_true(start_tiles <= end_tiles);
  // If start_tiles < base_tiles, this is the tail after EDRAM addressing
  // wrapping.
  assert_true(start_tiles >= base_tiles || end_tiles <= base_tiles);
  assert_not_zero(pitch_tiles);
  if (start_tiles == end_tiles) {
    return 0;
  }
  uint32_t tile_width = xenos::kEdramTileWidthSamples >>
                        (uint32_t(msaa_samples >= xenos::MsaaSamples::k4X) + uint32_t(is_64bpp));
  uint32_t tile_height =
      xenos::kEdramTileHeightSamples >> uint32_t(msaa_samples >= xenos::MsaaSamples::k2X);
  // If the first and / or the last rows have the same X spans as the middle
  // part, merge them with it.
  uint32_t rectangle_count = 0;
  // If start_tiles < base_tiles, this is the tail after EDRAM addressing
  // wrapping.
  uint32_t local_offset = start_tiles < base_tiles ? xenos::EdramTileCount() : 0;
  uint32_t local_start = local_offset + start_tiles - base_tiles;
  uint32_t local_end = local_offset + end_tiles - base_tiles;
  // Inclusive.
  uint32_t rows_start = local_start / pitch_tiles;
  // Exclusive.
  uint32_t rows_end = (local_end + (pitch_tiles - 1)) / pitch_tiles;
  uint32_t row_first_start = local_start - rows_start * pitch_tiles;
  uint32_t row_last_end = pitch_tiles - (rows_end * pitch_tiles - local_end);
  uint32_t rows = rows_end - rows_start;
  if (rows == 1 || row_first_start) {
    Rectangle rectangle_first;
    rectangle_first.x_pixels = row_first_start * tile_width;
    rectangle_first.y_pixels = rows_start * tile_height;
    rectangle_first.width_pixels =
        ((rows == 1 ? row_last_end : pitch_tiles) - row_first_start) * tile_width;
    rectangle_first.height_pixels = tile_height;
    rectangle_count += AddRectangle(
        rectangle_first, rectangles_out ? rectangles_out + rectangle_count : nullptr, cutout);
    if (rows == 1) {
      return rectangle_count;
    }
  }
  uint32_t mid_rows_start = rows_start + 1;
  uint32_t mid_rows = rows - 2;
  if (!row_first_start) {
    --mid_rows_start;
    ++mid_rows;
  }
  if (row_last_end == pitch_tiles) {
    ++mid_rows;
  }
  if (mid_rows) {
    Rectangle rectangle_mid;
    rectangle_mid.x_pixels = 0;
    rectangle_mid.y_pixels = mid_rows_start * tile_height;
    rectangle_mid.width_pixels = pitch_tiles * tile_width;
    rectangle_mid.height_pixels = mid_rows * tile_height;
    rectangle_count += AddRectangle(
        rectangle_mid, rectangles_out ? rectangles_out + rectangle_count : nullptr, cutout);
  }
  if (row_last_end != pitch_tiles) {
    Rectangle rectangle_last;
    rectangle_last.x_pixels = 0;
    rectangle_last.y_pixels = (rows_end - 1) * tile_height;
    rectangle_last.width_pixels = row_last_end * tile_width;
    rectangle_last.height_pixels = tile_height;
    rectangle_count += AddRectangle(
        rectangle_last, rectangles_out ? rectangles_out + rectangle_count : nullptr, cutout);
  }
  assert_true(rectangle_count <= (cutout ? kMaxRectanglesWithCutout : kMaxRectanglesWithoutCutout));
  return rectangle_count;
}

uint32_t RenderTargetCache::Transfer::AddRectangle(const Rectangle& rectangle,
                                                   Rectangle* rectangles_out,
                                                   const Rectangle* cutout) {
  uint32_t rectangle_right = rectangle.x_pixels + rectangle.width_pixels;
  uint32_t rectangle_bottom = rectangle.y_pixels + rectangle.height_pixels;
  // If nothing to cut out (no region specified, or no intersection - if the
  // cutout region is in the middle on Y, but completely to the left / right on
  // X, don't split), add the whole rectangle.
  if (!cutout || !cutout->width_pixels || !cutout->height_pixels ||
      cutout->x_pixels >= rectangle_right ||
      cutout->x_pixels + cutout->width_pixels <= rectangle.x_pixels ||
      cutout->y_pixels >= rectangle_bottom ||
      cutout->y_pixels + cutout->height_pixels <= rectangle.y_pixels) {
    if (rectangles_out) {
      rectangles_out[0] = rectangle;
    }
    return 1;
  }
  uint32_t rectangle_count = 0;
  uint32_t cutout_right = cutout->x_pixels + cutout->width_pixels;
  uint32_t cutout_bottom = cutout->y_pixels + cutout->height_pixels;
  // Upper part after cutout.
  if (cutout->y_pixels > rectangle.y_pixels) {
    // The completely outside case has already been checked.
    assert_true(cutout->y_pixels < rectangle_bottom);
    if (rectangles_out) {
      Rectangle& rectangle_upper = rectangles_out[rectangle_count];
      rectangle_upper.x_pixels = rectangle.x_pixels;
      rectangle_upper.y_pixels = rectangle.y_pixels;
      rectangle_upper.width_pixels = rectangle.width_pixels;
      // cutout->y_pixels is already known to be < rectangle_bottom, no need for
      // min(cutout->y_pixels - rectangle.y_pixels, rectangle.height_pixels).
      rectangle_upper.height_pixels = cutout->y_pixels - rectangle.y_pixels;
    }
    ++rectangle_count;
  }
  // Middle part after cutout.
  uint32_t middle_top = std::max(cutout->y_pixels, rectangle.y_pixels);
  uint32_t middle_height = std::min(cutout_bottom, rectangle_bottom) - middle_top;
  // Middle left.
  if (cutout->x_pixels > rectangle.x_pixels) {
    assert_true(cutout->x_pixels < rectangle_right);
    if (rectangles_out) {
      Rectangle& rectangle_middle_left = rectangles_out[rectangle_count];
      rectangle_middle_left.x_pixels = rectangle.x_pixels;
      rectangle_middle_left.y_pixels = middle_top;
      rectangle_middle_left.width_pixels = cutout->x_pixels - rectangle.x_pixels;
      rectangle_middle_left.height_pixels = middle_height;
    }
    ++rectangle_count;
  }
  // Middle right.
  if (cutout_right < rectangle_right) {
    assert_true(cutout_right > rectangle.x_pixels);
    if (rectangles_out) {
      Rectangle& rectangle_middle_right = rectangles_out[rectangle_count];
      rectangle_middle_right.x_pixels = cutout_right;
      rectangle_middle_right.y_pixels = middle_top;
      rectangle_middle_right.width_pixels = rectangle_right - cutout_right;
      rectangle_middle_right.height_pixels = middle_height;
    }
    ++rectangle_count;
  }
  // Lower part after cutout.
  if (cutout_bottom < rectangle_bottom) {
    assert_true(cutout_bottom > rectangle.y_pixels);
    if (rectangles_out) {
      Rectangle& rectangle_upper = rectangles_out[rectangle_count];
      rectangle_upper.x_pixels = rectangle.x_pixels;
      rectangle_upper.y_pixels = cutout_bottom;
      rectangle_upper.width_pixels = rectangle.width_pixels;
      rectangle_upper.height_pixels = rectangle_bottom - cutout_bottom;
    }
    ++rectangle_count;
  }
  assert_true(rectangle_count <= kMaxCutoutBorderRectangles);
  return rectangle_count;
}

void RenderTargetCache::ApplyEdramTileCountConfig() {
  uint32_t requested = REXCVAR_GET(gpu_edram_tiles);
  uint32_t applied = xenos::kEdramTileCount;
  if (requested == 4096 || requested == 8192) {
    if (GetPath() == Path::kHostRenderTargets) {
      applied = requested;
    } else {
      REXGPU_WARN("gpu_edram_tiles = {} needs the host render targets path; using {}", requested,
                  applied);
    }
  } else if (requested != xenos::kEdramTileCount) {
    REXGPU_WARN("gpu_edram_tiles = {} is not 2048, 4096 or 8192; using {}", requested, applied);
  }
  static_assert(xenos::kEdramMaxTileCount >= 8192);
  xenos::edram_tile_count_configured = applied;
  if (applied != xenos::kEdramTileCount) {
    REXGPU_INFO("Render target cache: EDRAM address space extended to {} tiles", applied);
  }
}

RenderTargetCache::~RenderTargetCache() {
  ShutdownCommon();
}

void RenderTargetCache::InitializeCommon() {
  assert_true(ownership_ranges_.empty());
  ownership_ranges_.emplace(std::piecewise_construct, std::forward_as_tuple(uint32_t(0)),
                            std::forward_as_tuple(xenos::EdramTileCount(), RenderTargetKey(),
                                                  RenderTargetKey(), RenderTargetKey()));
}

void RenderTargetCache::DestroyAllRenderTargets(bool shutting_down) {
  ownership_ranges_.clear();
  if (!shutting_down) {
    ownership_ranges_.emplace(std::piecewise_construct, std::forward_as_tuple(uint32_t(0)),
                              std::forward_as_tuple(xenos::EdramTileCount(), RenderTargetKey(),
                                                    RenderTargetKey(), RenderTargetKey()));
  }

  for (const auto& render_target_pair : render_targets_) {
    if (render_target_pair.second) {
      delete render_target_pair.second;
    }
  }
  render_targets_.clear();
}

void RenderTargetCache::ShutdownCommon() {
  DestroyAllRenderTargets(true);
}

void RenderTargetCache::ClearCache() {
  constant_depth_regions_.clear();
  // Keep only render targets currently owning any EDRAM data.
  if (!render_targets_.empty()) {
    std::unordered_set<RenderTargetKey, RenderTargetKey::Hasher> used_render_targets;
    for (const auto& ownership_range_pair : ownership_ranges_) {
      const OwnershipRange& ownership_range = ownership_range_pair.second;
      if (!ownership_range.render_target.IsEmpty()) {
        used_render_targets.emplace(ownership_range.render_target);
      }
      if (!ownership_range.host_depth_render_target_unorm24.IsEmpty()) {
        used_render_targets.emplace(ownership_range.host_depth_render_target_unorm24);
      }
      if (!ownership_range.host_depth_render_target_float24.IsEmpty()) {
        used_render_targets.emplace(ownership_range.host_depth_render_target_float24);
      }
    }
    if (render_targets_.size() != used_render_targets.size()) {
      typename decltype(render_targets_)::iterator it_next;
      for (auto it = render_targets_.begin(); it != render_targets_.end(); it = it_next) {
        it_next = std::next(it);
        if (!it->second) {
          render_targets_.erase(it);
          continue;
        }
        if (used_render_targets.find(it->second->key()) == used_render_targets.end()) {
          delete it->second;
          render_targets_.erase(it);
        }
      }
    }
  }
}

void RenderTargetCache::BeginFrame() {
  ResetAccumulatedRenderTargets();
}

bool RenderTargetCache::IsRenderTargetInLastUpdate(const RenderTarget* render_target) const {
  for (const RenderTarget* used : last_update_used_render_targets_) {
    if (used == render_target) return true;
  }
  for (const RenderTarget* accumulated : last_update_accumulated_render_targets_) {
    if (accumulated == render_target) return true;
  }
  return last_update_redirected_clear_rt_ == render_target;
}

bool RenderTargetCache::RemoveIdleRenderTarget(RenderTarget* render_target) {
  bool removed = false;
  for (auto it = render_targets_.begin(); it != render_targets_.end();) {
    if (it->second == render_target) {
      it = render_targets_.erase(it);
      removed = true;
    } else {
      ++it;
    }
  }
  return removed;
}

void RenderTargetCache::CollectIdleRenderTargets(uint64_t min_idle_frames,
                                                 std::vector<RenderTarget*>& idle) const {
  std::unordered_set<RenderTargetKey, RenderTargetKey::Hasher> owning;
  for (const auto& ownership_range_pair : ownership_ranges_) {
    const OwnershipRange& ownership_range = ownership_range_pair.second;
    if (!ownership_range.render_target.IsEmpty()) owning.emplace(ownership_range.render_target);
    if (!ownership_range.host_depth_render_target_unorm24.IsEmpty()) {
      owning.emplace(ownership_range.host_depth_render_target_unorm24);
    }
    if (!ownership_range.host_depth_render_target_float24.IsEmpty()) {
      owning.emplace(ownership_range.host_depth_render_target_float24);
    }
  }
  for (const auto& render_target_pair : render_targets_) {
    RenderTarget* render_target = render_target_pair.second;
    if (!render_target || owning.count(render_target->key())) continue;
    if (frame_index_ - render_target->last_used_frame < min_idle_frames) continue;
    idle.push_back(render_target);
  }
}

bool RenderTargetCache::Update(bool is_rasterization_done,
                               reg::RB_DEPTHCONTROL normalized_depth_control,
                               uint32_t normalized_color_mask, const Shader& vertex_shader) {
  const RegisterFile& regs = register_file();
  bool interlock_barrier_only = GetPath() == Path::kPixelShaderInterlock;
  last_update_redirected_clear_rt_ = nullptr;

  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  xenos::MsaaSamples msaa_samples = rb_surface_info.msaa_samples;
  assert_true(msaa_samples <= xenos::MsaaSamples::k4X);
  if (msaa_samples > xenos::MsaaSamples::k4X) {
    // Safety check because a lot of code assumes up to 4x.
    assert_always();
    REXGPU_ERROR("{}x MSAA requested by the guest, Xenos only supports up to 4x",
                 uint32_t(1) << uint32_t(msaa_samples));
    return false;
  }
  uint32_t msaa_samples_x_log2 = uint32_t(msaa_samples >= xenos::MsaaSamples::k4X);
  uint32_t pitch_pixels = rb_surface_info.surface_pitch;
  // surface_pitch 0 should be handled in disabling rasterization (hopefully
  // it's safe to assume that).
  assert_true(pitch_pixels || !is_rasterization_done);
  if (!pitch_pixels) {
    is_rasterization_done = false;
  } else if (pitch_pixels > xenos::kTexture2DCubeMaxWidthHeight) {
    REXGPU_ERROR(
        "Surface pitch {} larger than the maximum texture width {} specified "
        "by the guest",
        pitch_pixels, xenos::kTexture2DCubeMaxWidthHeight);
    return false;
  }
  uint32_t pitch_tiles_at_32bpp =
      ((pitch_pixels << msaa_samples_x_log2) + (xenos::kEdramTileWidthSamples - 1)) /
      xenos::kEdramTileWidthSamples;
  if (!interlock_barrier_only) {
    uint32_t pitch_pixels_tile_aligned_scaled =
        pitch_tiles_at_32bpp * (xenos::kEdramTileWidthSamples >> msaa_samples_x_log2) *
        draw_resolution_scale_x();
    uint32_t max_render_target_width = GetMaxRenderTargetWidth();
    if (pitch_pixels_tile_aligned_scaled > max_render_target_width) {
      // TODO(Triang3l): If really needed for some game on some device, clamp
      // the pitch and generate multiple ranges (each for every row of tiles)
      // with gaps for padding. Very few PowerVR GPUs have 4096, not 8192, as
      // the limit, though with 8192 (on Mali) the actual limit for Xenia is
      // 8160 because tile padding is stored - but 8192 should be extremely rare
      // anyway.
      REXGPU_ERROR(
          "Surface pitch aligned to EDRAM tiles and resolution-scaled {} "
          "larger than the maximum host render target width {}",
          pitch_pixels_tile_aligned_scaled, max_render_target_width);
      return false;
    }
  }

  // Get used render targets.
  // [0] is depth / stencil where relevant, [1...4] is color.
  // Depth / stencil testing / writing is before color in the pipeline.
  uint32_t depth_and_color_rts_used_bits = 0;
  // depth_and_color_rts_used_bits -> EDRAM base.
  uint32_t edram_bases[1 + xenos::kMaxColorRenderTargets];
  uint32_t resource_formats[1 + xenos::kMaxColorRenderTargets];
  uint32_t rts_are_64bpp = 0;
  if (is_rasterization_done) {
    if (normalized_depth_control.z_enable || normalized_depth_control.stencil_enable) {
      depth_and_color_rts_used_bits |= 1;
      auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
      edram_bases[0] = rb_depth_info.base_tiles();
      // With pixel shader interlock, always the same addressing disregarding
      // the format.
      resource_formats[0] = interlock_barrier_only ? 0 : uint32_t(rb_depth_info.depth_format);
    }
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (!(normalized_color_mask & (uint32_t(0b1111) << (4 * i)))) {
        continue;
      }
      auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
      uint32_t rt_bit_index = 1 + i;
      depth_and_color_rts_used_bits |= uint32_t(1) << rt_bit_index;
      edram_bases[rt_bit_index] = color_info.base_tiles();
      xenos::ColorRenderTargetFormat color_format =
          regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]).color_format;
      bool is_64bpp = xenos::IsColorRenderTargetFormat64bpp(color_format);
      if (is_64bpp) {
        rts_are_64bpp |= uint32_t(1) << rt_bit_index;
      }
      xenos::ColorRenderTargetFormat color_resource_format;
      if (interlock_barrier_only) {
        // Only changes in mapping between coordinates and addresses are
        // interesting (along with access overlap between draw calls), thus only
        // pixel size is relevant.
        color_resource_format = is_64bpp ? xenos::ColorRenderTargetFormat::k_16_16_16_16
                                         : xenos::ColorRenderTargetFormat::k_8_8_8_8;
      } else {
        color_resource_format = GetColorResourceFormat(xenos::GetStorageColorFormat(color_format));
      }
      resource_formats[rt_bit_index] = uint32_t(color_resource_format);
    }
  }

  uint32_t rts_remaining;
  uint32_t rt_index;

  // Eliminate other bound render targets if their EDRAM base conflicts with
  // another render target - it's an error in most host implementations to bind
  // the same render target into multiple slots, also the behavior would be
  // unpredictable if that happens.
  // Depth is considered the least important as it's earlier in the pipeline
  // (issues caused by color and depth render target collisions haven't been
  // found yet), but render targets with smaller index are considered more
  // important - specifically, because of the usage in the lighting pass of
  // 4D5307E6, which can be checked in the vertical look calibration sequence in
  // the beginning of the game: if render target 0 is removed in favor of 1, the
  // characters and the world will be too dark, like fully in shadow -
  // especially prominent on the helmet. This happens because the shader picks
  // between two render targets to write dynamically (though with a static, bool
  // constant condition), but all other state is set up in a way that implies
  // the same render target being bound twice. On Direct3D 9, if you don't write
  // to a color pixel shader output on the control flow that was taken, the
  // render target will not be written to. However, this has been relaxed in
  // Direct3D 10, where if the shader declares an output, it's assumed to be
  // always written (or with an undefined value otherwise).
  rts_remaining = depth_and_color_rts_used_bits & ~(uint32_t(1));
  while (rex::bit_scan_forward(rts_remaining, &rt_index)) {
    rts_remaining &= ~(uint32_t(1) << rt_index);
    uint32_t edram_base = edram_bases[rt_index];
    uint32_t rts_other_remaining =
        depth_and_color_rts_used_bits & (~((uint32_t(1) << (rt_index + 1)) - 1) | uint32_t(1));
    uint32_t rt_other_index;
    while (rex::bit_scan_forward(rts_other_remaining, &rt_other_index)) {
      rts_other_remaining &= ~(uint32_t(1) << rt_other_index);
      if (edram_bases[rt_other_index] == edram_base) {
        depth_and_color_rts_used_bits &= ~(uint32_t(1) << rt_other_index);
      }
    }
  }

  // Clear ownership transfers before adding any.
  if (!interlock_barrier_only) {
    for (size_t i = 0; i < rex::countof(last_update_transfers_); ++i) {
      last_update_transfers_[i].clear();
    }
  }

  if (!depth_and_color_rts_used_bits) {
    // Nothing to bind, don't waste time on things like memexport-only draws -
    // just check if old bindings can still be used.
    std::memset(last_update_used_render_targets_, 0, sizeof(last_update_used_render_targets_));
    if (are_accumulated_render_targets_valid_) {
      for (size_t i = 0; i < rex::countof(last_update_accumulated_render_targets_); ++i) {
        const RenderTarget* render_target = last_update_accumulated_render_targets_[i];
        if (!render_target) {
          continue;
        }
        RenderTargetKey rt_key = render_target->key();
        if (rt_key.pitch_tiles_at_32bpp != pitch_tiles_at_32bpp ||
            rt_key.msaa_samples != msaa_samples) {
          are_accumulated_render_targets_valid_ = false;
          break;
        }
      }
    }
    if (!are_accumulated_render_targets_valid_) {
      std::memset(last_update_accumulated_render_targets_, 0,
                  sizeof(last_update_accumulated_render_targets_));
    }
    return true;
  }

  // Estimate height used by render targets (for color for writes, for depth /
  // stencil for both reads and writes) from various sources.
  uint32_t height_used = draw_extent_estimator_.EstimateMaxY(
      interlock_barrier_only
          ? REXCVAR_GET(execute_unclipped_draw_vs_on_cpu_for_psi_render_backend)
          : true,
      vertex_shader);
  if (draw_samples_as_pixels_) {
    // In the guest's 4x pixels; the surface info is the 1x one - one more row covers the lower
    // samples of the last.
    height_used = (height_used + 1) << 1;
  }
  height_used = std::min(GetRenderTargetHeight(pitch_tiles_at_32bpp, msaa_samples), height_used);

  // Sorted by EDRAM base and then by index in the pipeline - for simplicity,
  // treat render targets placed closer to the end of the EDRAM as truncating
  // the previous one (and in case multiple render targets are placed at the
  // same EDRAM base, though normally this shouldn't happen, treat the color
  // ones as more important than the depth one, which may be not needed and just
  // a leftover if the draw, for instance, has depth / stencil happening to be
  // always passing and never writing with the current state, and also because
  // depth testing has to happen before the color is written). Overall it's
  // normal for estimated EDRAM ranges of render targets to intersect if drawing
  // without a viewport (as there's nothing to clamp the estimated height) and
  // multiple render targets are bound.
  std::pair<uint32_t, uint32_t> edram_bases_sorted[1 + xenos::kMaxColorRenderTargets];
  uint32_t edram_bases_sorted_count = 0;
  rts_remaining = depth_and_color_rts_used_bits;
  while (rex::bit_scan_forward(rts_remaining, &rt_index)) {
    rts_remaining &= ~(uint32_t(1) << rt_index);
    edram_bases_sorted[edram_bases_sorted_count++] =
        std::make_pair(edram_bases[rt_index], rt_index);
  }
  std::sort(edram_bases_sorted, edram_bases_sorted + edram_bases_sorted_count);
  // "As if it was 64bpp" (contribution of 32bpp render targets multiplied by 2,
  // and clamping for 32bpp render targets divides this by 2) because 32bpp
  // render targets can be combined with twice as long 64bpp render targets. An
  // example is the 4541099D menu background (1-sample 1152x720, or 1200x720
  // after rounding to tiles, with a 32bpp depth buffer at 0 requiring 675
  // tiles, and a 64bpp color buffer at 675 requiring 1350 tiles, but the
  // smallest distance between two render target bases is 675 tiles).
  uint32_t rt_max_distance_tiles_at_64bpp = xenos::EdramTileCount() * 2;
  if (REXCVAR_GET(mrt_edram_used_range_clamp_to_min) && edram_bases_sorted_count >= 2) {
    for (uint32_t i = 1; i < edram_bases_sorted_count; ++i) {
      const std::pair<uint32_t, uint32_t>& rt_base_prev = edram_bases_sorted[i - 1];
      rt_max_distance_tiles_at_64bpp =
          std::min(rt_max_distance_tiles_at_64bpp,
                   (edram_bases_sorted[i].first - rt_base_prev.first)
                       << (((rts_are_64bpp >> rt_base_prev.second) & 1) ^ 1));
    }
    // Clamp to the distance from the last render target to the first with
    // EDRAM addressing wrapping.
    const std::pair<uint32_t, uint32_t>& rt_base_last =
        edram_bases_sorted[edram_bases_sorted_count - 1];
    rt_max_distance_tiles_at_64bpp =
        std::min(rt_max_distance_tiles_at_64bpp,
                 (xenos::EdramTileCount() + edram_bases_sorted[0].first - rt_base_last.first)
                     << (((rts_are_64bpp >> rt_base_last.second) & 1) ^ 1));
  }

  // Make sure all the needed render targets are created, and gather lengths of
  // ranges used by each render target.
  RenderTargetKey rt_keys[1 + xenos::kMaxColorRenderTargets];
  RenderTarget* rts[1 + xenos::kMaxColorRenderTargets];
  uint32_t rt_lengths_tiles[1 + xenos::kMaxColorRenderTargets];
  uint32_t length_used_tiles_at_32bpp =
      ((height_used << uint32_t(msaa_samples >= xenos::MsaaSamples::k2X)) +
       (xenos::kEdramTileHeightSamples - 1)) /
      xenos::kEdramTileHeightSamples * pitch_tiles_at_32bpp;
  for (uint32_t i = 0; i < edram_bases_sorted_count; ++i) {
    const std::pair<uint32_t, uint32_t>& rt_base_index = edram_bases_sorted[i];
    uint32_t rt_base = rt_base_index.first;
    uint32_t rt_bit_index = rt_base_index.second;
    RenderTargetKey& rt_key = rt_keys[rt_bit_index];
    rt_key.base_tiles = rt_base;
    rt_key.pitch_tiles_at_32bpp = pitch_tiles_at_32bpp;
    rt_key.msaa_samples = msaa_samples;
    rt_key.is_depth = rt_bit_index == 0;
    rt_key.resource_format = resource_formats[rt_bit_index];
    if (!interlock_barrier_only) {
      RenderTarget* render_target = GetOrCreateRenderTarget(rt_key);
      if (!render_target) {
        return false;
      }
      rts[rt_bit_index] = render_target;
    }
    uint32_t rt_is_64bpp = (rts_are_64bpp >> rt_bit_index) & 1;
    // The last render target can occupy the EDRAM until the base of the first
    // render target (itself in case of 1 render target) with EDRAM addressing
    // wrapping.
    rt_lengths_tiles[i] = std::min(std::min(length_used_tiles_at_32bpp << rt_is_64bpp,
                                            rt_max_distance_tiles_at_64bpp >> (rt_is_64bpp ^ 1)),
                                   ((i + 1 < edram_bases_sorted_count)
                                        ? edram_bases_sorted[i + 1].first
                                        : (xenos::EdramTileCount() + edram_bases_sorted[0].first)) -
                                       rt_base);
    // At most the console's EDRAM, as the wrap above caps it there: with the
    // extended EDRAM a lone render target would otherwise own more than the
    // 2048-tile EDRAM buffer and its host image (see GetRenderTargetHeight).
    rt_lengths_tiles[i] = std::min(rt_lengths_tiles[i], xenos::kEdramTileCount);
  }

  if (interlock_barrier_only) {
    // Because a full pixel shader interlock barrier may clear the ownership map
    // (since it flushes all previous writes, and there's no need for another
    // barrier if an overlap is encountered later between pre-barrier and
    // post-barrier usages), check if any overlap requiring a barrier happens,
    // and then insert the barrier if needed.
    bool interlock_barrier_needed = false;
    for (uint32_t i = 0; i < edram_bases_sorted_count; ++i) {
      const std::pair<uint32_t, uint32_t>& rt_base_index = edram_bases_sorted[i];
      if (WouldOwnershipChangeRequireTransfers(rt_keys[rt_base_index.second], 0,
                                               rt_lengths_tiles[i])) {
        interlock_barrier_needed = true;
        break;
      }
    }
    if (interlock_barrier_needed) {
      RequestPixelShaderInterlockBarrier();
    }
  }

  // From now on ownership transfers should succeed for simplicity and
  // consistency, even if they fail in the implementation (just ignore that and
  // draw with whatever contents currently are in the render target in this
  // case).

  // A draw that overwrites a whole rectangle of the depth render target (games
  // clear with a rectangle drawn with depth ALWAYS) doesn't need the previous
  // contents of that rectangle: cut it out of the ownership transfers.
  last_update_transfer_cutout_valid_ = false;
  last_update_transfer_clear_ = false;
  last_update_transfer_clear_mask_ = 0;
  // Render targets drawn to now no longer hold a recorded constant (a new one
  // may be recorded below if this draw is a clear-like rectangle).
  if (!constant_depth_regions_.empty()) {
    // (With gpu_constant_transfers_as_color_clears, not by a draw that can only test depth:
    // Dark Souls II draws occlusion-style points on its 4x clear surface.)
    bool depth_write_possible =
        !REXCVAR_GET(gpu_constant_transfers_as_color_clears) ||
        normalized_depth_control.stencil_enable ||
        (normalized_depth_control.z_enable && normalized_depth_control.z_write_enable);
    for (uint32_t i = 0; i < edram_bases_sorted_count; ++i) {
      if (edram_bases_sorted[i].second == 0 && !depth_write_possible) {
        continue;
      }
      constant_depth_regions_.erase(rt_keys[edram_bases_sorted[i].second].key);
    }
  }
  // Why the cut-out wasn't made (for gpu_log_transfer_draws): 1 = not applicable
  // (off, no render targets, or a bound depth buffer not written with ALWAYS),
  // 2 = stencil not overwritten, 3 = may discard, 4x = coverage (40 + estimator
  // reason), 5 = empty after the scissor, 7 = face culling or a non-solid polygon mode,
  // 6 = a color target blended or not
  // fully written.
  uint32_t cutout_reject = 1;
  // A proven depth-only clear-like rectangle and its RB_DEPTH_CLEAR-style value
  // (gpu_redirect_msaa_depth_clears).
  bool clear_like = false;
  uint64_t clear_like_value = 0;
  // The first pixel row a single-rectangle draw covers (exact), for gpu_ownership_from_first_row; -1 unknown.
  int32_t draw_coverage_top = -1;
  uint32_t skip_overwritten_transfers = REXCVAR_GET(gpu_skip_overwritten_transfers);
  bool cutout_depth_used = (depth_and_color_rts_used_bits & 0b1) != 0;
  uint32_t cutout_color_used = depth_and_color_rts_used_bits >> 1;
  if (skip_overwritten_transfers && !interlock_barrier_only && is_rasterization_done &&
      depth_and_color_rts_used_bits &&
      (!cutout_depth_used ||
       (normalized_depth_control.z_enable && normalized_depth_control.z_write_enable &&
        normalized_depth_control.zfunc == xenos::CompareFunction::kAlways))) {
    // The transfer cut-out applies to every render target of the draw, so all
    // of them must be overwritten completely.
    bool overwrites = true;
    if (normalized_depth_control.stencil_enable) {
      auto stencil_overwritten = [&](xenos::CompareFunction func, xenos::StencilOp zpass,
                                     Register ref_mask_reg) {
        return func == xenos::CompareFunction::kAlways && zpass == xenos::StencilOp::kReplace &&
               regs.Get<reg::RB_STENCILREFMASK>(ref_mask_reg).stencilwritemask == 0xFF;
      };
      overwrites = stencil_overwritten(normalized_depth_control.stencilfunc,
                                       normalized_depth_control.stencilzpass,
                                       XE_GPU_REG_RB_STENCILREFMASK) &&
                   (!normalized_depth_control.backface_enable ||
                    stencil_overwritten(normalized_depth_control.stencilfunc_bf,
                                        normalized_depth_control.stencilzpass_bf,
                                        XE_GPU_REG_RB_STENCILREFMASK_BF));
    } else if (cutout_depth_used) {
      overwrites = skip_overwritten_transfers >= 2;
    }
    cutout_reject = overwrites ? 3 : 2;
    if (overwrites && cutout_color_used && !REXCVAR_GET(gpu_skip_overwritten_color_transfers)) {
      overwrites = false;
      cutout_reject = 6;
    }
    if (overwrites && cutout_color_used) {
      uint32_t color_remaining = cutout_color_used;
      uint32_t color_index;
      while (overwrites && rex::bit_scan_forward(color_remaining, &color_index)) {
        color_remaining &= ~(uint32_t(1) << color_index);
        auto blend = regs.Get<reg::RB_BLENDCONTROL>(
            reg::RB_BLENDCONTROL::rt_register_indices[color_index]);
        if (((normalized_color_mask >> (color_index * 4)) & 0b1111) != 0b1111 ||
            blend.color_srcblend != xenos::BlendFactor::kOne ||
            blend.color_destblend != xenos::BlendFactor::kZero ||
            blend.color_comb_fcn != xenos::BlendOp::kAdd ||
            blend.alpha_srcblend != xenos::BlendFactor::kOne ||
            blend.alpha_destblend != xenos::BlendFactor::kZero ||
            blend.alpha_comb_fcn != xenos::BlendOp::kAdd) {
          overwrites = false;
          cutout_reject = 6;
        }
      }
    }
    auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
    // Alpha test and alpha to coverage act on the pixel shader's alpha: with no pixel shader (a
    // depth-only EDRAM mode, or one not needed) the host draw has nothing to test and discards
    // nothing, whatever state the previous draws left (Dark Souls II's shadow-map clears keep an
    // alpha test enabled; MajulaRecomp found the same in its renderer).
    const bool pixel_shader_runs = update_pixel_shader_ != nullptr;
    if ((pixel_shader_runs && rb_colorcontrol.alpha_test_enable &&
         rb_colorcontrol.alpha_func != xenos::CompareFunction::kAlways) ||
        (pixel_shader_runs && rb_colorcontrol.alpha_to_mask_enable) ||
        (update_pixel_shader_ && update_pixel_shader_->kills_pixels()) ||
        (msaa_samples != xenos::MsaaSamples::k1X &&
         (regs[XE_GPU_REG_PA_SC_AA_MASK] & 0xFFFF) != 0xFFFF)) {
      if (overwrites) {
        cutout_reject = 3;
      }
      overwrites = false;
    }
    // Face culling may drop the rectangle (its winding isn't checked) and a polygon mode other
    // than solid fill doesn't cover it: neither overwrites anything for sure.
    auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    if (overwrites &&
        (pa_su_sc_mode_cntl.cull_front || pa_su_sc_mode_cntl.cull_back ||
         pa_su_sc_mode_cntl.poly_mode != xenos::PolygonModeEnable::kDisabled)) {
      overwrites = false;
      cutout_reject = 7;
    }
    if (overwrites) {
      draw_util::Scissor scissor;
      draw_util::GetScissor(regs, scissor);
      int32_t scissor_right = int32_t(scissor.offset[0] + scissor.extent[0]);
      int32_t scissor_bottom = int32_t(scissor.offset[1] + scissor.extent[1]);
      int32_t left, top, right, bottom;
      uint32_t rectangle_reject = 0;
      float constant_depth = -1.0f;
      if (!draw_extent_estimator_.EstimateRectangleCoverage(
              vertex_shader, int32_t(scissor.offset[0]), int32_t(scissor.offset[1]),
              scissor_right, scissor_bottom, left, top, right, bottom, &rectangle_reject,
              &constant_depth)) {
        cutout_reject = 40 + rectangle_reject;
      } else {
        left = std::max(left, int32_t(scissor.offset[0]));
        top = std::max(top, int32_t(scissor.offset[1]));
        right = std::min(right, scissor_right);
        bottom = std::min(bottom, scissor_bottom);
        draw_coverage_top = top;
        cutout_reject = 5;
        if (right > left && bottom > top) {
          cutout_reject = 0;
          last_update_transfer_cutout_.x_pixels = uint32_t(left);
          last_update_transfer_cutout_.y_pixels = uint32_t(top);
          last_update_transfer_cutout_.width_pixels = uint32_t(right - left);
          last_update_transfer_cutout_.height_pixels = uint32_t(bottom - top);
          last_update_transfer_cutout_valid_ = true;
          // Record the constant left by a depth-only clear-like rectangle.
          if (REXCVAR_GET(gpu_constant_transfers_as_clears) &&
              (depth_and_color_rts_used_bits == 0b1 ||
               ((depth_and_color_rts_used_bits & 0b1) &&
                REXCVAR_GET(gpu_constant_transfers_as_color_clears))) &&
              constant_depth >= 0.0f &&
              !(update_pixel_shader_ && update_pixel_shader_->writes_depth()) &&
              !pa_su_sc_mode_cntl.poly_offset_front_enable &&
              !pa_su_sc_mode_cntl.poly_offset_back_enable &&
              !pa_su_sc_mode_cntl.poly_offset_para_enable &&
              (normalized_depth_control.stencil_enable || skip_overwritten_transfers >= 2)) {
            const RenderTargetKey& depth_key = rt_keys[0];
            uint32_t depth_raw =
                depth_key.GetDepthFormat() == xenos::DepthRenderTargetFormat::kD24FS8
                    ? xenos::Float32To20e4(constant_depth, true)
                    : std::min(uint32_t(double(constant_depth) * double(0xFFFFFF) + 0.5),
                               uint32_t(0xFFFFFF));
            uint32_t stencil_raw = 0;
            if (normalized_depth_control.stencil_enable) {
              stencil_raw = regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK).stencilref;
            }
            constant_depth_regions_[depth_key.key] = {last_update_transfer_cutout_,
                                                      (depth_raw << 8) | stencil_raw,
                                                      bool(normalized_depth_control.stencil_enable)};
            clear_like = depth_and_color_rts_used_bits == 0b1;
            clear_like_value = (uint64_t(depth_raw) << 8) | stencil_raw;
            static uint32_t constant_logs = 0;
            if (constant_logs < 8) {
              ++constant_logs;
              REXGPU_INFO("Constant depth recorded: {} depth {} (raw {:06X}) stencil {} in "
                          "{},{} {}x{}",
                          depth_key.GetDebugName(), constant_depth, depth_raw, stencil_raw,
                          last_update_transfer_cutout_.x_pixels,
                          last_update_transfer_cutout_.y_pixels,
                          last_update_transfer_cutout_.width_pixels,
                          last_update_transfer_cutout_.height_pixels);
            }
          }
        }
      }
    }
  }

  // A depth-only draw needing transfers from a single source that holds a
  // recorded constant: transfer only outside the constant's rectangle, clear
  // inside it.
  if (!last_update_transfer_cutout_valid_ && REXCVAR_GET(gpu_constant_transfers_as_clears) &&
      !interlock_barrier_only && depth_and_color_rts_used_bits == 0b1 &&
      !constant_depth_regions_.empty()) {
    const RenderTargetKey& dest_key = rt_keys[0];
    uint32_t dest_start = dest_key.base_tiles;
    uint32_t dest_end = dest_start + rt_lengths_tiles[0];
    RenderTargetKey single_source;
    bool sources_ok = true;
    for (auto it = ownership_ranges_.begin(); sources_ok && it != ownership_ranges_.end(); ++it) {
      if (it->second.end_tiles <= dest_start || it->first >= dest_end) {
        continue;
      }
      RenderTargetKey owner = it->second.render_target;
      if (owner.IsEmpty() || owner == dest_key) {
        continue;
      }
      if (single_source.IsEmpty()) {
        single_source = owner;
      } else if (owner != single_source) {
        sources_ok = false;
      }
    }
    auto constant_it = (sources_ok && !single_source.IsEmpty())
                           ? constant_depth_regions_.find(single_source.key)
                           : constant_depth_regions_.end();
    if (constant_it != constant_depth_regions_.end() && single_source.is_depth &&
        single_source.base_tiles == dest_key.base_tiles &&
        single_source.pitch_tiles_at_32bpp == dest_key.pitch_tiles_at_32bpp &&
        dest_end <= xenos::EdramTileCount()) {
      // Same EDRAM layout: convert the rectangle through samples.
      auto samples_x = [](xenos::MsaaSamples m) {
        return uint32_t(1) << uint32_t(m >= xenos::MsaaSamples::k4X);
      };
      auto samples_y = [](xenos::MsaaSamples m) {
        return uint32_t(1) << uint32_t(m >= xenos::MsaaSamples::k2X);
      };
      const Transfer::Rectangle& source_rect = constant_it->second.rectangle;
      uint32_t ssx = samples_x(single_source.msaa_samples), ssy = samples_y(single_source.msaa_samples);
      uint32_t dsx = samples_x(dest_key.msaa_samples), dsy = samples_y(dest_key.msaa_samples);
      uint32_t x0 = (source_rect.x_pixels * ssx + dsx - 1) / dsx;
      uint32_t y0 = (source_rect.y_pixels * ssy + dsy - 1) / dsy;
      uint32_t x1 = ((source_rect.x_pixels + source_rect.width_pixels) * ssx) / dsx;
      uint32_t y1 = ((source_rect.y_pixels + source_rect.height_pixels) * ssy) / dsy;
      if (x1 > x0 && y1 > y0) {
        last_update_transfer_cutout_.x_pixels = x0;
        last_update_transfer_cutout_.y_pixels = y0;
        last_update_transfer_cutout_.width_pixels = x1 - x0;
        last_update_transfer_cutout_.height_pixels = y1 - y0;
        last_update_transfer_cutout_valid_ = true;
        std::memset(last_update_transfer_clear_values_, 0, sizeof(last_update_transfer_clear_values_));
        last_update_transfer_clear_values_[0] = constant_it->second.raw_value;
        last_update_transfer_clear_ = true;
        last_update_transfer_clear_mask_ = 0b1;
        cutout_reject = 7;
      }
    }
  }

  // gpu_constant_transfers_as_color_clears: the same for draws with color render targets, decided
  // per render target. A render target is cleared (inside the constant's rectangle) if the only
  // other owner of its range is a depth render target of the same base and pitch holding a
  // recorded constant; the others keep plain transfers.
  if (!last_update_transfer_cutout_valid_ && REXCVAR_GET(gpu_constant_transfers_as_clears) &&
      REXCVAR_GET(gpu_constant_transfers_as_color_clears) && !interlock_barrier_only &&
      !draw_samples_as_pixels_ && depth_and_color_rts_used_bits != 0b1 &&
      !constant_depth_regions_.empty()) {
    auto samples_x = [](xenos::MsaaSamples m) {
      return uint32_t(1) << uint32_t(m >= xenos::MsaaSamples::k4X);
    };
    auto samples_y = [](xenos::MsaaSamples m) {
      return uint32_t(1) << uint32_t(m >= xenos::MsaaSamples::k2X);
    };
    Transfer::Rectangle clear_rect = {};
    uint32_t clear_mask = 0;
    for (uint32_t i = 0; i < edram_bases_sorted_count; ++i) {
      uint32_t rt_bit_index = edram_bases_sorted[i].second;
      const RenderTargetKey& dest_key = rt_keys[rt_bit_index];
      uint32_t dest_start = dest_key.base_tiles;
      uint32_t dest_end = dest_start + rt_lengths_tiles[i];
      if (dest_key.Is64bpp() || dest_end > xenos::EdramTileCount()) {
        continue;
      }
      RenderTargetKey single_source;
      bool sources_ok = true;
      for (auto it = ownership_ranges_.begin(); sources_ok && it != ownership_ranges_.end(); ++it) {
        if (it->second.end_tiles <= dest_start || it->first >= dest_end) {
          continue;
        }
        RenderTargetKey owner = it->second.render_target;
        if (owner.IsEmpty() || owner == dest_key) {
          continue;
        }
        if (single_source.IsEmpty()) {
          single_source = owner;
        } else if (owner != single_source) {
          sources_ok = false;
        }
      }
      if (!sources_ok || single_source.IsEmpty()) {
        continue;
      }
      auto constant_it = constant_depth_regions_.find(single_source.key);
      if (constant_it == constant_depth_regions_.end() || !single_source.is_depth ||
          single_source.base_tiles != dest_key.base_tiles ||
          single_source.pitch_tiles_at_32bpp != dest_key.pitch_tiles_at_32bpp ||
          (!dest_key.is_depth && !constant_it->second.stencil_known)) {
        continue;
      }
      const Transfer::Rectangle& source_rect = constant_it->second.rectangle;
      uint32_t ssx = samples_x(single_source.msaa_samples), ssy = samples_y(single_source.msaa_samples);
      uint32_t dsx = samples_x(dest_key.msaa_samples), dsy = samples_y(dest_key.msaa_samples);
      uint32_t x0 = (source_rect.x_pixels * ssx + dsx - 1) / dsx;
      uint32_t y0 = (source_rect.y_pixels * ssy + dsy - 1) / dsy;
      uint32_t x1 = ((source_rect.x_pixels + source_rect.width_pixels) * ssx) / dsx;
      uint32_t y1 = ((source_rect.y_pixels + source_rect.height_pixels) * ssy) / dsy;
      if (x1 <= x0 || y1 <= y0) {
        continue;
      }
      // The clear must not touch tiles the destination already owns (its own data).
      uint32_t rect_start_tiles =
          dest_start + (y0 * dsy) / xenos::kEdramTileHeightSamples * dest_key.GetPitchTiles();
      uint32_t rect_end_tiles =
          dest_start + (y1 * dsy + (xenos::kEdramTileHeightSamples - 1)) /
                           xenos::kEdramTileHeightSamples * dest_key.GetPitchTiles();
      if (rect_end_tiles > xenos::EdramTileCount()) {
        continue;
      }
      bool dest_owns_in_rect = false;
      for (auto it = ownership_ranges_.begin(); it != ownership_ranges_.end(); ++it) {
        if (it->second.end_tiles <= rect_start_tiles || it->first >= rect_end_tiles) {
          continue;
        }
        if (it->second.render_target == dest_key) {
          dest_owns_in_rect = true;
          break;
        }
      }
      if (dest_owns_in_rect) {
        continue;
      }
      if (clear_mask && (clear_rect.x_pixels != x0 || clear_rect.y_pixels != y0 ||
                         clear_rect.width_pixels != x1 - x0 ||
                         clear_rect.height_pixels != y1 - y0)) {
        // One rectangle per update.
        continue;
      }
      if (!clear_mask) {
        clear_rect.x_pixels = x0;
        clear_rect.y_pixels = y0;
        clear_rect.width_pixels = x1 - x0;
        clear_rect.height_pixels = y1 - y0;
        std::memset(last_update_transfer_clear_values_, 0, sizeof(last_update_transfer_clear_values_));
      }
      // The EDRAM bits (depth << 8 | stencil), which the clear decodes as the destination's format.
      last_update_transfer_clear_values_[rt_bit_index] = constant_it->second.raw_value;
      clear_mask |= uint32_t(1) << rt_bit_index;
      static uint32_t color_clear_logs = 0;
      if (color_clear_logs < 16) {
        ++color_clear_logs;
        REXGPU_INFO("Constant transfer as clear: {} from {} raw {:08X} in {},{} {}x{}",
                    dest_key.GetDebugName(), single_source.GetDebugName(),
                    constant_it->second.raw_value, x0, y0, x1 - x0, y1 - y0);
      }
    }
    if (clear_mask) {
      last_update_transfer_cutout_ = clear_rect;
      last_update_transfer_cutout_valid_ = true;
      last_update_transfer_clear_ = true;
      last_update_transfer_clear_mask_ = clear_mask;
      cutout_reject = 8;
    }
  }

  // gpu_redirect_msaa_depth_clears: a clear rectangle on an MSAA alias whose 1x twin (same base,
  // pitch, format) exists is done on the twin: the twin takes the tile rows the rectangle covers
  // (transfers only for tiles it does not own yet, none inside the rectangle), the rectangle is
  // cleared there, and the draw is skipped. The samples of an MSAA pixel and the 1x pixels they
  // become are the same EDRAM bytes, so whoever reads these tiles later gets what the console has.
  if (clear_like && !interlock_barrier_only && !draw_samples_as_pixels_ &&
      depth_and_color_rts_used_bits == 0b1 && rt_keys[0].msaa_samples != xenos::MsaaSamples::k1X &&
      SupportsRedirectedDepthClears() && REXCVAR_GET(gpu_redirect_msaa_depth_clears)) {
    const RenderTargetKey& msaa_key = rt_keys[0];
    RenderTargetKey twin_key = msaa_key;
    twin_key.msaa_samples = xenos::MsaaSamples::k1X;
    auto twin_it = render_targets_.find(twin_key);
    if (twin_it != render_targets_.end() && twin_it->second) {
      uint32_t sx = uint32_t(1) << uint32_t(msaa_key.msaa_samples >= xenos::MsaaSamples::k4X);
      uint32_t sy = uint32_t(1) << uint32_t(msaa_key.msaa_samples >= xenos::MsaaSamples::k2X);
      Transfer::Rectangle rect;
      rect.x_pixels = last_update_transfer_cutout_.x_pixels * sx;
      rect.y_pixels = last_update_transfer_cutout_.y_pixels * sy;
      rect.width_pixels = last_update_transfer_cutout_.width_pixels * sx;
      rect.height_pixels = last_update_transfer_cutout_.height_pixels * sy;
      uint32_t row0 = rect.y_pixels / xenos::kEdramTileHeightSamples;
      uint32_t row1 = (rect.y_pixels + rect.height_pixels + xenos::kEdramTileHeightSamples - 1) /
                      xenos::kEdramTileHeightSamples;
      uint32_t pitch_tiles = twin_key.pitch_tiles_at_32bpp;
      uint32_t start_tiles = row0 * pitch_tiles;
      uint32_t length_tiles = (row1 - row0) * pitch_tiles;
      // Within the twin's image too: with the extended EDRAM the period check
      // no longer implies it (at most the console's 2048 tiles, and the
      // pitch's width).
      if (length_tiles && rect.width_pixels &&
          twin_key.base_tiles + start_tiles + length_tiles <= xenos::EdramTileCount() &&
          start_tiles + length_tiles <= xenos::kEdramTileCount &&
          rect.x_pixels + rect.width_pixels <= pitch_tiles * xenos::kEdramTileWidthSamples) {
        last_update_redirected_clear_transfers_.clear();
        ChangeOwnership(twin_key, start_tiles, length_tiles, &last_update_redirected_clear_transfers_,
                        &rect);
        constant_depth_regions_.erase(msaa_key.key);
        last_update_redirected_clear_rt_ = twin_it->second;
        last_update_redirected_clear_rect_ = rect;
        last_update_redirected_clear_value_ = clear_like_value;
        static uint32_t redirect_logs = 0;
        if (redirect_logs < 4) {
          ++redirect_logs;
          REXGPU_INFO("Depth clear on {} done on {} in {},{} {}x{} ({} tiles from row {}, {} transfers)",
                      msaa_key.GetDebugName(), twin_key.GetDebugName(), rect.x_pixels, rect.y_pixels,
                      rect.width_pixels, rect.height_pixels, length_tiles, row0,
                      last_update_redirected_clear_transfers_.size());
        }
        return true;
      }
    }
  }

  // gpu_ownership_from_first_row: tile rows above the first row the draw can write are left to
  // their owners. The host scissor bounds every write; a rectangle draw's own top (one row of
  // margin for rounding) bounds it further.
  uint32_t first_row_tiles = 0;
  if (REXCVAR_GET(gpu_ownership_from_first_row) && !interlock_barrier_only && !draw_samples_as_pixels_ &&
      msaa_samples == xenos::MsaaSamples::k1X) {
    draw_util::Scissor first_row_scissor;
    draw_util::GetScissor(regs, first_row_scissor);
    int32_t first_row = int32_t(first_row_scissor.offset[1]);
    if (draw_coverage_top >= 0) {
      first_row = std::max(first_row, draw_coverage_top - 1);
    }
    first_row_tiles = uint32_t(std::max(first_row, int32_t(0))) / xenos::kEdramTileHeightSamples;
  }
  // gpu_skip_slack_transfers: widen the overwritten rectangle to the tiles it covers partially
  // when all previous owners of the draw's range have another surface layout.
  if (REXCVAR_GET(gpu_skip_slack_transfers) && last_update_transfer_cutout_valid_ &&
      !last_update_transfer_clear_ && !interlock_barrier_only && !draw_samples_as_pixels_ &&
      msaa_samples == xenos::MsaaSamples::k1X) {
    uint32_t used_64bpp = rts_are_64bpp & depth_and_color_rts_used_bits;
    bool slack_ok = used_64bpp == 0 || used_64bpp == depth_and_color_rts_used_bits;
    bool slack_any_owner = false;
    for (uint32_t i = 0; slack_ok && i < edram_bases_sorted_count; ++i) {
      const RenderTargetKey& dest_key = rt_keys[edram_bases_sorted[i].second];
      uint32_t dest_start = dest_key.base_tiles;
      uint32_t dest_end = dest_start + rt_lengths_tiles[i];
      if (dest_end > xenos::EdramTileCount()) {
        slack_ok = false;
        break;
      }
      for (auto it = ownership_ranges_.begin(); it != ownership_ranges_.end(); ++it) {
        if (it->second.end_tiles <= dest_start || it->first >= dest_end) {
          continue;
        }
        RenderTargetKey owner = it->second.render_target;
        if (owner.IsEmpty() || owner == dest_key) {
          continue;
        }
        slack_any_owner = true;
        uint32_t base_distance = owner.base_tiles > dest_key.base_tiles
                                     ? owner.base_tiles - dest_key.base_tiles
                                     : dest_key.base_tiles - owner.base_tiles;
        if (owner.pitch_tiles_at_32bpp == dest_key.pitch_tiles_at_32bpp &&
            owner.Is64bpp() == dest_key.Is64bpp() && !(base_distance % dest_key.GetPitchTiles())) {
          // The same pixel grid (an MSAA or format alias, or rows of one surface): real data.
          slack_ok = false;
          break;
        }
      }
    }
    if (slack_ok && slack_any_owner) {
      uint32_t tile_width_pixels = xenos::kEdramTileWidthSamples >> uint32_t(used_64bpp != 0);
      uint32_t pitch_pixels = pitch_tiles_at_32bpp * xenos::kEdramTileWidthSamples;
      Transfer::Rectangle& cutout = last_update_transfer_cutout_;
      uint32_t x0 = cutout.x_pixels / tile_width_pixels * tile_width_pixels;
      uint32_t y0 = cutout.y_pixels / xenos::kEdramTileHeightSamples * xenos::kEdramTileHeightSamples;
      uint32_t x1 = std::min((cutout.x_pixels + cutout.width_pixels + (tile_width_pixels - 1)) /
                                 tile_width_pixels * tile_width_pixels,
                             std::max(pitch_pixels, cutout.x_pixels + cutout.width_pixels));
      uint32_t y1 = (cutout.y_pixels + cutout.height_pixels + (xenos::kEdramTileHeightSamples - 1)) /
                    xenos::kEdramTileHeightSamples * xenos::kEdramTileHeightSamples;
      cutout.x_pixels = x0;
      cutout.y_pixels = y0;
      cutout.width_pixels = x1 - x0;
      cutout.height_pixels = y1 - y0;
    }
  }
  for (uint32_t i = 0; i < edram_bases_sorted_count; ++i) {
    const std::pair<uint32_t, uint32_t>& rt_base_index = edram_bases_sorted[i];
    uint32_t rt_bit_index = rt_base_index.second;
    uint32_t start_tiles = std::min(
        first_row_tiles * (pitch_tiles_at_32bpp << ((rts_are_64bpp >> rt_bit_index) & 1)),
        rt_lengths_tiles[i]);
    if (start_tiles >= rt_lengths_tiles[i]) {
      continue;
    }
    ChangeOwnership(rt_keys[rt_bit_index], start_tiles, rt_lengths_tiles[i] - start_tiles,
                    interlock_barrier_only ? nullptr : &last_update_transfers_[rt_bit_index],
                    (last_update_transfer_cutout_valid_ &&
                     (!last_update_transfer_clear_ ||
                      ((last_update_transfer_clear_mask_ >> rt_bit_index) & 1)))
                        ? &last_update_transfer_cutout_
                        : nullptr);
  }

  // Diagnostic: each distinct ownership transfer situation once, with the state
  // of the draw that needs it (to find transfers the draw fully overwrites).
  if (REXCVAR_GET(gpu_log_transfer_draws) && !interlock_barrier_only) {
    static std::unordered_set<uint64_t> logged_situations;
    auto draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
    for (uint32_t i = 0; i < edram_bases_sorted_count; ++i) {
      uint32_t rt_bit_index = edram_bases_sorted[i].second;
      const std::vector<Transfer>& transfers = last_update_transfers_[rt_bit_index];
      if (transfers.empty() || logged_situations.size() >= 400) {
        continue;
      }
      uint32_t tiles = 0;
      std::string sources;
      uint64_t hash = rt_keys[rt_bit_index].key * 0x9E3779B97F4A7C15ull;
      for (const Transfer& transfer : transfers) {
        tiles += transfer.end_tiles - transfer.start_tiles;
        if (transfer.source) {
          sources += fmt::format(" [{} {}-{}]", transfer.source->key().GetDebugName(),
                                 transfer.start_tiles, transfer.end_tiles);
          hash = (hash ^ transfer.source->key().key) * 0x100000001B3ull;
        }
        hash = (hash ^ transfer.start_tiles ^ (uint64_t(transfer.end_tiles) << 16)) * 0x100000001B3ull;
      }
      hash = (hash ^ normalized_depth_control.value) * 0x100000001B3ull;
      hash = (hash ^ normalized_color_mask ^ (uint64_t(draw_initiator.value) << 32)) *
             0x100000001B3ull;
      if (!logged_situations.insert(hash).second) {
        continue;
      }
      REXGPU_INFO("Transfer into {} ({} tiles, rt range {}):{} | draw: prim {} indices {} "
                  "height {} | z en {} wr {} func {} | stencil en {} func {} ops {}/{}/{} | "
                  "color mask {:08X} | cut-out {} {},{} {}x{} (used bits {:X}) | colorcontrol "
                  "{:08X} aa mask {:04X} ps {}",
                  rt_keys[rt_bit_index].GetDebugName(), tiles, rt_lengths_tiles[i], sources,
                  uint32_t(draw_initiator.prim_type), uint32_t(draw_initiator.num_indices),
                  height_used, uint32_t(normalized_depth_control.z_enable),
                  uint32_t(normalized_depth_control.z_write_enable),
                  uint32_t(normalized_depth_control.zfunc),
                  uint32_t(normalized_depth_control.stencil_enable),
                  uint32_t(normalized_depth_control.stencilfunc),
                  uint32_t(normalized_depth_control.stencilfail),
                  uint32_t(normalized_depth_control.stencilzpass),
                  uint32_t(normalized_depth_control.stencilzfail), normalized_color_mask,
                  cutout_reject, last_update_transfer_cutout_.x_pixels,
                  last_update_transfer_cutout_.y_pixels, last_update_transfer_cutout_.width_pixels,
                  last_update_transfer_cutout_.height_pixels, depth_and_color_rts_used_bits,
                  regs[XE_GPU_REG_RB_COLORCONTROL], regs[XE_GPU_REG_PA_SC_AA_MASK] & 0xFFFF,
                  update_pixel_shader_ ? 1 : 0);
    }
  }

  if (interlock_barrier_only) {
    // No copying transfers or render target bindings - only needed the barrier.
    return true;
  }

  // Transfer sources are read by the GPU now although no draw binds them (idle render targets).
  for (const std::vector<Transfer>& transfers : last_update_transfers_) {
    for (const Transfer& transfer : transfers) {
      if (transfer.source) transfer.source->last_used_frame = frame_index_;
      if (transfer.host_depth_source) transfer.host_depth_source->last_used_frame = frame_index_;
    }
  }
  // If everything succeeded, update the used render targets.
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    last_update_used_render_targets_[i] =
        (depth_and_color_rts_used_bits & (uint32_t(1) << i)) ? rts[i] : nullptr;
    if (last_update_used_render_targets_[i]) last_update_used_render_targets_[i]->last_used_frame = frame_index_;
  }
  if (are_accumulated_render_targets_valid_) {
    // Check if the only re-enabling a previously bound render target.
    for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      RenderTarget* current_rt =
          (depth_and_color_rts_used_bits & (uint32_t(1) << i)) ? rts[i] : nullptr;
      const RenderTarget* accumulated_rt = last_update_accumulated_render_targets_[i];
      if (!accumulated_rt) {
        if (current_rt) {
          // Binding a totally new render target - won't keep the existing
          // render pass anyway, no much need to try to re-enable previously
          // disabled render targets in other slots as well, even though that
          // would be valid.
          are_accumulated_render_targets_valid_ = false;
          break;
        }
        // Append the new render target.
        last_update_accumulated_render_targets_[i] = current_rt;
        continue;
      }
      if (current_rt) {
        if (current_rt != accumulated_rt) {
          // Changing a render target in a slot.
          are_accumulated_render_targets_valid_ = false;
          break;
        }
      } else {
        RenderTargetKey accumulated_rt_key = accumulated_rt->key();
        if (accumulated_rt_key.pitch_tiles_at_32bpp != pitch_tiles_at_32bpp ||
            accumulated_rt_key.msaa_samples != msaa_samples) {
          // The previously bound render target is incompatible with the
          // current surface info.
          are_accumulated_render_targets_valid_ = false;
          break;
        }
      }
    }
    // Make sure the same render target isn't bound into two different slots
    // over time.
    for (uint32_t i = 1;
         are_accumulated_render_targets_valid_ && i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      const RenderTarget* render_target = last_update_accumulated_render_targets_[i];
      if (!render_target) {
        continue;
      }
      for (uint32_t j = 0; j < i; ++j) {
        if (last_update_accumulated_render_targets_[j] == render_target) {
          are_accumulated_render_targets_valid_ = false;
          break;
        }
      }
    }
  }
  if (!are_accumulated_render_targets_valid_) {
    std::memcpy(last_update_accumulated_render_targets_, last_update_used_render_targets_,
                sizeof(last_update_accumulated_render_targets_));
    are_accumulated_render_targets_valid_ = true;
  }

  return true;
}

uint32_t RenderTargetCache::GetLastUpdateBoundRenderTargets(
    uint32_t* depth_and_color_formats_out) const {
  if (GetPath() != Path::kHostRenderTargets) {
    if (depth_and_color_formats_out) {
      std::memset(depth_and_color_formats_out, 0,
                  sizeof(uint32_t) * (1 + xenos::kMaxColorRenderTargets));
    }
    return 0;
  }
  uint32_t rts_used = 0;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    const RenderTarget* render_target = last_update_accumulated_render_targets_[i];
    if (!render_target) {
      if (depth_and_color_formats_out) {
        depth_and_color_formats_out[i] = 0;
      }
      continue;
    }
    rts_used |= uint32_t(1) << i;
    if (depth_and_color_formats_out) {
      depth_and_color_formats_out[i] = render_target->key().resource_format;
    }
  }
  return rts_used;
}

uint32_t RenderTargetCache::GetRenderTargetHeight(uint32_t pitch_tiles_at_32bpp,
                                                  xenos::MsaaSamples msaa_samples) const {
  if (!pitch_tiles_at_32bpp) {
    return 0;
  }
  // Down to the beginning of the render target in the next 11-bit EDRAM
  // addressing period.
  // The console's tile count also with the extended EDRAM (gpu_edram_tiles):
  // host render targets stay as tall as on the console's EDRAM (not 2-4 times
  // the memory), and a resolve's tile offset within one stays below the
  // 2048-tile period of the EDRAM buffer and the resolve shaders.
  // With the extended EDRAM, rounded down: rounding up (69 rows of a 30-tile
  // pitch = 2070 tiles at 2336x1080) let one render target own more than the
  // 2048 tiles of the EDRAM buffer, which the host depth store / load index
  // without wrapping (a GPU fault on the Adreno 830) and the resolve shaders
  // wrap to the first row (frames of the wrong surface). 68 rows = 1088 px
  // still holds a 1080-high screen (2026-11-22).
  uint32_t tile_rows =
      xenos::EdramTileCount() > xenos::kEdramTileCount
          ? std::max(xenos::kEdramTileCount / pitch_tiles_at_32bpp, uint32_t(1))
          : (xenos::kEdramTileCount + (pitch_tiles_at_32bpp - 1)) / pitch_tiles_at_32bpp;
  // Clamp to the guest limit (tile padding should exceed it) and to the host
  // limit (tile padding mustn't exceed it).
  static_assert(!(xenos::kTexture2DCubeMaxWidthHeight % xenos::kEdramTileHeightSamples),
                "Maximum guest render target height is assumed to always be a multiple "
                "of an EDRAM tile height");
  uint32_t max_height_scaled = std::min(
      xenos::kTexture2DCubeMaxWidthHeight * draw_resolution_scale_y(), GetMaxRenderTargetHeight());
  uint32_t msaa_samples_y_log2 = uint32_t(msaa_samples >= xenos::MsaaSamples::k2X);
  uint32_t tile_height_samples_scaled = xenos::kEdramTileHeightSamples * draw_resolution_scale_y();
  tile_rows =
      std::min(tile_rows, (max_height_scaled << msaa_samples_y_log2) / tile_height_samples_scaled);
  // Optional cap for hosts that load / store whole attachments per render pass
  // (render_target_max_height): host images this tall at most, in pixels.
  int32_t max_height_cvar = REXCVAR_GET(render_target_max_height);
  if (max_height_cvar > 0) {
    uint32_t max_rows = std::max(
        ((uint32_t(max_height_cvar) * draw_resolution_scale_y() << msaa_samples_y_log2) +
         (tile_height_samples_scaled - 1)) /
            tile_height_samples_scaled,
        uint32_t(1));
    tile_rows = std::min(tile_rows, max_rows);
  }
  assert_not_zero(tile_rows);
  return tile_rows * (xenos::kEdramTileHeightSamples >> msaa_samples_y_log2);
}

void RenderTargetCache::GetHostDepthStoreRectangleInfo(
    const Transfer::Rectangle& transfer_rectangle, xenos::MsaaSamples msaa_samples,
    HostDepthStoreRectangleConstant& rectangle_constant_out, uint32_t& group_count_x_out,
    uint32_t& group_count_y_out) const {
  // Initialize to all bits zeroed.
  HostDepthStoreRectangleConstant rectangle_constant;
  // 8 pixels is the resolve granularity, both clearing and tile size are
  // aligned to 8.
  assert_zero(transfer_rectangle.x_pixels & 7);
  assert_zero(transfer_rectangle.y_pixels & 7);
  assert_zero(transfer_rectangle.width_pixels & 7);
  assert_zero(transfer_rectangle.height_pixels & 7);
  assert_not_zero(transfer_rectangle.width_pixels);
  rectangle_constant.x_pixels_div_8 = transfer_rectangle.x_pixels >> 3;
  rectangle_constant.y_pixels_div_8 = transfer_rectangle.y_pixels >> 3;
  rectangle_constant.width_pixels_div_8_minus_1 = (transfer_rectangle.width_pixels >> 3) - 1;
  rectangle_constant_out = rectangle_constant;
  // 1 thread group = 64x8 host samples.
  uint32_t pixel_size_x = draw_resolution_scale_x()
                          << uint32_t(msaa_samples >= xenos::MsaaSamples::k4X);
  uint32_t pixel_size_y = draw_resolution_scale_y()
                          << uint32_t(msaa_samples >= xenos::MsaaSamples::k2X);
  group_count_x_out = (transfer_rectangle.width_pixels * pixel_size_x + 63) >> 6;
  group_count_y_out = (transfer_rectangle.height_pixels * pixel_size_y) >> 3;
}

void RenderTargetCache::GetResolveCopyRectanglesToDump(
    uint32_t base, uint32_t row_length, uint32_t rows, uint32_t pitch,
    std::vector<ResolveCopyDumpRectangle>& rectangles_out) const {
  rectangles_out.clear();
  assert_true(row_length <= pitch);
  row_length = std::min(row_length, pitch);
  if (!row_length || !rows) {
    return;
  }
  auto get_rectangles_in_extent = [&](uint32_t extent_start, uint32_t extent_end,
                                      uint32_t range_local_offset) {
    // Collect render targets owning ranges within the specified rectangle. The
    // first render target in the range may be before the lower_bound, only
    // being in the range with its tail.
    auto it = ownership_ranges_.lower_bound(extent_start);
    if (it != ownership_ranges_.cbegin()) {
      auto it_pre = std::prev(it);
      if (it_pre->second.end_tiles > extent_start) {
        it = it_pre;
      }
    }
    for (; it != ownership_ranges_.cend(); ++it) {
      uint32_t range_global_start = std::max(it->first, extent_start);
      if (range_global_start >= extent_end) {
        break;
      }
      RenderTargetKey rt_key = it->second.render_target;
      if (rt_key.IsEmpty()) {
        continue;
      }
      // Merge with other render target ranges with the same current ownership,
      // but different depth ownership, since it's not relevant to resolving.
      while (it != ownership_ranges_.cend()) {
        auto it_next = std::next(it);
        if (it_next == ownership_ranges_.cend() || it_next->first >= extent_end ||
            it_next->second.render_target != rt_key) {
          break;
        }
        it = it_next;
      }

      uint32_t range_local_start =
          range_local_offset + std::max(range_global_start, extent_start) - base;
      uint32_t range_local_end =
          range_local_offset + std::min(it->second.end_tiles, extent_end) - base;
      assert_true(range_local_start < range_local_end);

      uint32_t rows_start = range_local_start / pitch;
      uint32_t rows_end = (range_local_end + (pitch - 1)) / pitch;
      uint32_t row_first_start = range_local_start - rows_start * pitch;
      if (row_first_start >= row_length) {
        // The first row starts within the pitch padding.
        if (rows_start + 1 < rows_end) {
          // Multiple rows - start at the second.
          ++rows_start;
          row_first_start = 0;
        } else {
          // Single row - nothing to dump.
          continue;
        }
      }

      auto it_rt = render_targets_.find(rt_key);
      assert_true(it_rt != render_targets_.cend());
      assert_not_null(it_rt->second);
      // Don't include pitch padding in the last row.
      rectangles_out.emplace_back(
          it_rt->second, rows_start, rows_end - rows_start, row_first_start,
          std::min(pitch - (rows_end * pitch - range_local_end), row_length));
    }
  };
  uint32_t resolve_area_end = base + (rows - 1) * pitch + row_length;
  get_rectangles_in_extent(base, std::min(resolve_area_end, xenos::EdramTileCount()), 0);
  if (resolve_area_end > xenos::EdramTileCount()) {
    // The resolve area goes to the next EDRAM addressing period.
    get_rectangles_in_extent(0, std::min(resolve_area_end & (xenos::EdramTileCount() - 1), base),
                             xenos::EdramTileCount());
  }
}

void RenderTargetCache::GetResolveCopyDispatchesToDump(
    uint32_t base, uint32_t row_length, uint32_t rows, uint32_t pitch,
    std::vector<ResolveCopyDumpRectangle>& rectangles_out,
    std::vector<ResolveCopyDispatch>& dispatches_out) const {
  GetResolveCopyRectanglesToDump(base, row_length, rows, pitch, rectangles_out);
  dispatches_out.clear();
  for (uint32_t rectangle_index = 0; rectangle_index < uint32_t(rectangles_out.size());
       ++rectangle_index) {
    const ResolveCopyDumpRectangle& rectangle = rectangles_out[rectangle_index];
    ResolveCopyDumpRectangle::Dispatch
        rectangle_dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
    uint32_t dispatch_count = rectangle.GetDispatches(pitch, row_length, rectangle_dispatches);
    for (uint32_t i = 0; i < dispatch_count; ++i) {
      dispatches_out.emplace_back(rectangle_index, rectangle_dispatches[i]);
    }
  }
}

bool RenderTargetCache::PrepareHostRenderTargetsResolveClear(
    const draw_util::ResolveInfo& resolve_info, Transfer::Rectangle& clear_rectangle_out,
    RenderTarget*& depth_render_target_out, std::vector<Transfer>& depth_transfers_out,
    RenderTarget*& color_render_target_out, std::vector<Transfer>& color_transfers_out) {
  assert_true(GetPath() == Path::kHostRenderTargets);
  constant_depth_regions_.clear();

  uint32_t pitch_tiles_at_32bpp;
  uint32_t base_offset_tiles_at_32bpp;
  xenos::MsaaSamples msaa_samples;
  if (resolve_info.IsClearingDepth()) {
    pitch_tiles_at_32bpp = resolve_info.depth_edram_info.pitch_tiles;
    base_offset_tiles_at_32bpp =
        resolve_info.depth_edram_base_tiles - resolve_info.depth_original_base;
    msaa_samples = resolve_info.depth_edram_info.msaa_samples;
  } else if (resolve_info.IsClearingColor()) {
    pitch_tiles_at_32bpp = resolve_info.color_edram_info.pitch_tiles;
    base_offset_tiles_at_32bpp =
        resolve_info.color_edram_base_tiles - resolve_info.color_original_base;
    if (resolve_info.color_edram_info.format_is_64bpp) {
      assert_zero(pitch_tiles_at_32bpp & 1);
      pitch_tiles_at_32bpp >>= 1;
      assert_zero(base_offset_tiles_at_32bpp & 1);
      base_offset_tiles_at_32bpp >>= 1;
    }
    msaa_samples = resolve_info.color_edram_info.msaa_samples;
  } else {
    return false;
  }
  assert_true(msaa_samples <= xenos::MsaaSamples::k4X);
  if (!pitch_tiles_at_32bpp) {
    return false;
  }
  uint32_t msaa_samples_x_log2 = uint32_t(msaa_samples >= xenos::MsaaSamples::k4X);
  uint32_t msaa_samples_y_log2 = uint32_t(msaa_samples >= xenos::MsaaSamples::k2X);
  if (pitch_tiles_at_32bpp > ((xenos::kTexture2DCubeMaxWidthHeight << msaa_samples_x_log2) +
                              (xenos::kEdramTileWidthSamples - 1)) /
                                 xenos::kEdramTileWidthSamples) {
    REXGPU_ERROR(
        "Surface pitch in 80-sample groups {} at {}x MSAA larger than the "
        "maximum texture width {} specified by the guest in a resolve",
        pitch_tiles_at_32bpp, uint32_t(1) << uint32_t(msaa_samples),
        xenos::kTexture2DCubeMaxWidthHeight);
    return false;
  }
  uint32_t pitch_pixels =
      pitch_tiles_at_32bpp * (xenos::kEdramTileWidthSamples >> msaa_samples_x_log2);
  uint32_t pitch_pixels_scaled = pitch_pixels * draw_resolution_scale_x();
  uint32_t max_render_target_width = GetMaxRenderTargetWidth();
  if (pitch_pixels_scaled > max_render_target_width) {
    // TODO(Triang3l): If really needed for some game on some device, clamp the
    // pitch the same way as explained in the comment in Update.
    REXGPU_ERROR(
        "Surface pitch aligned to EDRAM tiles and resolution-scaled {} larger "
        "than the maximum host render target width {} in a resolve",
        pitch_pixels_scaled, max_render_target_width);
    return false;
  }

  uint32_t render_target_height_pixels = GetRenderTargetHeight(pitch_tiles_at_32bpp, msaa_samples);
  uint32_t base_offset_rows_at_32bpp = base_offset_tiles_at_32bpp / pitch_tiles_at_32bpp;
  Transfer::Rectangle clear_rectangle;
  clear_rectangle.x_pixels =
      std::min((base_offset_tiles_at_32bpp - base_offset_rows_at_32bpp * pitch_tiles_at_32bpp) *
                       (xenos::kEdramTileWidthSamples >> msaa_samples_x_log2) +
                   (uint32_t(resolve_info.coordinate_info.edram_offset_x_div_8) << 3),
               pitch_pixels);
  clear_rectangle.y_pixels =
      std::min(base_offset_rows_at_32bpp * (xenos::kEdramTileHeightSamples >> msaa_samples_y_log2) +
                   (uint32_t(resolve_info.coordinate_info.edram_offset_y_div_8) << 3),
               render_target_height_pixels);
  clear_rectangle.width_pixels = std::min(uint32_t(resolve_info.coordinate_info.width_div_8) << 3,
                                          pitch_pixels - clear_rectangle.x_pixels);
  clear_rectangle.height_pixels = std::min(uint32_t(resolve_info.height_div_8) << 3,
                                           render_target_height_pixels - clear_rectangle.y_pixels);
  if (!clear_rectangle.width_pixels || !clear_rectangle.height_pixels) {
    // Outside the pitch / height (or initially specified as 0).
    return false;
  }

  // Change ownership of the tiles containing the area to be cleared, so the
  // up-to-date host render target for the cleared range will be the cleared
  // one.
  uint32_t clear_start_tiles_at_32bpp =
      ((clear_rectangle.y_pixels << msaa_samples_y_log2) / xenos::kEdramTileHeightSamples) *
          pitch_tiles_at_32bpp +
      (clear_rectangle.x_pixels << msaa_samples_x_log2) / xenos::kEdramTileWidthSamples;
  uint32_t clear_length_tiles_at_32bpp =
      (((clear_rectangle.y_pixels + clear_rectangle.height_pixels - 1) << msaa_samples_y_log2) /
       xenos::kEdramTileHeightSamples) *
          pitch_tiles_at_32bpp +
      ((clear_rectangle.x_pixels + clear_rectangle.width_pixels - 1) << msaa_samples_x_log2) /
          xenos::kEdramTileWidthSamples +
      1 - clear_start_tiles_at_32bpp;
  // Up to the range from the base in the current 11 tile index bits to the base
  // in the next 11 tile index bits after wrapping can be cleared.
  uint32_t depth_clear_start_tiles_base_relative = 0;
  uint32_t depth_clear_length_tiles = 0;
  if (resolve_info.IsClearingDepth()) {
    depth_clear_start_tiles_base_relative =
        std::min(clear_start_tiles_at_32bpp, xenos::EdramTileCount());
    depth_clear_length_tiles =
        std::min(clear_start_tiles_at_32bpp + clear_length_tiles_at_32bpp, xenos::EdramTileCount()) -
        depth_clear_start_tiles_base_relative;
  }
  uint32_t color_clear_start_tiles_base_relative = 0;
  uint32_t color_clear_length_tiles = 0;
  if (resolve_info.IsClearingColor()) {
    color_clear_start_tiles_base_relative =
        std::min(clear_start_tiles_at_32bpp << resolve_info.color_edram_info.format_is_64bpp,
                 xenos::EdramTileCount());
    color_clear_length_tiles = std::min((clear_start_tiles_at_32bpp + clear_length_tiles_at_32bpp)
                                            << resolve_info.color_edram_info.format_is_64bpp,
                                        xenos::EdramTileCount()) -
                               color_clear_start_tiles_base_relative;
  }
  if (depth_clear_length_tiles && color_clear_length_tiles) {
    // Prevent overlap - clear the depth only until the color, the color only
    // until the depth, in the current or the next 11 bits of the tile index.
    uint32_t depth_clear_start_tiles_wrapped =
        (resolve_info.depth_original_base + depth_clear_start_tiles_base_relative) &
        (xenos::EdramTileCount() - 1);
    uint32_t color_clear_start_tiles_wrapped =
        (resolve_info.color_original_base + color_clear_start_tiles_base_relative) &
        (xenos::EdramTileCount() - 1);
    depth_clear_length_tiles =
        std::min(depth_clear_length_tiles,
                 ((color_clear_start_tiles_wrapped < depth_clear_start_tiles_wrapped)
                      ? xenos::EdramTileCount()
                      : 0) +
                     color_clear_start_tiles_wrapped - depth_clear_start_tiles_wrapped);
    color_clear_length_tiles =
        std::min(color_clear_length_tiles,
                 ((depth_clear_start_tiles_wrapped < color_clear_start_tiles_wrapped)
                      ? xenos::EdramTileCount()
                      : 0) +
                     depth_clear_start_tiles_wrapped - color_clear_start_tiles_wrapped);
  }

  RenderTargetKey depth_render_target_key;
  RenderTarget* depth_render_target = nullptr;
  if (depth_clear_length_tiles) {
    depth_render_target_key.base_tiles = resolve_info.depth_original_base;
    depth_render_target_key.pitch_tiles_at_32bpp = pitch_tiles_at_32bpp;
    depth_render_target_key.msaa_samples = msaa_samples;
    depth_render_target_key.is_depth = 1;
    depth_render_target_key.resource_format = resolve_info.depth_edram_info.format;
    depth_render_target = GetOrCreateRenderTarget(depth_render_target_key);
    if (!depth_render_target) {
      // Failed to create the depth render target, don't clear it.
      depth_render_target_key = RenderTargetKey();
      depth_clear_length_tiles = 0;
    }
  }
  RenderTargetKey color_render_target_key;
  RenderTarget* color_render_target = nullptr;
  if (color_clear_length_tiles) {
    color_render_target_key.base_tiles = resolve_info.color_original_base;
    color_render_target_key.pitch_tiles_at_32bpp = pitch_tiles_at_32bpp;
    color_render_target_key.msaa_samples = msaa_samples;
    color_render_target_key.is_depth = 0;
    color_render_target_key.resource_format = uint32_t(GetColorResourceFormat(
        xenos::ColorRenderTargetFormat(resolve_info.color_edram_info.format)));
    color_render_target = GetOrCreateRenderTarget(color_render_target_key);
    if (!color_render_target) {
      // Failed to create the color render target, don't clear it.
      color_render_target_key = RenderTargetKey();
      color_clear_length_tiles = 0;
    }
  }
  if (!depth_clear_length_tiles && !color_clear_length_tiles) {
    // Complete overlap, or failed to create all the render targets.
    return false;
  }

  clear_rectangle_out = clear_rectangle;
  depth_render_target_out = depth_render_target;
  depth_transfers_out.clear();
  if (depth_render_target) {
    ChangeOwnership(depth_render_target_key, depth_clear_start_tiles_base_relative,
                    depth_clear_length_tiles, &depth_transfers_out, &clear_rectangle);
  }
  color_render_target_out = color_render_target;
  color_transfers_out.clear();
  if (color_render_target) {
    ChangeOwnership(color_render_target_key, color_clear_start_tiles_base_relative,
                    color_clear_length_tiles, &color_transfers_out, &clear_rectangle);
  }
  return true;
}

void RenderTargetCache::PixelShaderInterlockFullEdramBarrierPlaced() {
  assert_true(GetPath() == Path::kPixelShaderInterlock);
  // Clear ownership - any overlap of data written before the barrier is safe.
  OwnershipRange empty_range(xenos::EdramTileCount(), RenderTargetKey(), RenderTargetKey(),
                             RenderTargetKey());
  if (ownership_ranges_.size() == 1) {
    // Do not reallocate map elements if not needed (either nothing drawn since
    // the last barrier, or all of the EDRAM is owned by one render target).
    // The ownership map contains no gaps - the first element should always be
    // at 0.
    assert_true(!ownership_ranges_.begin()->first);
    OwnershipRange& all_edram_range = ownership_ranges_.begin()->second;
    assert_true(all_edram_range.end_tiles == xenos::EdramTileCount());
    all_edram_range = empty_range;
    return;
  }
  ownership_ranges_.clear();
  ownership_ranges_.emplace(0, empty_range);
}

RenderTargetCache::RenderTarget* RenderTargetCache::GetOrCreateRenderTarget(RenderTargetKey key) {
  assert_true(GetPath() == Path::kHostRenderTargets);
  auto it_rt = render_targets_.find(key);
  RenderTarget* render_target;
  if (it_rt != render_targets_.end()) {
    render_target = it_rt->second;
  } else {
    render_target = CreateRenderTarget(key);
    uint32_t width = key.GetWidth();
    uint32_t height = GetRenderTargetHeight(key.pitch_tiles_at_32bpp, key.msaa_samples);
    if (render_target) {
      REXGPU_DEBUG(
          "Created a {}x{} {}xMSAA {} render target with guest format {} at "
          "EDRAM base {}",
          width, height, uint32_t(1) << uint32_t(key.msaa_samples),
          key.is_depth ? "depth" : "color", static_cast<uint32_t>(key.resource_format),
          static_cast<uint32_t>(key.base_tiles));
    } else {
      REXGPU_ERROR(
          "Failed to create a {}x{} {}xMSAA {} render target with guest format "
          "{} at EDRAM base {}",
          width, height, uint32_t(1) << uint32_t(key.msaa_samples),
          key.is_depth ? "depth" : "color", static_cast<uint32_t>(key.resource_format),
          static_cast<uint32_t>(key.base_tiles));
    }
    // Insert even if failed to create, not to try to create again.
    render_targets_.emplace(key, render_target);
  }
  // Any lookup is a use (idle render targets): resolve clears take theirs here without a draw binding them, and
  // looked idle while used every frame (freed and recreated every 10 s, 2026-11-23).
  if (render_target) render_target->last_used_frame = frame_index_;
  return render_target;
}

bool RenderTargetCache::WouldOwnershipChangeRequireTransfers(RenderTargetKey dest,
                                                             uint32_t start_tiles_base_relative,
                                                             uint32_t length_tiles) const {
  // xenos::EdramTileCount() with length 0 is fine if both the start and the end
  // are clamped to xenos::EdramTileCount().
  assert_true(start_tiles_base_relative <= (xenos::EdramTileCount() - uint32_t(length_tiles != 0)));
  assert_true(length_tiles <= xenos::EdramTileCount());
  if (length_tiles == 0) {
    return false;
  }
  bool host_depth_encoding_different = dest.is_depth && GetPath() == Path::kHostRenderTargets &&
                                       IsHostDepthEncodingDifferent(dest.GetDepthFormat());
  auto would_require_transfers_in_extent = [&](uint32_t extent_start, uint32_t extent_end) -> bool {
    // The map contains consecutive ranges, merged if the adjacent ones are the
    // same. Find the range starting at >= the start. A portion of the range
    // preceding it may be intersecting the render target's range (or even fully
    // contain it).
    auto it = ownership_ranges_.lower_bound(extent_start);
    if (it != ownership_ranges_.begin()) {
      auto it_pre = std::prev(it);
      if (it_pre->second.end_tiles > extent_start) {
        it = it_pre;
      }
    }
    for (; it != ownership_ranges_.end(); ++it) {
      if (it->first >= extent_end) {
        // Outside the touched extent already.
        break;
      }
      if (it->second.IsOwnedBy(dest, host_depth_encoding_different)) {
        // Already owned by the needed render target - no need to transfer
        // anything.
        continue;
      }
      RenderTargetKey transfer_source = it->second.render_target;
      // Only perform the transfer when actually changing the latest owner, not
      // just the latest host depth owner - the transfer source is expected to
      // be different than the destination.
      if (!transfer_source.IsEmpty() && transfer_source != dest) {
        return true;
      }
    }
    return false;
  };
  // start_tiles_base_relative may already be in the next 11 bits - wrap the
  // start tile index to use the same code as if that was not the case.
  uint32_t start_tiles =
      (dest.base_tiles + start_tiles_base_relative) & (xenos::EdramTileCount() - 1);
  uint32_t end_tiles = start_tiles + length_tiles;
  if (would_require_transfers_in_extent(start_tiles, std::min(end_tiles, xenos::EdramTileCount()))) {
    return true;
  }
  if (end_tiles > xenos::EdramTileCount()) {
    // The check extent goes to the next EDRAM addressing period.
    if (would_require_transfers_in_extent(
            0, std::min(end_tiles & (xenos::EdramTileCount() - 1), start_tiles))) {
      return true;
    }
  }
  return false;
}

void RenderTargetCache::ChangeOwnership(RenderTargetKey dest, uint32_t start_tiles_base_relative,
                                        uint32_t length_tiles,
                                        std::vector<Transfer>* transfers_append_out,
                                        const Transfer::Rectangle* resolve_clear_cutout) {
  // xenos::EdramTileCount() with length 0 is fine if both the start and the end
  // are clamped to xenos::EdramTileCount().
  assert_true(start_tiles_base_relative <= (xenos::EdramTileCount() - uint32_t(length_tiles != 0)));
  assert_true(length_tiles <= xenos::EdramTileCount());
  if (length_tiles == 0) {
    return;
  }
  uint32_t dest_pitch_tiles = dest.GetPitchTiles();
  bool dest_is_64bpp = dest.Is64bpp();
  bool host_depth_encoding_different = dest.is_depth && GetPath() == Path::kHostRenderTargets &&
                                       IsHostDepthEncodingDifferent(dest.GetDepthFormat());
  auto change_ownership_in_extent = [&](uint32_t extent_start, uint32_t extent_end) {
    // The map contains consecutive ranges, merged if the adjacent ones are the
    // same. Find the range starting at >= the start. A portion of the range
    // preceding it may be intersecting the render target's range (or even fully
    // contain it) - split it into the untouched head and the claimed tail if
    // needed.
    auto it = ownership_ranges_.lower_bound(extent_start);
    if (it != ownership_ranges_.begin()) {
      auto it_pre = std::prev(it);
      if (it_pre->second.end_tiles > extent_start &&
          !it_pre->second.IsOwnedBy(dest, host_depth_encoding_different)) {
        // Different render target overlapping the range - split the head.
        ownership_ranges_.emplace(extent_start, it_pre->second);
        it_pre->second.end_tiles = extent_start;
        // Let the next loop do the transfer and needed merging and splitting
        // starting from the added tail.
        it = std::next(it_pre);
      }
    }
    while (it != ownership_ranges_.end()) {
      if (it->first >= extent_end) {
        // Outside the touched extent already.
        break;
      }
      if (it->second.IsOwnedBy(dest, host_depth_encoding_different)) {
        // Already owned by the needed render target - no need to transfer
        // anything.
        ++it;
        continue;
      }
      // Take over the current range. Handle the tail - may be outside the range
      // (split in this case) or within it.
      if (it->second.end_tiles > extent_end) {
        // Split the tail.
        ownership_ranges_.emplace(extent_end, it->second);
        it->second.end_tiles = extent_end;
      }
      if (transfers_append_out) {
        RenderTargetKey transfer_source = it->second.render_target;
        // Only perform the copying when actually changing the latest owner, not
        // just the latest host depth owner - the transfer source is expected to
        // be different than the destination.
        if (!transfer_source.IsEmpty() && transfer_source != dest) {
          uint32_t transfer_end_tiles = std::min(it->second.end_tiles, extent_end);
          if (!resolve_clear_cutout ||
              Transfer::GetRangeRectangles(it->first, transfer_end_tiles, dest.base_tiles,
                                           dest_pitch_tiles, dest.msaa_samples, dest_is_64bpp,
                                           nullptr, resolve_clear_cutout)) {
            RenderTargetKey transfer_host_depth_source =
                host_depth_encoding_different
                    ? it->second.GetHostDepthRenderTarget(dest.GetDepthFormat())
                    : RenderTargetKey();
            if (transfer_host_depth_source == transfer_source) {
              // Same render target, don't provide a separate host depth source.
              transfer_host_depth_source = RenderTargetKey();
            }
            if (!transfers_append_out->empty() &&
                transfers_append_out->back().end_tiles == it->first &&
                transfers_append_out->back().source->key() == transfer_source &&
                ((transfers_append_out->back().host_depth_source == nullptr) ==
                 transfer_host_depth_source.IsEmpty()) &&
                (transfer_host_depth_source.IsEmpty() ||
                 transfers_append_out->back().host_depth_source->key() ==
                     transfer_host_depth_source)) {
              // Extend the last transfer if, for example, transferring color,
              // but host depth is different.
              transfers_append_out->back().end_tiles = transfer_end_tiles;
            } else {
              auto transfer_source_rt_it = render_targets_.find(transfer_source);
              if (transfer_source_rt_it != render_targets_.end()) {
                assert_not_null(transfer_source_rt_it->second);
                auto transfer_host_depth_source_rt_it =
                    !transfer_host_depth_source.IsEmpty()
                        ? render_targets_.find(transfer_host_depth_source)
                        : render_targets_.end();
                if (transfer_host_depth_source.IsEmpty() ||
                    transfer_host_depth_source_rt_it != render_targets_.end()) {
                  assert_false(transfer_host_depth_source_rt_it != render_targets_.end() &&
                               !transfer_host_depth_source_rt_it->second);
                  transfers_append_out->emplace_back(
                      it->first, transfer_end_tiles, transfer_source_rt_it->second,
                      transfer_host_depth_source_rt_it != render_targets_.end()
                          ? transfer_host_depth_source_rt_it->second
                          : nullptr);
                }
              }
            }
          }
        }
      }
      // Claim the current range.
      it->second.render_target = dest;
      if (host_depth_encoding_different) {
        it->second.GetHostDepthRenderTarget(dest.GetDepthFormat()) = dest;
      }
      // Check if can merge with the next range after claiming.
      std::map<uint32_t, OwnershipRange>::iterator it_next;
      if (it != ownership_ranges_.end()) {
        it_next = std::next(it);
        if (it_next != ownership_ranges_.end() && it_next->second.AreOwnersSame(it->second)) {
          // Merge with the next range.
          it->second.end_tiles = it_next->second.end_tiles;
          auto it_after = std::next(it_next);
          ownership_ranges_.erase(it_next);
          it_next = it_after;
        }
      } else {
        it_next = ownership_ranges_.end();
      }
      // Check if can merge with the previous range after claiming and merging
      // with the next (thus obtaining the correct end pointer).
      if (it != ownership_ranges_.begin()) {
        auto it_prev = std::prev(it);
        if (it_prev->second.AreOwnersSame(it->second)) {
          it_prev->second.end_tiles = it->second.end_tiles;
          ownership_ranges_.erase(it);
        }
      }
      it = it_next;
    }
  };
  // start_tiles_base_relative may already be in the next 11 bits - wrap the
  // start tile index to use the same code as if that was not the case.
  uint32_t start_tiles =
      (dest.base_tiles + start_tiles_base_relative) & (xenos::EdramTileCount() - 1);
  uint32_t end_tiles = start_tiles + length_tiles;
  change_ownership_in_extent(start_tiles, std::min(end_tiles, xenos::EdramTileCount()));
  if (end_tiles > xenos::EdramTileCount()) {
    // The ownership change extent goes to the next EDRAM addressing period.
    change_ownership_in_extent(0, std::min(end_tiles & (xenos::EdramTileCount() - 1), start_tiles));
  }
}

}  // namespace rex::graphics
