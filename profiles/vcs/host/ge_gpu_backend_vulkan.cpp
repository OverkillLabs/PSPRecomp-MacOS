// MoltenVK/Vulkan GE backend.
//
// Real hardware rasterization of VCS geometry on macOS, standing in for
// ge_gpu_backend_dx12.cpp behind the same ge_gpu_backend.hpp interface.
//
// Scope of this revision: the screen-space submission path
// (ge_gpu_backend_accumulate_color_triangles). ge_renderer.cpp has already
// done transform, lighting, clipping and primitive assembly on the CPU and
// hands over finished screen-space triangles with all PSP pixel state packed
// per vertex. That is the tractable, verifiable half of the port, and it is
// where the actual per-pixel rasterization cost lives.
//
// The hardware-transform paths (accumulate_hardware_triangles /
// accumulate_hardware_packed_0115) are NOT implemented here yet and report
// "not accepted", so run with PSPRECOMP_GE_GPU_HW_TRANSFORM=0 (or
// Rendering.HardwareTransform=false) to keep geometry on the route this
// backend serves.
//
// Pixel semantics -- blend variants, depth compare, texture function, fog,
// alpha test, framebuffer quantization -- are ported from the validated DX12
// backend and its HLSL so the two are diffable when output disagrees. See
// shaders/psp_ge.{vert,frag}.
//
// Requires: brew install vulkan-headers vulkan-loader molten-vk shaderc
// (shaderc supplies glslc, used at build time to produce the SPIR-V headers).

#include "ge_gpu_backend.hpp"
#include "vcs_config.hpp"
#include "vcs_runtime_log.hpp"

#include <vulkan/vulkan.h>

#include "psp_ge_vert_spv.h"
#include "psp_ge_frag_spv.h"
#include "psp_ge_hw_vert_spv.h"
#include "psp_ge_hw_packed0115_vert_spv.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <unordered_set>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace vcs {

namespace {

constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;

// Picks the highest-precision depth format the device actually supports for
// an optimal-tiling depth attachment, matching what ge_gpu_backend_dx12.cpp
// defaults to (DXGI_FORMAT_D32_FLOAT) instead of this backend's own
// previous hardcoded 16-bit choice. 16-bit depth was precise enough near
// the camera but not across VCS's actual depth range (a nearby character
// against a distant skyline in the same frame), which showed up as visible
// Z-fighting/incorrect occlusion -- a roof polygon rendering in front of
// things behind it, a character's face losing detail against whatever was
// mis-ordered just behind it. The Vulkan spec only guarantees D16_UNORM
// support unconditionally, so this still needs a real fallback chain, not
// just switching the hardcoded constant.
// `requested_bits` is Rendering.DepthPrecision from the .ini (16, 24 or 32).
// Previously this always picked the highest-precision format the device
// supported regardless of that setting -- silently ignoring it, the same way
// MSAA/SMAA/AnisotropicFiltering were confirmed to be no-ops on this backend.
// Now the requested precision is tried first and only falls back (to the
// next-best, then the mandatory D16) if the device genuinely can't do it.
[[nodiscard]] VkFormat select_depth_format(VkPhysicalDevice physical_device,
                                           std::uint32_t requested_bits) noexcept {
    const auto supports = [&](VkFormat candidate) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physical_device, candidate, &properties);
        return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0u;
    };
    VkFormat preferred = VK_FORMAT_D32_SFLOAT;
    if (requested_bits <= 16u) preferred = VK_FORMAT_D16_UNORM;
    else if (requested_bits == 24u) preferred = VK_FORMAT_D24_UNORM_S8_UINT;
    if (supports(preferred)) return preferred;
    for (const VkFormat candidate : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT,
                                     VK_FORMAT_D16_UNORM}) {
        if (supports(candidate)) return candidate;
    }
    return VK_FORMAT_D16_UNORM;  // Mandatory support; always reachable as a last resort.
}
// PSP screen space. Vertices arrive in these coordinates; the viewport scales
// them to the internal render target, which is how the internal-resolution
// setting turns into actual extra pixels for free.
constexpr float kPspWidth = 480.0f;
constexpr float kPspHeight = 272.0f;
constexpr VkDeviceSize kInitialVertexBytes = 8u * 1024u * 1024u;

struct PushConstants {
    float inverse_viewport[2];
    std::uint32_t framebuffer_format;
};

// Matches psp_ge_hw.vert/psp_ge_hw_packed0115.vert's push_constant block
// layout exactly (both shaders share one struct/pipeline layout; the
// generic hw-transform shader simply ignores draw_control, which only the
// packed-0115 shader reads, since its per-draw alpha/texture/env/fog state
// doesn't otherwise fit in that shader's per-vertex attributes -- see
// psp_ge_hw_packed0115.vert's header comment). Ported from DX12's
// Dx12TransformConstants/DrawTransform cbuffer (ge_gpu_backend_dx12.cpp).
struct HwTransformPushConstants {
    float row0[4];
    float row1[4];
    float row2[4];
    float row3[4];
    float model_to_view_z[4];
    float uv_scale_offset[4];
    float fog_parameters[4];
    std::uint32_t transform_control[4];
    float vertex_color_mul[4];
    float vertex_color_add[4];
    std::uint32_t draw_control[4];
};

// Raw PSP 0x0115 vertex bytes (u8 UV + 5551 colour + s16 XYZ, 10-byte
// stride) as psp_ge_hw_packed0115.vert's vertex attributes expect them.
// #pragma pack keeps this exactly 10 bytes with no compiler-inserted
// padding, matching the PSP's own byte layout bit for bit.
#pragma pack(push, 1)
struct Packed0115Vertex {
    std::uint8_t u;
    std::uint8_t v;
    std::uint16_t color5551;
    std::int16_t x;
    std::int16_t y;
    std::int16_t z;
};
#pragma pack(pop)
static_assert(sizeof(Packed0115Vertex) == 10u, "PSP 0x0115 vertex must be exactly 10 bytes");

// One hardware-transform draw: like Batch above, but the CPU has not
// transformed these vertices at all (screen-space Batch's whole premise),
// so each one carries its own per-draw transform (baked into push constants
// at draw time) alongside the pipeline/blend/scissor state Batch already
// tracks.
struct HwTransformBatch {
    std::uint32_t first_vertex{};
    std::uint32_t vertex_count{};
    std::uint32_t first_index{};
    std::uint32_t index_count{};  // 0 means non-indexed
    bool packed_0115{};
    VkPipeline pipeline{VK_NULL_HANDLE};
    VkDescriptorSet descriptor{VK_NULL_HANDLE};
    std::array<float, 4> blend_constants{};
    std::array<std::int32_t, 4> scissor{};
    HwTransformPushConstants constants{};
    bool cull_enabled{};
    bool accept_counter_clockwise{};
};

// ---------------------------------------------------------------------------
// PSP state -> pipeline state. Ported from ge_gpu_backend_dx12.cpp so the two
// backends classify identically; see blend_variant()/depth_compare() there.
// ---------------------------------------------------------------------------

[[nodiscard]] VkCompareOp depth_compare(std::uint32_t function) noexcept {
    switch (function & 7u) {
    case 0u: return VK_COMPARE_OP_NEVER;
    case 1u: return VK_COMPARE_OP_ALWAYS;
    case 2u: return VK_COMPARE_OP_EQUAL;
    case 3u: return VK_COMPARE_OP_NOT_EQUAL;
    case 4u: return VK_COMPARE_OP_LESS;
    case 5u: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 6u: return VK_COMPARE_OP_GREATER;
    case 7u: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    }
    return VK_COMPARE_OP_ALWAYS;
}

[[nodiscard]] std::uint32_t blend_variant(const GeGpuDrawDescriptor &draw) noexcept {
    if (!draw.blend_enabled || draw.clear_mode) return 0u;
    const std::uint32_t equation = draw.blend_equation & 7u;
    const std::uint32_t source = draw.blend_source_factor & 0xFu;
    const std::uint32_t destination = draw.blend_dest_factor & 0xFu;
    if (equation == 0u && source == 2u && destination == 3u) return 1u; // src alpha / inv src alpha
    if (equation == 0u && source == 10u && destination == 10u) {
        const std::uint32_t fixed_source = draw.blend_fix_source & 0x00FFFFFFu;
        const std::uint32_t fixed_destination = draw.blend_fix_dest & 0x00FFFFFFu;
        if (fixed_source == 0x00FFFFFFu && fixed_destination == 0u) return 2u;          // replace
        if (fixed_source == 0x00FFFFFFu && fixed_destination == 0x00FFFFFFu) return 3u; // additive
        bool complements = true;
        for (std::uint32_t shift = 0u; shift < 24u; shift += 8u)
            complements &= (((fixed_source >> shift) & 0xFFu) +
                            ((fixed_destination >> shift) & 0xFFu)) == 0xFFu;
        if (complements) return 4u; // blend factor / inverse blend factor
    }
    if (equation == 0u && source == 2u && destination == 10u &&
        (draw.blend_fix_dest & 0x00FFFFFFu) == 0x00FFFFFFu) return 5u;
    return 0u;
}

// PSP's mask byte is 0xFF for "do not write this channel"; Vulkan's mask bit
// means "do write". Same inversion the DX12 backend performs.
[[nodiscard]] VkColorComponentFlags color_write_mask(const GeGpuDrawDescriptor &draw) noexcept {
    VkColorComponentFlags mask = 0u;
    constexpr std::array<VkColorComponentFlagBits, 4> bits{
        VK_COLOR_COMPONENT_R_BIT, VK_COLOR_COMPONENT_G_BIT, VK_COLOR_COMPONENT_B_BIT,
        VK_COLOR_COMPONENT_A_BIT};
    for (std::uint32_t channel = 0u; channel < 4u; ++channel) {
        const std::uint32_t byte = (draw.color_write_mask >> (channel * 8u)) & 0xFFu;
        if (byte != 0xFFu) mask |= bits[channel];
    }
    return mask;
}

[[nodiscard]] std::uint64_t pipeline_key(const GeGpuDrawDescriptor &draw) noexcept {
    std::uint64_t key = static_cast<std::uint64_t>(draw.depth_test_enabled ? 1u : 0u);
    key |= static_cast<std::uint64_t>(draw.depth_write_enabled ? 1u : 0u) << 1u;
    key |= static_cast<std::uint64_t>(draw.depth_function & 7u) << 2u;
    key |= static_cast<std::uint64_t>(blend_variant(draw) & 7u) << 5u;
    key |= static_cast<std::uint64_t>(color_write_mask(draw) & 0xFu) << 8u;
    return key;
}

[[nodiscard]] std::uint64_t texture_key(const GeGpuDrawDescriptor &draw) noexcept {
    if (draw.texture_cache_key_hint != 0u) return draw.texture_cache_key_hint;
    // Fallback when the renderer did not precompute a key: hash the state that
    // identifies the decoded image. clut_checksum matters -- VCS recolours
    // vehicles by rewriting the palette at the same address, so keying on the
    // CLUT address alone makes two differently coloured cars collide.
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    mix(draw.texture_address);
    mix(draw.texture_format);
    mix(draw.texture_buffer_width);
    mix(draw.texture_width);
    mix(draw.texture_height);
    mix(draw.clut_address);
    mix(draw.clut_checksum);
    mix(draw.texture_swizzled ? 1u : 0u);
    return hash;
}

[[nodiscard]] std::uint64_t sampler_key(const GeGpuDrawDescriptor &draw) noexcept {
    return (draw.texture_min_linear ? 1ull : 0ull) | (draw.texture_mag_linear ? 2ull : 0ull) |
           (draw.texture_clamp_u ? 4ull : 0ull) | (draw.texture_clamp_v ? 8ull : 0ull);
}

struct TextureEntry {
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkDescriptorSet descriptor{VK_NULL_HANDLE};
    std::uint32_t width{};
    std::uint32_t height{};
    // Stamped with VulkanGeState::frame_epoch every time this entry is
    // uploaded or looked up by a draw. Lets upload() evict the least
    // recently used entry instead of refusing every texture past whatever
    // count happened to be resident when the cache first filled up -- see
    // the eviction loop there for why that refusal was the actual cause of
    // textures "breaking" progressively during a long play session (DX12's
    // backend already evicts by this same recency rule; this one never did).
    std::uint64_t last_used_epoch{0u};
};

// One recorded draw: a contiguous run of vertices plus the state they need.
struct Batch {
    std::uint32_t first_vertex{};
    std::uint32_t vertex_count{};
    VkPipeline pipeline{VK_NULL_HANDLE};
    std::uint32_t framebuffer_format{};
    VkDescriptorSet descriptor{VK_NULL_HANDLE};
    std::array<float, 4> blend_constants{};
    std::array<std::int32_t, 4> scissor{};
    // GE framebuffer stride (draw.framebuffer_stride) for whichever target
    // this batch renders into. PSP VRAM buffers are routinely allocated
    // wider than the visible 480px (512 is the common stride, for
    // alignment) -- the extra columns exist but are never meant to be seen.
    // finish_color_frame() used to map every batch's vertices as though its
    // target were exactly kPspWidth (480) wide regardless of this, which
    // stretches a 512-wide target's content by 512/480 (~6.7%) end to end:
    // confirmed live via the F9-adjacent edge-diag capture as the direct
    // cause of interface elements (money counter among them) rendering
    // partly off the true screen edge, at every internal resolution and
    // every real display aspect alike (DX12 never had this: it already
    // tracks this per-target width -- see ge_gpu_backend_dx12.cpp's
    // logical_width). 0 means "not yet known" / "use kPspWidth".
    std::uint32_t framebuffer_stride{};
};

struct VulkanGeState {
    GeGpuBackendReport report{};
    // Also read by ge_renderer.cpp's skip_owned/skip_displayed CPU-raster-
    // skip logic (see ge_gpu_backend_owned_framebuffer()'s comment below for
    // why it reports report.presented_framebuffer_target here instead of
    // this raw sceDisplaySetFrameBuf() value, which is one frame stale).
    // Address-history-based bucket selection was tried and reverted (see
    // finish_color_frame()'s selection comment) -- this field itself is
    // still real and used, just not for that.
    std::uint32_t display_framebuffer{};
    bool enabled{false};
    bool anisotropy_supported{false};
    float max_supported_anisotropy{1.0f};

    VkInstance instance{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue graphics_queue{VK_NULL_HANDLE};
    std::uint32_t graphics_queue_family{0xFFFFFFFFu};
    VkCommandPool command_pool{VK_NULL_HANDLE};
    VkCommandBuffer command_buffer{VK_NULL_HANDLE};
    VkFence fence{VK_NULL_HANDLE};

    std::uint32_t width{480u};
    std::uint32_t height{272u};
    VkImage color_image{VK_NULL_HANDLE};
    VkDeviceMemory color_memory{VK_NULL_HANDLE};
    VkImageView color_view{VK_NULL_HANDLE};
    VkImage depth_image{VK_NULL_HANDLE};
    VkDeviceMemory depth_memory{VK_NULL_HANDLE};
    VkImageView depth_view{VK_NULL_HANDLE};
    VkRenderPass render_pass{VK_NULL_HANDLE};
    VkFramebuffer framebuffer{VK_NULL_HANDLE};

    VkShaderModule vertex_shader{VK_NULL_HANDLE};
    VkShaderModule fragment_shader{VK_NULL_HANDLE};
    VkDescriptorSetLayout descriptor_layout{VK_NULL_HANDLE};
    VkDescriptorPool descriptor_pool{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
    std::unordered_map<std::uint64_t, VkPipeline> pipelines;

    VkBuffer vertex_buffer{VK_NULL_HANDLE};
    VkDeviceMemory vertex_memory{VK_NULL_HANDLE};
    VkDeviceSize vertex_capacity{};
    void *vertex_mapped{nullptr};

    // Hardware-transform pipeline state: a separate pipeline layout (the
    // push-constant block is much larger than the screen-space path's) and
    // shader modules for psp_ge_hw.vert (generic) and
    // psp_ge_hw_packed0115.vert (dominant-format fast path). Both share the
    // existing fragment shader (s.fragment_shader) unchanged. Pipelines are
    // cached the same way as the screen-space path's s.pipelines, keyed
    // separately since the key space (which now includes packed-vs-generic
    // and CPU-side cull state) does not overlap.
    VkShaderModule hw_vertex_shader{VK_NULL_HANDLE};
    VkShaderModule hw_packed_vertex_shader{VK_NULL_HANDLE};
    VkPipelineLayout hw_pipeline_layout{VK_NULL_HANDLE};
    std::unordered_map<std::uint64_t, VkPipeline> hw_pipelines;

    // Separate GPU-visible buffers for hardware-transform data: the generic
    // path's vertex format (GeGpuVertex, model-space) differs from the
    // screen-space path's (also GeGpuVertex, but already-transformed) only
    // in what the shader does with it, so it could in principle share
    // s.vertex_buffer -- kept separate instead so growing one buffer's
    // capacity independently of the other never has to reason about which
    // frame's data is currently resident in a shared allocation.
    VkBuffer hw_vertex_buffer{VK_NULL_HANDLE};
    VkDeviceMemory hw_vertex_memory{VK_NULL_HANDLE};
    VkDeviceSize hw_vertex_capacity{};
    void *hw_vertex_mapped{nullptr};

    VkBuffer hw_packed_vertex_buffer{VK_NULL_HANDLE};
    VkDeviceMemory hw_packed_vertex_memory{VK_NULL_HANDLE};
    VkDeviceSize hw_packed_vertex_capacity{};
    void *hw_packed_vertex_mapped{nullptr};

    VkBuffer hw_index_buffer{VK_NULL_HANDLE};
    VkDeviceMemory hw_index_memory{VK_NULL_HANDLE};
    VkDeviceSize hw_index_capacity{};
    void *hw_index_mapped{nullptr};

    VkBuffer readback_buffer{VK_NULL_HANDLE};
    VkDeviceMemory readback_memory{VK_NULL_HANDLE};
    VkDeviceSize readback_capacity{};

    VkBuffer staging_buffer{VK_NULL_HANDLE};
    VkDeviceMemory staging_memory{VK_NULL_HANDLE};
    VkDeviceSize staging_capacity{};

    std::unordered_map<std::uint64_t, TextureEntry> textures;
    std::unordered_map<std::uint64_t, VkSampler> samplers;
    TextureEntry dummy_texture{};
    // Cache capacity actually enforced at runtime -- sized from
    // Rendering.TextureCacheEntries at create_backend() time (see there),
    // not the old hardcoded 4096 the descriptor pool alone still bounds.
    std::uint32_t texture_capacity{4096u};
    // Bumped once per finish_color_frame() call; texture last_used_epoch
    // stamps against this let upload() tell "still in use this frame" (never
    // evict) apart from "just hasn't been drawn in a while" (evict first).
    std::uint64_t frame_epoch{0u};

    // All draws this vblank, grouped by their actual target address. VCS
    // renders several passes a frame into different targets (prelight,
    // reflection, composition, the real scene, ...); at finish_color_frame()
    // time the bucket with the most vertices is treated as "the" frame.
    // A full bucket dump (see finish_color_frame()'s selection comment)
    // confirmed this is reliably correct: the real scene always dwarfed
    // every competing HUD-sized bucket by two to three orders of magnitude
    // in every sample taken. This needs no address history and no
    // assumption about swap-chain flip timing.
    struct FrameBucket {
        std::uint32_t address{};
        std::vector<GeGpuVertex> vertices;
        std::vector<Batch> batches;
        // Hardware-transform geometry for this bucket: unlike `vertices`
        // above (already screen-space by the time it gets here), these are
        // untouched model-space vertices -- the whole point is the GPU does
        // the transform, not ge_renderer.cpp's CPU loop. hw_vertices holds
        // the generic path's GeGpuVertex-format data (see
        // ge_gpu_backend_accumulate_hardware_triangles); hw_packed holds the
        // dominant-format raw PSP bytes (see
        // ge_gpu_backend_accumulate_hardware_packed_0115). Both share one
        // index buffer's worth of storage, hw_indices.
        std::vector<GeGpuVertex> hw_vertices;
        std::vector<Packed0115Vertex> hw_packed;
        std::vector<std::uint32_t> hw_indices;
        std::vector<HwTransformBatch> hw_batches;
    };
    std::vector<FrameBucket> frame_buckets;

    // GPU work for one frame is submitted here and only waited on at the
    // start of the *next* finish_color_frame() call (by which point a full
    // vblank of guest CPU work has elapsed, so the wait is normally
    // instant) rather than immediately after submitting it. That overlap is
    // the difference between the CPU sitting idle for an entire frame's
    // render time every single vblank and not -- previously the single
    // biggest cost in this backend. This adds exactly one vblank of output
    // latency, which is imperceptible and is the standard trade-off double-
    // buffered renderers make everywhere.
    bool submission_pending{false};
    std::uint64_t pending_vblank{};
    std::uint32_t pending_target_address{};

    // The winning bucket's address from the last frame, so selection can
    // stick with it rather than re-picking the single largest bucket fresh
    // every vblank. Two (or more) targets whose vertex counts happen to be
    // close and fluctuate frame to frame -- observed in practice -- would
    // otherwise make the winner flip between them, alternating two
    // different-looking renders and reading as flicker/corruption even
    // though any one captured frame looks correct. Only switches away when
    // some other bucket is decisively larger, not just momentarily ahead.
    std::uint32_t last_winner_address{};
    bool has_last_winner{false};

    std::vector<std::byte> frame_rgba;
    bool frame_valid{false};
};

VulkanGeState &state() {
    static VulkanGeState s;
    return s;
}

// See ge_gpu_backend_lock()/unlock()'s comment in ge_gpu_backend.hpp: this
// file has no internal synchronization of its own, so callers serialize
// against each other with this.
std::recursive_mutex &backend_mutex() {
    static std::recursive_mutex m;
    return m;
}

[[nodiscard]] bool find_memory_type(VkPhysicalDevice physical_device, std::uint32_t type_bits,
                                    VkMemoryPropertyFlags required, std::uint32_t &out_index) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
    for (std::uint32_t index = 0u; index < properties.memoryTypeCount; ++index) {
        if ((type_bits & (1u << index)) == 0u) continue;
        if ((properties.memoryTypes[index].propertyFlags & required) == required) {
            out_index = index;
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool create_buffer(VulkanGeState &s, VkDeviceSize size, VkBufferUsageFlags usage,
                                 VkMemoryPropertyFlags properties, VkBuffer &buffer,
                                 VkDeviceMemory &memory) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(s.device, &info, nullptr, &buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(s.device, buffer, &requirements);
    std::uint32_t type_index = 0u;
    if (!find_memory_type(s.physical_device, requirements.memoryTypeBits, properties, type_index))
        return false;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type_index;
    if (vkAllocateMemory(s.device, &allocate, nullptr, &memory) != VK_SUCCESS) return false;
    return vkBindBufferMemory(s.device, buffer, memory, 0) == VK_SUCCESS;
}

[[nodiscard]] bool instance_extension_supported(const char *name) {
    std::uint32_t count = 0u;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> extensions(count);
    if (count != 0u &&
        vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()) != VK_SUCCESS)
        return false;
    for (const VkExtensionProperties &extension : extensions)
        if (std::strcmp(extension.extensionName, name) == 0) return true;
    return false;
}

[[nodiscard]] bool device_extension_supported(VkPhysicalDevice device, const char *name) {
    std::uint32_t count = 0u;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS)
        return false;
    std::vector<VkExtensionProperties> extensions(count);
    if (count != 0u && vkEnumerateDeviceExtensionProperties(device, nullptr, &count,
                                                            extensions.data()) != VK_SUCCESS)
        return false;
    for (const VkExtensionProperties &extension : extensions)
        if (std::strcmp(extension.extensionName, name) == 0) return true;
    return false;
}

[[nodiscard]] VkCommandBuffer begin_one_shot(VulkanGeState &s) {
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = s.command_pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1u;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(s.device, &allocate, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) return VK_NULL_HANDLE;
    return cmd;
}

[[nodiscard]] bool end_one_shot(VulkanGeState &s, VkCommandBuffer cmd) {
    if (cmd == VK_NULL_HANDLE) return false;
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) return false;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &cmd;
    vkResetFences(s.device, 1, &s.fence);
    if (vkQueueSubmit(s.graphics_queue, 1, &submit, s.fence) != VK_SUCCESS) return false;
    const bool ok = vkWaitForFences(s.device, 1, &s.fence, VK_TRUE, 5000000000ull) == VK_SUCCESS;
    vkFreeCommandBuffers(s.device, s.command_pool, 1, &cmd);
    return ok;
}

void transition_image(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                      VkAccessFlags source_access, VkAccessFlags destination_access,
                      VkPipelineStageFlags source_stage, VkPipelineStageFlags destination_stage) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1u;
    barrier.subresourceRange.layerCount = 1u;
    barrier.srcAccessMask = source_access;
    barrier.dstAccessMask = destination_access;
    vkCmdPipelineBarrier(cmd, source_stage, destination_stage, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
}

[[nodiscard]] VkSampler get_sampler(VulkanGeState &s, const GeGpuDrawDescriptor &draw) {
    const std::uint64_t key = sampler_key(draw);
    if (const auto found = s.samplers.find(key); found != s.samplers.end()) return found->second;
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = draw.texture_mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter = draw.texture_min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = draw.texture_clamp_u ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                             : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = draw.texture_clamp_v ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                             : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    // PSP sampling is itself isotropic (the hardware had no anisotropic
    // filtering), but this backend previously left the feature hardcoded off
    // entirely rather than making it available as a real upscale-quality
    // option -- confirmed a silent no-op, same as MSAA/SMAA/DepthPrecision.
    // Rendering.AnisotropicFiltering (1-16) now actually reaches the
    // sampler, clamped to what the device really supports.
    const std::uint32_t requested_anisotropy =
        std::clamp(vcs_configuration().rendering.anisotropic_filtering, 1u, 16u);
    if (s.anisotropy_supported && requested_anisotropy > 1u) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = std::min(static_cast<float>(requested_anisotropy), s.max_supported_anisotropy);
    } else {
        info.anisotropyEnable = VK_FALSE;
        info.maxAnisotropy = 1.0f;
    }
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(s.device, &info, nullptr, &sampler) != VK_SUCCESS) return VK_NULL_HANDLE;
    s.samplers.emplace(key, sampler);
    ++s.report.texture_samplers_created;
    return sampler;
}

// Reverses create_texture()'s allocations exactly, in reverse order. Safe to
// call between frames: this backend submits and vkWaitForFences's on every
// finish_color_frame() (see s.fence there), so by the time upload_decoded_
// texture() (and therefore this) runs, the GPU is never still reading a
// previous frame's descriptor -- no multi-frame-in-flight retirement list
// like DX12's is needed here.
void destroy_texture_entry(VulkanGeState &s, TextureEntry &entry) noexcept {
    if (entry.descriptor != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(s.device, s.descriptor_pool, 1, &entry.descriptor);
    }
    if (entry.view != VK_NULL_HANDLE) vkDestroyImageView(s.device, entry.view, nullptr);
    if (entry.image != VK_NULL_HANDLE) vkDestroyImage(s.device, entry.image, nullptr);
    if (entry.memory != VK_NULL_HANDLE) vkFreeMemory(s.device, entry.memory, nullptr);
    entry = TextureEntry{};
}

[[nodiscard]] bool create_texture(VulkanGeState &s, std::uint32_t width, std::uint32_t height,
                                  std::span<const std::byte> pixels, VkSampler sampler,
                                  TextureEntry &out) {
    if (width == 0u || height == 0u) return false;
    const VkDeviceSize byte_size = VkDeviceSize{width} * height * 4u;
    if (pixels.size() < static_cast<std::size_t>(byte_size)) return false;

    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = kColorFormat;
    image_info.extent = {width, height, 1u};
    image_info.mipLevels = 1u;
    image_info.arrayLayers = 1u;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(s.device, &image_info, nullptr, &out.image) != VK_SUCCESS) return false;

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(s.device, out.image, &requirements);
    std::uint32_t type_index = 0u;
    if (!find_memory_type(s.physical_device, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type_index))
        return false;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type_index;
    if (vkAllocateMemory(s.device, &allocate, nullptr, &out.memory) != VK_SUCCESS) return false;
    if (vkBindImageMemory(s.device, out.image, out.memory, 0) != VK_SUCCESS) return false;

    if (s.staging_capacity < byte_size) {
        if (s.staging_buffer != VK_NULL_HANDLE) vkDestroyBuffer(s.device, s.staging_buffer, nullptr);
        if (s.staging_memory != VK_NULL_HANDLE) vkFreeMemory(s.device, s.staging_memory, nullptr);
        s.staging_buffer = VK_NULL_HANDLE;
        s.staging_memory = VK_NULL_HANDLE;
        if (!create_buffer(s, byte_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           s.staging_buffer, s.staging_memory))
            return false;
        s.staging_capacity = byte_size;
    }
    void *mapped = nullptr;
    if (vkMapMemory(s.device, s.staging_memory, 0, byte_size, 0, &mapped) != VK_SUCCESS)
        return false;
    std::memcpy(mapped, pixels.data(), static_cast<std::size_t>(byte_size));
    vkUnmapMemory(s.device, s.staging_memory);

    VkCommandBuffer cmd = begin_one_shot(s);
    if (cmd == VK_NULL_HANDLE) return false;
    transition_image(cmd, out.image, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1u;
    // Full extent. A zero-extent copy silently transfers nothing.
    region.imageExtent = {width, height, 1u};
    vkCmdCopyBufferToImage(cmd, s.staging_buffer, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region);
    transition_image(cmd, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (!end_one_shot(s, cmd)) return false;

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = out.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = kColorFormat;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1u;
    view_info.subresourceRange.layerCount = 1u;
    if (vkCreateImageView(s.device, &view_info, nullptr, &out.view) != VK_SUCCESS) return false;

    VkDescriptorSetAllocateInfo descriptor_allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    descriptor_allocate.descriptorPool = s.descriptor_pool;
    descriptor_allocate.descriptorSetCount = 1u;
    descriptor_allocate.pSetLayouts = &s.descriptor_layout;
    if (vkAllocateDescriptorSets(s.device, &descriptor_allocate, &out.descriptor) != VK_SUCCESS)
        return false;

    VkDescriptorImageInfo image_descriptor{};
    image_descriptor.sampler = sampler;
    image_descriptor.imageView = out.view;
    image_descriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = out.descriptor;
    write.descriptorCount = 1u;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_descriptor;
    vkUpdateDescriptorSets(s.device, 1, &write, 0, nullptr);

    out.width = width;
    out.height = height;
    ++s.report.texture_descriptor_sets_allocated;
    return true;
}

[[nodiscard]] VkPipeline get_pipeline(VulkanGeState &s, const GeGpuDrawDescriptor &draw) {
    const std::uint64_t key = pipeline_key(draw);
    if (const auto found = s.pipelines.find(key); found != s.pipelines.end()) return found->second;

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0] = VkPipelineShaderStageCreateInfo{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = s.vertex_shader;
    stages[0].pName = "main";
    stages[1] = VkPipelineShaderStageCreateInfo{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = s.fragment_shader;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0u;
    binding.stride = sizeof(GeGpuVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // Must match shaders/psp_ge.vert's input locations exactly.
    const std::array<VkVertexInputAttributeDescription, 9> attributes{{
        {0u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GeGpuVertex, x)},
        {1u, 0u, VK_FORMAT_R8G8B8A8_UINT, offsetof(GeGpuVertex, rgba)},
        {2u, 0u, VK_FORMAT_R32G32_SFLOAT, offsetof(GeGpuVertex, u)},
        {3u, 0u, VK_FORMAT_R32_UINT, offsetof(GeGpuVertex, alpha_control)},
        {4u, 0u, VK_FORMAT_R32_UINT, offsetof(GeGpuVertex, texture_control)},
        {5u, 0u, VK_FORMAT_R32_UINT, offsetof(GeGpuVertex, texture_env)},
        {6u, 0u, VK_FORMAT_R32_SFLOAT, offsetof(GeGpuVertex, fog_factor)},
        {7u, 0u, VK_FORMAT_R32_UINT, offsetof(GeGpuVertex, fog_control)},
        {8u, 0u, VK_FORMAT_R32_SFLOAT, offsetof(GeGpuVertex, q)},
    }};

    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1u;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1u;
    viewport_state.scissorCount = 1u;

    VkPipelineRasterizationStateCreateInfo rasterization{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    // ge_renderer.cpp already culled on the CPU for this path; culling again
    // here would drop the faces it deliberately kept.
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth_stencil.depthTestEnable = draw.depth_test_enabled ? VK_TRUE : VK_FALSE;
    depth_stencil.depthWriteEnable = draw.depth_write_enabled ? VK_TRUE : VK_FALSE;
    depth_stencil.depthCompareOp =
        draw.depth_test_enabled ? depth_compare(draw.depth_function) : VK_COMPARE_OP_ALWAYS;
    depth_stencil.maxDepthBounds = 1.0f;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = color_write_mask(draw);
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    const std::uint32_t variant = blend_variant(draw);
    if (variant != 0u && variant != 2u) blend.blendEnable = VK_TRUE;
    switch (variant) {
    case 1u:
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case 3u:
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    case 4u:
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        break;
    case 5u:
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    default:
        break;
    }

    VkPipelineColorBlendStateCreateInfo color_blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    color_blend.attachmentCount = 1u;
    color_blend.pAttachments = &blend;

    const std::array<VkDynamicState, 3> dynamic_states{
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = static_cast<std::uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth_stencil;
    info.pColorBlendState = &color_blend;
    info.pDynamicState = &dynamic;
    info.layout = s.pipeline_layout;
    info.renderPass = s.render_pass;
    info.subpass = 0u;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) !=
        VK_SUCCESS)
        return VK_NULL_HANDLE;
    s.pipelines.emplace(key, pipeline);
    s.report.unique_pipeline_keys = s.pipelines.size();
    ++s.report.depth_pipeline_variants_created;
    return pipeline;
}

// Hardware-transform pipeline variant. Mirrors get_pipeline() above almost
// exactly -- same blend/depth/color-write state classification -- with three
// differences: a different vertex shader/input layout (this draw's
// untransformed vertices, generic-format or packed-0115), the larger
// hw-transform pipeline layout, and real GPU-side face culling. The
// screen-space path (get_pipeline) hardcodes VK_CULL_MODE_NONE because
// ge_renderer.cpp has already culled on the CPU by the time vertices get
// there; this path skips that CPU step entirely (the whole point), so the
// GPU must do the culling ge_renderer.cpp would otherwise have done, using
// the same cull_enabled/accept_counter_clockwise state DX12's PSO
// (D3D12_CULL_MODE_BACK, ge_gpu_backend_dx12.cpp) already keys on.
[[nodiscard]] VkPipeline get_hw_pipeline(VulkanGeState &s, const GeGpuDrawDescriptor &draw,
                                         bool packed_0115, bool cull_enabled,
                                         bool accept_counter_clockwise) {
    std::uint64_t key = pipeline_key(draw);
    key |= static_cast<std::uint64_t>(packed_0115 ? 1u : 0u) << 12u;
    key |= static_cast<std::uint64_t>(cull_enabled ? 1u : 0u) << 13u;
    key |= static_cast<std::uint64_t>(accept_counter_clockwise ? 1u : 0u) << 14u;
    if (const auto found = s.hw_pipelines.find(key); found != s.hw_pipelines.end())
        return found->second;

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0] = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = packed_0115 ? s.hw_packed_vertex_shader : s.hw_vertex_shader;
    stages[0].pName = "main";
    stages[1] = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = s.fragment_shader;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0u;
    binding.stride = packed_0115 ? sizeof(Packed0115Vertex) : sizeof(GeGpuVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // Must match shaders/psp_ge_hw.vert or psp_ge_hw_packed0115.vert's input
    // locations exactly, depending on which is bound above.
    std::array<VkVertexInputAttributeDescription, 9> generic_attributes{{
        {0u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GeGpuVertex, x)},
        {1u, 0u, VK_FORMAT_R8G8B8A8_UINT, offsetof(GeGpuVertex, rgba)},
        {2u, 0u, VK_FORMAT_R32G32_SFLOAT, offsetof(GeGpuVertex, u)},
        {3u, 0u, VK_FORMAT_R32_UINT, offsetof(GeGpuVertex, alpha_control)},
        {4u, 0u, VK_FORMAT_R32_UINT, offsetof(GeGpuVertex, texture_control)},
        {5u, 0u, VK_FORMAT_R32_UINT, offsetof(GeGpuVertex, texture_env)},
        {6u, 0u, VK_FORMAT_R32_SFLOAT, offsetof(GeGpuVertex, fog_factor)},
        {7u, 0u, VK_FORMAT_R32_UINT, offsetof(GeGpuVertex, fog_control)},
        {8u, 0u, VK_FORMAT_R32_SFLOAT, offsetof(GeGpuVertex, q)},
    }};
    std::array<VkVertexInputAttributeDescription, 4> packed_attributes{{
        {0u, 0u, VK_FORMAT_R8G8_UINT, offsetof(Packed0115Vertex, u)},
        {1u, 0u, VK_FORMAT_R16_UINT, offsetof(Packed0115Vertex, color5551)},
        {2u, 0u, VK_FORMAT_R16G16_SINT, offsetof(Packed0115Vertex, x)},
        {3u, 0u, VK_FORMAT_R16_SINT, offsetof(Packed0115Vertex, z)},
    }};

    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1u;
    vertex_input.pVertexBindingDescriptions = &binding;
    if (packed_0115) {
        vertex_input.vertexAttributeDescriptionCount =
            static_cast<std::uint32_t>(packed_attributes.size());
        vertex_input.pVertexAttributeDescriptions = packed_attributes.data();
    } else {
        vertex_input.vertexAttributeDescriptionCount =
            static_cast<std::uint32_t>(generic_attributes.size());
        vertex_input.pVertexAttributeDescriptions = generic_attributes.data();
    }

    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1u;
    viewport_state.scissorCount = 1u;

    VkPipelineRasterizationStateCreateInfo rasterization{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = cull_enabled ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    // PSP winding matches DX12's D3D12_CULL_MODE_BACK setup, which treats
    // clockwise as front-facing by D3D convention; accept_counter_clockwise
    // flips which winding this draw's geometry actually uses.
    rasterization.frontFace = accept_counter_clockwise
        ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth_stencil.depthTestEnable = draw.depth_test_enabled ? VK_TRUE : VK_FALSE;
    depth_stencil.depthWriteEnable = draw.depth_write_enabled ? VK_TRUE : VK_FALSE;
    depth_stencil.depthCompareOp =
        draw.depth_test_enabled ? depth_compare(draw.depth_function) : VK_COMPARE_OP_ALWAYS;
    depth_stencil.maxDepthBounds = 1.0f;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = color_write_mask(draw);
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    const std::uint32_t variant = blend_variant(draw);
    if (variant != 0u && variant != 2u) blend.blendEnable = VK_TRUE;
    switch (variant) {
    case 1u:
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case 3u:
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    case 4u:
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        break;
    case 5u:
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    default:
        break;
    }

    VkPipelineColorBlendStateCreateInfo color_blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    color_blend.attachmentCount = 1u;
    color_blend.pAttachments = &blend;

    const std::array<VkDynamicState, 3> dynamic_states{
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = static_cast<std::uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth_stencil;
    info.pColorBlendState = &color_blend;
    info.pDynamicState = &dynamic;
    info.layout = s.hw_pipeline_layout;
    info.renderPass = s.render_pass;
    info.subpass = 0u;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) !=
        VK_SUCCESS)
        return VK_NULL_HANDLE;
    s.hw_pipelines.emplace(key, pipeline);
    return pipeline;
}

void destroy_backend(VulkanGeState &s) noexcept {
    if (s.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(s.device);
        for (auto &entry : s.pipelines) vkDestroyPipeline(s.device, entry.second, nullptr);
        s.pipelines.clear();
        for (auto &entry : s.hw_pipelines) vkDestroyPipeline(s.device, entry.second, nullptr);
        s.hw_pipelines.clear();
        for (auto &entry : s.samplers) vkDestroySampler(s.device, entry.second, nullptr);
        s.samplers.clear();
        const auto destroy_texture = [&s](TextureEntry &entry) {
            if (entry.view != VK_NULL_HANDLE) vkDestroyImageView(s.device, entry.view, nullptr);
            if (entry.image != VK_NULL_HANDLE) vkDestroyImage(s.device, entry.image, nullptr);
            if (entry.memory != VK_NULL_HANDLE) vkFreeMemory(s.device, entry.memory, nullptr);
            entry = TextureEntry{};
        };
        for (auto &entry : s.textures) destroy_texture(entry.second);
        s.textures.clear();
        destroy_texture(s.dummy_texture);
        if (s.vertex_mapped != nullptr) vkUnmapMemory(s.device, s.vertex_memory);
        s.vertex_mapped = nullptr;
        if (s.hw_vertex_mapped != nullptr) vkUnmapMemory(s.device, s.hw_vertex_memory);
        s.hw_vertex_mapped = nullptr;
        if (s.hw_packed_vertex_mapped != nullptr)
            vkUnmapMemory(s.device, s.hw_packed_vertex_memory);
        s.hw_packed_vertex_mapped = nullptr;
        if (s.hw_index_mapped != nullptr) vkUnmapMemory(s.device, s.hw_index_memory);
        s.hw_index_mapped = nullptr;
        const auto destroy_buffer = [&s](VkBuffer &buffer, VkDeviceMemory &memory) {
            if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(s.device, buffer, nullptr);
            if (memory != VK_NULL_HANDLE) vkFreeMemory(s.device, memory, nullptr);
            buffer = VK_NULL_HANDLE;
            memory = VK_NULL_HANDLE;
        };
        destroy_buffer(s.vertex_buffer, s.vertex_memory);
        destroy_buffer(s.readback_buffer, s.readback_memory);
        destroy_buffer(s.staging_buffer, s.staging_memory);
        destroy_buffer(s.hw_vertex_buffer, s.hw_vertex_memory);
        destroy_buffer(s.hw_packed_vertex_buffer, s.hw_packed_vertex_memory);
        destroy_buffer(s.hw_index_buffer, s.hw_index_memory);
        if (s.framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(s.device, s.framebuffer, nullptr);
        if (s.render_pass != VK_NULL_HANDLE) vkDestroyRenderPass(s.device, s.render_pass, nullptr);
        if (s.color_view != VK_NULL_HANDLE) vkDestroyImageView(s.device, s.color_view, nullptr);
        if (s.color_image != VK_NULL_HANDLE) vkDestroyImage(s.device, s.color_image, nullptr);
        if (s.color_memory != VK_NULL_HANDLE) vkFreeMemory(s.device, s.color_memory, nullptr);
        if (s.depth_view != VK_NULL_HANDLE) vkDestroyImageView(s.device, s.depth_view, nullptr);
        if (s.depth_image != VK_NULL_HANDLE) vkDestroyImage(s.device, s.depth_image, nullptr);
        if (s.depth_memory != VK_NULL_HANDLE) vkFreeMemory(s.device, s.depth_memory, nullptr);
        if (s.pipeline_layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(s.device, s.pipeline_layout, nullptr);
        if (s.hw_pipeline_layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(s.device, s.hw_pipeline_layout, nullptr);
        if (s.descriptor_pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(s.device, s.descriptor_pool, nullptr);
        if (s.descriptor_layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(s.device, s.descriptor_layout, nullptr);
        if (s.vertex_shader != VK_NULL_HANDLE)
            vkDestroyShaderModule(s.device, s.vertex_shader, nullptr);
        if (s.fragment_shader != VK_NULL_HANDLE)
            vkDestroyShaderModule(s.device, s.fragment_shader, nullptr);
        if (s.hw_vertex_shader != VK_NULL_HANDLE)
            vkDestroyShaderModule(s.device, s.hw_vertex_shader, nullptr);
        if (s.hw_packed_vertex_shader != VK_NULL_HANDLE)
            vkDestroyShaderModule(s.device, s.hw_packed_vertex_shader, nullptr);
        if (s.fence != VK_NULL_HANDLE) vkDestroyFence(s.device, s.fence, nullptr);
        if (s.command_pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(s.device, s.command_pool, nullptr);
        vkDestroyDevice(s.device, nullptr);
    }
    if (s.instance != VK_NULL_HANDLE) vkDestroyInstance(s.instance, nullptr);

    const GeGpuBackendReport preserved = s.report;
    s = VulkanGeState{};
    s.report = preserved;
    s.enabled = false;
}

[[nodiscard]] bool create_shader_module(VulkanGeState &s, const std::uint32_t *code,
                                        std::size_t byte_size, VkShaderModule &out) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = byte_size;
    info.pCode = code;
    return vkCreateShaderModule(s.device, &info, nullptr, &out) == VK_SUCCESS;
}

[[nodiscard]] std::uint32_t environment_dimension(const char *name, std::uint32_t fallback) {
    const char *text = std::getenv(name);
    if (text == nullptr || *text == '\0') return fallback;
    const long value = std::strtol(text, nullptr, 10);
    if (value < 1 || value > 16384) return fallback;
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] bool create_backend(VulkanGeState &s, std::string &error) {
    // --- Instance / device -------------------------------------------------
    VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app_info.pApplicationName = "VCSNative";
    app_info.pEngineName = "psprecomp-vcs";
    app_info.apiVersion = VK_API_VERSION_1_1;

    std::vector<const char *> instance_extensions;
    VkInstanceCreateFlags instance_flags = 0;
#if defined(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)
    if (instance_extension_supported(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        instance_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app_info;
    instance_info.enabledExtensionCount = static_cast<std::uint32_t>(instance_extensions.size());
    instance_info.ppEnabledExtensionNames = instance_extensions.data();
    instance_info.flags = instance_flags;
    if (const VkResult result = vkCreateInstance(&instance_info, nullptr, &s.instance);
        result != VK_SUCCESS) {
        error = "vkCreateInstance failed (VkResult " + std::to_string(static_cast<int>(result)) +
                "); install the Vulkan loader and MoltenVK (brew install vulkan-loader molten-vk)";
        return false;
    }
    s.report.instance_created = true;
    s.report.loader_opened = true;

    std::uint32_t device_count = 0u;
    vkEnumeratePhysicalDevices(s.instance, &device_count, nullptr);
    if (device_count == 0u) {
        error = "no Vulkan physical devices; the loader could not find MoltenVK's ICD";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(s.instance, &device_count, devices.data());
    s.physical_device = devices.front();
    s.report.physical_device_count = device_count;

    std::uint32_t family_count = 0u;
    vkGetPhysicalDeviceQueueFamilyProperties(s.physical_device, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(s.physical_device, &family_count, families.data());
    for (std::uint32_t index = 0u; index < family_count; ++index) {
        if (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            s.graphics_queue_family = index;
            break;
        }
    }
    if (s.graphics_queue_family == 0xFFFFFFFFu) {
        error = "no graphics-capable Vulkan queue family";
        return false;
    }
    s.report.graphics_queue_family = s.graphics_queue_family;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = s.graphics_queue_family;
    queue_info.queueCount = 1u;
    queue_info.pQueuePriorities = &priority;

    std::vector<const char *> device_extensions;
#if defined(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)
    if (device_extension_supported(s.physical_device, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
        device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif
    // Rendering.AnisotropicFiltering was confirmed a silent no-op previously:
    // every sampler hardcoded maxAnisotropy=1.0f and this device was never
    // asked to enable the feature at all, so even setting anisotropyEnable
    // on a sampler would have gone nowhere. samplerAnisotropy must be
    // requested here, at device creation, before any sampler can use it.
    VkPhysicalDeviceFeatures supported_features{};
    vkGetPhysicalDeviceFeatures(s.physical_device, &supported_features);
    s.anisotropy_supported = supported_features.samplerAnisotropy == VK_TRUE;
    VkPhysicalDeviceFeatures enabled_features{};
    enabled_features.samplerAnisotropy = s.anisotropy_supported ? VK_TRUE : VK_FALSE;

    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1u;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = static_cast<std::uint32_t>(device_extensions.size());
    device_info.ppEnabledExtensionNames = device_extensions.data();
    device_info.pEnabledFeatures = &enabled_features;
    if (vkCreateDevice(s.physical_device, &device_info, nullptr, &s.device) != VK_SUCCESS) {
        error = "vkCreateDevice failed";
        return false;
    }
    s.report.device_created = true;
    vkGetDeviceQueue(s.device, s.graphics_queue_family, 0u, &s.graphics_queue);
    if (s.anisotropy_supported) {
        VkPhysicalDeviceProperties device_properties{};
        vkGetPhysicalDeviceProperties(s.physical_device, &device_properties);
        s.max_supported_anisotropy = device_properties.limits.maxSamplerAnisotropy;
    }

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = s.graphics_queue_family;
    if (vkCreateCommandPool(s.device, &pool_info, nullptr, &s.command_pool) != VK_SUCCESS) {
        error = "vkCreateCommandPool failed";
        return false;
    }
    s.report.command_pool_created = true;

    VkCommandBufferAllocateInfo command_allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_allocate.commandPool = s.command_pool;
    command_allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_allocate.commandBufferCount = 1u;
    if (vkAllocateCommandBuffers(s.device, &command_allocate, &s.command_buffer) != VK_SUCCESS) {
        error = "vkAllocateCommandBuffers failed";
        return false;
    }
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(s.device, &fence_info, nullptr, &s.fence) != VK_SUCCESS) {
        error = "vkCreateFence failed";
        return false;
    }

    // --- Render target ----------------------------------------------------
    s.width = environment_dimension("PSPRECOMP_INTERNAL_WIDTH", 480u);
    s.height = environment_dimension("PSPRECOMP_INTERNAL_HEIGHT", 272u);
    s.report.offscreen_width = s.width;
    s.report.offscreen_height = s.height;

    const auto create_attachment = [&](VkFormat format, VkImageUsageFlags usage,
                                       VkImageAspectFlags aspect, VkImage &image,
                                       VkDeviceMemory &memory, VkImageView &view) -> bool {
        VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = format;
        image_info.extent = {s.width, s.height, 1u};
        image_info.mipLevels = 1u;
        image_info.arrayLayers = 1u;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = usage;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(s.device, &image_info, nullptr, &image) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(s.device, image, &requirements);
        std::uint32_t type_index = 0u;
        if (!find_memory_type(s.physical_device, requirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type_index))
            return false;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type_index;
        if (vkAllocateMemory(s.device, &allocate, nullptr, &memory) != VK_SUCCESS) return false;
        if (vkBindImageMemory(s.device, image, memory, 0) != VK_SUCCESS) return false;
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format;
        view_info.subresourceRange.aspectMask = aspect;
        view_info.subresourceRange.levelCount = 1u;
        view_info.subresourceRange.layerCount = 1u;
        return vkCreateImageView(s.device, &view_info, nullptr, &view) == VK_SUCCESS;
    };

    if (!create_attachment(kColorFormat,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, s.color_image, s.color_memory,
                           s.color_view)) {
        error = "failed to create the color render target";
        return false;
    }
    s.report.offscreen_image_created = true;
    s.report.offscreen_image_memory_bound = true;
    s.report.offscreen_image_view_created = true;

    const VkFormat depth_format =
        select_depth_format(s.physical_device, vcs_configuration().rendering.depth_precision);
    if (!create_attachment(depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                           VK_IMAGE_ASPECT_DEPTH_BIT, s.depth_image, s.depth_memory,
                           s.depth_view)) {
        error = "failed to create the depth attachment";
        return false;
    }
    s.report.depth_image_created = true;
    s.report.depth_image_memory_bound = true;
    s.report.depth_image_view_created = true;
    s.report.depth_attachment_active = true;
    s.report.depth_bits = depth_format == VK_FORMAT_D32_SFLOAT ? 32u
                         : depth_format == VK_FORMAT_D24_UNORM_S8_UINT ? 24u
                         : 16u;

    std::array<VkAttachmentDescription, 2> attachments{};
    attachments[0].format = kColorFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // Ends the pass ready for the readback copy, so no extra barrier is needed.
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments[1].format = depth_format;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_reference{0u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_reference{1u, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1u;
    subpass.pColorAttachments = &color_reference;
    subpass.pDepthStencilAttachment = &depth_reference;

    VkRenderPassCreateInfo render_pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    render_pass_info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    render_pass_info.pAttachments = attachments.data();
    render_pass_info.subpassCount = 1u;
    render_pass_info.pSubpasses = &subpass;
    if (vkCreateRenderPass(s.device, &render_pass_info, nullptr, &s.render_pass) != VK_SUCCESS) {
        error = "vkCreateRenderPass failed";
        return false;
    }
    s.report.render_pass_created = true;

    const std::array<VkImageView, 2> views{s.color_view, s.depth_view};
    VkFramebufferCreateInfo framebuffer_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer_info.renderPass = s.render_pass;
    framebuffer_info.attachmentCount = static_cast<std::uint32_t>(views.size());
    framebuffer_info.pAttachments = views.data();
    framebuffer_info.width = s.width;
    framebuffer_info.height = s.height;
    framebuffer_info.layers = 1u;
    if (vkCreateFramebuffer(s.device, &framebuffer_info, nullptr, &s.framebuffer) != VK_SUCCESS) {
        error = "vkCreateFramebuffer failed";
        return false;
    }
    s.report.framebuffer_created = true;

    // --- Shaders / descriptors / layout ------------------------------------
    if (!create_shader_module(s, kPspGeVertSpv, sizeof(kPspGeVertSpv), s.vertex_shader) ||
        !create_shader_module(s, kPspGeFragSpv, sizeof(kPspGeFragSpv), s.fragment_shader)) {
        error = "vkCreateShaderModule failed for the GE shaders";
        return false;
    }
    if (!create_shader_module(s, kPspGeHwVertSpv, sizeof(kPspGeHwVertSpv), s.hw_vertex_shader) ||
        !create_shader_module(s, kPspGeHwPacked0115VertSpv, sizeof(kPspGeHwPacked0115VertSpv),
                              s.hw_packed_vertex_shader)) {
        error = "vkCreateShaderModule failed for the hardware-transform GE shaders";
        return false;
    }
    s.report.shader_modules_created = true;
    s.report.textured_shader_modules_created = true;

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0u;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1u;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.bindingCount = 1u;
    layout_info.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(s.device, &layout_info, nullptr, &s.descriptor_layout) !=
        VK_SUCCESS) {
        error = "vkCreateDescriptorSetLayout failed";
        return false;
    }
    s.report.texture_descriptor_layout_created = true;

    // Was a hardcoded 4096, unlike DX12's cache (Rendering.TextureCacheEntries,
    // 8192 in the shipped .ini) -- and with no eviction at all (see the old
    // unconditional "cache full, refuse" in upload_decoded_texture below),
    // VCS's texture variety across a real play session walks straight past
    // 4096 unique textures after a minute or two, after which every further
    // unique texture silently fails to upload for the rest of the run and
    // that geometry is left showing whatever stale/CPU-composited result was
    // already there -- confirmed live via the F9 GPU frame dump (textures=
    // tex_uploads=4096 exactly, 0 rejected, at the point corruption was
    // reported starting). Sized from the same config DX12 reads, floored so
    // a pathological config value still leaves useful headroom.
    s.texture_capacity =
        std::max<std::uint32_t>(512u, vcs_configuration().rendering.texture_cache_entries);
    const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                         s.texture_capacity + 1u};
    VkDescriptorPoolCreateInfo descriptor_pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    // FREE_DESCRIPTOR_SET_BIT: eviction below frees one entry's descriptor
    // set individually to reuse the slot, rather than only ever growing
    // until the pool (sized once, up front) runs out.
    descriptor_pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    descriptor_pool_info.maxSets = s.texture_capacity + 1u;
    descriptor_pool_info.poolSizeCount = 1u;
    descriptor_pool_info.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(s.device, &descriptor_pool_info, nullptr, &s.descriptor_pool) !=
        VK_SUCCESS) {
        error = "vkCreateDescriptorPool failed";
        return false;
    }
    s.report.texture_descriptor_pool_created = true;

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo pipeline_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout_info.setLayoutCount = 1u;
    pipeline_layout_info.pSetLayouts = &s.descriptor_layout;
    pipeline_layout_info.pushConstantRangeCount = 1u;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(s.device, &pipeline_layout_info, nullptr, &s.pipeline_layout) !=
        VK_SUCCESS) {
        error = "vkCreatePipelineLayout failed";
        return false;
    }

    // Second pipeline layout for the hardware-transform shaders: their
    // push-constant block (HwTransformPushConstants, ~176 bytes) is well
    // over the screen-space path's, and over the Vulkan spec's guaranteed
    // minimum (128 bytes) -- but this backend only ever runs on Apple
    // Silicon via MoltenVK, which reports a much larger limit in practice.
    // Checked explicitly rather than assumed, so a future non-Apple Vulkan
    // target fails loudly here instead of silently corrupting geometry.
    VkPhysicalDeviceProperties device_properties{};
    vkGetPhysicalDeviceProperties(s.physical_device, &device_properties);
    if (device_properties.limits.maxPushConstantsSize < sizeof(HwTransformPushConstants)) {
        error = "device's maxPushConstantsSize is too small for the hardware-transform shaders";
        return false;
    }
    VkPushConstantRange hw_push_range{};
    hw_push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    hw_push_range.size = sizeof(HwTransformPushConstants);
    VkPipelineLayoutCreateInfo hw_pipeline_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    hw_pipeline_layout_info.setLayoutCount = 1u;
    hw_pipeline_layout_info.pSetLayouts = &s.descriptor_layout;
    hw_pipeline_layout_info.pushConstantRangeCount = 1u;
    hw_pipeline_layout_info.pPushConstantRanges = &hw_push_range;
    if (vkCreatePipelineLayout(s.device, &hw_pipeline_layout_info, nullptr, &s.hw_pipeline_layout) !=
        VK_SUCCESS) {
        error = "vkCreatePipelineLayout failed for the hardware-transform layout";
        return false;
    }

    // --- Buffers ----------------------------------------------------------
    if (!create_buffer(s, kInitialVertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       s.vertex_buffer, s.vertex_memory)) {
        error = "failed to create the vertex buffer";
        return false;
    }
    s.vertex_capacity = kInitialVertexBytes;
    if (vkMapMemory(s.device, s.vertex_memory, 0, kInitialVertexBytes, 0, &s.vertex_mapped) !=
        VK_SUCCESS) {
        error = "vkMapMemory failed for the vertex buffer";
        return false;
    }
    s.report.upload_capacity_bytes = kInitialVertexBytes;

    // Hardware-transform buffers, sized the same as the screen-space vertex
    // buffer to start; finish_color_frame() grows each on demand exactly
    // like the screen-space path already does for s.vertex_buffer.
    if (!create_buffer(s, kInitialVertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       s.hw_vertex_buffer, s.hw_vertex_memory) ||
        vkMapMemory(s.device, s.hw_vertex_memory, 0, kInitialVertexBytes, 0, &s.hw_vertex_mapped) !=
            VK_SUCCESS) {
        error = "failed to create the hardware-transform vertex buffer";
        return false;
    }
    s.hw_vertex_capacity = kInitialVertexBytes;

    if (!create_buffer(s, kInitialVertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       s.hw_packed_vertex_buffer, s.hw_packed_vertex_memory) ||
        vkMapMemory(s.device, s.hw_packed_vertex_memory, 0, kInitialVertexBytes, 0,
                   &s.hw_packed_vertex_mapped) != VK_SUCCESS) {
        error = "failed to create the packed-0115 hardware-transform vertex buffer";
        return false;
    }
    s.hw_packed_vertex_capacity = kInitialVertexBytes;

    if (!create_buffer(s, kInitialVertexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       s.hw_index_buffer, s.hw_index_memory) ||
        vkMapMemory(s.device, s.hw_index_memory, 0, kInitialVertexBytes, 0, &s.hw_index_mapped) !=
            VK_SUCCESS) {
        error = "failed to create the hardware-transform index buffer";
        return false;
    }
    s.hw_index_capacity = kInitialVertexBytes;

    const VkDeviceSize readback_bytes = VkDeviceSize{s.width} * s.height * 4u;
    if (!create_buffer(s, readback_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       s.readback_buffer, s.readback_memory)) {
        error = "failed to create the readback buffer";
        return false;
    }
    s.readback_capacity = readback_bytes;
    s.report.transfer_buffer_created = true;
    s.report.transfer_memory_mapped = true;

    // 1x1 white texture, bound for untextured draws so one pipeline layout and
    // one descriptor slot serve every batch.
    const std::array<std::byte, 4> white{std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
                                         std::byte{0xFF}};
    VkSamplerCreateInfo dummy_sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    dummy_sampler_info.maxAnisotropy = 1.0f;
    VkSampler dummy_sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(s.device, &dummy_sampler_info, nullptr, &dummy_sampler) != VK_SUCCESS) {
        error = "vkCreateSampler failed for the dummy texture";
        return false;
    }
    s.samplers.emplace(~0ull, dummy_sampler);
    if (!create_texture(s, 1u, 1u, std::span<const std::byte>(white.data(), white.size()),
                        dummy_sampler, s.dummy_texture)) {
        error = "failed to create the 1x1 dummy texture";
        return false;
    }

    s.report.graphics_pipeline_created = true;
    s.report.textured_pipeline_created = true;
    s.report.alpha_test_shader_active = true;
    s.report.fog_shader_active = true;
    s.report.complete_texture_function_shader_active = true;
    s.report.standard_alpha_blend_pipeline_active = true;
    s.report.observed_blend_modes_pipeline_active = true;
    s.report.color_write_mask_pipeline_active = true;
    s.report.base_texture_formats_active = true;
    s.report.frames_in_flight_capacity = 1u;
    return true;
}

// Packed as R8G8B8A8_UINT and consumed by psp_ge.frag as texture_control:
// byte0 = GE texture function (MODULATE/DECAL/BLEND/REPLACE/ADD), byte1 =
// use-alpha, byte2 = double-color, byte3 = whether texturing is enabled for
// this draw at all -- the shader's sole gate on sampling source_texture.
// Mirrors ge_gpu_backend_dx12.cpp's packed_texture_control() exactly. Every
// GeGpuVertex arrives from ge_renderer.cpp with this field zeroed (see its
// finalize_vertex(), "backend fills current texture function"); leaving it
// zero here means texture_control.w is always 0, the shader's texture
// branch never runs, and every draw shows its raw (often white/unused,
// since REPLACE-mode geometry doesn't care about vertex color) vertex color
// instead -- which is exactly the flat white screen this was chasing.
[[nodiscard]] std::uint32_t packed_texture_control(const GeGpuDrawDescriptor &draw,
                                                   bool enabled) noexcept {
    return (draw.texture_function & 0xFFu) |
           (static_cast<std::uint32_t>(draw.texture_use_alpha ? 1u : 0u) << 8u) |
           (static_cast<std::uint32_t>(draw.texture_double_color ? 1u : 0u) << 16u) |
           (static_cast<std::uint32_t>(enabled ? 1u : 0u) << 24u);
}

} // namespace

// ---------------------------------------------------------------------------
// Interface
// ---------------------------------------------------------------------------

bool initialize_ge_gpu_backend(std::string &error) {
    VulkanGeState &s = state();
    destroy_backend(s);
    s.report = GeGpuBackendReport{};
    s.display_framebuffer = 0u;
    s.frame_buckets.clear();
    s.submission_pending = false;
    s.frame_valid = false;
    s.has_last_winner = false;

    const RenderingConfiguration &rendering = vcs_configuration().rendering;
    s.report.requested = rendering.backend == RenderingBackend::DirectX12
        ? GeGpuBackendKind::Vulkan : GeGpuBackendKind::Software;
    s.report.active = GeGpuBackendKind::Software;
    if (rendering.backend != RenderingBackend::DirectX12) {
        s.report.message = "Software GE backend active";
        error.clear();
        return true;
    }
    if (!rendering.dx12_ge_color) {
        s.report.message = "Native Vulkan GE available but DX12GEColor=false";
        error.clear();
        return true;
    }

    if (!create_backend(s, error)) {
        const std::string native_error = error;
        runtime_log_error("vulkan ge initialize", native_error);
        destroy_backend(s);
        const char *strict = std::getenv("PSPRECOMP_VULKAN_GE_STRICT");
        const bool strict_mode = strict != nullptr && *strict != '\0' && *strict != '0';
        s.report.requested = GeGpuBackendKind::Vulkan;
        s.report.active = GeGpuBackendKind::Software;
        s.report.message = "Native Vulkan/MoltenVK GE failed; using software GE: " + native_error;
        error = native_error;
        return !strict_mode;
    }

    s.enabled = true;
    s.report.active = GeGpuBackendKind::Vulkan;
    s.report.message = "Native Vulkan/MoltenVK GE active (screen-space path; requires "
                       "PSPRECOMP_GE_GPU_HW_TRANSFORM=0)";
    error.clear();
    return true;
}

void shutdown_ge_gpu_backend() noexcept { destroy_backend(state()); }

void ge_gpu_backend_lock() noexcept { backend_mutex().lock(); }
void ge_gpu_backend_unlock() noexcept { backend_mutex().unlock(); }

bool ge_gpu_backend_active() noexcept { return state().enabled; }
bool ge_gpu_backend_transfer_ready() noexcept { return state().enabled; }
bool ge_gpu_backend_graphics_ready() noexcept { return state().enabled; }

void ge_gpu_backend_record_draw(const GeGpuDrawDescriptor &draw) noexcept {
    VulkanGeState &s = state();
    if (!s.enabled) return;
    ++s.report.captured_draws;
    if (draw.framebuffer_address != 0u) ++s.report.framebuffer_targets_observed;
}

void ge_gpu_backend_observe_camera(const std::array<float, 12> &, const std::array<float, 16> &,
                                   const std::array<float, 6> &, const std::array<float, 3> &,
                                   const GeGpuDrawDescriptor &, std::uint32_t) noexcept {}

bool ge_gpu_backend_stage_vertices(const GeGpuDrawDescriptor &,
                                   std::span<const GeGpuVertex>) noexcept {
    return false;
}

bool ge_gpu_backend_texture_needed(const GeGpuDrawDescriptor &draw) noexcept {
    VulkanGeState &s = state();
    if (!s.enabled || !draw.texture_enabled) return false;
    return s.textures.find(texture_key(draw)) == s.textures.end();
}

void ge_gpu_backend_prepare_texture_keys(GeGpuDrawDescriptor &draw) noexcept {
    if (draw.texture_cache_key_hint == 0u) draw.texture_cache_key_hint = texture_key(draw);
    if (draw.texture_image_key_hint == 0u)
        draw.texture_image_key_hint = draw.texture_cache_key_hint;
}

bool ge_gpu_backend_texture_signature_needed(const GeGpuDrawDescriptor &draw) noexcept {
    return ge_gpu_backend_texture_needed(draw);
}

bool ge_gpu_backend_is_framebuffer_feedback_texture(const GeGpuDrawDescriptor &) noexcept {
    // Framebuffer feedback (draws sampling a render target) is not implemented
    // in this revision; false keeps the renderer on its CPU path for those
    // draws rather than sampling an image this backend never produced.
    return false;
}

// Was a complete no-op stub (always returned shrink=1.0, i.e. "no
// correction"), unlike DX12's real implementation -- confirmed the direct
// cause of HUD elements (money counter, health bar) running off the edge of
// the screen on this Vulkan build specifically at a non-16:9 internal
// resolution (a MacBook's actual display is very often not 16:9 -- e.g.
// 1.6:1 on this machine -- once InternalResolutionMode::Desktop started
// querying the real display instead of silently defaulting to 1920x1080).
// The 2D HUD is authored in a fixed 480-wide screen space that has no idea
// the 3D view was widened to fill a wider-than-4:3 output; without shrinking
// HUD X coordinates back down by the same factor the view was stretched by,
// elements anchored near the original 480-wide right edge land past the
// actual (wider) output's right edge instead. Ported from DX12's
// implementation (ge_gpu_backend_dx12.cpp) using the same shared, portable
// widescreen_stretch_factor()/resolve_display_surface_dimensions() helpers
// (vcs_config.cpp) DX12 uses -- this math was never backend-specific, only
// never called from here. One difference: DX12 looks up a per-target
// logical_width (oversized offscreen render targets can use a wider space
// than the standard 480), which this backend does not currently track per
// target; falling back to the standard 480 reference width covers the
// reported bug (the ordinary display HUD) without needing that extra
// tracking machinery.
GeGpuWidescreenHud ge_gpu_backend_widescreen_hud(const GeGpuDrawDescriptor &) noexcept {
    GeGpuWidescreenHud hud{};
    VulkanGeState &s = state();
    if (!s.enabled) return hud;

    const VcsConfiguration &config = vcs_configuration();
    if (!config.initialized || !config.widescreen.enabled) return hud;

    const DisplaySurfaceDimensions output = resolve_display_surface_dimensions(config.display);
    const float shrink = widescreen_stretch_factor(config, output.width, output.height);
    if (!std::isfinite(shrink) || shrink <= 0.0f || std::abs(shrink - 1.0f) < 1.0e-5f)
        return hud;

    constexpr float kReferenceWidth = kPspWidth;  // 480 -- see kPspWidth's definition above.
    hud.shrink = shrink;
    hud.display_scale_x = 1.0f;  // logical_width == kReferenceWidth with no per-target override.
    hud.source_center = kReferenceWidth * 0.5f;
    return hud;
}

void ge_gpu_backend_note_through_extent(const GeGpuDrawDescriptor &, float, float) noexcept {}

bool ge_gpu_backend_adopt_shared_texture(const GeGpuDrawDescriptor &) noexcept { return false; }

bool ge_gpu_backend_texture_available(const GeGpuDrawDescriptor &draw) noexcept {
    VulkanGeState &s = state();
    if (!s.enabled) return false;
    if (!draw.texture_enabled) return true;
    // This is the per-draw "is it cached" check ge_renderer.cpp makes every
    // frame for every textured draw, cached or not -- upload_decoded_texture
    // only runs again once a texture falls OUT of the cache, so a still-
    // resident texture that is drawn every frame would otherwise never
    // refresh its LRU stamp here and could be evicted out from under active
    // geometry the moment the cache fills. Stamping on the lookup itself
    // (not just on upload) is what makes "least recently *drawn*" true.
    const auto found = s.textures.find(texture_key(draw));
    if (found == s.textures.end()) return false;
    found->second.last_used_epoch = s.frame_epoch;
    return true;
}

bool ge_gpu_backend_upload_decoded_texture(const GeGpuDrawDescriptor &draw, std::uint32_t width,
                                           std::uint32_t height,
                                           std::span<const std::byte> rgba8) noexcept {
    VulkanGeState &s = state();
    if (!s.enabled || width == 0u || height == 0u || rgba8.empty()) return false;
    const std::uint64_t key = texture_key(draw);
    if (const auto found = s.textures.find(key); found != s.textures.end()) {
        found->second.last_used_epoch = s.frame_epoch;
        ++s.report.texture_cache_hits;
        return true;
    }
    if (s.textures.size() >= s.texture_capacity) {
        // Evict the least-recently-used entry not touched this frame (same
        // rule DX12's cache uses). If everything resident happens to be in
        // use this very frame, there is nothing safe to evict; fail the
        // upload rather than fight this frame's own draws for a slot.
        auto victim = s.textures.end();
        for (auto it = s.textures.begin(); it != s.textures.end(); ++it) {
            if (it->second.last_used_epoch == s.frame_epoch) continue;
            if (victim == s.textures.end() ||
                it->second.last_used_epoch < victim->second.last_used_epoch)
                victim = it;
        }
        if (victim == s.textures.end()) {
            ++s.report.rejected_texture_decodes;
            return false;
        }
        destroy_texture_entry(s, victim->second);
        s.textures.erase(victim);
        ++s.report.texture_cache_evictions;
    }
    const VkSampler sampler = get_sampler(s, draw);
    if (sampler == VK_NULL_HANDLE) return false;
    TextureEntry entry{};
    if (!create_texture(s, width, height, rgba8, sampler, entry)) {
        ++s.report.rejected_texture_decodes;
        return false;
    }
    entry.last_used_epoch = s.frame_epoch;
    s.textures.emplace(key, entry);
    ++s.report.texture_images_created;
    ++s.report.decoded_texture_uploads;
    s.report.decoded_texture_bytes += static_cast<std::uint64_t>(width) * height * 4u;
    s.report.unique_texture_keys = s.textures.size();
    s.report.unique_texture_image_keys = s.textures.size();
    s.report.last_texture_key = key;
    s.report.last_texture_width = width;
    s.report.last_texture_height = height;
    s.report.last_texture_format = draw.texture_format;
    return true;
}

bool ge_gpu_backend_upload_decoded_texture_chain(
    const GeGpuDrawDescriptor &draw, std::span<const GeGpuDecodedMipLevel> levels) noexcept {
    // Mip chains upload as their base level in this revision: sampling uses a
    // single level, so the rest would never be read.
    if (levels.empty()) return false;
    const GeGpuDecodedMipLevel &base = levels.front();
    return ge_gpu_backend_upload_decoded_texture(draw, base.width, base.height, base.rgba8);
}

bool ge_gpu_backend_upload_decoded_texture_chain_packed(const GeGpuDrawDescriptor &draw,
                                                        std::uint32_t base_width,
                                                        std::uint32_t base_height, std::uint32_t,
                                                        std::vector<std::byte> rgba8) noexcept {
    return ge_gpu_backend_upload_decoded_texture(
        draw, base_width, base_height, std::span<const std::byte>(rgba8.data(), rgba8.size()));
}

bool ge_gpu_backend_copy_last_texture_rgba(std::span<std::byte>) noexcept { return false; }

void ge_gpu_backend_accumulate_color_triangles(
    const GeGpuDrawDescriptor &draw, std::span<const GeGpuVertex> triangle_vertices) noexcept {
    VulkanGeState &s = state();
    if (!s.enabled || triangle_vertices.size() < 3u) return;
    // An empty scissor rectangle means the batch contributes nothing.
    if (draw.scissor_x1 < draw.scissor_x0 || draw.scissor_y1 < draw.scissor_y0) return;

    VkDescriptorSet descriptor = s.dummy_texture.descriptor;
    if (draw.texture_enabled) {
        const auto found = s.textures.find(texture_key(draw));
        if (found == s.textures.end()) {
            // The renderer has not decoded this texture yet. Drawing it with
            // the white dummy would paint untextured geometry over the frame,
            // which is worse than leaving the software result in place.
            ++s.report.game_textured_draws_without_texture;
            return;
        }
        descriptor = found->second.descriptor;
        ++s.report.textured_game_draw_calls;
    }

    const VkPipeline pipeline = get_pipeline(s, draw);
    if (pipeline == VK_NULL_HANDLE) {
        ++s.report.rejected_gpu_draws;
        return;
    }

    if (std::getenv("PSPRECOMP_GE_GPU_DRAW_DIAG") != nullptr) {
        float min_y = triangle_vertices[0].y, max_y = triangle_vertices[0].y;
        float min_x = triangle_vertices[0].x, max_x = triangle_vertices[0].x;
        for (const GeGpuVertex &v : triangle_vertices) {
            min_y = std::min(min_y, v.y); max_y = std::max(max_y, v.y);
            min_x = std::min(min_x, v.x); max_x = std::max(max_x, v.x);
        }
        // Broadened: any draw spanning a large vertical range, regardless of
        // depth-test state (the depth_test==false filter caught nothing but
        // two harmless boot-time VRAM clears -- the real culprit apparently
        // does have depth testing on, so it's not a simple undepth-tested
        // overlay).
        if ((max_y - min_y) > 100.0f) {
            static std::unordered_set<std::uint64_t> seen;
            const std::uint64_t sig =
                (static_cast<std::uint64_t>(draw.framebuffer_address) << 32u) ^
                (static_cast<std::uint64_t>(draw.texture_address) << 16u) ^
                (static_cast<std::uint64_t>(draw.scissor_y0) << 8u) ^
                static_cast<std::uint64_t>(draw.scissor_y1);
            if (seen.insert(sig).second) {
                std::fprintf(stderr,
                    "[diag-draw] fb=0x%08X tex=0x%08X clear_mode=%d depth_test=%d "
                    "depth_write=%d depth_fn=%u blend=%d texture=%d texfunc=%u "
                    "scissor=[%d,%d,%d,%d] x=[%.1f,%.1f] y=[%.1f,%.1f] verts=%zu\n",
                    draw.framebuffer_address, draw.texture_address, draw.clear_mode ? 1 : 0,
                    draw.depth_test_enabled ? 1 : 0, draw.depth_write_enabled ? 1 : 0,
                    draw.depth_function, draw.blend_enabled ? 1 : 0, draw.texture_enabled ? 1 : 0,
                    draw.texture_function,
                    draw.scissor_x0, draw.scissor_y0, draw.scissor_x1, draw.scissor_y1,
                    min_x, max_x, min_y, max_y, triangle_vertices.size());
            }
        }
    }

    // Every draw goes into the bucket for its own target address; which
    // bucket actually becomes "the frame" is decided once, at
    // finish_color_frame() time, by comparing accumulated geometry across
    // all of them (see VulkanGeState::frame_buckets).
    const std::uint32_t address = draw.framebuffer_address & 0x001FFFF0u;
    VulkanGeState::FrameBucket *bucket = nullptr;
    for (VulkanGeState::FrameBucket &candidate : s.frame_buckets) {
        if (candidate.address == address) { bucket = &candidate; break; }
    }
    if (bucket == nullptr) {
        s.frame_buckets.push_back(VulkanGeState::FrameBucket{address, {}, {}});
        bucket = &s.frame_buckets.back();
    }

    Batch batch{};
    batch.first_vertex = static_cast<std::uint32_t>(bucket->vertices.size());
    batch.vertex_count =
        static_cast<std::uint32_t>(triangle_vertices.size() - triangle_vertices.size() % 3u);
    if (batch.vertex_count == 0u) return;
    batch.pipeline = pipeline;
    batch.framebuffer_format = draw.framebuffer_format;
    batch.descriptor = descriptor;
    batch.scissor = {draw.scissor_x0, draw.scissor_y0, draw.scissor_x1, draw.scissor_y1};
    batch.framebuffer_stride = draw.framebuffer_stride;
    if (blend_variant(draw) == 4u) {
        const std::uint32_t fixed = draw.blend_fix_source;
        batch.blend_constants = {static_cast<float>(fixed & 0xFFu) / 255.0f,
                                 static_cast<float>((fixed >> 8u) & 0xFFu) / 255.0f,
                                 static_cast<float>((fixed >> 16u) & 0xFFu) / 255.0f, 1.0f};
    }

    const std::uint32_t texture_control =
        packed_texture_control(draw, draw.texture_enabled);
    const std::size_t insert_offset = bucket->vertices.size();
    bucket->vertices.insert(bucket->vertices.end(), triangle_vertices.begin(),
                            triangle_vertices.begin() +
                                static_cast<std::ptrdiff_t>(batch.vertex_count));
    for (std::size_t i = insert_offset; i < bucket->vertices.size(); ++i)
        bucket->vertices[i].texture_control = texture_control;
    bucket->batches.push_back(batch);
    ++s.report.game_draw_calls;
    s.report.game_vertices += batch.vertex_count;
    s.report.game_triangles += batch.vertex_count / 3u;
    if (draw.depth_test_enabled) ++s.report.depth_tested_game_draw_calls;
    if (draw.depth_write_enabled) ++s.report.depth_writing_game_draw_calls;
    if (draw.alpha_test_enabled) ++s.report.alpha_tested_game_draw_calls;
    if (draw.fog_enabled) ++s.report.fogged_game_draw_calls;
}

// Builds the push-constant block for one hardware-transform draw, folding
// the viewport scale/offset directly into row0/1/2 exactly the way DX12's
// build_transform_constants() does (ge_gpu_backend_dx12.cpp) -- so this
// shader can stay a plain dot(row, p) per component, like DX12's VSMain,
// instead of carrying separate viewport constants. The one deliberate
// difference: DX12 negates the y terms there for D3D's y-up NDC; this does
// NOT, matching psp_ge.vert's existing y-down handling for Vulkan (see that
// shader's header comment).
[[nodiscard]] HwTransformPushConstants build_hw_transform_push_constants(
    const GeGpuHardwareTransform &hw) noexcept {
    HwTransformPushConstants pc{};
    const auto row = [&](std::size_t r) {
        return std::array<float, 4>{hw.model_to_clip[r], hw.model_to_clip[4u + r],
                                    hw.model_to_clip[8u + r], hw.model_to_clip[12u + r]};
    };
    const auto add_scaled = [](const std::array<float, 4> &a, float sa,
                               const std::array<float, 4> &b, float sb) {
        return std::array<float, 4>{a[0] * sa + b[0] * sb, a[1] * sa + b[1] * sb,
                                    a[2] * sa + b[2] * sb, a[3] * sa + b[3] * sb};
    };
    const auto clip_x = row(0u);
    const auto clip_y = row(1u);
    const auto clip_z = row(2u);
    const auto clip_w = row(3u);
    const float x_a = hw.viewport_scale_x * (2.0f / kPspWidth);
    const float x_b = (hw.viewport_center_x - hw.viewport_offset_x) * (2.0f / kPspWidth) - 1.0f;
    // No negation here (see header comment) -- Vulkan's y-down NDC wants the
    // natural sign, unlike D3D's y-up.
    const float y_a = hw.viewport_scale_y * (2.0f / kPspHeight);
    const float y_b = (hw.viewport_center_y - hw.viewport_offset_y) * (2.0f / kPspHeight) - 1.0f;
    constexpr float inv_depth = 1.0f / 65535.0f;
    const float z_a = hw.viewport_scale_z * inv_depth;
    const float z_b = hw.viewport_center_z * inv_depth;
    const auto store = [](float *dst, const std::array<float, 4> &src) {
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
    };
    store(pc.row0, add_scaled(clip_x, x_a, clip_w, x_b));
    store(pc.row1, add_scaled(clip_y, y_a, clip_w, y_b));
    store(pc.row2, add_scaled(clip_z, z_a, clip_w, z_b));
    store(pc.row3, clip_w);
    pc.model_to_view_z[0] = hw.model_to_view_z[0];
    pc.model_to_view_z[1] = hw.model_to_view_z[1];
    pc.model_to_view_z[2] = hw.model_to_view_z[2];
    pc.model_to_view_z[3] = hw.model_to_view_z[3];
    pc.uv_scale_offset[0] = hw.uv_scale_u;
    pc.uv_scale_offset[1] = hw.uv_scale_v;
    pc.uv_scale_offset[2] = hw.uv_offset_u;
    pc.uv_scale_offset[3] = hw.uv_offset_v;
    pc.fog_parameters[0] = hw.fog_end;
    pc.fog_parameters[1] = hw.fog_slope;
    pc.transform_control[0] = 1u;
    pc.transform_control[1] = hw.depth_clip_enabled ? 1u : 0u;
    pc.transform_control[2] = hw.vertex_color_affine ? 1u : 0u;
    pc.vertex_color_mul[0] = hw.vertex_color_mul[0];
    pc.vertex_color_mul[1] = hw.vertex_color_mul[1];
    pc.vertex_color_mul[2] = hw.vertex_color_mul[2];
    pc.vertex_color_mul[3] = hw.vertex_color_mul[3];
    pc.vertex_color_add[0] = hw.vertex_color_add[0];
    pc.vertex_color_add[1] = hw.vertex_color_add[1];
    pc.vertex_color_add[2] = hw.vertex_color_add[2];
    pc.vertex_color_add[3] = hw.vertex_color_add[3];
    return pc;
}

// Shared by both accumulate_hardware_* entry points below: resolves this
// draw's texture descriptor (or bails the same way accumulate_color_triangles
// does when the renderer hasn't decoded it yet) and finds/creates this
// vblank's bucket for the draw's target address.
[[nodiscard]] bool prepare_hw_batch_target(VulkanGeState &s, const GeGpuDrawDescriptor &draw,
                                           VkDescriptorSet &descriptor,
                                           VulkanGeState::FrameBucket *&bucket) noexcept {
    if (!s.enabled) return false;
    if (draw.scissor_x1 < draw.scissor_x0 || draw.scissor_y1 < draw.scissor_y0) return false;
    descriptor = s.dummy_texture.descriptor;
    if (draw.texture_enabled) {
        const auto found = s.textures.find(texture_key(draw));
        if (found == s.textures.end()) {
            ++s.report.game_textured_draws_without_texture;
            return false;
        }
        descriptor = found->second.descriptor;
        ++s.report.textured_game_draw_calls;
    }
    const std::uint32_t address = draw.framebuffer_address & 0x001FFFF0u;
    bucket = nullptr;
    for (VulkanGeState::FrameBucket &candidate : s.frame_buckets) {
        if (candidate.address == address) { bucket = &candidate; break; }
    }
    if (bucket == nullptr) {
        s.frame_buckets.push_back(VulkanGeState::FrameBucket{address, {}, {}, {}, {}, {}, {}});
        bucket = &s.frame_buckets.back();
    }
    return true;
}

void ge_gpu_backend_accumulate_hardware_triangles(
    const GeGpuDrawDescriptor &draw, const GeGpuHardwareTransform &transform,
    std::span<const GeGpuVertex> vertices, std::span<const std::uint32_t> triangle_indices) noexcept {
    VulkanGeState &s = state();
    if (vertices.empty()) return;
    const std::size_t emitted_count = triangle_indices.empty() ? vertices.size()
                                                               : triangle_indices.size();
    if (emitted_count == 0u ||
        (transform.primitive == 4u ? emitted_count < 3u : (emitted_count % 3u) != 0u)) return;

    VkDescriptorSet descriptor{};
    VulkanGeState::FrameBucket *bucket = nullptr;
    if (!prepare_hw_batch_target(s, draw, descriptor, bucket)) return;

    const VkPipeline pipeline = get_hw_pipeline(s, draw, /*packed_0115=*/false,
                                                transform.cull_enabled,
                                                transform.accept_counter_clockwise);
    if (pipeline == VK_NULL_HANDLE) {
        ++s.report.rejected_gpu_draws;
        return;
    }

    HwTransformBatch batch{};
    batch.first_vertex = static_cast<std::uint32_t>(bucket->hw_vertices.size());
    batch.vertex_count = static_cast<std::uint32_t>(vertices.size());
    batch.first_index = static_cast<std::uint32_t>(bucket->hw_indices.size());
    batch.index_count = static_cast<std::uint32_t>(triangle_indices.size());
    batch.packed_0115 = false;
    batch.pipeline = pipeline;
    batch.descriptor = descriptor;
    batch.scissor = {draw.scissor_x0, draw.scissor_y0, draw.scissor_x1, draw.scissor_y1};
    if (blend_variant(draw) == 4u) {
        const std::uint32_t fixed = draw.blend_fix_source;
        batch.blend_constants = {static_cast<float>(fixed & 0xFFu) / 255.0f,
                                 static_cast<float>((fixed >> 8u) & 0xFFu) / 255.0f,
                                 static_cast<float>((fixed >> 16u) & 0xFFu) / 255.0f, 1.0f};
    }
    batch.constants = build_hw_transform_push_constants(transform);
    const std::uint32_t texture_control = packed_texture_control(draw, draw.texture_enabled);

    bucket->hw_vertices.insert(bucket->hw_vertices.end(), vertices.begin(), vertices.end());
    for (std::size_t i = batch.first_vertex; i < bucket->hw_vertices.size(); ++i)
        bucket->hw_vertices[i].texture_control = texture_control;
    if (!triangle_indices.empty())
        bucket->hw_indices.insert(bucket->hw_indices.end(), triangle_indices.begin(),
                                  triangle_indices.end());
    bucket->hw_batches.push_back(batch);

    const std::uint32_t logical = std::max<std::uint32_t>(1u, transform.logical_prim_batches);
    s.report.game_draw_calls += logical;
    s.report.game_triangles += transform.primitive == 4u
        ? (emitted_count > 2u ? emitted_count - 2u : 0u) : emitted_count / 3u;
    s.report.game_vertices += emitted_count;
    s.report.hw_transform_draw_calls += logical;
    s.report.hw_transform_vertices += vertices.size();
    s.report.hw_transform_prim_batches += logical;
    s.report.hw_transform_unique_vertices_decoded += transform.unique_vertices_decoded;
    s.report.hw_transform_index_reuses += transform.index_reuses;
    if (draw.depth_test_enabled) ++s.report.depth_tested_game_draw_calls;
    if (draw.depth_write_enabled) ++s.report.depth_writing_game_draw_calls;
    if (draw.alpha_test_enabled) ++s.report.alpha_tested_game_draw_calls;
    if (draw.fog_enabled) ++s.report.fogged_game_draw_calls;
}

bool ge_gpu_backend_accumulate_hardware_packed_0115(
    const GeGpuDrawDescriptor &draw, const GeGpuHardwareTransform &transform,
    std::span<const std::byte> packed_vertices, std::uint32_t vertex_count,
    std::span<const std::uint32_t> triangle_indices) noexcept {
    VulkanGeState &s = state();
    constexpr std::size_t kPackedStride = sizeof(Packed0115Vertex);
    if (vertex_count == 0u ||
        packed_vertices.size() != static_cast<std::size_t>(vertex_count) * kPackedStride)
        return false;
    const std::size_t emitted_count = triangle_indices.empty()
        ? static_cast<std::size_t>(vertex_count) : triangle_indices.size();
    if (emitted_count == 0u ||
        (transform.primitive == 4u ? emitted_count < 3u : (emitted_count % 3u) != 0u)) return false;

    VkDescriptorSet descriptor{};
    VulkanGeState::FrameBucket *bucket = nullptr;
    if (!prepare_hw_batch_target(s, draw, descriptor, bucket)) return false;

    const VkPipeline pipeline = get_hw_pipeline(s, draw, /*packed_0115=*/true,
                                                transform.cull_enabled,
                                                transform.accept_counter_clockwise);
    if (pipeline == VK_NULL_HANDLE) {
        ++s.report.rejected_gpu_draws;
        return false;
    }

    HwTransformBatch batch{};
    batch.first_vertex = static_cast<std::uint32_t>(bucket->hw_packed.size());
    batch.vertex_count = vertex_count;
    batch.first_index = static_cast<std::uint32_t>(bucket->hw_indices.size());
    batch.index_count = static_cast<std::uint32_t>(triangle_indices.size());
    batch.packed_0115 = true;
    batch.pipeline = pipeline;
    batch.descriptor = descriptor;
    batch.scissor = {draw.scissor_x0, draw.scissor_y0, draw.scissor_x1, draw.scissor_y1};
    if (blend_variant(draw) == 4u) {
        const std::uint32_t fixed = draw.blend_fix_source;
        batch.blend_constants = {static_cast<float>(fixed & 0xFFu) / 255.0f,
                                 static_cast<float>((fixed >> 8u) & 0xFFu) / 255.0f,
                                 static_cast<float>((fixed >> 16u) & 0xFFu) / 255.0f, 1.0f};
    }
    batch.constants = build_hw_transform_push_constants(transform);
    // Per-draw control words the packed-0115 shader can't carry per-vertex
    // (see psp_ge_hw_packed0115.vert's header comment). Bit layout matches
    // ge_renderer.cpp's pack_gpu_alpha_control()/pack_gpu_fog_control()
    // exactly (those are anonymous-namespace-local to that file, so this
    // mirrors rather than calls them) -- psp_ge.frag decodes alpha/fog
    // control words with this exact layout, shared by every GE path.
    batch.constants.draw_control[0] =
        (draw.alpha_test_enabled ? 1u : 0u) |
        ((draw.alpha_function & 7u) << 8u) |
        ((draw.alpha_reference & 0xFFu) << 16u) |
        ((draw.alpha_mask & 0xFFu) << 24u);
    batch.constants.draw_control[1] = packed_texture_control(draw, draw.texture_enabled);
    batch.constants.draw_control[2] = draw.texture_env;
    batch.constants.draw_control[3] =
        (draw.fog_color & 0x00FFFFFFu) |
        (static_cast<std::uint32_t>(draw.fog_enabled ? 0xFFu : 0u) << 24u);

    bucket->hw_packed.resize(bucket->hw_packed.size() + vertex_count);
    std::memcpy(bucket->hw_packed.data() + batch.first_vertex, packed_vertices.data(),
               packed_vertices.size());
    if (!triangle_indices.empty())
        bucket->hw_indices.insert(bucket->hw_indices.end(), triangle_indices.begin(),
                                  triangle_indices.end());
    bucket->hw_batches.push_back(batch);

    const std::uint32_t logical = std::max<std::uint32_t>(1u, transform.logical_prim_batches);
    s.report.game_draw_calls += logical;
    s.report.game_triangles += transform.primitive == 4u
        ? (emitted_count > 2u ? emitted_count - 2u : 0u) : emitted_count / 3u;
    s.report.game_vertices += emitted_count;
    s.report.hw_transform_draw_calls += logical;
    s.report.hw_transform_vertices += vertex_count;
    s.report.hw_transform_prim_batches += logical;
    s.report.hw_transform_unique_vertices_decoded += transform.unique_vertices_decoded;
    s.report.hw_transform_index_reuses += transform.index_reuses;
    if (draw.depth_test_enabled) ++s.report.depth_tested_game_draw_calls;
    if (draw.depth_write_enabled) ++s.report.depth_writing_game_draw_calls;
    if (draw.alpha_test_enabled) ++s.report.alpha_tested_game_draw_calls;
    if (draw.fog_enabled) ++s.report.fogged_game_draw_calls;
    return true;
}

void ge_gpu_backend_set_native_window(void *) noexcept {}

void ge_gpu_backend_set_display_framebuffer(std::uint32_t address) noexcept {
    VulkanGeState &s = state();
    s.display_framebuffer = address & 0x001FFFF0u;
}

bool ge_gpu_backend_finish_color_frame(std::uint64_t vblank) noexcept {
    VulkanGeState &s = state();
    if (!s.enabled) return false;

    // Texture LRU epoch for this frame's worth of accumulated draws (see
    // upload_decoded_texture()'s eviction loop). vblank is already a
    // monotonically increasing per-frame counter, so it doubles as the
    // epoch directly -- no separate counter needed.
    s.frame_epoch = vblank;

    bool produced_frame = false;

    // Step 1: collect whatever the *previous* call submitted, if anything.
    // This wait is normally instant -- a full vblank of guest CPU work
    // (game logic, CPU rasterization elsewhere, audio, present) has
    // elapsed since that submission went in, so the GPU has almost
    // certainly finished already. Overlapping this frame's GPU work with
    // the guest's own next vblank of CPU work is the entire point: this
    // function used to submit and then block on the fence immediately
    // afterward, every single vblank, so the CPU sat completely idle for
    // the GPU's whole render+readback time on top of the guest's own
    // per-vblank cost. That was the single biggest cost in this backend.
    if (s.submission_pending) {
        s.submission_pending = false;
        if (vkWaitForFences(s.device, 1, &s.fence, VK_TRUE, 5'000'000'000ull) == VK_SUCCESS) {
            void *mapped = nullptr;
            if (vkMapMemory(s.device, s.readback_memory, 0, s.readback_capacity, 0, &mapped) ==
                VK_SUCCESS) {
                s.frame_rgba.resize(static_cast<std::size_t>(s.readback_capacity));
                std::memcpy(s.frame_rgba.data(), mapped, s.frame_rgba.size());
                vkUnmapMemory(s.device, s.readback_memory);
                s.frame_valid = true;
                ++s.report.game_frames;
                s.report.game_frame_vblank = s.pending_vblank;
                s.report.game_frame_readback_bytes += s.frame_rgba.size();
                s.report.presented_framebuffer_target = s.pending_target_address;
                produced_frame = true;
            }
        }
    }

    // Step 2: pick this vblank's frame -- the bucket with the most geometry,
    // with hysteresis toward last frame's winner (see last_winner_address)
    // -- and submit it, to be collected by the *next* call.
    //
    // Address-based selection and a scissor-height filter were both tried
    // here and reverted. A full bucket dump (PSPRECOMP_GE_GPU_BUCKET_DIAG)
    // confirmed plain vertex count reliably identifies the real scene
    // (tens of thousands of vertices vs. a few hundred for every competing
    // HUD-sized bucket) -- but with no memory between frames, two targets
    // whose vertex counts happen to be close and fluctuate frame to frame
    // made the pick flip between them, alternating two different renders
    // and reading as flicker/corruption in motion even though any single
    // captured frame looked correct.
    if (s.frame_buckets.empty()) return produced_frame;

    // Total geometry a bucket contributed, screen-space and
    // hardware-transform combined -- a bucket whose scene is drawn entirely
    // through the hardware-transform path would otherwise show 0 in
    // `vertices` (that field is screen-space-only) and never win the
    // largest-bucket comparison below, no matter how much real geometry it
    // has. hw_packed vertices count too: those are the dominant format for
    // real scene geometry, so a bucket using only that fast path is the
    // single most important case this must not undercount.
    const auto bucket_geometry = [](const VulkanGeState::FrameBucket &bucket) {
        return bucket.vertices.size() + bucket.hw_vertices.size() + bucket.hw_packed.size();
    };

    VulkanGeState::FrameBucket *largest = nullptr;
    VulkanGeState::FrameBucket *sticky = nullptr;
    for (VulkanGeState::FrameBucket &bucket : s.frame_buckets) {
        if (largest == nullptr || bucket_geometry(bucket) > bucket_geometry(*largest)) largest = &bucket;
        if (s.has_last_winner && bucket.address == s.last_winner_address) sticky = &bucket;
    }
    // Only give up the sticky target when something else is decisively
    // ahead (not just momentarily larger), so two close, fluctuating
    // candidates don't flip the pick every vblank.
    constexpr std::size_t kSwitchNumerator = 3, kSwitchDenominator = 2;  // 1.5x
    VulkanGeState::FrameBucket *winner =
        (sticky != nullptr &&
         bucket_geometry(*largest) * kSwitchDenominator < bucket_geometry(*sticky) * kSwitchNumerator)
            ? sticky
            : largest;

    std::size_t rejected_vertices = 0;
    for (const VulkanGeState::FrameBucket &bucket : s.frame_buckets)
        if (&bucket != winner) rejected_vertices += bucket_geometry(bucket);
    s.report.frames_without_displayed_target += rejected_vertices;
    if (winner == nullptr || (winner->batches.empty() && winner->hw_batches.empty())) {
        s.frame_buckets.clear();
        return produced_frame;
    }
    s.last_winner_address = winner->address;
    s.has_last_winner = true;

    const VkDeviceSize needed =
        static_cast<VkDeviceSize>(winner->vertices.size()) * sizeof(GeGpuVertex);
    if (needed > s.vertex_capacity) {
        // Grow and drop this frame; the software path still has the image.
        // Safe to touch the vertex buffer here regardless of the wait
        // above: if nothing was pending, nothing could still be reading
        // from it either.
        vkDeviceWaitIdle(s.device);
        if (s.vertex_mapped != nullptr) vkUnmapMemory(s.device, s.vertex_memory);
        s.vertex_mapped = nullptr;
        vkDestroyBuffer(s.device, s.vertex_buffer, nullptr);
        vkFreeMemory(s.device, s.vertex_memory, nullptr);
        s.vertex_buffer = VK_NULL_HANDLE;
        s.vertex_memory = VK_NULL_HANDLE;
        const VkDeviceSize capacity = needed * 2u;
        if (!create_buffer(s, capacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           s.vertex_buffer, s.vertex_memory) ||
            vkMapMemory(s.device, s.vertex_memory, 0, capacity, 0, &s.vertex_mapped) !=
                VK_SUCCESS) {
            s.enabled = false;
            s.report.message = "vertex buffer growth failed; reverted to software GE";
            s.frame_buckets.clear();
            return produced_frame;
        }
        s.vertex_capacity = capacity;
        s.report.upload_capacity_bytes = capacity;
        ++s.report.game_vertex_overflows;
        s.frame_buckets.clear();
        return produced_frame;
    }

    std::memcpy(s.vertex_mapped, winner->vertices.data(), static_cast<std::size_t>(needed));
    s.report.staged_bytes += needed;
    s.report.staged_vertices += winner->vertices.size();
    s.report.staged_draw_calls += winner->batches.size();

    // Hardware-transform buffers. Unlike the screen-space vertex buffer
    // above, overflow here does not drop the whole frame -- it only skips
    // this frame's hardware-transform draws (hw_draws_ready = false below),
    // leaving whatever screen-space geometry exists to present as before.
    // Regular play never approaches kInitialVertexBytes per frame for any
    // one of these, so this is a defensive fallback, not an expected path.
    bool hw_draws_ready = true;
    const VkDeviceSize hw_vertex_needed =
        static_cast<VkDeviceSize>(winner->hw_vertices.size()) * sizeof(GeGpuVertex);
    const VkDeviceSize hw_packed_needed =
        static_cast<VkDeviceSize>(winner->hw_packed.size()) * sizeof(Packed0115Vertex);
    const VkDeviceSize hw_index_needed =
        static_cast<VkDeviceSize>(winner->hw_indices.size()) * sizeof(std::uint32_t);
    if (hw_vertex_needed > s.hw_vertex_capacity || hw_packed_needed > s.hw_packed_vertex_capacity ||
        hw_index_needed > s.hw_index_capacity) {
        hw_draws_ready = false;
        ++s.report.game_vertex_overflows;
    } else {
        if (hw_vertex_needed > 0u)
            std::memcpy(s.hw_vertex_mapped, winner->hw_vertices.data(),
                       static_cast<std::size_t>(hw_vertex_needed));
        if (hw_packed_needed > 0u)
            std::memcpy(s.hw_packed_vertex_mapped, winner->hw_packed.data(),
                       static_cast<std::size_t>(hw_packed_needed));
        if (hw_index_needed > 0u)
            std::memcpy(s.hw_index_mapped, winner->hw_indices.data(),
                       static_cast<std::size_t>(hw_index_needed));
    }

    vkResetCommandBuffer(s.command_buffer, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(s.command_buffer, &begin) != VK_SUCCESS) {
        s.frame_buckets.clear();
        return produced_frame;
    }

    std::array<VkClearValue, 2> clears{};
    clears[0].color = VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}};
    // PSP depth: 0 is farthest, so the depth buffer clears to 0.0, not 1.0.
    clears[1].depthStencil = VkClearDepthStencilValue{0.0f, 0u};

    VkRenderPassBeginInfo render_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    render_begin.renderPass = s.render_pass;
    render_begin.framebuffer = s.framebuffer;
    render_begin.renderArea.extent = {s.width, s.height};
    render_begin.clearValueCount = static_cast<std::uint32_t>(clears.size());
    render_begin.pClearValues = clears.data();
    vkCmdBeginRenderPass(s.command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(s.width);
    viewport.height = static_cast<float>(s.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(s.command_buffer, 0, 1, &viewport);

    const VkDeviceSize vertex_offset = 0u;
    vkCmdBindVertexBuffers(s.command_buffer, 0, 1, &s.vertex_buffer, &vertex_offset);

    // PSP screen space -> internal target. Vertex positions scale through the
    // viewport automatically; only the scissor rectangle needs converting.
    const float scale_x = static_cast<float>(s.width) / kPspWidth;
    const float scale_y = static_cast<float>(s.height) / kPspHeight;

    VkPipeline bound = VK_NULL_HANDLE;
    for (const Batch &batch : winner->batches) {
        if (batch.pipeline != bound) {
            vkCmdBindPipeline(s.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, batch.pipeline);
            bound = batch.pipeline;
        }

        const std::int32_t x0 = std::max<std::int32_t>(0, batch.scissor[0]);
        const std::int32_t y0 = std::max<std::int32_t>(0, batch.scissor[1]);
        const std::int32_t x1 =
            std::min<std::int32_t>(static_cast<std::int32_t>(kPspWidth) - 1, batch.scissor[2]);
        const std::int32_t y1 =
            std::min<std::int32_t>(static_cast<std::int32_t>(kPspHeight) - 1, batch.scissor[3]);
        if (x1 < x0 || y1 < y0) continue;
        VkRect2D scissor{};
        // GE scissor bounds are inclusive on both ends.
        scissor.offset = {static_cast<std::int32_t>(static_cast<float>(x0) * scale_x),
                          static_cast<std::int32_t>(static_cast<float>(y0) * scale_y)};
        scissor.extent = {
            static_cast<std::uint32_t>(static_cast<float>(x1 - x0 + 1) * scale_x),
            static_cast<std::uint32_t>(static_cast<float>(y1 - y0 + 1) * scale_y)};
        scissor.extent.width =
            std::min(scissor.extent.width, s.width - static_cast<std::uint32_t>(scissor.offset.x));
        scissor.extent.height = std::min(scissor.extent.height,
                                         s.height - static_cast<std::uint32_t>(scissor.offset.y));
        vkCmdSetScissor(s.command_buffer, 0, 1, &scissor);
        vkCmdSetBlendConstants(s.command_buffer, batch.blend_constants.data());

        PushConstants push{};
        push.inverse_viewport[0] = 2.0f / kPspWidth;
        push.inverse_viewport[1] = 2.0f / kPspHeight;
        push.framebuffer_format = batch.framebuffer_format;
        vkCmdPushConstants(s.command_buffer, s.pipeline_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);

        vkCmdBindDescriptorSets(s.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                s.pipeline_layout, 0, 1, &batch.descriptor, 0, nullptr);
        vkCmdDraw(s.command_buffer, batch.vertex_count, 1u, batch.first_vertex, 0u);
    }

    // Hardware-transform draws: each batch carries its own model->clip
    // transform (baked into push constants at accumulate time -- see
    // build_hw_transform_push_constants()), so unlike the screen-space loop
    // above, the vertex buffer bind and push-constant update both happen
    // per-batch here, not once for the whole draw.
    if (hw_draws_ready && !winner->hw_batches.empty()) {
        VkPipeline hw_bound = VK_NULL_HANDLE;
        for (const HwTransformBatch &batch : winner->hw_batches) {
            if (batch.pipeline != hw_bound) {
                vkCmdBindPipeline(s.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, batch.pipeline);
                hw_bound = batch.pipeline;
            }
            // The vertex buffer bind depends on which of the two hw buffers
            // this batch's format uses, not on which pipeline is bound, so
            // it is always re-bound here rather than tracked as separate
            // "already correct" state alongside hw_bound.
            const VkBuffer hw_vb = batch.packed_0115 ? s.hw_packed_vertex_buffer : s.hw_vertex_buffer;
            const VkDeviceSize hw_vertex_offset = 0u;
            vkCmdBindVertexBuffers(s.command_buffer, 0, 1, &hw_vb, &hw_vertex_offset);

            const std::int32_t x0 = std::max<std::int32_t>(0, batch.scissor[0]);
            const std::int32_t y0 = std::max<std::int32_t>(0, batch.scissor[1]);
            const std::int32_t x1 =
                std::min<std::int32_t>(static_cast<std::int32_t>(kPspWidth) - 1, batch.scissor[2]);
            const std::int32_t y1 =
                std::min<std::int32_t>(static_cast<std::int32_t>(kPspHeight) - 1, batch.scissor[3]);
            if (x1 < x0 || y1 < y0) continue;
            VkRect2D scissor{};
            scissor.offset = {static_cast<std::int32_t>(static_cast<float>(x0) * scale_x),
                              static_cast<std::int32_t>(static_cast<float>(y0) * scale_y)};
            scissor.extent = {
                static_cast<std::uint32_t>(static_cast<float>(x1 - x0 + 1) * scale_x),
                static_cast<std::uint32_t>(static_cast<float>(y1 - y0 + 1) * scale_y)};
            scissor.extent.width = std::min(scissor.extent.width,
                s.width - static_cast<std::uint32_t>(scissor.offset.x));
            scissor.extent.height = std::min(scissor.extent.height,
                s.height - static_cast<std::uint32_t>(scissor.offset.y));
            vkCmdSetScissor(s.command_buffer, 0, 1, &scissor);
            vkCmdSetBlendConstants(s.command_buffer, batch.blend_constants.data());

            vkCmdPushConstants(s.command_buffer, s.hw_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(batch.constants), &batch.constants);
            vkCmdBindDescriptorSets(s.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    s.hw_pipeline_layout, 0, 1, &batch.descriptor, 0, nullptr);

            if (batch.index_count != 0u) {
                vkCmdBindIndexBuffer(s.command_buffer, s.hw_index_buffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(s.command_buffer, batch.index_count, 1u, batch.first_index,
                                 static_cast<std::int32_t>(batch.first_vertex), 0u);
            } else {
                vkCmdDraw(s.command_buffer, batch.vertex_count, 1u, batch.first_vertex, 0u);
            }
        }
    }
    vkCmdEndRenderPass(s.command_buffer);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1u;
    region.imageExtent = {s.width, s.height, 1u};
    vkCmdCopyImageToBuffer(s.command_buffer, s.color_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           s.readback_buffer, 1, &region);

    if (vkEndCommandBuffer(s.command_buffer) != VK_SUCCESS) {
        s.frame_buckets.clear();
        return produced_frame;
    }

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &s.command_buffer;
    vkResetFences(s.device, 1, &s.fence);
    if (vkQueueSubmit(s.graphics_queue, 1, &submit, s.fence) == VK_SUCCESS) {
        ++s.report.perf_queue_submit_calls;
        s.submission_pending = true;
        s.pending_vblank = vblank;
        s.pending_target_address = winner->address;
    }

    s.frame_buckets.clear();
    return produced_frame;
}

bool ge_gpu_backend_copy_game_frame_rgba(std::span<std::byte> destination) noexcept {
    const VulkanGeState &s = state();
    if (!s.frame_valid || destination.size() < s.frame_rgba.size()) return false;
    std::memcpy(destination.data(), s.frame_rgba.data(), s.frame_rgba.size());
    return true;
}

bool ge_gpu_backend_presents_directly() noexcept { return false; }

std::uint32_t ge_gpu_backend_owned_framebuffer() noexcept {
    const VulkanGeState &s = state();
    // report.presented_framebuffer_target is the bucket that actually won and
    // got rendered as of the last completed GPU frame (see
    // finish_color_frame()'s bucket selection) -- not display_framebuffer,
    // which only reflects the guest's last sceDisplaySetFrameBuf() call and
    // is provably one frame stale relative to whatever is being drawn right
    // now (see the comment on VulkanGeState::display_framebuffer). Callers
    // use this to decide which draws' CPU rasterization is safe to skip as
    // redundant with the GPU output; using the stale value here skipped
    // essentially nothing (target rarely matched it), while unconditionally
    // skipping everything for Vulkan (tried and reverted) skipped composition/
    // feedback passes too and broke water/reflection effects that read their
    // own guest-RAM pixels back as a texture. This is scoped to only the one
    // address actually confirmed rendered, leaving every other target's CPU
    // rasterization untouched.
    return s.frame_valid ? s.report.presented_framebuffer_target : 0u;
}

std::uint32_t ge_gpu_backend_display_framebuffer() noexcept { return state().display_framebuffer; }

std::uint32_t ge_gpu_backend_last_winner_target() noexcept {
    const VulkanGeState &s = state();
    return s.has_last_winner ? s.last_winner_address : 0u;
}

std::span<const std::byte> ge_gpu_backend_game_frame_rgba() noexcept {
    const VulkanGeState &s = state();
    if (!s.frame_valid) return {};
    return std::span<const std::byte>(s.frame_rgba.data(), s.frame_rgba.size());
}

bool ge_gpu_backend_copy_offscreen_rgba(std::span<std::byte> destination) noexcept {
    return ge_gpu_backend_copy_game_frame_rgba(destination);
}

void ge_gpu_backend_mark_window_presented() noexcept {
    state().report.gpu_frame_presented_to_window = true;
}

GeGpuBackendReport ge_gpu_backend_report() { return state().report; }

const char *ge_gpu_backend_name(GeGpuBackendKind kind) noexcept {
    switch (kind) {
    case GeGpuBackendKind::Software: return "software";
    case GeGpuBackendKind::DirectX12: return "directx12";
    case GeGpuBackendKind::Vulkan: return "vulkan";
    }
    return "unknown";
}

} // namespace vcs
