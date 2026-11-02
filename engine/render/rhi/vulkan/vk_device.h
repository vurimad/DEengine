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
VkStateInfo vk_state_info(ResourceState state);

// --- Resource records behind the handles ---

struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;         // all mips and layers; depth and stencil aspects together
    VmaAllocation allocation = VK_NULL_HANDLE; // null for swapchain images
    VkImageAspectFlags aspects = 0;
    TextureDesc desc;
    bool swapchain = false;
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    std::byte* mapped = nullptr;
    BufferDesc desc;
};

// GPU objects waiting for the frames that may use them to finish.
struct PendingDestroy {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
};

struct CommandListImpl {
    DeviceImpl* device = nullptr;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint32_t label_depth = 0;
    bool in_rendering = false;
};

inline constexpr uint32_t kMaxCommandListsPerFrame = 64;
inline constexpr uint32_t kMaxSwapchainImages = 8;

struct FrameSlot {
    VkCommandPool command_pool = VK_NULL_HANDLE;
    // Stable storage: CommandList views point into it for the whole frame.
    CommandListImpl lists[kMaxCommandListsPerFrame];
    VkCommandBuffer command_buffers[kMaxCommandListsPerFrame] = {}; // allocated on first use, reused
    uint32_t allocated_count = 0;
    uint32_t list_count = 0; // lists begun this frame
    VkSemaphore acquire_semaphore = VK_NULL_HANDLE;
    uint64_t timeline_value = 0; // the frame timeline value that marks this slot's last frame done
    core::Array<PendingDestroy> pending{core::heap_allocator(core::MemoryTag::Render)};
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
    // Signaled by the frame's submission, waited on by the present of that image. One per image:
    // the image's next acquire proves the present is done with it.
    VkSemaphore present_semaphores[kMaxSwapchainImages] = {};
};

struct DeviceImpl {
    DeviceImpl();

    // --- Creation and teardown (vk_device.cpp) ---
    core::Result<void> init(const DeviceDesc& desc);
    void shutdown();

    // --- Swapchain (vk_swapchain.cpp) ---
    core::Result<void> create_swapchain();
    void destroy_swapchain();
    bool swapchain_needs_recreation() const;

    // --- Frames (vk_device.cpp) ---
    CommandList begin_command_list(QueueType queue);

    // --- Helpers ---
    Texture& texture(TextureHandle handle);
    Buffer& buffer(BufferHandle handle);
    void set_debug_name(VkObjectType type, uint64_t object, const char* name) const;
    void free_pending(core::Array<PendingDestroy>& pending);
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

    // Frame pacing: frame N signals value N on the graphics queue when its work completes.
    VkSemaphore frame_timeline = VK_NULL_HANDLE;
    uint64_t frame_index = 0; // frames begun; the current frame while in_frame
    bool in_frame = false;
    FrameSlot frames[kFramesInFlight];

    Swapchain swapchain;
    bool want_vsync = true;
    bool swapchain_out_of_date = false;
    bool acquired = false; // a swapchain image was acquired this frame
    uint32_t acquired_image = 0;
    uint32_t swapchain_recreations = 0;

    std::atomic<uint32_t> validation_errors{0};
    std::atomic<uint32_t> validation_warnings{0};
};

} // namespace de::rhi
