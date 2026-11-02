#pragma once

// Vulkan backend internals: shared by the backend's .cpp files only, never included above the RHI.

#include "core/allocator.h"
#include "core/array.h"
#include "core/slot_map.h"
#include "render/rhi/command_list.h"
#include "render/rhi/device.h"
#include "render/rhi/rhi.h"

#include <volk.h>
// volk first: it declares the Vulkan prototypes VMA needs.
#include <vk_mem_alloc.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace de::rhi {

// Fatal on any result but VK_SUCCESS: for calls that fail only on device loss or driver bugs.
#define DE_VK_CHECK(call) ::de::rhi::vk_check((call), #call, __FILE__, __LINE__)

void vk_check(VkResult result, const char* call, const char* file, int line);
const char* vk_result_name(VkResult result);

// --- Format and state tables (vk_format.cpp) ---

VkFormat to_vk_format(Format format);
Format from_vk_format(VkFormat format);
VkImageAspectFlags format_aspects(Format format);

struct VkStateInfo {
    VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};
// shader_stages applies to the shader states (ShaderRead, ShaderWrite, UniformBuffer) in state.
VkStateInfo vk_state_info(ResourceState state, ShaderStages shader_stages);

// The view type of a texture: as it is sampled (cubes as cubes), or for storage (cubes as arrays of faces).
VkImageViewType vk_view_type(const TextureDesc& desc, bool storage);

// --- Resource records behind the handles ---

inline constexpr uint32_t kMaxMips = 16; // a 32768-texel texture's full chain

// A texture's bindless entries and the views behind them, set up and released together
// (DeviceImpl::register_bindless / release_bindless).
struct TextureBindless {
    VkImageView sampled_view = VK_NULL_HANDLE; // depth aspect only, for depth-stencil formats; else null
    uint32_t sampled_index = kInvalidBindlessIndex;
    uint32_t storage_mips = 0;
    uint32_t storage_indices[kMaxMips] = {};
    VkImageView storage_views[kMaxMips] = {}; // one mip, every layer
};

struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;         // all mips and layers; depth and stencil aspects together
    VmaAllocation allocation = VK_NULL_HANDLE; // null for swapchain images
    VkImageAspectFlags aspects = 0;
    TextureBindless bindless;
    TextureDesc desc;
    bool swapchain = false;
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    std::byte* mapped = nullptr;
    uint64_t address = 0; // 0 when no shader-visible usage
    BufferDesc desc;
};

struct Pipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
};

// GPU objects waiting for the frames that may use them to finish.
struct PendingDestroy {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

// Hands out the entries of one kind of table (sampled or storage): an entry is the same position
// in each table of that kind. Entry 0 is the fallback. Never-used entries go first; freed ones
// (only once the GPU is done with their old texture) are reused oldest first, and only when no
// fresh entry is left. A stale index so keeps reading the fallback for as long as possible.
class BindlessSlots {
public:
    void init(const char* name, uint32_t capacity);
    uint32_t allocate(); // 0 when full (0 is never handed out)
    void release(uint32_t entry);
    uint32_t used() const { return used_; } // the fallback included

private:
    core::Array<uint32_t> freed_{core::heap_allocator(core::MemoryTag::Render)}; // a queue from freed_head_
    size_t freed_head_ = 0;
    const char* name_ = "";
    uint32_t next_ = 1;
    uint32_t capacity_ = 0;
    uint32_t used_ = 1;
    bool warned_ = false;
};

struct CommandListImpl {
    DeviceImpl* device = nullptr;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    QueueType queue = QueueType::Graphics;
    ListStart start = ListStart::InOrder;
    uint32_t label_depth = 0;
    bool in_rendering = false;
    // Vulkan keeps a graphics and a compute binding side by side.
    bool graphics_bound = false;
    bool compute_bound = false;
};

inline constexpr uint32_t kMaxCommandListsPerFrame = 64;
inline constexpr uint32_t kMaxTransferListsPerFrame = 16; // of the 64
inline constexpr uint32_t kMaxSwapchainImages = 16;

struct FrameSlot {
    VkCommandPool command_pool = VK_NULL_HANDLE;
    // Stable storage: CommandList views point into it for the whole frame.
    CommandListImpl lists[kMaxCommandListsPerFrame];
    VkCommandBuffer command_buffers[kMaxCommandListsPerFrame] = {}; // allocated on first use, reused
    uint32_t allocated_count = 0;
    uint32_t list_count = 0; // lists begun this frame, every queue
    // Transfer lists come from a pool of the transfer queue's family (Capabilities::transfer_queue).
    VkCommandPool transfer_pool = VK_NULL_HANDLE;
    VkCommandBuffer transfer_buffers[kMaxTransferListsPerFrame] = {};
    uint32_t transfer_allocated = 0;
    uint32_t transfer_count = 0;
    VkSemaphore acquire_semaphore = VK_NULL_HANDLE;
    uint64_t timeline_value = 0; // the frame timeline value that marks this slot's last frame done
    core::Array<PendingDestroy> pending{core::heap_allocator(core::MemoryTag::Render)};
    core::Array<TextureBindless> pending_bindless{core::heap_allocator(core::MemoryTag::Render)};
};

struct Swapchain {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    Format format = Format::Unknown;
    VkFormat vk_format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t requested_width = 0; // the window size it was made for (the surface may differ)
    uint32_t requested_height = 0;
    bool vsync = true;
    uint32_t image_count = 0;
    TextureHandle textures[kMaxSwapchainImages];
};

struct DeviceImpl {
    DeviceImpl();

    // --- Creation and teardown (vk_device.cpp) ---
    core::Result<void> init(const DeviceDesc& desc);
    void shutdown();

    // --- Surface and swapchain (vk_swapchain.cpp) ---
    core::Result<void> create_surface();
    // After VK_ERROR_SURFACE_LOST_KHR: waits for the GPU, drops the swapchain and makes a new surface.
    core::Result<void> recreate_surface();
    bool graphics_queue_can_present() const;
    core::Result<void> create_swapchain();
    void destroy_swapchain();
    bool swapchain_needs_recreation() const;

    // --- Bindless tables (vk_bindless.cpp) ---
    core::Result<void> create_bindless();
    void destroy_bindless();
    // Gives a new texture its table entries and writes their descriptors. On failure nothing is
    // left to release.
    core::Result<void> register_bindless(Texture& texture);
    // Points the entries back at the fallbacks (one batched descriptor update), frees them and
    // destroys the views. Only once the GPU is done with the texture. At shutdown the descriptors
    // aren't rewritten: the set is about to go.
    void release_bindless(TextureBindless& bindless, bool rewrite_descriptors = true);
    void write_descriptor(BindlessTable table, uint32_t entry, VkImageView view);

    // --- Frames (vk_device.cpp) ---
    CommandList begin_command_list(QueueType queue, ListStart start);

    // --- Helpers ---
    Texture& texture(TextureHandle handle);
    Buffer& buffer(BufferHandle handle);
    Pipeline& pipeline(PipelineHandle handle);
    void set_debug_name(VkObjectType type, uint64_t object, const char* name) const;
    void free_pending(FrameSlot& slot);
    FrameSlot& current_slot() { return frames[frame_index % kFramesInFlight]; }

    platform::Window* window = nullptr;
    bool validation = false;
    bool debug_utils = false;
    bool memory_budget = false;

    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;

    VkQueue queues[static_cast<size_t>(QueueType::Count)] = {};
    uint32_t queue_families[static_cast<size_t>(QueueType::Count)] = {};

    Capabilities capabilities;

    core::SlotMap<Texture> textures;
    core::SlotMap<Buffer> buffers;
    core::SlotMap<Pipeline> pipelines;
    // The one layout every pipeline uses: kMaxPushConstantBytes of push constants for all stages,
    // and the bindless set (set 0).
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;

    // The bindless set: one binding per BindlessTable (shaders/common/bindless.slang). Bound once
    // at the start of every command list, for graphics and compute.
    VkDescriptorSetLayout bindless_layout = VK_NULL_HANDLE;
    VkDescriptorPool bindless_pool = VK_NULL_HANDLE;
    VkDescriptorSet bindless_set = VK_NULL_HANDLE;
    BindlessSlots sampled_slots;
    BindlessSlots storage_slots;
    VkSampler samplers[static_cast<size_t>(DefaultSampler::Count)] = {};
    // Entry 0 of every texture table, and what free entries point at: tiny textures owned here
    // rather than in `textures`. Sampled ones are magenta; storage ones are scratch texels.
    static constexpr uint32_t kFallbackImages = 5;
    VkImage fallback_images[kFallbackImages] = {};
    VmaAllocation fallback_allocations[kFallbackImages] = {};
    VkImageView fallback_views[static_cast<size_t>(BindlessTable::Count)] = {};

    // Frame pacing: frame N signals value N on the graphics queue when its work completes.
    VkSemaphore frame_timeline = VK_NULL_HANDLE;
    // Frame N's transfer submissions signal values up to 2N; frame N's graphics submission waits
    // for 2N, so frame N complete on frame_timeline means its transfers are too.
    VkSemaphore transfer_timeline = VK_NULL_HANDLE;
    uint64_t frame_index = 0; // frames begun; the current frame while in_frame
    bool in_frame = false;
    FrameSlot frames[kFramesInFlight];

    Swapchain swapchain;
    // Signaled by a frame's submission, waited on by the present of that image: one per image
    // index, so the image's next acquire proves the present is done with it. They outlive
    // swapchain recreations (only shutdown destroys them): without VK_EXT_swapchain_maintenance1
    // nothing tells when a present from a retired swapchain has stopped using its semaphore.
    VkSemaphore present_semaphores[kMaxSwapchainImages] = {};
    bool want_vsync = true;
    bool swapchain_out_of_date = false; // must be rebuilt before the next acquire
    bool swapchain_suboptimal = false;  // works, but a rebuild would match the surface better
    // Some surfaces keep reporting suboptimal with the best swapchain they allow: after one
    // rebuild for suboptimal alone, only a real change (size, vsync, out of date) rebuilds again.
    bool ignore_suboptimal = false;
    bool surface_lost = false;
    bool acquired = false; // a swapchain image was acquired this frame
    uint32_t acquired_image = 0;
    uint32_t swapchain_recreations = 0;

    std::atomic<uint32_t> validation_errors{0};
    std::atomic<uint32_t> validation_warnings{0};
};

} // namespace de::rhi
