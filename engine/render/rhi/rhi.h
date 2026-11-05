#pragma once

// RHI types: formats, resource descriptions, states, handles and capabilities.
// No graphics API type appears here or in any other header of engine/render/rhi/ outside
// vulkan/ (docs/design.md, Platforms & RHI).
//
// Conventions on every backend:
// - Clip space is +Y up with depth in 0..1 (the Vulkan backend flips the viewport).
// - Reverse Z: depth 1 is the near plane and 0 the far plane, so depth buffers clear to 0.
// - Texture coordinates and pixel rectangles start at the top-left corner.

#include "core/enum_flags.h"
#include "core/fixed_string.h"
#include "core/slot_map.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace de::rhi {

// Frames the CPU may record ahead of the GPU. Resources destroyed in a frame are freed once
// the GPU has finished it, at most this many frames later.
inline constexpr uint32_t kFramesInFlight = 2;

// GPU timestamps a frame can write (CommandList::write_timestamp), every list together.
inline constexpr uint32_t kMaxTimestampsPerFrame = 256;
// What write_timestamp() returns when it wrote nothing.
inline constexpr uint32_t kNoTimestamp = UINT32_MAX;
inline constexpr uint32_t kMaxColorAttachments = 8;
// Every pipeline shares one layout: push constants of this size, visible to all stages (the
// guaranteed minimum on every GPU), and the bindless tables. Resources are reached by index.
inline constexpr uint32_t kMaxPushConstantBytes = 128;

// Bindless (docs/design.md, Rendering): every sampled texture, storage texture and sampler has an
// index into a global table that shaders read (shaders/common/bindless.slang). Push constants
// carry the indices; buffers are reached by address instead.
//
// One table per texture shape, so a texture is only ever read as the shape it is. The value is
// the table's binding in the bindless set; shaders/common/bindless.slang declares the same numbers
// (a test checks they agree).
enum class BindlessTable : uint32_t {
    Texture2D,
    Texture2DArray,
    TextureCube,
    TextureCubeArray,
    Texture3D,
    Storage2D,      // float formats, one entry per mip
    Storage2DArray, // float formats; cubes as arrays of faces
    Storage3D,      // float formats
    Storage2DUint,  // R32Uint, RG32Uint, RGBA32Uint
    Sampler,
    ComparisonSampler,
    Count,
};

// Entries per table. Fixed, so shaders know them: every Vulkan 1.3 GPU with descriptor indexing
// allows far more (the spec's minimum is 500,000 per stage).
inline constexpr uint32_t kBindlessTextureCapacity = 16384; // per sampled table
inline constexpr uint32_t kBindlessStorageCapacity = 4096;  // per storage table
inline constexpr uint32_t kBindlessSamplerCapacity = 64;

// A bindless texture index: the entry in the low 24 bits, the table above them. Shader helpers
// check the table in debug and dev builds (a mismatch draws the fallback) and mask and clamp the
// entry in every build, so a wrong index never reads outside a table.
inline constexpr uint32_t kBindlessEntryBits = 24;
inline constexpr uint32_t kBindlessEntryMask = (1U << kBindlessEntryBits) - 1;

constexpr uint32_t make_bindless_index(BindlessTable table, uint32_t entry) {
    return (static_cast<uint32_t>(table) << kBindlessEntryBits) | (entry & kBindlessEntryMask);
}
constexpr BindlessTable bindless_table(uint32_t index) {
    return static_cast<BindlessTable>(index >> kBindlessEntryBits);
}
constexpr uint32_t bindless_entry(uint32_t index) {
    return index & kBindlessEntryMask;
}

// What texture_index() returns without the usage or for a stale handle. Its table is invalid, so
// shaders in debug and dev builds draw the fallback for it.
inline constexpr uint32_t kInvalidBindlessIndex = UINT32_MAX;

// Entry 0 of every texture table is a fallback: 1x1 magenta for sampled tables, a scratch texel
// for storage tables. Every entry not in use points at it, freed ones included, so even a stale
// index reads something defined. This one is the 2D fallback.
inline constexpr uint32_t kFallbackTextureIndex = 0;

// The samplers every device has; the value is the sampler's index in the table. Anisotropic
// samplers use up to 16x, as far as the GPU allows. ShadowCompare compares with GreaterEqual
// (reverse Z) and clamps to the edge.
enum class DefaultSampler : uint32_t {
    LinearWrap,
    LinearClamp,
    PointWrap,
    PointClamp,
    AnisotropicWrap,
    AnisotropicClamp,
    ShadowCompare,
    Count,
};

inline constexpr uint32_t sampler_index(DefaultSampler sampler) {
    return static_cast<uint32_t>(sampler);
}

// Backend records behind the handles. Only the backend defines them.
struct Texture;
struct Buffer;
struct Pipeline;

// Generational handles: a handle to a destroyed resource never resolves, even after its slot is reused.
using TextureHandle = core::SlotHandle<Texture>;
using BufferHandle = core::SlotHandle<Buffer>;
using PipelineHandle = core::SlotHandle<Pipeline>;

enum class QueueType : uint8_t {
    Graphics, // graphics, compute and copies
    Compute,  // async compute
    Transfer, // copies, alongside rendering
    Count,
};

// When a command list runs within its frame (Device::begin_command_list).
enum class ListStart : uint8_t {
    InOrder,            // after the frame's lists begun before it on its queue
    FrameStart,         // graphics: before every other graphics list of the frame (uploads land first)
    AfterPreviousFrame, // transfer: waits for the GPU to finish the previous frame (overwrites what it may read)
};

enum class Format : uint8_t {
    Unknown,

    R8Unorm,
    RG8Unorm,
    RGBA8Unorm,
    RGBA8Srgb,
    BGRA8Unorm,
    BGRA8Srgb,
    R16Unorm,
    RG16Unorm,
    RGBA16Unorm,
    R16Float,
    RG16Float,
    RGBA16Float,
    R32Uint,
    RG32Uint,
    RGBA32Uint,
    R32Float,
    RG32Float,
    RGBA32Float,
    RGB10A2Unorm,
    RG11B10Float,

    D16Unorm,
    D32Float,
    D24UnormS8Uint,
    D32FloatS8Uint,

    BC1Unorm,
    BC1Srgb,
    BC3Unorm,
    BC3Srgb,
    BC4Unorm,
    BC5Unorm,
    BC6HUfloat,
    BC7Unorm,
    BC7Srgb,

    Count,
};

inline constexpr size_t kFormatCount = static_cast<size_t>(Format::Count);

struct FormatInfo {
    const char* name = "unknown";
    uint8_t block_bytes = 0;  // bytes per texel, or per block for compressed formats
    uint8_t block_width = 1;  // texels per block (4 for BC formats)
    uint8_t block_height = 1; //
    bool depth = false;
    bool stencil = false;
    bool srgb = false;
};

const FormatInfo& format_info(Format format);

// The format storage views of a texture use: the UNORM twin of an sRGB format (GPUs don't write
// sRGB storage images: shaders write sRGB-encoded values themselves), the format itself otherwise.
Format storage_format(Format format);

// What a format can do on this GPU (Capabilities::format_support), for optimally tiled textures.
enum class FormatSupport : uint16_t {
    None = 0,
    Sampled = 1 << 0,
    LinearFilter = 1 << 1,
    Storage = 1 << 2,
    RenderTarget = 1 << 3,
    Blend = 1 << 4,
    DepthStencil = 1 << 5,
    CopySrc = 1 << 6,
    CopyDst = 1 << 7,
    Blit = 1 << 8, // filtered copies between mips or textures, both ways (CommandList::blit_texture)
};
DE_ENUM_FLAGS(FormatSupport)

enum class TextureUsage : uint8_t {
    None = 0,
    Sampled = 1 << 0,
    Storage = 1 << 1,
    RenderTarget = 1 << 2,
    DepthStencil = 1 << 3,
    CopySrc = 1 << 4,
    CopyDst = 1 << 5,
};
DE_ENUM_FLAGS(TextureUsage)

enum class TextureDimension : uint8_t {
    Texture2D, // layer_count > 1 makes it an array
    Texture3D,
    Cube, // layer_count is a multiple of 6: one cube per 6 layers
};

struct TextureDesc {
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1; // Texture3D only
    uint32_t mip_count = 1;
    uint32_t layer_count = 1;
    Format format = Format::Unknown;
    TextureUsage usage = TextureUsage::None;
    TextureDimension dimension = TextureDimension::Texture2D;
    const char* debug_name = nullptr; // shown in RenderDoc and validation messages; copied
};

// The size of one mip in texels; depth counts the slices of a 3D texture and is 1 otherwise.
struct MipExtent {
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
};
MipExtent mip_extent(const TextureDesc& desc, uint32_t mip);

enum class BufferUsage : uint8_t {
    None = 0,
    Vertex = 1 << 0,
    Index = 1 << 1,
    Uniform = 1 << 2,
    Storage = 1 << 3,
    Indirect = 1 << 4,
    CopySrc = 1 << 5,
    CopyDst = 1 << 6,
};
DE_ENUM_FLAGS(BufferUsage)

enum class MemoryLocation : uint8_t {
    GpuOnly,  // fastest for the GPU, not visible to the CPU
    Upload,   // CPU writes, GPU reads: staging and per-frame data. Persistently mapped
    Readback, // GPU writes, CPU reads. Persistently mapped
};

struct BufferDesc {
    uint64_t size = 0;
    BufferUsage usage = BufferUsage::None;
    MemoryLocation memory = MemoryLocation::GpuOnly;
    const char* debug_name = nullptr; // copied
};

// How a resource is used. Barriers move resources between states; the render graph will emit them
// from the reads and writes passes declare.
//
// Flags: read-only states combine (a buffer read as indirect arguments and by shaders in one pass
// is IndirectArgument | ShaderRead), a write state stands alone. Textures combine only read states
// that share an image layout (DepthRead | ShaderRead); see is_valid_state().
enum class ResourceState : uint32_t {
    // Nothing on the GPU uses the resource: it was just created, or every earlier use has finished
    // (its frame was waited for). As a barrier's old state it waits for nothing. Anything that may
    // still be in flight must name its real previous state instead (with discard for textures).
    Undefined = 0,
    RenderTarget = 1U << 0,     // color attachment, read and written
    DepthWrite = 1U << 1,       // depth-stencil attachment, tested and written
    DepthRead = 1U << 2,        // depth-stencil attachment, tested read-only; add ShaderRead to also sample it
    ShaderRead = 1U << 3,       // sampled textures, and buffers read by shaders (through pointers)
    ShaderWrite = 1U << 4,      // storage textures (even when only read) and buffers written by shaders
    UniformBuffer = 1U << 5,    // buffers only
    VertexBuffer = 1U << 6,     // buffers only
    IndexBuffer = 1U << 7,      // buffers only
    IndirectArgument = 1U << 8, // buffers only: draw and dispatch arguments
    CopySrc = 1U << 9,
    CopyDst = 1U << 10,
    Present = 1U << 11,  // swapchain textures only: handed to the display
    HostRead = 1U << 12, // buffers only: after GPU writes, before the CPU reads a Readback buffer
    General = 1U << 13,  // any access by any stage: slow, for debugging and odd cases
};
DE_ENUM_FLAGS(ResourceState)

inline constexpr ResourceState kAllResourceStates = static_cast<ResourceState>((1U << 14) - 1);

// The states whose shader stages a barrier names (TextureBarrier::before_stages and friends).
inline constexpr ResourceState kShaderStates =
    ResourceState::ShaderRead | ResourceState::ShaderWrite | ResourceState::UniformBuffer;

// The states that write; each stands alone (the others are reads and combine).
inline constexpr ResourceState kWriteStates = ResourceState::RenderTarget | ResourceState::DepthWrite |
                                              ResourceState::ShaderWrite | ResourceState::CopyDst |
                                              ResourceState::General;

// True if the state is one write state, or a combination of read states (for textures, read
// states that share a layout), with no unknown bits. Barriers assert it.
bool is_valid_state(ResourceState state, bool texture);

// "render_target", "depth_read|shader_read"... for logs and debug views.
core::FixedString<255> resource_state_name(ResourceState state);

// The shader stages a ShaderRead, ShaderWrite or UniformBuffer state applies to. Naming only the
// stages that really touch the resource lets unrelated work overlap the barrier.
enum class ShaderStages : uint8_t {
    None = 0,
    Vertex = 1U << 0,
    Fragment = 1U << 1,
    Compute = 1U << 2,
    Graphics = Vertex | Fragment,
    All = Vertex | Fragment | Compute,
};
DE_ENUM_FLAGS(ShaderStages)

inline constexpr uint32_t kAllMips = UINT32_MAX;
inline constexpr uint32_t kAllLayers = UINT32_MAX;
inline constexpr uint64_t kWholeBuffer = UINT64_MAX;

// Barriers of each kind a barrier command handles without touching the heap (BarrierBatch keeps
// as many inline).
inline constexpr size_t kInlineBarriers = 32;

struct TextureRange {
    uint32_t first_mip = 0;
    uint32_t mip_count = kAllMips; // kAllMips: from first_mip to the last mip
    uint32_t first_layer = 0;
    uint32_t layer_count = kAllLayers; // kAllLayers: from first_layer to the last layer
};

// Handing a resource from one queue family to another (the transfer queue to graphics, say). The
// queue giving it up records a Release on its list; the queue taking it records the matching
// Acquire (same resource, range, before and after states) in a submission that waits for the
// first. The state change happens once, between the two. other_queue names the queue on the other
// side; the two queues must be different families (Capabilities::transfer_queue for Transfer).
// Contents that aren't kept (discard, or from Undefined) need no handover: just transition there.
enum class QueueOwnership : uint8_t {
    None,
    Release, // after_stages are ignored: the acquiring queue's barrier waits instead
    Acquire, // before_stages are ignored: it waits behind the semaphore between the queues
};

struct TextureBarrier {
    TextureHandle texture{};
    ResourceState before = ResourceState::Undefined;
    ResourceState after = ResourceState::Undefined;
    ShaderStages before_stages = ShaderStages::All; // for shader states in before
    ShaderStages after_stages = ShaderStages::All;  // for shader states in after
    TextureRange range{};
    // The old contents aren't needed: skip preserving them through the layout change (cheaper),
    // while still waiting for the previous use to finish. For targets overwritten every frame.
    bool discard = false;
    QueueOwnership ownership = QueueOwnership::None;
    QueueType other_queue = QueueType::Graphics;
};

struct BufferBarrier {
    BufferHandle buffer{};
    ResourceState before = ResourceState::Undefined;
    ResourceState after = ResourceState::Undefined;
    ShaderStages before_stages = ShaderStages::All;
    ShaderStages after_stages = ShaderStages::All;
    uint64_t offset = 0;
    uint64_t size = kWholeBuffer; // kWholeBuffer: from offset to the end
    QueueOwnership ownership = QueueOwnership::None;
    QueueType other_queue = QueueType::Graphics;
};

// Orders every buffer access matching before against every one matching after, with no handle:
// one global barrier is cheaper than many buffer barriers when a pass touches lots of buffers
// (GPU-driven culling, particles). No texture layout changes. States may combine freely here.
struct GlobalBarrier {
    ResourceState before = ResourceState::Undefined;
    ResourceState after = ResourceState::Undefined;
    ShaderStages before_stages = ShaderStages::All;
    ShaderStages after_stages = ShaderStages::All;
};

enum class LoadOp : uint8_t {
    Load,
    Clear,
    DontCare,
};

enum class StoreOp : uint8_t {
    Store,
    DontCare,
};

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

struct ColorAttachment {
    TextureHandle texture{}; // in RenderTarget state
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    Color clear{};
};

struct DepthAttachment {
    TextureHandle texture{}; // in DepthWrite state, or DepthRead (| ShaderRead if sampled too) when read_only
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    float clear_depth = 0.0f; // reverse Z: 0 is the far plane
    uint8_t clear_stencil = 0;
    bool read_only = false; // tested, never written: load must be Load
};

// A rectangle in pixels, from the top-left corner.
struct Rect {
    int32_t x = 0;
    int32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct Viewport {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float min_depth = 0.0f;
    float max_depth = 1.0f;
};

// Attachments use mip 0 and layer 0 of single-mip, single-layer textures for now; rendering into
// one mip or layer of a bigger texture arrives with the render graph.
struct RenderingDesc {
    std::span<const ColorAttachment> colors{}; // at most kMaxColorAttachments
    const DepthAttachment* depth = nullptr;
    Rect area{}; // width 0: the size of the first attachment. Also set as the viewport and scissor
};

// A filtered copy of a whole mip into another mip (of the same texture or another one), every
// layer from first_layer on: the source in CopySrc state, the destination in CopyDst. Sizes may
// differ: the source is scaled with a linear filter (sRGB decoded first, encoded after).
struct TextureBlit {
    uint32_t src_mip = 0;
    uint32_t dst_mip = 0;
    uint32_t first_layer = 0;
    uint32_t layer_count = kAllLayers; // kAllLayers: from first_layer to the last layer
};

// Buffer <-> texture copies. The texture must be in CopyDst (or CopySrc) state.
struct BufferTextureCopy {
    uint64_t buffer_offset = 0;
    uint32_t buffer_row_texels = 0; // texels per row in the buffer; 0: tightly packed
    uint32_t mip = 0;
    uint32_t layer = 0;
    uint32_t layer_count = 1; // layers from layer on, one after another in the buffer
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t z = 0;
    uint32_t width = 0; // 0: the whole mip, from x/y/z
    uint32_t height = 0;
    uint32_t depth = 0;
};

enum class ShaderStage : uint8_t {
    Vertex,
    Fragment,
    Compute,
    Count,
};

const char* shader_stage_name(ShaderStage stage);

// Compiled shader code for this backend (SPIR-V for Vulkan), with one entry point named "main".
struct ShaderCode {
    ShaderStage stage = ShaderStage::Vertex;
    std::span<const std::byte> code{};
};

enum class Topology : uint8_t {
    TriangleList,
    TriangleStrip,
    LineList,
    PointList,
};

enum class CullMode : uint8_t {
    None,
    Front,
    Back,
};

// Reverse Z: nearer is greater, so depth tests use Greater or GreaterEqual.
enum class CompareOp : uint8_t {
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always,
};

enum class BlendMode : uint8_t {
    Opaque,        // overwrite
    Alpha,         // src * a + dst * (1 - a)
    Premultiplied, // src + dst * (1 - a)
    Additive,      // src * a + dst
};

// Vertices are pulled from buffers by shaders (by device address), so there are no vertex
// input layouts. Attachment formats must match the textures used in begin_rendering.
struct GraphicsPipelineDesc {
    ShaderCode vertex{};
    ShaderCode fragment{};
    Format color_formats[kMaxColorAttachments] = {};
    BlendMode blend[kMaxColorAttachments] = {};
    uint32_t color_count = 0;
    Format depth_format = Format::Unknown;
    bool depth_test = false;
    bool depth_write = false;
    CompareOp depth_compare = CompareOp::GreaterEqual;
    Topology topology = Topology::TriangleList;
    CullMode cull = CullMode::Back;
    bool front_counter_clockwise = true;
    const char* debug_name = nullptr;
};

struct ComputePipelineDesc {
    ShaderCode shader{};
    const char* debug_name = nullptr;
};

// What indirect draws and dispatches read from a buffer, as the GPU lays them out.
struct DrawIndirectArgs {
    uint32_t vertex_count = 0;
    uint32_t instance_count = 1;
    uint32_t first_vertex = 0;
    uint32_t first_instance = 0;
};

struct DrawIndexedIndirectArgs {
    uint32_t index_count = 0;
    uint32_t instance_count = 1;
    uint32_t first_index = 0;
    int32_t vertex_offset = 0;
    uint32_t first_instance = 0;
};

struct DispatchIndirectArgs {
    uint32_t groups_x = 0;
    uint32_t groups_y = 1;
    uint32_t groups_z = 1;
};

enum class IndexType : uint8_t {
    Uint16,
    Uint32,
};

enum class GpuVendor : uint8_t {
    Unknown,
    Nvidia,
    Amd,
    Intel,
    Arm,
    Qualcomm,
    Apple,
    Software, // CPU implementations such as Mesa's lavapipe
};

enum class GpuType : uint8_t {
    Other,
    Discrete,
    Integrated,
    Virtual,
    Cpu,
};

// What the GPU can do. Passes ask "is ray query available?" here, never "is this Vulkan?".
struct Capabilities {
    char device_name[256] = {};
    char driver_name[256] = {};
    char driver_info[256] = {};
    GpuVendor vendor = GpuVendor::Unknown;
    GpuType type = GpuType::Other;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t api_version_major = 0; // the graphics API version the device supports
    uint32_t api_version_minor = 0;
    uint64_t device_local_memory_bytes = 0; // size of the largest GPU-local memory heap

    bool async_compute = false;      // the compute queue runs alongside graphics (its own queue family)
    bool dedicated_transfer = false; // the transfer queue is a copy-only queue family
    // Transfer command lists run on a queue of their own family, alongside graphics. Without it,
    // there are no transfer lists: copies go on graphics lists.
    bool transfer_queue = false;
    bool ray_query = false;  // inline ray tracing from any shader, with acceleration structures
    bool timestamps = false; // GPU timestamps on the graphics and compute queues
    float timestamp_period_ns = 0.0f;

    uint32_t max_texture_size_2d = 0;
    uint32_t max_texture_size_3d = 0;
    uint32_t max_texture_size_cube = 0;
    uint32_t max_texture_layers = 0;
    uint32_t max_bindless_textures = 0; // sampled textures in one bindless table
    uint32_t max_bindless_buffers = 0;  // storage buffers in one bindless table
    uint32_t max_push_constant_bytes = 0;
    uint32_t max_compute_group_size[3] = {};
    uint32_t max_compute_group_invocations = 0;
    uint32_t max_draw_indirect_count = 0; // draws in one indirect call; the spec promises only 65535
    // Groups in one dispatch, along each axis. An indirect dispatch's arguments have to stay within it: nothing
    // checks what a shader wrote, and past it what happens is undefined. The spec promises 65535.
    uint32_t max_dispatch_groups[3] = {};
    uint32_t subgroup_size = 0;
    uint64_t uniform_buffer_alignment = 0; // offsets into uniform buffers are multiples of this
    uint64_t storage_buffer_alignment = 0;
    uint64_t copy_row_pitch_alignment = 0; // optimal (not required) row pitch for buffer <-> texture copies
    float max_anisotropy = 0.0f;

    FormatSupport formats[kFormatCount] = {};

    FormatSupport format_support(Format format) const { return formats[static_cast<size_t>(format)]; }
    bool supports(Format format, FormatSupport needed) const { return core::has_all(format_support(format), needed); }
};

const char* gpu_vendor_name(GpuVendor vendor);
const char* gpu_type_name(GpuType type);

} // namespace de::rhi
