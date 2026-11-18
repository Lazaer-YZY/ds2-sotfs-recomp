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
#include <cfloat>
#include <cstdint>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/format/ucode.h>
#include <rex/graphics/registers.h>
#include <rex/logging.h>
#include <rex/graphics/util/draw_extent_estimator.h>
#include <rex/graphics/xenos.h>
#include <rex/memory.h>
#include <rex/ui/graphics_util.h>

// Default true as in Xenia (the original DEFINE below): otherwise unclipped
// screen-space draws such as clears count as using the EDRAM down to the end of
// the render target, causing oversized ownership transfers. Lightning Returns on
// an Adreno 740: transfers 55 -> 47 per battle frame, GPU work -8-10%.
REXCVAR_DEFINE_BOOL(execute_unclipped_draw_vs_on_cpu, true, "GPU",
                    "Execute the vertex shader of unclipped draws (such as clears) on the CPU "
                    "to estimate how much of the EDRAM they use");

REXCVAR_DEFINE_BOOL(execute_unclipped_draw_vs_on_cpu_with_scissor, false, "GPU",
                    "Execute unclipped draw VS on CPU with scissor");

// DEFINE_bool(
//     execute_unclipped_draw_vs_on_cpu, true,
//     "Execute the vertex shader for draws with clipping disabled, primarily "
//     "screen-space draws (such as clears), on the CPU when possible to estimate "
//     "the extent of the EDRAM involved in the draw.\n"
//     "Enabling this may significantly improve GPU performance as otherwise up "
//     "to the entire EDRAM may be considered used in draws without clipping, "
//     "potentially resulting in spurious EDRAM range ownership transfer round "
//     "trips between host render targets.\n"
//     "Also, on hosts where certain render target formats have to be emulated in "
//     "a lossy way (for instance, 16-bit fixed-point via 16-bit floating-point), "
//     "this prevents corruption of other render targets located after the "
//     "current ones in the EDRAM by lossy range ownership transfers done for "
//     "those draws.",
//     "GPU");
// DEFINE_bool(
//     execute_unclipped_draw_vs_on_cpu_with_scissor, false,
//     "Don't restrict the usage of execute_unclipped_draw_vs_on_cpu to only "
//     "non-scissored draws (with the right and the bottom sides of the scissor "
//     "rectangle at 8192 or beyond) even though if the scissor rectangle is "
//     "present, it's usually sufficient for esimating the height of the render "
//     "target.\n"
//     "Enabling this may cause excessive processing of vertices on the CPU, as "
//     "some games draw rectangles (for their UI, for instance) without clipping, "
//     "but with a proper scissor rectangle.",
//     "GPU");

namespace rex::graphics {

void DrawExtentEstimator::PositionYExportSink::Export(ucode::ExportRegister export_register,
                                                      const float* value, uint32_t value_mask) {
  if (export_register == ucode::ExportRegister::kVSPosition) {
    if (value_mask & 0b0001) {
      position_x_ = value[0];
    }
    if (value_mask & 0b0100) {
      position_z_ = value[2];
    }
    if (value_mask & 0b0010) {
      position_y_ = value[1];
    }
    if (value_mask & 0b1000) {
      position_w_ = value[3];
    }
  } else if (export_register == ucode::ExportRegister::kVSPointSizeEdgeFlagKillVertex) {
    if (value_mask & 0b0001) {
      point_size_ = value[0];
    }
    if (value_mask & 0b0100) {
      vertex_kill_ = rex::memory::Reinterpret<uint32_t>(value[2]);
    }
  }
}

uint32_t DrawExtentEstimator::EstimateVertexMaxY(const Shader& vertex_shader) {
  SCOPE_profile_cpu_f("gpu");

  const RegisterFile& regs = register_file_;

  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if (!vgt_draw_initiator.num_indices) {
    return 0;
  }
  if (vgt_draw_initiator.source_select != xenos::SourceSelect::kDMA &&
      vgt_draw_initiator.source_select != xenos::SourceSelect::kAutoIndex) {
    // TODO(Triang3l): Support immediate indices.
    return xenos::kTexture2DCubeMaxWidthHeight;
  }

  // Not reproducing tessellation.
  if (xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, vgt_draw_initiator.prim_type) &&
      regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
          xenos::VGTOutputPath::kTessellationEnable) {
    return xenos::kTexture2DCubeMaxWidthHeight;
  }

  assert_true(vertex_shader.type() == xenos::ShaderType::kVertex);
  assert_true(vertex_shader.is_ucode_analyzed());
  if (!ShaderInterpreter::CanInterpretShader(vertex_shader)) {
    return xenos::kTexture2DCubeMaxWidthHeight;
  }

  auto vgt_dma_size = regs.Get<reg::VGT_DMA_SIZE>();
  union {
    const void* index_buffer;
    const uint16_t* index_buffer_16;
    const uint32_t* index_buffer_32;
  };
  xenos::Endian index_endian = vgt_dma_size.swap_mode;
  if (vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA) {
    xenos::IndexFormat index_format = vgt_draw_initiator.index_size;
    uint32_t index_buffer_base = regs[XE_GPU_REG_VGT_DMA_BASE];
    if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16) {
      // Handle the index endianness to same way as the PrimitiveProcessor.
      if (index_endian == xenos::Endian::k8in32) {
        index_endian = xenos::Endian::k8in16;
      } else if (index_endian == xenos::Endian::k16in32) {
        index_endian = xenos::Endian::kNone;
      }
      index_buffer_base &= ~uint32_t(sizeof(uint16_t) - 1);
    } else {
      assert_true(vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32);
      index_buffer_base &= ~uint32_t(sizeof(uint32_t) - 1);
    }
    index_buffer = memory_.TranslatePhysical(index_buffer_base);
  }
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  uint32_t reset_index = regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
  uint32_t index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  uint32_t min_index = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
  uint32_t max_index = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;

  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  float viewport_y_scale =
      pa_cl_vte_cntl.vport_y_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f;
  float viewport_y_offset =
      pa_cl_vte_cntl.vport_y_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f;

  int32_t point_vertex_min_diameter_float = 0;
  int32_t point_vertex_max_diameter_float = 0;
  float point_constant_radius_y = 0.0f;
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    point_vertex_min_diameter_float =
        rex::memory::Reinterpret<int32_t>(float(pa_su_point_minmax.min_size) * (2.0f / 16.0f));
    point_vertex_max_diameter_float =
        rex::memory::Reinterpret<int32_t>(float(pa_su_point_minmax.max_size) * (2.0f / 16.0f));
    point_constant_radius_y = float(regs.Get<reg::PA_SU_POINT_SIZE>().height) * (1.0f / 16.0f);
  }

  float max_y = -FLT_MAX;

  shader_interpreter_.SetShader(vertex_shader);

  PositionYExportSink position_y_export_sink;
  shader_interpreter_.SetExportSink(&position_y_export_sink);
  for (uint32_t i = 0; i < vgt_draw_initiator.num_indices; ++i) {
    uint32_t vertex_index;
    if (vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA) {
      if (i < vgt_dma_size.num_words) {
        if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16) {
          vertex_index = index_buffer_16[i];
        } else {
          vertex_index = index_buffer_32[i];
        }
        // The Xenos only uses 24 bits of the index (reset_indx is 24-bit).
        vertex_index = xenos::GpuSwap(vertex_index, index_endian) & 0xFFFFFF;
      } else {
        vertex_index = 0;
      }
      if (pa_su_sc_mode_cntl.multi_prim_ib_ena && vertex_index == reset_index) {
        continue;
      }
    } else {
      assert_true(vgt_draw_initiator.source_select == xenos::SourceSelect::kAutoIndex);
      vertex_index = i;
    }
    vertex_index =
        std::min(max_index, std::max(min_index, (vertex_index + index_offset) & 0xFFFFFF));

    position_y_export_sink.Reset();

    shader_interpreter_.temp_registers()[0] = float(vertex_index);
    shader_interpreter_.Execute();

    if (position_y_export_sink.vertex_kill().has_value() &&
        (position_y_export_sink.vertex_kill().value() & ~(UINT32_C(1) << 31))) {
      continue;
    }
    if (!position_y_export_sink.position_y().has_value()) {
      continue;
    }
    float vertex_y = position_y_export_sink.position_y().value();
    if (!pa_cl_vte_cntl.vtx_xy_fmt) {
      if (!position_y_export_sink.position_w().has_value()) {
        continue;
      }
      vertex_y /= position_y_export_sink.position_w().value();
    }

    vertex_y = vertex_y * viewport_y_scale + viewport_y_offset;

    if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
      float point_radius_y;
      if (position_y_export_sink.point_size().has_value()) {
        // Vertex-specified diameter. Clamped effectively as a signed integer in
        // the hardware, -NaN, -Infinity ... -0 to the minimum, +Infinity, +NaN
        // to the maximum.
        point_radius_y = 0.5f * rex::memory::Reinterpret<float>(std::min(
                                    point_vertex_max_diameter_float,
                                    std::max(point_vertex_min_diameter_float,
                                             rex::memory::Reinterpret<int32_t>(
                                                 position_y_export_sink.point_size().value()))));
      } else {
        // Constant radius.
        point_radius_y = point_constant_radius_y;
      }
      vertex_y += point_radius_y;
    }

    // std::max is `a < b ? b : a`, thus in case of NaN, the first argument is
    // always returned - max_y, which is initialized to a normalized value.
    max_y = std::max(max_y, vertex_y);
  }
  shader_interpreter_.SetExportSink(nullptr);

  int32_t max_y_24p8 = ui::FloatToD3D11Fixed16p8(max_y);
  // 16p8 range is -32768 to 32767+255/256, but it's stored as uint32_t here,
  // as 24p8, so overflowing up to -8388608 to 8388608+255/256 is safe. The
  // range of the window offset plus the half-pixel offset is -16384 to 16384.5,
  // so it's safe to add both - adding it will neither move the 16p8 clamping
  // bounds -32768 and 32767+255/256 into the 0...8192 screen space range, nor
  // cause 24p8 overflow.
  if (regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero) {
    max_y_24p8 += 128;
  }
  if (pa_su_sc_mode_cntl.vtx_window_offset_enable) {
    max_y_24p8 += regs.Get<reg::PA_SC_WINDOW_OFFSET>().window_y_offset * 256;
  }
  // Top-left rule - .5 exclusive without MSAA, 1. exclusive with MSAA.
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  return (uint32_t(std::max(int32_t(0), max_y_24p8)) +
          ((rb_surface_info.msaa_samples == xenos::MsaaSamples::k1X) ? 127 : 255)) >>
         8;
}

bool DrawExtentEstimator::EstimateRectangleCoverage(const Shader& vertex_shader,
                                                    int32_t bounds_left, int32_t bounds_top,
                                                    int32_t bounds_right, int32_t bounds_bottom,
                                                    int32_t& left_out, int32_t& top_out,
                                                    int32_t& right_out, int32_t& bottom_out,
                                                    uint32_t* reject_out,
                                                    float* constant_depth_out) {
  if (constant_depth_out) {
    // Not constant (or unknown) unless found otherwise.
    *constant_depth_out = -1.0f;
  }
  // Reject reasons: 1 primitive / index source / shader, 2 vertex kill or no
  // position, 3 w or z outside the clip volume, 4 not an axis-aligned
  // rectangle, 5 empty.
  uint32_t reject_dummy;
  uint32_t& reject = reject_out ? *reject_out : reject_dummy;
  reject = 1;
  const RegisterFile& regs = register_file_;
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  uint32_t vertex_count;
  bool is_triangle = false;
  switch (vgt_draw_initiator.prim_type) {
    case xenos::PrimitiveType::kTriangleList:
      vertex_count = 3;
      is_triangle = true;
      break;
    case xenos::PrimitiveType::kRectangleList:
      vertex_count = 3;
      break;
    case xenos::PrimitiveType::kQuadList:
    case xenos::PrimitiveType::kTriangleFan:
    case xenos::PrimitiveType::kTriangleStrip:
      vertex_count = 4;
      break;
    default:
      return false;
  }
  if (vgt_draw_initiator.num_indices != vertex_count ||
      (vgt_draw_initiator.source_select != xenos::SourceSelect::kDMA &&
       vgt_draw_initiator.source_select != xenos::SourceSelect::kAutoIndex) ||
      xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, vgt_draw_initiator.prim_type) ||
      !ShaderInterpreter::CanInterpretShader(vertex_shader)) {
    return false;
  }
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  bool clipped = !pa_cl_clip_cntl.clip_disable;
  auto vgt_dma_size = regs.Get<reg::VGT_DMA_SIZE>();
  const void* index_buffer = nullptr;
  xenos::Endian index_endian = vgt_dma_size.swap_mode;
  bool index_32 = vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32;
  if (vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA) {
    if (vgt_dma_size.num_words < vertex_count) {
      return false;
    }
    uint32_t index_buffer_base = regs[XE_GPU_REG_VGT_DMA_BASE];
    if (!index_32) {
      if (index_endian == xenos::Endian::k8in32) {
        index_endian = xenos::Endian::k8in16;
      } else if (index_endian == xenos::Endian::k16in32) {
        index_endian = xenos::Endian::kNone;
      }
      index_buffer_base &= ~uint32_t(1);
    } else {
      index_buffer_base &= ~uint32_t(3);
    }
    index_buffer = memory_.TranslatePhysical(index_buffer_base);
  }
  uint32_t index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  uint32_t min_index = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
  uint32_t max_index = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  float x_scale =
      pa_cl_vte_cntl.vport_x_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE) : 1.0f;
  float x_offset =
      pa_cl_vte_cntl.vport_x_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET) : 0.0f;
  float y_scale =
      pa_cl_vte_cntl.vport_y_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f;
  float y_offset =
      pa_cl_vte_cntl.vport_y_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f;
  // To pixel-center-at-0.5 coordinates.
  float center_offset =
      regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f : 0.0f;
  float window_x = 0.0f, window_y = 0.0f;
  if (pa_su_sc_mode_cntl.vtx_window_offset_enable) {
    auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    window_x = float(pa_sc_window_offset.window_x_offset);
    window_y = float(pa_sc_window_offset.window_y_offset);
  }
  float xs[4], ys[4], zs[4];
  bool zs_known = true;
  float z_scale =
      pa_cl_vte_cntl.vport_z_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE) : 1.0f;
  float z_offset =
      pa_cl_vte_cntl.vport_z_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET) : 0.0f;
  reject = 2;
  shader_interpreter_.SetShader(vertex_shader);
  PositionYExportSink sink;
  shader_interpreter_.SetExportSink(&sink);
  bool valid = true;
  for (uint32_t i = 0; i < vertex_count; ++i) {
    uint32_t vertex_index = i;
    if (index_buffer) {
      vertex_index = index_32 ? static_cast<const uint32_t*>(index_buffer)[i]
                              : static_cast<const uint16_t*>(index_buffer)[i];
      vertex_index = xenos::GpuSwap(vertex_index, index_endian) & 0xFFFFFF;
    }
    vertex_index =
        std::min(max_index, std::max(min_index, (vertex_index + index_offset) & 0xFFFFFF));
    sink.Reset();
    shader_interpreter_.temp_registers()[0] = float(vertex_index);
    shader_interpreter_.Execute();
    if (sink.vertex_kill().has_value() && (sink.vertex_kill().value() & ~(UINT32_C(1) << 31))) {
      reject = 20;
      valid = false;
      break;
    }
    if (!sink.position_x().has_value() || !sink.position_y().has_value()) {
      reject = 21 + uint32_t(sink.position_x().has_value());
      valid = false;
      break;
    }
    float x = sink.position_x().value(), y = sink.position_y().value();
    if (!pa_cl_vte_cntl.vtx_xy_fmt || clipped) {
      if (!sink.position_w().has_value() || !(sink.position_w().value() > 0.0f)) {
        reject = sink.position_w().has_value() ? 3 : 24;
        valid = false;
        break;
      }
    }
    if (clipped) {
      // The whole rectangle must be inside the near / far planes (otherwise
      // it's clipped by depth, not only to the viewport).
      if (!sink.position_z().has_value()) {
        reject = 23;
        valid = false;
        break;
      }
      float z = sink.position_z().value(), w = sink.position_w().value();
      if (!(z <= w && z >= (pa_cl_clip_cntl.dx_clip_space_def ? 0.0f : -w))) {
        reject = 3;
        valid = false;
        break;
      }
    }
    if (!pa_cl_vte_cntl.vtx_xy_fmt) {
      x /= sink.position_w().value();
      y /= sink.position_w().value();
    }
    xs[i] = x * x_scale + x_offset + center_offset + window_x;
    ys[i] = y * y_scale + y_offset + center_offset + window_y;
    if (sink.position_z().has_value()) {
      float z = sink.position_z().value();
      if (!pa_cl_vte_cntl.vtx_z_fmt) {
        if (sink.position_w().has_value() && sink.position_w().value() != 0.0f) {
          z /= sink.position_w().value();
        } else {
          zs_known = false;
        }
      }
      zs[i] = z * z_scale + z_offset;
    } else {
      zs_known = false;
    }
    if (!std::isfinite(xs[i]) || !std::isfinite(ys[i])) {
      reject = 25;
      valid = false;
      break;
    }
  }
  shader_interpreter_.SetExportSink(nullptr);
  if (!valid) {
    return false;
  }
  if (is_triangle) {
    // The triangle must contain the centers of the corner pixels of the
    // bounds (limited to the viewport for clipped draws).
    float bx0 = float(bounds_left), by0 = float(bounds_top);
    float bx1 = float(bounds_right), by1 = float(bounds_bottom);
    if (clipped) {
      bx0 = std::max(bx0, x_offset - std::abs(x_scale) + center_offset + window_x);
      bx1 = std::min(bx1, x_offset + std::abs(x_scale) + center_offset + window_x);
      by0 = std::max(by0, y_offset - std::abs(y_scale) + center_offset + window_y);
      by1 = std::min(by1, y_offset + std::abs(y_scale) + center_offset + window_y);
    }
    int32_t left = int32_t(std::ceil(bx0)), top = int32_t(std::ceil(by0));
    int32_t right = int32_t(std::floor(bx1)), bottom = int32_t(std::floor(by1));
    reject = 5;
    if (right <= left || bottom <= top) {
      return false;
    }
    float area = (xs[1] - xs[0]) * (ys[2] - ys[0]) - (ys[1] - ys[0]) * (xs[2] - xs[0]);
    if (area == 0.0f) {
      return false;
    }
    float corner_xs[4] = {left + 0.5f, right - 0.5f, left + 0.5f, right - 0.5f};
    float corner_ys[4] = {top + 0.5f, top + 0.5f, bottom - 0.5f, bottom - 0.5f};
    for (uint32_t c = 0; c < 4; ++c) {
      for (uint32_t e = 0; e < 3; ++e) {
        uint32_t a = e, b = (e + 1) % 3;
        float edge = (xs[b] - xs[a]) * (corner_ys[c] - ys[a]) -
                     (ys[b] - ys[a]) * (corner_xs[c] - xs[a]);
        if (area > 0.0f ? edge < 0.0f : edge > 0.0f) {
          reject = 6;
          return false;
        }
      }
    }
    left_out = left;
    top_out = top;
    right_out = right;
    bottom_out = bottom;
    if (constant_depth_out && zs_known && zs[0] == zs[1] && zs[0] == zs[2] && zs[0] >= 0.0f &&
        zs[0] <= 1.0f) {
      *constant_depth_out = zs[0];
    }
    reject = 0;
    return true;
  }
  // The vertex opposite to v0 (for the bowtie check below).
  uint32_t opposite_of_0 = (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kQuadList ||
                            vgt_draw_initiator.prim_type == xenos::PrimitiveType::kTriangleFan)
                               ? 2
                               : 3;
  if (vertex_count == 3) {
    // Rectangle list: the longest edge is the diagonal, the fourth corner is
    // its endpoints minus the right-angle corner (as Xenia expands them).
    float d01 = (xs[1] - xs[0]) * (xs[1] - xs[0]) + (ys[1] - ys[0]) * (ys[1] - ys[0]);
    float d02 = (xs[2] - xs[0]) * (xs[2] - xs[0]) + (ys[2] - ys[0]) * (ys[2] - ys[0]);
    float d12 = (xs[2] - xs[1]) * (xs[2] - xs[1]) + (ys[2] - ys[1]) * (ys[2] - ys[1]);
    uint32_t corner = (d12 >= d01 && d12 >= d02) ? 0 : (d02 >= d01 ? 1 : 2);
    uint32_t a = corner == 0 ? 1 : 0, b = corner == 2 ? 1 : 2;
    xs[3] = xs[a] + xs[b] - xs[corner];
    ys[3] = ys[a] + ys[b] - ys[corner];
    // The derived corner is opposite to the right-angle one by construction.
    opposite_of_0 = UINT32_MAX;
  }
  float x0 = std::min(std::min(xs[0], xs[1]), std::min(xs[2], xs[3]));
  float x1 = std::max(std::max(xs[0], xs[1]), std::max(xs[2], xs[3]));
  float y0 = std::min(std::min(ys[0], ys[1]), std::min(ys[2], ys[3]));
  float y1 = std::max(std::max(ys[0], ys[1]), std::max(ys[2], ys[3]));
  // Every vertex must be a distinct corner of the bounding box (an axis-aligned
  // rectangle).
  uint32_t corners = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    bool at_x0 = xs[i] == x0, at_x1 = xs[i] == x1, at_y0 = ys[i] == y0, at_y1 = ys[i] == y1;
    if (at_x0 == at_x1 || at_y0 == at_y1) {
      reject = 4;
      return false;
    }
    corners |= uint32_t(1) << (uint32_t(at_x1) | (uint32_t(at_y1) << 1));
  }
  if (corners != 0b1111) {
    reject = 4;
    return false;
  }
  // The two triangles must not form a bowtie: the vertex opposite to v0 is v3
  // for rectangle lists (derived) and triangle strips, v2 for quad lists.
  if (opposite_of_0 != UINT32_MAX &&
      (xs[opposite_of_0] == xs[0] || ys[opposite_of_0] == ys[0])) {
    reject = 4;
    return false;
  }
  if (clipped) {
    // Clipping to the viewport (the guard band only avoids clipping the
    // primitive, it's still rasterized only within the viewport).
    float viewport_x0 = x_offset - std::abs(x_scale) + center_offset + window_x;
    float viewport_x1 = x_offset + std::abs(x_scale) + center_offset + window_x;
    float viewport_y0 = y_offset - std::abs(y_scale) + center_offset + window_y;
    float viewport_y1 = y_offset + std::abs(y_scale) + center_offset + window_y;
    x0 = std::max(x0, viewport_x0);
    x1 = std::min(x1, viewport_x1);
    y0 = std::max(y0, viewport_y0);
    y1 = std::min(y1, viewport_y1);
  }
  reject = 5;
  // Pixel i is covered if its center i + 0.5 is in [x0, x1).
  left_out = int32_t(std::ceil(x0 - 0.5f));
  right_out = int32_t(std::ceil(x1 - 0.5f));
  top_out = int32_t(std::ceil(y0 - 0.5f));
  bottom_out = int32_t(std::ceil(y1 - 0.5f));
  if (right_out <= left_out || bottom_out <= top_out) {
    return false;
  }
  if (constant_depth_out && zs_known && zs[0] == zs[1] && zs[0] == zs[2] &&
      (vertex_count == 3 || zs[0] == zs[3]) && zs[0] >= 0.0f && zs[0] <= 1.0f) {
    *constant_depth_out = zs[0];
  }
  reject = 0;
  return true;
}

uint32_t DrawExtentEstimator::EstimateMaxY(bool try_to_estimate_vertex_max_y,
                                           const Shader& vertex_shader) {
  SCOPE_profile_cpu_f("gpu");

  const RegisterFile& regs = register_file_;

  auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
  int32_t window_y_offset = pa_sc_window_offset.window_y_offset;

  // Scissor.
  auto pa_sc_window_scissor_br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
  int32_t scissor_bottom = int32_t(pa_sc_window_scissor_br.br_y);
  bool scissor_window_offset = !regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>().window_offset_disable;
  if (scissor_window_offset) {
    scissor_bottom += window_y_offset;
  }
  auto pa_sc_screen_scissor_br = regs.Get<reg::PA_SC_SCREEN_SCISSOR_BR>();
  scissor_bottom = std::min(scissor_bottom, int32_t(pa_sc_screen_scissor_br.br_y));
  uint32_t max_y = uint32_t(std::max(scissor_bottom, int32_t(0)));

  if (regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable) {
    // Actual extent from the vertices.
    if (try_to_estimate_vertex_max_y && REXCVAR_GET(execute_unclipped_draw_vs_on_cpu)) {
      bool estimate_vertex_max_y;
      if (REXCVAR_GET(execute_unclipped_draw_vs_on_cpu_with_scissor)) {
        estimate_vertex_max_y = true;
      } else {
        estimate_vertex_max_y = false;
        if (scissor_bottom >= xenos::kTexture2DCubeMaxWidthHeight) {
          // Handle just the usual special 8192x8192 case in Direct3D 9 - 8192
          // may be a normal render target height (80x8192 is well within the
          // EDRAM size, for instance), no need to process the vertices on the
          // CPU in this case.
          int32_t scissor_right = int32_t(pa_sc_window_scissor_br.br_x);
          if (scissor_window_offset) {
            scissor_right += pa_sc_window_offset.window_x_offset;
          }
          scissor_right = std::min(scissor_right, int32_t(pa_sc_screen_scissor_br.br_x));
          if (scissor_right >= xenos::kTexture2DCubeMaxWidthHeight) {
            estimate_vertex_max_y = true;
          }
        }
      }
      if (estimate_vertex_max_y) {
        max_y = std::min(max_y, EstimateVertexMaxY(vertex_shader));
      }
    }
  } else {
    // Viewport. Though the Xenos itself doesn't have an implicit viewport
    // scissor (it's set by Direct3D 9 when a viewport is used), on hosts, it
    // usually exists and can't be disabled.
    auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
    float viewport_bottom = 0.0f;
    // First calculate all the integer.0 or integer.5 offsetting exactly at full
    // precision.
    if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
      viewport_bottom += float(window_y_offset);
    }
    if (regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero) {
      viewport_bottom += 0.5f;
    }
    // Then apply the floating-point viewport offset.
    if (pa_cl_vte_cntl.vport_y_offset_ena) {
      viewport_bottom += regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET);
    }
    viewport_bottom += pa_cl_vte_cntl.vport_y_scale_ena
                           ? std::abs(regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE))
                           : 1.0f;
    // Using floor, or, rather, truncation (because maxing with zero anyway)
    // similar to how viewport scissoring behaves on real AMD, Intel and Nvidia
    // GPUs on Direct3D 12 (but not WARP), also like in
    // draw_util::GetHostViewportInfo.
    // max(0.0f, viewport_bottom) to drop NaN and < 0 - max picks the first
    // argument in the !(a < b) case (always for NaN), min as float (max_y is
    // well below 2^24) to safely drop very large values.
    max_y = uint32_t(std::min(float(max_y), std::max(0.0f, viewport_bottom)));
  }

  return max_y;
}

}  // namespace rex::graphics
