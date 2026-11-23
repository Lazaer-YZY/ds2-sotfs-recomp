#pragma once
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

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/hash.h>
#include <rex/platform.h>
#include <rex/thread.h>
#include <rex/graphics/primitive_processor.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/vulkan/render_target_cache.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/vulkan/api.h>

namespace rex::graphics::vulkan {

class VulkanCommandProcessor;

// TODO(Triang3l): Create a common base for both the Vulkan and the Direct3D
// implementations.
class VulkanPipelineCache {
 public:
  static constexpr size_t kLayoutUIDEmpty = 0;

  class PipelineLayoutProvider {
   public:
    virtual ~PipelineLayoutProvider() {}
    virtual VkPipelineLayout GetPipelineLayout() const = 0;
    // With pipeline libraries: layouts holding only the sets the pre-rasterization / fragment
    // shader libraries use (VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT).
    virtual VkPipelineLayout GetPreRasterizationLibraryLayout() const { return VK_NULL_HANDLE; }
    virtual VkPipelineLayout GetFragmentLibraryLayout() const { return VK_NULL_HANDLE; }

   protected:
    PipelineLayoutProvider() = default;
  };

  VulkanPipelineCache(VulkanCommandProcessor& command_processor, const RegisterFile& register_file,
                      VulkanRenderTargetCache& render_target_cache,
                      VkShaderStageFlags guest_shader_vertex_stages);
  ~VulkanPipelineCache();

  bool Initialize();
  void Shutdown();
  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking);
  void ShutdownShaderStorage();
  void EndSubmission();

  VulkanShader* LoadShader(xenos::ShaderType shader_type, const uint32_t* host_address,
                           uint32_t dword_count);
  // Analyze shader microcode on the translator thread.
  void AnalyzeShaderUcode(Shader& shader) { shader.AnalyzeUcode(ucode_disasm_buffer_); }

  // Retrieves the shader modification for the current state. The shader must
  // have microcode analyzed.
  SpirvShaderTranslator::Modification GetCurrentVertexShaderModification(
      const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
      uint32_t interpolator_mask, bool ps_param_gen_used) const;
  SpirvShaderTranslator::Modification GetCurrentPixelShaderModification(
      const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
      reg::RB_DEPTHCONTROL normalized_depth_control) const;

  bool EnsureShadersTranslated(VulkanShader::VulkanTranslation* vertex_shader,
                               VulkanShader::VulkanTranslation* pixel_shader);
  // TODO(Triang3l): Return a deferred creation handle.
  bool ConfigurePipeline(
      VulkanShader::VulkanTranslation* vertex_shader, VulkanShader::VulkanTranslation* pixel_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
      VulkanRenderTargetCache::RenderPassKey render_pass_key, VkPipeline& pipeline_out,
      const PipelineLayoutProvider*& pipeline_layout_out, void** pipeline_handle_out = nullptr,
      uint32_t texture_sign_classes = 0, uint32_t vertex_texture_sign_classes = 0);
  bool IsCreatingPipelines() const;
  // Whether ConfigurePipeline translates new shaders on the creation threads
  // (vulkan_async_shader_translation): the command processor must then not translate them first.
  bool TranslatesShadersOnCreationThreads() const;
  // Texture sign classes (2 bits per slot, SpirvShaderTranslator::kTextureSignClassSlots slots) of
  // the textures bound to the first fetch constants of a pixel shader's used texture mask.
  // Vertex shaders: slots = SpirvShaderTranslator::kVertexTextureSignClassSlots.
  template <typename GetSwizzledSigns>
  static uint32_t GetTextureSignClasses(
      uint32_t pixel_shader_used_texture_mask, GetSwizzledSigns get_swizzled_signs,
      uint32_t slots = SpirvShaderTranslator::kTextureSignClassSlotsDefault) {
    uint32_t classes = 0;
    uint32_t remaining = pixel_shader_used_texture_mask;
    for (uint32_t slot = 0; slot < slots && remaining; ++slot) {
      uint32_t fetch_constant_index = uint32_t(__builtin_ctz(remaining));
      remaining &= remaining - 1;
      classes |= uint32_t(SpirvShaderTranslator::GetTextureSignClass(
                     get_swizzled_signs(fetch_constant_index)))
                 << (SpirvShaderTranslator::kTextureSignClassBits * slot);
    }
    return classes;
  }
  void GetPipelineAndLayoutByHandle(void* handle, VkPipeline& pipeline_out,
                                    const PipelineLayoutProvider*& pipeline_layout_out,
                                    bool* is_placeholder_out = nullptr) const;

  // The driver pipeline cache file for the title in the cache root.
  static std::filesystem::path GetDriverPipelineCachePath(const std::filesystem::path& cache_root,
                                                          uint32_t title_id);
  // Validates the file and merges it into the device's pipeline cache. Returns the merged byte
  // count, or 0 if there was no usable file. No other thread may create pipelines meanwhile.
  static size_t MergeDriverPipelineCacheFile(const ui::vulkan::VulkanDevice* vulkan_device,
                                             const std::filesystem::path& path);

 private:
  REXPACKEDSTRUCT(ShaderStoredHeader, {
    uint64_t ucode_data_hash;

    uint32_t ucode_dword_count : 31;
    xenos::ShaderType type : 1;

    static constexpr uint32_t kVersion = 0x20201219;
  });

  enum class PipelineGeometryShader : uint32_t {
    kNone,
    kPointList,
    kRectangleList,
    kQuadList,
  };

  enum class PipelinePrimitiveTopology : uint32_t {
    kPointList,
    kLineList,
    kLineStrip,
    kTriangleList,
    kTriangleStrip,
    kTriangleFan,
    kLineListWithAdjacency,
    kPatchList,
  };

  enum class PipelinePolygonMode : uint32_t {
    kFill,
    kLine,
    kPoint,
  };

  enum class PipelineBlendFactor : uint32_t {
    kZero,
    kOne,
    kSrcColor,
    kOneMinusSrcColor,
    kDstColor,
    kOneMinusDstColor,
    kSrcAlpha,
    kOneMinusSrcAlpha,
    kDstAlpha,
    kOneMinusDstAlpha,
    kConstantColor,
    kOneMinusConstantColor,
    kConstantAlpha,
    kOneMinusConstantAlpha,
    kSrcAlphaSaturate,
  };

  // Update PipelineDescription::kVersion if anything is changed!
  REXPACKEDSTRUCT(PipelineRenderTarget, {
    PipelineBlendFactor src_color_blend_factor : 4;  // 4
    PipelineBlendFactor dst_color_blend_factor : 4;  // 8
    xenos::BlendOp color_blend_op : 3;               // 11
    PipelineBlendFactor src_alpha_blend_factor : 4;  // 15
    PipelineBlendFactor dst_alpha_blend_factor : 4;  // 19
    xenos::BlendOp alpha_blend_op : 3;               // 22
    uint32_t color_write_mask : 4;                   // 26
  });

  REXPACKEDSTRUCT(PipelineDescription, {
    uint64_t vertex_shader_hash;
    uint64_t vertex_shader_modification;
    // 0 if no pixel shader.
    uint64_t pixel_shader_hash;
    uint64_t pixel_shader_modification;
    VulkanRenderTargetCache::RenderPassKey render_pass_key;

    // Shader stages.
    PipelineGeometryShader geometry_shader : 2;  // 2
    // Input assembly.
    PipelinePrimitiveTopology primitive_topology : 3;  // 5
    uint32_t primitive_restart : 1;                    // 6
    xenos::TessellationMode tessellation_mode : 2;     // 8
    // Rasterization.
    uint32_t depth_clamp_enable : 1;       // 9
    PipelinePolygonMode polygon_mode : 2;  // 11
    uint32_t cull_front : 1;               // 12
    uint32_t cull_back : 1;                // 13
    uint32_t front_face_clockwise : 1;     // 14
    uint32_t rasterizer_discard : 1;       // 15
    // Depth / stencil.
    uint32_t depth_write_enable : 1;                      // 16
    xenos::CompareFunction depth_compare_op : 3;          // 19
    uint32_t stencil_test_enable : 1;                     // 20
    xenos::StencilOp stencil_front_fail_op : 3;           // 23
    xenos::StencilOp stencil_front_pass_op : 3;           // 26
    xenos::StencilOp stencil_front_depth_fail_op : 3;     // 29
    xenos::CompareFunction stencil_front_compare_op : 3;  // 32
    xenos::StencilOp stencil_back_fail_op : 3;            // 35

    xenos::StencilOp stencil_back_pass_op : 3;           // 3
    xenos::StencilOp stencil_back_depth_fail_op : 3;     // 6
    xenos::CompareFunction stencil_back_compare_op : 3;  // 9
    uint32_t sample_rate_shading : 1;                    // 10
    // Pixel shader texture sign classes (see SpirvShaderTranslator::TextureSignClass), 2 bits per
    // slot; 0 = all from the system constants at runtime (and the same hash as before this field
    // existed, as the padding was zero).
    uint32_t texture_sign_classes : 16;  // 26
    // With sign classes: every texture of the pixel shader has exponent adjustment 0
    // (SpirvShaderTranslator::kTextureSignClassExpAdjustZeroBit). One of the 3 bits left in the
    // 8 bytes (35 + 27 = 62): stored descriptions keep their size and read it as 0.
    uint32_t texture_exp_adjust_zero : 1;  // 27
    // Sign class of the ninth texture (SpirvShaderTranslator::kTextureSignClassSlots = 9, used with
    // vulkan_texture_sign_class_ninth_slot): the last 2 bits of the 8 bytes; stored descriptions
    // read it as 0 (runtime signs), as before it existed.
    uint32_t texture_sign_class_slot8 : 2;  // 29
    // NO more bits here: the struct is packed, its bit-fields are contiguous, so a 65th bit makes
    // every stored description a byte longer and the whole pipeline storage "corrupted"
    // (2026-11-11). Changing the layout needs a kVersion bump.

    // Filled only for the attachments present in the render pass object.
    PipelineRenderTarget render_targets[xenos::kMaxColorRenderTargets];

    // Including all the padding, for a stable hash.
    static constexpr uint32_t kVersion = 0x20260228;
    // The sign classes as passed around (SpirvShaderTranslator::kTextureSignClassesExpAdjustZero
    // in bit 31).
    uint32_t TextureSignClassesWord() const {
      return texture_sign_classes | (uint32_t(texture_sign_class_slot8) << 16) |
             (texture_exp_adjust_zero ? (UINT32_C(1) << 31) : 0);
    }
    PipelineDescription() {
      Reset();
    }
    PipelineDescription(const PipelineDescription& description) {
      std::memcpy(this, &description, sizeof(*this));
    }
    PipelineDescription& operator=(const PipelineDescription& description) {
      std::memcpy(this, &description, sizeof(*this));
      return *this;
    }
    bool operator==(const PipelineDescription& description) const {
      return std::memcmp(this, &description, sizeof(*this)) == 0;
    }
    void Reset() {
      std::memset(this, 0, sizeof(*this));
    }
    uint64_t GetHash() const {
      return XXH3_64bits(this, sizeof(*this));
    }
    struct Hasher {
      size_t operator()(const PipelineDescription& description) const {
        return size_t(description.GetHash());
      }
    };
  });
  REXPACKEDSTRUCT(PipelineStoredDescription, {
    uint64_t description_hash;
    PipelineDescription description;
  });

 public:
  // Fixed-function state of a pipeline linked from libraries, set as dynamic state after binding.
  struct PipelineDynamicState {
    VkCullModeFlags cull_mode = VK_CULL_MODE_NONE;
    VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkBool32 depth_test_enable = VK_FALSE;
    VkBool32 depth_write_enable = VK_FALSE;
    VkCompareOp depth_compare_op = VK_COMPARE_OP_ALWAYS;
    VkBool32 stencil_test_enable = VK_FALSE;
    VkStencilOp stencil_front_ops[3] = {};  // fail, pass, depth fail
    VkCompareOp stencil_front_compare_op = VK_COMPARE_OP_ALWAYS;
    VkStencilOp stencil_back_ops[3] = {};
    VkCompareOp stencil_back_compare_op = VK_COMPARE_OP_ALWAYS;
    VkBool32 depth_bias_enable = VK_FALSE;
    VkBool32 rasterizer_discard_enable = VK_FALSE;
    VkPolygonMode polygon_mode = VK_POLYGON_MODE_FILL;
    VkBool32 depth_clamp_enable = VK_FALSE;
    uint32_t color_attachment_count = 0;
    VkBool32 color_blend_enable[xenos::kMaxColorRenderTargets] = {};
    VkColorBlendEquationEXT color_blend_equation[xenos::kMaxColorRenderTargets] = {};
    VkColorComponentFlags color_write_mask[xenos::kMaxColorRenderTargets] = {};
  };
  // nullptr if the pipeline behind the handle has all its state baked in.
  const PipelineDynamicState* GetPipelineDynamicState(void* handle) const;

 private:
  struct Pipeline {
    std::atomic<VkPipeline> pipeline{VK_NULL_HANDLE};
    // Valid once uses_dynamic_state is set (before the pipeline is published).
    PipelineDynamicState dynamic_state;
    std::atomic<bool> uses_dynamic_state{false};
    // The layouts are owned by the VulkanCommandProcessor, and must not be
    // destroyed by it while the pipeline cache is active.
    std::atomic<const PipelineLayoutProvider*> pipeline_layout{nullptr};
    std::atomic<bool> is_placeholder{false};
    Pipeline() = default;
    Pipeline(const PipelineLayoutProvider* pipeline_layout_provider)
        : pipeline_layout(pipeline_layout_provider) {}
    Pipeline(const Pipeline& other)
        : pipeline(other.pipeline.load(std::memory_order_acquire)),
          dynamic_state(other.dynamic_state),
          uses_dynamic_state(other.uses_dynamic_state.load(std::memory_order_acquire)),
          pipeline_layout(other.pipeline_layout.load(std::memory_order_acquire)),
          is_placeholder(other.is_placeholder.load(std::memory_order_acquire)) {}
    Pipeline& operator=(const Pipeline& other) {
      if (this == &other) {
        return *this;
      }
      pipeline.store(other.pipeline.load(std::memory_order_acquire), std::memory_order_release);
      pipeline_layout.store(other.pipeline_layout.load(std::memory_order_acquire),
                            std::memory_order_release);
      is_placeholder.store(other.is_placeholder.load(std::memory_order_acquire),
                           std::memory_order_release);
      return *this;
    }
  };

  // Description that can be passed from the command processor thread to the
  // creation threads, with everything needed from caches pre-looked-up.
  struct PipelineCreationArguments {
    uint8_t priority = 0;
    std::pair<const PipelineDescription, Pipeline>* pipeline = nullptr;
    const PipelineLayoutProvider* pipeline_layout = nullptr;
    // Guest shader translation (VS or TES depending on host vertex type).
    const VulkanShader::VulkanTranslation* vertex_shader = nullptr;
    const VulkanShader::VulkanTranslation* pixel_shader = nullptr;
    // Non-guest stages for tessellation.
    VkShaderModule tessellation_vertex_shader = VK_NULL_HANDLE;
    VkShaderModule tessellation_control_shader = VK_NULL_HANDLE;
    uint32_t tessellation_patch_control_points = 0;
    VkShaderModule geometry_shader = VK_NULL_HANDLE;
    // VK_NULL_HANDLE when dynamic rendering is used.
    VkRenderPass render_pass = VK_NULL_HANDLE;
  };
  struct PipelineCreationArgumentsPriorityComparator {
    bool operator()(const PipelineCreationArguments& a, const PipelineCreationArguments& b) const {
      return a.priority < b.priority;
    }
  };

  union TessellationControlShaderKey {
    uint32_t key;
    struct {
      Shader::HostVertexShaderType host_vertex_shader_type : Shader::kHostVertexShaderTypeBitCount;
      xenos::TessellationMode tessellation_mode : 2;
    };

    TessellationControlShaderKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const TessellationControlShaderKey& key) const {
        return std::hash<uint32_t>{}(key.key);
      }
    };
    bool operator==(const TessellationControlShaderKey& other_key) const {
      return key == other_key.key;
    }
  };

  union GeometryShaderKey {
    uint32_t key;
    struct {
      PipelineGeometryShader type : 2;
      // Interpolators passed through, at locations equal to their indices (with packed
      // locations, the lowest bits: a dense mask gives the packed locations).
      uint32_t interpolator_mask : 16;
      uint32_t user_clip_plane_count : 3;
      uint32_t user_clip_plane_cull : 1;
      uint32_t has_vertex_kill_and : 1;
      uint32_t has_point_size : 1;
      uint32_t has_point_coordinates : 1;
      // PA_CL_CLIP_CNTL::ps_ucp_mode for point primitives.
      uint32_t point_ps_ucp_mode : 2;
    };

    GeometryShaderKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const GeometryShaderKey& key) const {
        return std::hash<uint32_t>{}(key.key);
      }
    };
    bool operator==(const GeometryShaderKey& other_key) const { return key == other_key.key; }
    bool operator!=(const GeometryShaderKey& other_key) const { return !(*this == other_key); }
  };

  VulkanShader* LoadShader(xenos::ShaderType shader_type, const uint32_t* host_address,
                           uint32_t dword_count, uint64_t data_hash);

  // Can be called from multiple threads.
  bool TranslateAnalyzedShader(SpirvShaderTranslator& translator,
                               VulkanShader::VulkanTranslation& translation);

  void WritePipelineRenderTargetDescription(reg::RB_BLENDCONTROL blend_control, uint32_t write_mask,
                                            PipelineRenderTarget& render_target_out) const;
  bool GetCurrentStateDescription(
      const VulkanShader::VulkanTranslation* vertex_shader,
      const VulkanShader::VulkanTranslation* pixel_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
      VulkanRenderTargetCache::RenderPassKey render_pass_key,
      PipelineDescription& description_out) const;

  // Whether the pipeline for the given description is supported by the device.
  bool ArePipelineRequirementsMet(const PipelineDescription& description) const;

  static bool GetGeometryShaderKey(PipelineGeometryShader geometry_shader_type,
                                   SpirvShaderTranslator::Modification vertex_shader_modification,
                                   SpirvShaderTranslator::Modification pixel_shader_modification,
                                   GeometryShaderKey& key_out);
  static uint32_t GetTessellationPatchControlPointCount(
      Shader::HostVertexShaderType host_vertex_shader_type,
      xenos::TessellationMode tessellation_mode);
  VkShaderModule GetTessellationVertexShader(bool adaptive);
  VkShaderModule GetTessellationControlShader(Shader::HostVertexShaderType host_vertex_shader_type,
                                              xenos::TessellationMode tessellation_mode);
  VkShaderModule GetGeometryShader(GeometryShaderKey key);
  bool TryGetPipelineCreationArgumentsForDescription(
      const PipelineDescription& description,
      std::pair<const PipelineDescription, Pipeline>* pipeline,
      PipelineCreationArguments& creation_arguments, bool for_placeholder = false,
      bool allow_untranslated = false);
  // Translates the shaders of queued creation arguments that aren't translated yet (on a
  // creation thread, vulkan_async_shader_translation) and fills in the pipeline layout,
  // which depends on the texture bindings known after translation.
  bool TranslateShadersForCreation(PipelineCreationArguments& creation_arguments);

  // Can be called from creation threads - all needed data must be fully set up
  // at the point of the call: shaders must be translated, and the pipeline
  // layout and render pass object (unless dynamic rendering is used) must be
  // available.
  bool EnsurePipelineCreated(const PipelineCreationArguments& creation_arguments,
                             VkShaderModule fragment_shader_override = VK_NULL_HANDLE);
  void CreationThread(size_t thread_index);
  void CreateQueuedPipelinesOnProcessorThread();
  void ProcessDeferredPipelineDestructions(bool force_all);

  VulkanCommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  VulkanRenderTargetCache& render_target_cache_;
  VkShaderStageFlags guest_shader_vertex_stages_;

  // Temporary storage for AnalyzeUcode calls on the processor thread.
  string::StringBuffer ucode_disasm_buffer_;
  // Reusable shader translator on the command processor thread.
  std::unique_ptr<SpirvShaderTranslator> shader_translator_;
  // Shader translation on the pipeline creation threads (vulkan_async_shader_translation):
  // one translator shared under async_translation_mutex_ (translations are serialized, the
  // point is keeping them off the GPU thread).
  std::unique_ptr<SpirvShaderTranslator> async_shader_translator_;
  std::mutex async_translation_mutex_;
  // Guards the texture binding layout map in TranslateAnalyzedShader, which the shader storage
  // loading calls from several threads.
  std::mutex texture_binding_layouts_mutex_;

 public:
  // Counters of pipeline and shader work at draw time (after the startup preload), read and
  // reset by the command processor every 10 s: pipelines queued for the creation threads,
  // pipelines created synchronously on the GPU command thread (with their time), and shader
  // translations done on the GPU command thread (with their time).
  struct DrawTimeCounters {
    uint32_t pipelines_queued = 0;
    uint32_t pipelines_sync = 0;
    uint64_t pipelines_sync_us = 0;
    uint32_t translations = 0;
    uint64_t translations_us = 0;
    uint32_t translations_async = 0;
    uint64_t translations_async_us = 0;
    uint32_t libraries_created = 0;
    uint64_t libraries_created_us = 0;
    uint32_t pipelines_linked = 0;
    uint64_t pipelines_linked_us = 0;
    uint32_t pipelines_linked_sync = 0;
    uint64_t pipelines_linked_sync_us = 0;
    // Texture sign specialization: draws with a specialized pipeline, draws with sign classes
    // that used the runtime variant (specialized one pending or failed), variants created.
    uint64_t sign_specialized_draws = 0;
    uint64_t sign_runtime_draws = 0;
    uint32_t sign_specialized_created = 0;
  };
  DrawTimeCounters TakeDrawTimeCounters();
  // Pipelines are linked from per-stage libraries (VK_EXT_graphics_pipeline_library): shader
  // translations then use pairing-independent interpolator masks and fixed interpolator
  // locations (SpirvShaderTranslator::SetFixedInterpolatorLocations).
  bool UsesPipelineLibraries() const { return uses_pipeline_libraries_; }
  bool UsesCanonicalInterpolatorMasks() const { return uses_pipeline_libraries_; }
  // Eco mode: guest pipelines take the fragment shading rate as dynamic state (the command
  // processor must set it before every guest draw after a pipeline without it was bound).
  bool UsesDynamicFragmentShadingRate() const { return dynamic_fragment_shading_rate_; }

 private:
  bool uses_pipeline_libraries_ = false;
  bool dynamic_fragment_shading_rate_ = false;
  // Pipeline libraries (VK_EXT_graphics_pipeline_library) by a hash of their key; created by the
  // creation threads, the storage loading and the GPU thread, so guarded.
  std::mutex pipeline_libraries_mutex_;
  std::unordered_map<uint64_t, VkPipeline> pipeline_libraries_;
  std::atomic<uint32_t> counter_libraries_created_{0};
  std::atomic<uint64_t> counter_libraries_created_us_{0};
  std::atomic<uint32_t> counter_pipelines_linked_{0};
  std::atomic<uint64_t> counter_pipelines_linked_us_{0};
  // only_if_cached: fail without compiling anything unless every library already exists or is
  // known to be in the driver's pipeline cache (then creating it is a cache hit).
  bool EnsurePipelineCreatedFromLibraries(const PipelineCreationArguments& creation_arguments,
                                          bool only_if_cached = false);
  // Library keys created on this device + driver build (persisted next to the driver cache), so
  // a later start knows which libraries the driver cache can produce quickly.
  std::unordered_set<uint64_t> known_library_keys_;
  // Created since the last driver cache save (written to the key list after the next save).
  std::vector<uint64_t> unsaved_library_keys_;
  FILE* library_keys_file_ = nullptr;
  bool IsLibraryAvailable(uint64_t key_hash);
  void LoadLibraryKeys(const std::filesystem::path& path);
  // Ahead-of-time fragment libraries for every stored pixel shader (offline-extracted set).
  struct PrecompileShader {
    uint64_t hash;
    std::vector<uint32_t> ucode;
  };
  std::vector<PrecompileShader> precompile_pixel_shaders_;
  // The first precompile_priority_count_ entries are the pixel shaders of the stored pipelines
  // (.xpso: used in play on this device or on the one its shader cache came from), in first-use
  // order; the app's notice covers only them.
  size_t precompile_priority_count_ = 0;
  // Ahead-of-time pre-rasterization (vertex) libraries (vulkan_precompile_vertex_libraries):
  // every stored vertex shader with the modification its draws request - from the stored
  // pipelines when used in play (those first, in first-use order; precompile_vertex_used_count_),
  // else the canonical one (host vertex shader type kVertex, every written interpolator exported).
  struct PrecompileVertexVariant {
    size_t shader_index;  // In precompile_vertex_shaders_.
    uint64_t modification;
    bool from_storage;
  };
  std::vector<PrecompileShader> precompile_vertex_shaders_;
  std::vector<PrecompileVertexVariant> precompile_vertex_variants_;
  size_t precompile_vertex_used_count_ = 0;
  std::unordered_map<uint64_t, uint32_t> pixel_shader_meta_;
  std::unique_ptr<rex::thread::Thread> precompile_thread_;
  std::atomic<bool> precompile_shutdown_{false};
  std::filesystem::path precompile_marker_path_;
  void PrecompileThread();
  VkPipeline GetOrCreateFragmentLibrary(uint64_t key_hash, VkShaderModule fragment_module,
                                        VkPipelineLayout layout,
                                        const VkPipelineMultisampleStateCreateInfo& multisample,
                                        const VkSpecializationInfo* specialization = nullptr);
  static uint64_t PreRasterizationLibraryKeyHash(uint64_t vertex_shader_hash,
                                                 uint64_t vertex_shader_modification,
                                                 uint32_t vertex_texture_sign_classes,
                                                 uint32_t geometry_shader_key);
  // The pre-rasterization library (vertex shader, optional geometry shader, rasterization state
  // dynamic) - shared by the draws and the boot precompile.
  VkPipeline GetOrCreatePreRasterizationLibrary(
      const VulkanShader::VulkanTranslation* vertex_shader, uint64_t vertex_shader_modification,
      uint32_t vertex_texture_sign_classes, uint32_t geometry_shader_key,
      VkShaderModule geometry_shader, VkPipelineLayout layout, bool only_if_cached);
  // Returns the library for the key (creating it with create_info on a miss), VK_NULL_HANDLE on
  // failure.
  VkPipeline GetOrCreatePipelineLibrary(const void* key, size_t key_size,
                                        VkGraphicsPipelineCreateInfo& create_info,
                                        VkGraphicsPipelineLibraryFlagsEXT library_flags,
                                        bool only_if_cached = false);
  VkPipeline GetOrCreatePipelineLibraryByHash(uint64_t key_hash,
                                              VkGraphicsPipelineCreateInfo& create_info,
                                              VkGraphicsPipelineLibraryFlagsEXT library_flags,
                                              bool only_if_cached);
  std::atomic<uint32_t> counter_pipelines_linked_sync_{0};
  std::atomic<uint64_t> counter_pipelines_linked_sync_us_{0};
  std::atomic<uint64_t> counter_sign_specialized_draws_{0};
  std::atomic<uint64_t> counter_sign_runtime_draws_{0};
  std::atomic<uint32_t> counter_sign_specialized_created_{0};
  std::atomic<uint32_t> counter_pipelines_queued_{0};
  std::atomic<uint32_t> counter_pipelines_sync_{0};
  std::atomic<uint64_t> counter_pipelines_sync_us_{0};
  std::atomic<uint32_t> counter_translations_{0};
  std::atomic<uint64_t> counter_translations_us_{0};
  std::atomic<uint32_t> counter_translations_async_{0};
  std::atomic<uint64_t> counter_translations_async_us_{0};

  struct LayoutUID {
    size_t uid;
    size_t vector_span_offset;
    size_t vector_span_length;
  };
  std::mutex layouts_mutex_;
  // Texture binding layouts of different shaders, for obtaining layout UIDs.
  std::vector<VulkanShader::TextureBinding> texture_binding_layouts_;
  // Map of texture binding layouts used by shaders, for obtaining UIDs. Keys
  // are XXH3 hashes of layouts, values need manual collision resolution using
  // layout_vector_offset:layout_length of texture_binding_layouts_.
  std::unordered_multimap<uint64_t, LayoutUID, rex::IdentityHasher<uint64_t>>
      texture_binding_layout_map_;

  // Ucode hash -> shader.
  std::unordered_map<uint64_t, VulkanShader*, rex::IdentityHasher<uint64_t>> shaders_;

  // Geometry shaders for Xenos primitive types not supported by Vulkan.
  // Stores VK_NULL_HANDLE if failed to create.
  std::unordered_map<GeometryShaderKey, VkShaderModule, GeometryShaderKey::Hasher>
      geometry_shaders_;

  // Fixed-function emulation shaders for tessellation.
  bool tessellation_indexed_vertex_shader_attempted_ = false;
  VkShaderModule tessellation_indexed_vertex_shader_ = VK_NULL_HANDLE;
  bool tessellation_adaptive_vertex_shader_attempted_ = false;
  VkShaderModule tessellation_adaptive_vertex_shader_ = VK_NULL_HANDLE;
  std::unordered_map<TessellationControlShaderKey, VkShaderModule,
                     TessellationControlShaderKey::Hasher>
      tessellation_control_shaders_;

  // Empty depth-only pixel shader for writing to depth buffer using fragment
  // shader interlock when no Xenos pixel shader provided.
  VkShaderModule depth_only_fragment_shader_ = VK_NULL_HANDLE;
  VkShaderModule placeholder_pixel_shader_ = VK_NULL_HANDLE;
  // Depth-only shaders for float24 emulation when no Xenos pixel shader is
  // provided in host render target mode.
  VkShaderModule depth_float24_truncate_fragment_shader_ = VK_NULL_HANDLE;
  VkShaderModule depth_float24_round_fragment_shader_ = VK_NULL_HANDLE;

  std::unordered_map<PipelineDescription, Pipeline, PipelineDescription::Hasher> pipelines_;

  // Previously used pipeline, to avoid lookups if the state wasn't changed.
  const std::pair<const PipelineDescription, Pipeline>* last_pipeline_ = nullptr;
  // <Submission index, pipeline>.
  std::deque<std::pair<uint64_t, VkPipeline>> deferred_destroy_pipelines_;
  std::mutex deferred_destroy_lock_;

  // Currently open shader storage path.
  std::filesystem::path shader_storage_cache_root_;
  uint32_t shader_storage_title_id_ = 0;

  // Shader storage output stream, for preload in the next emulator runs.
  FILE* shader_storage_file_ = nullptr;
  // For only writing shaders to the currently open storage once, incremented
  // when switching the storage.
  uint32_t shader_storage_index_ = 0;
  bool shader_storage_file_flush_needed_ = false;

  // Pipeline storage output stream, for preload in the next emulator runs.
  FILE* pipeline_storage_file_ = nullptr;
  bool pipeline_storage_file_flush_needed_ = false;

  // Persistence of the driver-level VkPipelineCache owned by the VulkanDevice (which every
  // pipeline creation in the Vulkan backend uses), in the shareable storage folder. The file is
  // merged into the device's cache when the storage is initialized, before the stored pipelines
  // are created, and is saved by the storage writer thread once new pipeline creations have
  // settled (the Android app is ended with a force-stop, never a clean shutdown), through a
  // temporary file and a rename so a kill cannot leave a torn file.
  static constexpr uint64_t kDriverPipelineCacheSaveSettleMs = 3000;
  static constexpr uint64_t kDriverPipelineCacheSaveMaxDelayMs = 30000;
  void LoadDriverPipelineCache(const std::filesystem::path& path);
  void SaveDriverPipelineCache();
  // Milliseconds until the driver pipeline cache should be saved, or -1 if nothing new to save.
  int64_t GetDriverPipelineCacheSaveDelayMs() const;
  void MarkDriverPipelineCacheUsed();
  std::filesystem::path driver_pipeline_cache_path_;
  std::mutex driver_pipeline_cache_save_mutex_;
  // Size of the data last loaded or saved, to skip saves when nothing new was compiled.
  size_t driver_pipeline_cache_saved_size_ = 0;
  std::atomic<uint64_t> driver_pipeline_cache_last_use_ms_{0};
  // 0 when there are no pipeline creations since the last save.
  std::atomic<uint64_t> driver_pipeline_cache_first_unsaved_use_ms_{0};
  // Set when a big batch of creations ended (the precompile): save after the short settle time.
  std::atomic<bool> driver_pipeline_cache_save_soon_{false};

  // Thread for asynchronous writing to the storage streams.
  void StorageWriteThread();
  std::mutex storage_write_request_lock_;
  std::condition_variable storage_write_request_cond_;
  // Storage thread input is protected with storage_write_request_lock_, and the
  // thread is notified about its change via storage_write_request_cond_.
  std::deque<const Shader*> storage_write_shader_queue_;
  std::deque<PipelineStoredDescription> storage_write_pipeline_queue_;
  bool storage_write_flush_shaders_ = false;
  bool storage_write_flush_pipelines_ = false;
  bool storage_write_thread_shutdown_ = false;
  std::unique_ptr<rex::thread::Thread> storage_write_thread_;

  mutable std::mutex creation_request_lock_;
  std::condition_variable creation_request_cond_;
  std::priority_queue<PipelineCreationArguments, std::vector<PipelineCreationArguments>,
                      PipelineCreationArgumentsPriorityComparator>
      creation_queue_;
  size_t creation_threads_busy_ = 0;
  bool startup_loading_ = false;
  std::unique_ptr<rex::thread::Event> creation_completion_event_;
  bool creation_completion_set_event_ = false;
  size_t creation_threads_shutdown_from_ = SIZE_MAX;
  std::vector<std::unique_ptr<rex::thread::Thread>> creation_threads_;
};

}  // namespace rex::graphics::vulkan
