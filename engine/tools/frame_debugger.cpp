#include "tools/frame_debugger.h"

#include "console/console.h"
#include "core/allocator.h"
#include "core/array.h"
#include "core/fixed_string.h"
#include "core/hash.h"
#include "core/hash_map.h"
#include "core/log.h"
#include "core/profile.h"
#include "platform/time.h"
#include "render/imgui/imgui_renderer.h"
#include "render/rhi/command_list.h"
#include "render/rhi/device.h"
#include "render/shader/shader_library.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <utility>

namespace de::tools {

namespace {

core::Allocator& tools_heap() {
    return core::heap_allocator(core::MemoryTag::Tools);
}

using Name = core::FixedString<47>;

// The view's own passes, by name: listed like any other, and never chosen by a click on a texture's last writer.
constexpr const char* kViewPass = "frame debugger view";
constexpr const char* kProbePass = "frame debugger probe";
constexpr const char* kReadbackPass = "frame debugger readback";
constexpr const char* kIdsPass = "frame debugger ids";

// The longest name the graph's GPU times keep (PassGpuTime::name): passes are told apart by that much of theirs.
constexpr size_t kTimedNameLength = 31;

// How often the window's numbers and its list of passes are brought up to date. Every frame, they can't be read:
// times jitter, and a list sorted by them never rests.
constexpr uint64_t kRefreshNs = 300 * platform::kNsPerMs;
// A pass of the graph that has no row: one of the view's own.
constexpr uint32_t kNoRow = UINT32_MAX;

struct PassRow {
    Name name;
    uint64_t key = 0; // its name and which of that name it is: what its times are kept by
    uint32_t occurrence = 0;
    uint32_t first_use = 0;
    uint32_t use_count = 0;
    bool ran = false;
    rhi::ListWork work;
};

struct UseRow {
    bool is_texture = true;
    uint32_t resource = 0;
    rhi::ResourceState state = rhi::ResourceState::Undefined;
    rhi::TextureRange range{};
    bool writes = false;
};

struct TextureRow {
    Name name;
    rhi::TextureDesc desc;
    bool imported = false;
    bool used = false;
};

struct BufferRow {
    Name name;
    uint64_t size = 0;
    bool imported = false;
    bool used = false;
};

// A pass's GPU time over the frames seen: the last, a moving average, a peak that sinks back slowly, and the
// average as it was when Mark was pressed (below 0: not marked).
struct Timing {
    float last = 0.0f;
    float average = 0.0f;
    float peak = 0.0f;
    float mark = -1.0f;
    bool seen = false;
    // What the window shows: the average and the peak as they were when it last refreshed.
    float shown_average = 0.0f;
    float shown_peak = 0.0f;
};

// Matches ViewPush in shaders/debug/frame_debugger.slang.
struct ViewPush {
    uint32_t texture = 0;
    uint32_t mip = 0;
    uint32_t layer = 0;
    uint32_t flags = 0;
    float origin_x = 0.0f;
    float origin_y = 0.0f;
    float texels_per_pixel = 1.0f;
    float range_min = 0.0f;
    float range_max = 1.0f;
    float size_x = 1.0f;
    float size_y = 1.0f;
    int32_t probe_x = 0;
    int32_t probe_y = 0;
};
static_assert(sizeof(ViewPush) == 52 && sizeof(ViewPush) <= rhi::kMaxPushConstantBytes);

// Matches IdsPush in shaders/debug/frame_debugger_ids.slang.
struct IdsPush {
    uint32_t source = 0;
    uint32_t target = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

constexpr uint32_t kChannelRed = 1U << 0U;
constexpr uint32_t kChannelGreen = 1U << 1U;
constexpr uint32_t kChannelBlue = 1U << 2U;
constexpr uint32_t kChannelAlpha = 1U << 3U;
constexpr uint32_t kFlagSrgbTarget = 1U << 4U;
constexpr uint32_t kFlagColor = 1U << 5U;
constexpr uint32_t kFlagProbe = 1U << 6U;
constexpr uint32_t kFlagMarkBad = 1U << 7U;
constexpr uint32_t kFlagIds = 1U << 8U;

// How the texture chosen is looked at.
struct ViewSettings {
    uint32_t channels = kChannelRed | kChannelGreen | kChannelBlue;
    bool color = false;   // linear color, shown as a display would; else as stored
    bool mark_bad = true; // NaN and infinity striped
    float range_min = 0.0f;
    float range_max = 1.0f;
    uint32_t mip = 0;
    uint32_t layer = 0;
    bool fit = true; // the whole texture in the view, until it's zoomed or panned
    float texels_per_pixel = 1.0f;
    float origin_x = 0.0f;
    float origin_y = 0.0f;
};

enum class ViewProblem : uint8_t {
    None,
    PassMissing,
    NoTextures,
    TextureMissing,
    NotReadable,
    Unsupported,
    Compiling,
    NoTarget,
};

const char* problem_text(ViewProblem problem) {
    switch (problem) {
    case ViewProblem::None:
        return "";
    case ViewProblem::PassMissing:
        return "that pass isn't in the frame";
    case ViewProblem::NoTextures:
        return "this pass uses no textures";
    case ViewProblem::TextureMissing:
        return "that pass doesn't use that texture";
    case ViewProblem::NotReadable:
        return "this texture can't be read by a shader (it wasn't made to be sampled)";
    case ViewProblem::Unsupported:
        return "integer textures show only as plain 2D textures";
    case ViewProblem::Compiling:
        return "the view's shader is still compiling";
    case ViewProblem::NoTarget:
        return "the view's own texture couldn't be made";
    }
    return "";
}

bool is_integer(rhi::Format format) {
    return format == rhi::Format::R32Uint || format == rhi::Format::RG32Uint || format == rhi::Format::RGBA32Uint;
}

// How many of red, green, blue and alpha the format stores.
uint32_t channel_count(rhi::Format format) {
    switch (format) {
    case rhi::Format::R8Unorm:
    case rhi::Format::R16Unorm:
    case rhi::Format::R16Float:
    case rhi::Format::R32Uint:
    case rhi::Format::R32Float:
    case rhi::Format::D16Unorm:
    case rhi::Format::D32Float:
    case rhi::Format::D24UnormS8Uint:
    case rhi::Format::D32FloatS8Uint:
    case rhi::Format::BC4Unorm:
        return 1;
    case rhi::Format::RG8Unorm:
    case rhi::Format::RG16Unorm:
    case rhi::Format::RG16Float:
    case rhi::Format::RG32Uint:
    case rhi::Format::RG32Float:
    case rhi::Format::BC5Unorm:
        return 2;
    case rhi::Format::RG11B10Float:
    case rhi::Format::BC6HUfloat:
        return 3;
    default:
        return 4;
    }
}

// Whether a format holds light (linear color the display should encode) rather than numbers.
bool holds_color(rhi::Format format) {
    return rhi::format_info(format).srgb || format == rhi::Format::RGBA16Float || format == rhi::Format::RG11B10Float ||
           format == rhi::Format::BC6HUfloat;
}

uint64_t texture_bytes(const rhi::TextureDesc& desc) {
    const rhi::FormatInfo& info = rhi::format_info(desc.format);
    uint64_t bytes = 0;
    for (uint32_t mip = 0; mip < desc.mip_count; ++mip) {
        const rhi::MipExtent extent = rhi::mip_extent(desc, mip);
        const uint64_t blocks_x = (extent.width + info.block_width - 1U) / info.block_width;
        const uint64_t blocks_y = (extent.height + info.block_height - 1U) / info.block_height;
        bytes += blocks_x * blocks_y * extent.depth * desc.layer_count * info.block_bytes;
    }
    return bytes;
}

template <typename... Args>
const char* format_text(std::span<char> out, const char* format, Args... args) {
    static_cast<void>(std::snprintf(out.data(), out.size(), format, args...));
    return out.data();
}

// "12 draws, 2 indirect (up to 4096), 1 dispatch, 3 copies", or "nothing".
const char* work_text(const rhi::ListWork& work, std::span<char> out) {
    out[0] = '\0';
    size_t length = 0;
    const auto grow = [&](int written) {
        if (written > 0) {
            length = std::min(length + static_cast<size_t>(written), out.size() - 1);
        }
    };
    const auto add = [&](uint32_t count, const char* one, const char* many, uint32_t most) {
        if (count == 0 || length + 1 >= out.size()) {
            return;
        }
        grow(std::snprintf(out.data() + length, out.size() - length, "%s%u %s", length > 0 ? ", " : "", count,
                           count == 1 ? one : many));
        if (most > 0 && length + 1 < out.size()) {
            grow(std::snprintf(out.data() + length, out.size() - length, " (up to %u draws)", most));
        }
    };
    add(work.draws, "draw", "draws", 0);
    add(work.indirect_calls, "indirect call", "indirect calls", work.indirect_max);
    add(work.dispatches, "dispatch", "dispatches", 0);
    add(work.copies, "copy", "copies", 0);
    return out.data();
}

bool contains_no_case(std::string_view text, std::string_view part) {
    if (part.empty()) {
        return true;
    }
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    for (size_t start = 0; start + part.size() <= text.size(); ++start) {
        size_t i = 0;
        while (i < part.size() && lower(text[start + i]) == lower(part[i])) {
            ++i;
        }
        if (i == part.size()) {
            return true;
        }
    }
    return false;
}

uint64_t pass_key(std::string_view name, uint32_t occurrence) {
    const std::string_view cut = name.substr(0, kTimedNameLength);
    return core::fnv1a_64(cut.data(), cut.size()) ^ core::mix64(uint64_t{occurrence} + 1U);
}

} // namespace

struct FrameDebuggerState {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
    console::Console* console = nullptr;   // add_commands(): its commands are taken away with the debugger
    render::RenderGraph* hooked = nullptr; // the graph that has our pass hook

    bool open = false;
    bool paused = false;
    bool by_cost = false;
    bool has_mark = false;
    char filter[48] = {};

    // The frame as last taken in (end_frame).
    core::Array<PassRow> passes{tools_heap()};
    core::Array<UseRow> uses{tools_heap()};
    core::Array<TextureRow> textures{tools_heap()};
    core::Array<BufferRow> buffers{tools_heap()};
    core::Array<uint32_t> order{tools_heap()}; // the passes as listed: filtered, sorted
    core::HashMap<uint64_t, Timing> timings{tools_heap()};
    core::Array<uint32_t> row_of_pass{tools_heap()}; // a pass's place in the graph to its row, or kNoRow
    render::RenderGraphStats graph_stats{};
    uint32_t culled_rows = 0;
    uint64_t taken_ns = 0;       // when the rows were last taken and the shown numbers refreshed
    float frame_ms = 0.0f;       // the frame on the GPU, averaged, the view's own passes included
    float own_ms = 0.0f;         // the view's own passes, averaged
    float shown_frame_ms = 0.0f; // the frame without them, as at the last refresh
    float shown_own_ms = 0.0f;
    bool auto_selected = false; // the window chose the costliest pass once, when it first had times
    ProbeValue shown_probe;     // the probe's value as the window shows it: not changed every frame
    double shown_probe_s = 0.0;

    // What's looked at: a pass, and one of its textures (by name, or by its number in the graph if it has none).
    bool has_selection = false;
    Name selected_pass;
    uint32_t selected_occurrence = 0;
    // The texture: one chosen, or (auto_texture) the first the pass writes, whichever that is each frame.
    bool auto_texture = true;
    bool has_texture = false; // there is one to look at: chosen, or found by the last frame
    Name selected_texture;
    uint32_t selected_resource = 0; // its number in the frame's graph, kept current by each frame that draws it
    bool defaults_pending = false;  // the texture is new: its format decides how it's first shown
    uint32_t generation = 0;        // grows when what's looked at changes: a readback from before isn't shown
    ViewSettings view;

    // The view's pass: its programs, its target, and this frame's state.
    shader::ProgramHandle view_program;
    shader::ProgramHandle probe_program;
    shader::ProgramHandle ids_program; // integer textures into floats (compute)
    rhi::Format program_format = rhi::Format::Unknown;
    rhi::Format ui_format = rhi::Format::Unknown;
    rhi::TextureHandle target;
    rhi::TextureDesc target_desc{};
    rhi::ResourceState target_state = rhi::ResourceState::Undefined;
    uint32_t view_width = 512; // pixels: the panel's
    uint32_t view_height = 512;
    uint32_t drawn_width = 0; // what the target was last drawn at
    uint32_t drawn_height = 0;
    uint32_t viewed_width = 1; // the mip looked at, in texels
    uint32_t viewed_height = 1;
    rhi::TextureDesc viewed_desc{};
    render::GraphTexture shown{};
    bool view_added = false;
    uint32_t seen_selected = 0;
    ViewProblem problem = ViewProblem::None;
    ViewProblem last_problem = ViewProblem::None; // the frame before's: what the window says

    // The probe: one texel into a 1x1 texture, copied to a buffer the CPU reads kFramesInFlight frames later.
    rhi::TextureHandle probe_texture;
    rhi::ResourceState probe_state = rhi::ResourceState::Undefined;
    rhi::BufferHandle probe_readback[rhi::kFramesInFlight];
    bool probe_used[rhi::kFramesInFlight] = {};
    bool probe_waiting[rhi::kFramesInFlight] = {};
    uint32_t probe_sent_x[rhi::kFramesInFlight] = {};
    uint32_t probe_sent_y[rhi::kFramesInFlight] = {};
    uint32_t probe_sent_generation[rhi::kFramesInFlight] = {};
    bool probe_wanted = false;
    uint32_t probe_x = 0;
    uint32_t probe_y = 0;
    ProbeValue probe_value;

    // --- What's looked at ---

    void select(std::string_view pass, uint32_t occurrence, std::string_view texture, uint32_t resource) {
        has_selection = !pass.empty();
        static_cast<void>(selected_pass.assign(pass));
        selected_occurrence = occurrence;
        auto_texture = texture.empty() && resource == UINT32_MAX;
        has_texture = !auto_texture;
        static_cast<void>(selected_texture.assign(texture));
        selected_resource = resource;
        defaults_pending = true;
        ++generation;
        probe_wanted = false;
        probe_value = {};
        view = {};
    }

    bool is_selected_texture(std::string_view name, uint32_t resource) const {
        const std::string_view cut = name.substr(0, Name::capacity());
        return selected_texture.empty() ? resource == selected_resource : cut == selected_texture.view();
    }

    // How a texture is first shown, from its format.
    void apply_defaults(const rhi::TextureDesc& desc) {
        view = {};
        const uint32_t channels = channel_count(desc.format);
        view.channels = channels == 1   ? kChannelRed
                        : channels == 2 ? kChannelRed | kChannelGreen
                                        : kChannelRed | kChannelGreen | kChannelBlue;
        view.color = holds_color(desc.format);
        defaults_pending = false;
    }

    const PassRow* selected_row() const {
        if (!has_selection) {
            return nullptr;
        }
        for (const PassRow& row : passes) {
            if (row.name.view() == selected_pass.view() && row.occurrence == selected_occurrence) {
                return &row;
            }
        }
        return nullptr;
    }

    const Timing* timing(const PassRow& row) const {
        const Timing* found = timings.find(row.key);
        return found != nullptr && found->seen ? found : nullptr;
    }

    // --- The view's passes ---

    bool ensure_programs() {
        if (program_format == ui_format && view_program.is_valid() && probe_program.is_valid()) {
            return true;
        }
        shaders->destroy(view_program);
        shaders->destroy(probe_program);
        view_program = {};
        probe_program = {};
        program_format = ui_format;
        rhi::GraphicsPipelineDesc pipeline{
            .color_count = 1, .cull = rhi::CullMode::None, .debug_name = "frame debugger view"};
        pipeline.color_formats[0] = ui_format;
        if (const auto made = shaders->create_graphics({.module = "debug.frame_debugger", .state = pipeline}); made) {
            view_program = *made;
        }
        pipeline.color_formats[0] = rhi::Format::RGBA32Float;
        pipeline.debug_name = "frame debugger probe";
        if (const auto made = shaders->create_graphics({.module = "debug.frame_debugger", .state = pipeline}); made) {
            probe_program = *made;
        }
        if (!ids_program.is_valid()) {
            if (const auto made =
                    shaders->create_compute({.module = "debug.frame_debugger_ids", .debug_name = "frame debugger ids"});
                made) {
                ids_program = *made;
            }
        }
        if (!view_program.is_valid() || !probe_program.is_valid()) {
            DE_LOG_ERROR("tools", "frame debugger: its shader (debug.frame_debugger) couldn't be made");
            return false;
        }
        return true;
    }

    bool ensure_target() {
        // In big steps, and never smaller: made anew, it shows nothing for a frame, which a panel being dragged
        // wider would do over and over.
        const auto rounded = [](uint32_t size) { return std::clamp((size + 255U) & ~255U, 256U, 8192U); };
        const bool keep = target.is_valid() && target_desc.format == ui_format;
        const uint32_t width = std::max(rounded(view_width), keep ? target_desc.width : 0U);
        const uint32_t height = std::max(rounded(view_height), keep ? target_desc.height : 0U);
        if (keep && target_desc.width == width && target_desc.height == height) {
            return true;
        }
        device->destroy(target); // freed once the GPU is done with it
        target = {};
        target_state = rhi::ResourceState::Undefined;
        drawn_width = 0;
        drawn_height = 0;
        target_desc = {.width = width,
                       .height = height,
                       .format = ui_format,
                       .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled |
                                rhi::TextureUsage::CopySrc, // copied out for pictures, and by its test
                       .debug_name = "frame debugger view"};
        const auto made = device->create_texture(target_desc);
        if (!made) {
            DE_LOG_ERROR("tools", "frame debugger: no %ux%u view texture: %s", width, height, made.error().message);
            return false;
        }
        target = *made;
        return true;
    }

    bool ensure_probe(uint32_t slot) {
        if (!probe_texture.is_valid()) {
            const auto made =
                device->create_texture({.width = 1,
                                        .height = 1,
                                        .format = rhi::Format::RGBA32Float,
                                        .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc,
                                        .debug_name = "frame debugger probe"});
            if (!made) {
                return false;
            }
            probe_texture = *made;
            probe_state = rhi::ResourceState::Undefined;
        }
        if (!probe_readback[slot].is_valid()) {
            const auto made = device->create_buffer({.size = 4 * sizeof(float),
                                                     .usage = rhi::BufferUsage::CopyDst,
                                                     .memory = rhi::MemoryLocation::Readback,
                                                     .debug_name = "frame debugger probe readback"});
            if (!made) {
                return false;
            }
            probe_readback[slot] = *made;
            probe_used[slot] = false;
        }
        return true;
    }

    // The texel this slot's last frame probed, now that the GPU is done with it.
    void read_probe() {
        const auto slot = static_cast<uint32_t>(device->frame_index() % rhi::kFramesInFlight);
        if (!probe_waiting[slot]) {
            return;
        }
        probe_waiting[slot] = false;
        if (probe_sent_generation[slot] != generation) {
            return; // of a texture no longer looked at
        }
        device->invalidate_mapped(probe_readback[slot]);
        const std::span<std::byte> mapped = device->mapped(probe_readback[slot]);
        if (mapped.size() < sizeof(probe_value.value)) {
            return;
        }
        std::memcpy(probe_value.value, mapped.data(), sizeof(probe_value.value));
        probe_value.x = probe_sent_x[slot];
        probe_value.y = probe_sent_y[slot];
        probe_value.valid = true;
    }

    // The view of a texture of the pass just added: drawn into the target, and its probed texel read back.
    void add_view(render::RenderGraph& graph, uint32_t resource, const render::GraphTextureInfo& texture) {
        const rhi::PipelineHandle view_pipeline = shaders->pipeline(view_program);
        const rhi::PipelineHandle probe_pipeline = shaders->pipeline(probe_program);
        if (!view_pipeline.is_valid()) {
            problem = ViewProblem::Compiling;
            return;
        }
        const rhi::TextureDesc& desc = texture.desc;
        const bool integer = is_integer(desc.format);
        const bool depth = rhi::format_info(desc.format).depth;
        // A transient gets the usage its uses ask for; an imported texture has what it was made with. Integer
        // textures are read as storage textures.
        const rhi::TextureUsage needed = integer ? rhi::TextureUsage::Storage : rhi::TextureUsage::Sampled;
        if (texture.imported && !core::has_any(desc.usage, needed)) {
            problem = ViewProblem::NotReadable;
            return;
        }
        if (integer && (desc.dimension != rhi::TextureDimension::Texture2D || desc.layer_count > 1 ||
                        !device->capabilities().supports(desc.format, rhi::FormatSupport::Storage))) {
            problem = ViewProblem::Unsupported;
            return;
        }
        const rhi::PipelineHandle ids_pipeline = shaders->pipeline(ids_program);
        if (integer && !ids_pipeline.is_valid()) {
            problem = ViewProblem::Compiling;
            return;
        }
        if (!ensure_target()) {
            problem = ViewProblem::NoTarget;
            return;
        }
        if (defaults_pending) {
            apply_defaults(desc);
        }
        viewed_desc = desc;
        view.mip = std::min(view.mip, desc.mip_count - 1);
        const rhi::MipExtent extent = rhi::mip_extent(desc, view.mip);
        const uint32_t layers = desc.dimension == rhi::TextureDimension::Texture3D ? extent.depth : desc.layer_count;
        view.layer = std::min(view.layer, layers - 1);
        viewed_width = extent.width;
        viewed_height = extent.height;
        const auto width_px = static_cast<float>(view_width);
        const auto height_px = static_cast<float>(view_height);
        if (view.fit) {
            view.texels_per_pixel =
                std::max(static_cast<float>(extent.width) / width_px, static_cast<float>(extent.height) / height_px);
            view.origin_x = (static_cast<float>(extent.width) - (width_px * view.texels_per_pixel)) * 0.5f;
            view.origin_y = (static_cast<float>(extent.height) - (height_px * view.texels_per_pixel)) * 0.5f;
        }
        ViewPush push{.mip = view.mip,
                      .layer = view.layer,
                      .flags = view.channels | (rhi::format_info(ui_format).srgb ? kFlagSrgbTarget : 0U) |
                               (view.color ? kFlagColor : 0U) | (view.mark_bad ? kFlagMarkBad : 0U) |
                               (integer ? kFlagIds : 0U),
                      .origin_x = view.origin_x,
                      .origin_y = view.origin_y,
                      .texels_per_pixel = view.texels_per_pixel,
                      .range_min = view.range_min,
                      .range_max = view.range_max,
                      .size_x = static_cast<float>(extent.width),
                      .size_y = static_cast<float>(extent.height)};

        render::GraphTexture source{resource};
        const rhi::ResourceState read =
            depth ? rhi::ResourceState::DepthRead | rhi::ResourceState::ShaderRead : rhi::ResourceState::ShaderRead;
        if (integer) {
            // Integer textures are reached only as storage textures, which a fragment shader can't read here: a
            // compute pass first copies the mip into a float texture of the pool's, and that is what's shown.
            struct IdsData {
                render::GraphTexture floats;
            };
            const auto& ids = graph.add_pass<IdsData>(
                kIdsPass,
                [&](render::PassBuilder& pass, IdsData& data) {
                    data.floats = pass.create_texture({.desc = {.width = extent.width,
                                                                .height = extent.height,
                                                                .format = rhi::Format::R32Float,
                                                                .debug_name = "frame debugger ids"}});
                    pass.use(source, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
                    pass.use(data.floats, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
                },
                [integers = source, ids_pipeline, mip = view.mip, width = extent.width,
                 height = extent.height](const IdsData& data, render::PassContext& context) {
                    context.list.bind_pipeline(ids_pipeline);
                    context.list.push_constants(
                        IdsPush{.source = context.device.storage_texture_index(context.texture(integers), mip),
                                .target = context.device.storage_texture_index(context.texture(data.floats)),
                                .width = width,
                                .height = height});
                    context.list.dispatch((width + 7) / 8, (height + 7) / 8);
                });
            source = ids.floats;
            push.mip = 0;
            push.layer = 0;
        }
        // Its contents are kept: a pass after the UI's leaves them for the next frame's UI.
        const render::GraphTexture into = graph.import_texture({.texture = target,
                                                                .name = "frame debugger view",
                                                                .state = target_state,
                                                                .stages = rhi::ShaderStages::Fragment,
                                                                .final_state = rhi::ResourceState::ShaderRead,
                                                                .final_stages = rhi::ShaderStages::Fragment});
        target_state = rhi::ResourceState::ShaderRead;
        const rhi::Rect area{.width = std::min(view_width, target_desc.width),
                             .height = std::min(view_height, target_desc.height)};
        drawn_width = area.width;
        drawn_height = area.height;
        graph.add_pass(
            kViewPass,
            [&](render::PassBuilder& pass) {
                pass.use(source, read, rhi::ShaderStages::Fragment);
                pass.use(into, rhi::ResourceState::RenderTarget);
                pass.never_cull();
            },
            [source, into, view_pipeline, push, area](render::PassContext& context) {
                DE_PROFILE_ZONE("frame debugger view");
                ViewPush sent = push;
                sent.texture = context.device.texture_index(context.texture(source));
                const rhi::ColorAttachment color{.texture = context.texture(into), .load = rhi::LoadOp::DontCare};
                context.list.begin_rendering({.colors = std::span(&color, 1), .area = area});
                context.list.bind_pipeline(view_pipeline);
                context.list.push_constants(sent);
                context.list.draw(3);
                context.list.end_rendering();
            });
        shown = into;
        view_added = true;
        problem = ViewProblem::None;

        const auto slot = static_cast<uint32_t>(device->frame_index() % rhi::kFramesInFlight);
        if (!probe_wanted || probe_x >= extent.width || probe_y >= extent.height || !probe_pipeline.is_valid() ||
            !ensure_probe(slot)) {
            return;
        }
        push.flags |= kFlagProbe;
        push.probe_x = static_cast<int32_t>(probe_x);
        push.probe_y = static_cast<int32_t>(probe_y);
        const render::GraphTexture texel = graph.import_texture({.texture = probe_texture,
                                                                 .name = "frame debugger probe",
                                                                 .state = probe_state,
                                                                 .final_state = rhi::ResourceState::CopySrc,
                                                                 .discard = true});
        probe_state = rhi::ResourceState::CopySrc;
        const render::GraphBuffer readback = graph.import_buffer(
            {.buffer = probe_readback[slot],
             .name = "frame debugger probe readback",
             .state = probe_used[slot] ? rhi::ResourceState::HostRead : rhi::ResourceState::Undefined,
             .final_state = rhi::ResourceState::HostRead});
        probe_used[slot] = true;
        graph.add_pass(
            kProbePass,
            [&](render::PassBuilder& pass) {
                pass.use(source, read, rhi::ShaderStages::Fragment);
                pass.use(texel, rhi::ResourceState::RenderTarget);
                pass.never_cull();
            },
            [source, texel, probe_pipeline, push](render::PassContext& context) {
                ViewPush sent = push;
                sent.texture = context.device.texture_index(context.texture(source));
                const rhi::ColorAttachment color{.texture = context.texture(texel), .load = rhi::LoadOp::DontCare};
                context.list.begin_rendering({.colors = std::span(&color, 1)});
                context.list.bind_pipeline(probe_pipeline);
                context.list.push_constants(sent);
                context.list.draw(3);
                context.list.end_rendering();
            });
        graph.add_pass(
            kReadbackPass,
            [&](render::PassBuilder& pass) {
                pass.use(texel, rhi::ResourceState::CopySrc);
                pass.use(readback, rhi::ResourceState::CopyDst);
                pass.never_cull();
            },
            [texel, readback](render::PassContext& context) {
                context.list.copy_texture_to_buffer(context.texture(texel), context.buffer(readback));
            });
        probe_waiting[slot] = true;
        probe_sent_x[slot] = probe_x;
        probe_sent_y[slot] = probe_y;
        probe_sent_generation[slot] = generation;
    }

    // RenderGraph's pass hook: after the pass looked at, the view of its texture.
    static void pass_added(void* user, render::RenderGraph& graph, uint32_t pass) {
        FrameDebuggerState& state = *static_cast<FrameDebuggerState*>(user);
        if (state.view_added || !state.has_selection) {
            return;
        }
        const render::GraphPassInfo info = graph.pass_info(pass);
        if (std::string_view(info.name).substr(0, Name::capacity()) != state.selected_pass.view() ||
            state.seen_selected++ != state.selected_occurrence) {
            return;
        }
        state.problem = ViewProblem::TextureMissing;
        // No texture chosen: the first it writes, or failing that the first it uses, whichever that is this frame
        // (a pass may write another target from one frame to the next: the window's image, then a panel's).
        if (state.auto_texture) {
            uint32_t first = UINT32_MAX;
            for (uint32_t u = 0; u < info.use_count; ++u) {
                const render::GraphUseInfo use = graph.pass_use(pass, u);
                if (use.is_texture && (first == UINT32_MAX || use.writes)) {
                    first = use.resource;
                    if (use.writes) {
                        break;
                    }
                }
            }
            if (first == UINT32_MAX) {
                state.problem = ViewProblem::NoTextures;
                state.has_texture = false;
                return;
            }
            const render::GraphTextureInfo texture = graph.texture_info(first);
            const std::string_view name = std::string_view(texture.name).substr(0, Name::capacity());
            if (!state.has_texture || name != state.selected_texture.view()) {
                // Not the texture of the frame before: shown as its own format asks, its texels read afresh.
                static_cast<void>(state.selected_texture.assign(name));
                state.has_texture = true;
                state.defaults_pending = true;
                ++state.generation;
                state.probe_value = {};
            }
            state.selected_resource = first;
            state.add_view(graph, first, texture);
            return;
        }
        // The one chosen: by its name and its number in the graph, or by name alone when the graph has changed.
        uint32_t found = UINT32_MAX;
        for (uint32_t u = 0; u < info.use_count; ++u) {
            const render::GraphUseInfo use = graph.pass_use(pass, u);
            if (use.is_texture && state.is_selected_texture(graph.texture_info(use.resource).name, use.resource) &&
                (found == UINT32_MAX || use.resource == state.selected_resource)) {
                found = use.resource;
            }
        }
        if (found != UINT32_MAX) {
            state.selected_resource = found;
            state.add_view(graph, found, graph.texture_info(found));
        }
    }

    // --- The frame, taken in ---

    static bool own_pass(std::string_view name) {
        return name == kViewPass || name == kProbePass || name == kReadbackPass || name == kIdsPass;
    }

    // The frame's passes, what they used, and its resources, as rows. The view's own passes are left out: they come
    // and go with what's looked at, and would push the others about.
    void take_rows(const render::RenderGraph& graph) {
        passes.clear();
        uses.clear();
        textures.clear();
        buffers.clear();
        row_of_pass.clear();
        culled_rows = 0;
        for (uint32_t p = 0; p < graph.pass_count(); ++p) {
            const render::GraphPassInfo info = graph.pass_info(p);
            if (own_pass(info.name)) {
                row_of_pass.push(kNoRow);
                continue;
            }
            row_of_pass.push(static_cast<uint32_t>(passes.size()));
            PassRow row{.name = Name(info.name),
                        .first_use = static_cast<uint32_t>(uses.size()),
                        .use_count = info.use_count,
                        .ran = info.ran,
                        .work = info.work};
            for (const PassRow& earlier : passes) {
                row.occurrence += earlier.name.view() == row.name.view() ? 1U : 0U;
            }
            row.key = pass_key(row.name.view(), row.occurrence);
            culled_rows += row.ran ? 0U : 1U;
            for (uint32_t u = 0; u < info.use_count; ++u) {
                const render::GraphUseInfo use = graph.pass_use(p, u);
                uses.push(UseRow{.is_texture = use.is_texture,
                                 .resource = use.resource,
                                 .state = use.state,
                                 .range = use.range,
                                 .writes = use.writes});
            }
            passes.push(row);
        }
        for (uint32_t t = 0; t < graph.texture_count(); ++t) {
            const render::GraphTextureInfo info = graph.texture_info(t);
            textures.push(
                TextureRow{.name = Name(info.name), .desc = info.desc, .imported = info.imported, .used = info.used});
        }
        for (uint32_t b = 0; b < graph.buffer_count(); ++b) {
            const render::GraphBufferInfo info = graph.buffer_info(b);
            buffers.push(BufferRow{
                .name = Name(info.name), .size = info.desc.size, .imported = info.imported, .used = info.used});
        }
        graph_stats = graph.stats();
    }

    // The GPU's times, of a frame or two back, into each pass's average. A time finds its pass by its place in the
    // graph; when the graph has changed since (the view's own passes come and go), by its name and which of that
    // name it is.
    void accumulate(const render::RenderGraph& graph) {
        const std::span<const render::PassGpuTime> times = graph.gpu_times();
        float own = 0.0f;
        for (size_t t = 0; t < times.size(); ++t) {
            const render::PassGpuTime& time = times[t];
            const std::string_view name = time.name.view();
            if (own_pass(name)) {
                own += time.ms;
                continue;
            }
            const PassRow* row = nullptr;
            const uint32_t placed = time.pass < row_of_pass.size() ? row_of_pass[time.pass] : kNoRow;
            if (placed != kNoRow && passes[placed].name.view().substr(0, kTimedNameLength) == name) {
                row = &passes[placed];
            } else {
                uint32_t which = 0;
                for (size_t earlier = 0; earlier < t; ++earlier) {
                    which += times[earlier].name.view() == name ? 1U : 0U;
                }
                for (const PassRow& candidate : passes) {
                    if (candidate.ran && candidate.name.view().substr(0, kTimedNameLength) == name) {
                        if (which == 0) {
                            row = &candidate;
                            break;
                        }
                        --which;
                    }
                }
            }
            if (row == nullptr) {
                continue;
            }
            Timing& timing = timings.find_or_insert(row->key);
            timing.last = time.ms;
            timing.average = timing.seen ? timing.average + ((time.ms - timing.average) * 0.08f) : time.ms;
            timing.peak = std::max(time.ms, timing.peak - ((timing.peak - timing.average) * 0.01f));
            timing.seen = true;
        }
        const float frame = graph.gpu_frame_ms();
        frame_ms = frame_ms > 0.0f ? frame_ms + ((frame - frame_ms) * 0.08f) : frame;
        own_ms += (own - own_ms) * 0.08f;
    }

    // The numbers the window shows, brought up to date.
    void refresh_shown() {
        for (auto entry : timings) {
            entry.value.shown_average = entry.value.average;
            entry.value.shown_peak = entry.value.peak;
        }
        shown_own_ms = own_ms;
        shown_frame_ms = std::max(frame_ms - own_ms, 0.0f);
    }

    void mark() {
        for (auto entry : timings) {
            entry.value.mark = entry.value.seen ? entry.value.average : -1.0f;
        }
        has_mark = true;
    }

    void clear_mark() {
        for (auto entry : timings) {
            entry.value.mark = -1.0f;
        }
        has_mark = false;
    }

    // The last pass that ran and wrote the texture, or failing that the last that used it.
    bool last_user(uint32_t texture, uint32_t& pass_out) const {
        bool found = false;
        bool found_writer = false;
        for (uint32_t p = 0; p < passes.size(); ++p) {
            const PassRow& row = passes[p];
            if (!row.ran) {
                continue;
            }
            for (uint32_t u = row.first_use; u < row.first_use + row.use_count; ++u) {
                if (uses[u].is_texture && uses[u].resource == texture && (uses[u].writes || !found_writer)) {
                    pass_out = p;
                    found = true;
                    found_writer = found_writer || uses[u].writes;
                }
            }
        }
        return found;
    }

    // The pass that costs the most, chosen once when the window first has times: it opens on something.
    void select_costliest() {
        const PassRow* costliest = nullptr;
        float most = 0.0f;
        for (const PassRow& row : passes) {
            const Timing* time = timing(row);
            if (row.ran && time != nullptr && time->average > most) {
                costliest = &row;
                most = time->average;
            }
        }
        if (costliest != nullptr) {
            const Name name = costliest->name; // select() takes it in before the rows change
            select(name.view(), costliest->occurrence, {}, UINT32_MAX);
            auto_selected = true;
        }
    }

    // The passes as text, or one pass's textures and buffers (FrameDebugger::report).
    void report(console::Output& out, int32_t pass) const {
        if (passes.empty()) {
            out.info("no frame taken in yet: the frame debugger must be open (frame_debugger on)");
            return;
        }
        char work[128];
        if (pass < 0) {
            out.info("GPU %.3f ms, %u passes (%u culled), %u barriers",
                     static_cast<double>(std::max(frame_ms - own_ms, 0.0f)), static_cast<uint32_t>(passes.size()),
                     culled_rows, graph_stats.barriers);
            for (uint32_t p = 0; p < passes.size(); ++p) {
                const PassRow& row = passes[p];
                const Timing* time = timing(row);
                if (!row.ran) {
                    out.info("%3u  %-32s  culled", p, row.name.c_str());
                } else if (time != nullptr) {
                    out.info("%3u  %-32s  %7.3f ms  peak %7.3f  %s", p, row.name.c_str(),
                             static_cast<double>(time->average), static_cast<double>(time->peak),
                             work_text(row.work, work));
                } else {
                    out.info("%3u  %-32s  not timed  %s", p, row.name.c_str(), work_text(row.work, work));
                }
            }
            return;
        }
        if (static_cast<size_t>(pass) >= passes.size()) {
            out.error("no pass %d: the frame has %zu", pass, passes.size());
            return;
        }
        const PassRow& row = passes[static_cast<size_t>(pass)];
        out.info("%s%s  %s", row.name.c_str(), row.ran ? "" : " (culled)", work_text(row.work, work));
        char size[96];
        for (uint32_t u = row.first_use; u < row.first_use + row.use_count; ++u) {
            const UseRow& use = uses[u];
            if (use.is_texture && use.resource < textures.size()) {
                const TextureRow& texture = textures[use.resource];
                describe(texture.desc, size);
                out.info("  %s  %-28s  %s  %s", use.writes ? "writes" : "reads ", texture.name.c_str(), size,
                         rhi::resource_state_name(use.state).c_str());
            } else if (!use.is_texture && use.resource < buffers.size()) {
                const BufferRow& buffer = buffers[use.resource];
                out.info("  %s  %-28s  %llu bytes  %s", use.writes ? "writes" : "reads ", buffer.name.c_str(),
                         static_cast<unsigned long long>(buffer.size), rhi::resource_state_name(use.state).c_str());
            }
        }
    }

    // --- The window ---

    // A button lit while what it stands for is on.
    static bool toggle_button(const char* label, bool on) {
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        }
        const bool clicked = ImGui::Button(label);
        if (on) {
            ImGui::PopStyleColor(3);
        }
        return clicked;
    }

    // A row of a list, across its whole table. Chosen, it stays lit whatever the mouse does; only a row that isn't
    // chosen answers the mouse, with a tint. So the two never trade places under the pointer.
    static bool list_row(const char* label, bool chosen) {
        const ImVec4 lit = ImGui::GetStyleColorVec4(ImGuiCol_Header);
        const ImVec4 tint = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, chosen ? lit : tint);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, chosen ? lit : tint);
        const bool clicked = ImGui::Selectable(label, chosen, ImGuiSelectableFlags_SpanAllColumns);
        ImGui::PopStyleColor(2);
        return clicked;
    }

    // A band across the pane, saying what is under it: a quiet caption, the name of the thing, a few quiet words
    // after it, and at the right end its numbers.
    static void band(const char* caption, const char* name, const char* after = nullptr, const char* detail = nullptr) {
        const ImGuiStyle& style = ImGui::GetStyle();
        const ImVec2 from = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = ImGui::GetFrameHeight();
        const ImVec2 to(from.x + width, from.y + height);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(from, to, ImGui::GetColorU32(ImGuiCol_TableHeaderBg));
        const float y = from.y + ((height - ImGui::GetFontSize()) * 0.5f);
        const float gap = ImGui::GetFontSize() * 0.45f;
        const ImU32 quiet = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        float right = to.x - style.FramePadding.x - gap;
        if (detail != nullptr && detail[0] != '\0') {
            right -= ImGui::CalcTextSize(detail).x;
            draw->AddText(ImVec2(right, y), ImGui::GetColorU32(ImGuiCol_Text), detail);
            right -= gap * 2.0f;
        }
        float x = from.x + style.FramePadding.x + gap;
        draw->PushClipRect(from, ImVec2(std::max(right, from.x), to.y), true);
        const auto put = [&](const char* text, ImU32 color) {
            if (text != nullptr && text[0] != '\0') {
                draw->AddText(ImVec2(x, y), color, text);
                x += ImGui::CalcTextSize(text).x + gap;
            }
        };
        put(caption, quiet);
        put(name, ImGui::GetColorU32(ImGuiCol_Text));
        put(after, quiet);
        draw->PopClipRect();
        ImGui::Dummy(ImVec2(width, height));
    }

    // Text against the right edge of its table cell: numbers line up.
    static void cell_right(const char* text) {
        const float room = ImGui::GetContentRegionAvail().x;
        const float width = ImGui::CalcTextSize(text).x;
        if (width < room) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room - width);
        }
        ImGui::TextUnformatted(text);
    }

    // A pass's share of the frame, as a bar in its own cell: neutral, so it reads on a chosen row as on any other.
    static void cell_share(float share) {
        const ImVec2 from = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float line = ImGui::GetTextLineHeight();
        const float thick = std::max(3.0f, std::floor(line * 0.3f));
        const float top = from.y + std::floor((line - thick) * 0.5f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(ImVec2(from.x, top), ImVec2(from.x + width, top + thick),
                            ImGui::GetColorU32(ImGuiCol_Text, 0.10f));
        const float filled = std::max(width * std::clamp(share, 0.0f, 1.0f), share > 0.0f ? 1.0f : 0.0f);
        draw->AddRectFilled(ImVec2(from.x, top), ImVec2(from.x + filled, top + thick),
                            ImGui::GetColorU32(ImGuiCol_Text, 0.6f));
        ImGui::Dummy(ImVec2(width, line));
    }

    // A few quiet words in the middle of what's left of the pane: why there is nothing here.
    static void centered_note(const char* text) {
        const ImVec2 room = ImGui::GetContentRegionAvail();
        const ImVec2 size = ImGui::CalcTextSize(text, nullptr, false, room.x * 0.8f);
        const ImVec2 from = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddText(nullptr, 0.0f,
                                            ImVec2(from.x + std::max((room.x - size.x) * 0.5f, 0.0f),
                                                   from.y + std::max((room.y - size.y) * 0.4f, 0.0f)),
                                            ImGui::GetColorU32(ImGuiCol_TextDisabled), text, nullptr, room.x * 0.8f);
    }

    // "12 draws, 4 indirect": what a pass recorded, short enough for the list.
    static const char* work_short(const rhi::ListWork& work, std::span<char> out) {
        out[0] = '\0';
        size_t length = 0;
        const auto add = [&](uint32_t count, const char* one, const char* many) {
            if (count == 0 || length + 1 >= out.size()) {
                return;
            }
            const int written = std::snprintf(out.data() + length, out.size() - length, "%s%u %s",
                                              length > 0 ? ", " : "", count, count == 1 ? one : many);
            if (written > 0) {
                length = std::min(length + static_cast<size_t>(written), out.size() - 1);
            }
        };
        add(work.draws, "draw", "draws");
        add(work.indirect_calls, "indirect", "indirect");
        add(work.dispatches, "dispatch", "dispatches");
        add(work.copies, "copy", "copies");
        return out.data();
    }

    // "2329x1180", "212x116x64", "1024x1024 [6], 5 mips".
    static const char* size_text(const rhi::TextureDesc& desc, std::span<char> out) {
        char more[32] = {};
        if (desc.dimension == rhi::TextureDimension::Texture3D) {
            format_text(more, "x%u", desc.depth);
        } else if (desc.layer_count > 1) {
            format_text(more, " [%u]", desc.layer_count);
        }
        char mips[24] = {};
        if (desc.mip_count > 1) {
            format_text(mips, ", %u mips", desc.mip_count);
        }
        return format_text(out, "%ux%u%s%s", desc.width, desc.height, more, mips);
    }

    void describe(const rhi::TextureDesc& desc, std::span<char> out) const {
        char size[64];
        format_text(out, "%s %s", size_text(desc, size), rhi::format_info(desc.format).name);
    }

    void build_order() {
        order.clear();
        for (uint32_t p = 0; p < passes.size(); ++p) {
            if (contains_no_case(passes[p].name.view(), filter)) {
                order.push(p);
            }
        }
        if (by_cost) {
            // By the times shown, which change a few times a second: the list rests in between.
            std::ranges::stable_sort(order.span(), [this](uint32_t a, uint32_t b) {
                const Timing* ta = timing(passes[a]);
                const Timing* tb = timing(passes[b]);
                return (ta != nullptr ? ta->shown_average : -1.0f) > (tb != nullptr ? tb->shown_average : -1.0f);
            });
        }
    }

    void draw_toolbar() {
        const float font = ImGui::GetFontSize();
        if (toggle_button("Pause", paused)) {
            paused = !paused;
        }
        ImGui::SetItemTooltip("Hold the passes and their times as they are. The texture shown stays live.");
        ImGui::SameLine(0.0f, font);
        if (ImGui::Button("Mark")) {
            mark();
        }
        ImGui::SetItemTooltip("Remember each pass's time as it is now. Every pass then shows how far it has moved "
                              "from that: change a setting, read the difference.");
        ImGui::SameLine();
        ImGui::BeginDisabled(!has_mark);
        if (ImGui::Button("Clear mark")) {
            clear_mark();
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, font);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Sort");
        ImGui::SameLine();
        if (toggle_button("Order", !by_cost)) {
            by_cost = false;
        }
        ImGui::SetItemTooltip("The passes in the order they run.");
        ImGui::SameLine(0.0f, 1.0f);
        if (toggle_button("Cost", by_cost)) {
            by_cost = true;
        }
        ImGui::SetItemTooltip("The costliest pass first.");
        ImGui::SameLine(0.0f, font);
        ImGui::SetNextItemWidth(font * 9.0f);
        ImGui::InputTextWithHint("##filter", "filter by name", filter, sizeof(filter));
        // What the frame costs, against the right edge.
        char text[128];
        if (culled_rows > 0) {
            format_text(text, "GPU %.2f ms    %zu passes, %u culled", static_cast<double>(shown_frame_ms),
                        passes.size(), culled_rows);
        } else {
            format_text(text, "GPU %.2f ms    %zu passes", static_cast<double>(shown_frame_ms), passes.size());
        }
        ImGui::SameLine();
        const float room = ImGui::GetContentRegionAvail().x;
        const float width = ImGui::CalcTextSize(text).x;
        if (width < room) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room - width);
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SetItemTooltip("The frame on the GPU, from its first pass to its last, without this window's own "
                              "passes (%.2f ms).",
                              static_cast<double>(shown_own_ms));
    }

    void draw_passes() {
        build_order();
        const float font = ImGui::GetFontSize();
        if (!ImGui::BeginTable("passes", has_mark ? 5 : 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg)) {
            return;
        }
        // Fixed widths: a column that fits its numbers moves every time they change.
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, font * 2.4f);
        ImGui::TableSetupColumn("share", ImGuiTableColumnFlags_WidthFixed, font * 3.0f);
        if (has_mark) {
            ImGui::TableSetupColumn("vs mark", ImGuiTableColumnFlags_WidthFixed, font * 3.2f);
        }
        ImGui::TableSetupColumn("work", ImGuiTableColumnFlags_WidthFixed, font * 6.5f);
        ImGui::TableHeadersRow();
        const PassRow* selected = selected_row();
        char text[128];
        for (const uint32_t index : order) {
            const PassRow& row = passes[index];
            const Timing* time = timing(row);
            ImGui::PushID(static_cast<int>(index));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (!row.ran) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            }
            if (list_row(row.name.c_str(), &row == selected)) {
                const Name name = row.name;
                select(name.view(), row.occurrence, {}, UINT32_MAX);
            }
            ImGui::TableNextColumn();
            if (!row.ran) {
                cell_right("culled");
            } else if (time != nullptr) {
                cell_right(format_text(text, "%.2f", static_cast<double>(time->shown_average)));
            } else {
                cell_right("-");
            }
            ImGui::TableNextColumn();
            if (row.ran && time != nullptr && shown_frame_ms > 0.0f) {
                cell_share(time->shown_average / shown_frame_ms);
            }
            if (has_mark) {
                ImGui::TableNextColumn();
                if (row.ran && time != nullptr && time->mark >= 0.0f) {
                    const float moved = time->shown_average - time->mark;
                    const ImVec4 ink = moved > 0.02f    ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f)
                                       : moved < -0.02f ? ImVec4(0.5f, 0.9f, 0.5f, 1.0f)
                                                        : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                    ImGui::PushStyleColor(ImGuiCol_Text, ink);
                    cell_right(format_text(text, "%+.2f", static_cast<double>(moved)));
                    ImGui::PopStyleColor();
                }
            }
            ImGui::TableNextColumn();
            if (row.ran) {
                ImGui::TextUnformatted(work_short(row.work, text));
            }
            if (!row.ran) {
                ImGui::PopStyleColor();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // Every texture of the frame: choosing one shows it as the last pass to write it left it.
    void draw_textures() {
        const float font = ImGui::GetFontSize();
        if (!ImGui::BeginTable("textures", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg)) {
            return;
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Texture", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed, font * 7.5f);
        ImGui::TableSetupColumn("format", ImGuiTableColumnFlags_WidthFixed, font * 6.5f);
        ImGui::TableSetupColumn("MiB", ImGuiTableColumnFlags_WidthFixed, font * 3.0f);
        ImGui::TableHeadersRow();
        char text[128];
        for (uint32_t t = 0; t < textures.size(); ++t) {
            const TextureRow& row = textures[t];
            // Not the view's own: its picture, its probe, its copy of an integer texture.
            if (!row.used || row.name.view().starts_with("frame debugger") ||
                !contains_no_case(row.name.view(), filter)) {
                continue;
            }
            ImGui::PushID(static_cast<int>(t));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const char* label = row.name.empty() ? format_text(text, "(texture %u)", t) : row.name.c_str();
            if (list_row(label, has_texture && t == selected_resource)) {
                if (uint32_t pass = 0; last_user(t, pass)) {
                    const Name pass_name = passes[pass].name;
                    const Name name = row.name;
                    select(pass_name.view(), passes[pass].occurrence, name.view(), t);
                }
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(size_text(row.desc, text));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(rhi::format_info(row.desc.format).name);
            ImGui::TableNextColumn();
            cell_right(format_text(text, "%.1f", static_cast<double>(texture_bytes(row.desc)) / (1024.0 * 1024.0)));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // What the pass writes, then what it reads: its textures to choose from, its buffers beside them.
    void draw_uses(const PassRow& row) {
        const float font = ImGui::GetFontSize();
        const float line = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0f);
        const float height = static_cast<float>(std::clamp(row.use_count, 1U, 7U)) * line;
        if (!ImGui::BeginTable("uses", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg, ImVec2(0.0f, height))) {
            return;
        }
        ImGui::TableSetupColumn("use", ImGuiTableColumnFlags_WidthFixed, font * 3.2f);
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed, font * 7.5f);
        ImGui::TableSetupColumn("format", ImGuiTableColumnFlags_WidthFixed, font * 6.5f);
        char text[128];
        for (int pass = 0; pass < 2; ++pass) {
            const bool writes = pass == 0;
            for (uint32_t u = row.first_use; u < row.first_use + row.use_count; ++u) {
                const UseRow& use = uses[u];
                if (use.writes != writes) {
                    continue;
                }
                ImGui::PushID(static_cast<int>(u));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const char* tag = writes ? "writes" : "reads";
                if (use.is_texture && use.resource < textures.size()) {
                    const TextureRow& texture = textures[use.resource];
                    if (!writes) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    }
                    const bool clicked = list_row(tag, has_texture && use.resource == selected_resource);
                    if (!writes) {
                        ImGui::PopStyleColor();
                    }
                    ImGui::SetItemTooltip("In the state %s.", rhi::resource_state_name(use.state).c_str());
                    if (clicked) {
                        const Name pass_name = row.name;
                        const Name name = texture.name;
                        select(pass_name.view(), row.occurrence, name.view(), use.resource);
                        ImGui::PopID();
                        break; // the rows aren't what they were
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(texture.name.empty() ? "(unnamed)" : texture.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(size_text(texture.desc, text));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(rhi::format_info(texture.desc.format).name);
                } else if (!use.is_texture && use.resource < buffers.size()) {
                    // A buffer: listed, not looked into.
                    const BufferRow& buffer = buffers[use.resource];
                    ImGui::TextDisabled("%s", tag);
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", buffer.name.empty() ? "(unnamed)" : buffer.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", format_text(text, "%.1f KiB", static_cast<double>(buffer.size) / 1024.0));
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("buffer");
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    void draw_view_controls() {
        const float font = ImGui::GetFontSize();
        // What of it shows.
        const auto channel = [&](const char* label, uint32_t bit) {
            if (toggle_button(label, (view.channels & bit) != 0)) {
                view.channels ^= bit;
            }
            ImGui::SameLine(0.0f, 1.0f);
        };
        channel("R", kChannelRed);
        channel("G", kChannelGreen);
        channel("B", kChannelBlue);
        channel("A", kChannelAlpha);
        ImGui::SameLine(0.0f, font);
        if (toggle_button("Values", !view.color)) {
            view.color = false;
        }
        ImGui::SetItemTooltip("The numbers as they are stored.");
        ImGui::SameLine(0.0f, 1.0f);
        if (toggle_button("Color", view.color)) {
            view.color = true;
        }
        ImGui::SetItemTooltip("As light: shown the way the display would show it.");
        ImGui::SameLine(0.0f, font);
        if (ImGui::Button("Fit")) {
            view.fit = true;
        }
        ImGui::SetItemTooltip("The whole texture in view (a double-click on it does the same).");
        ImGui::SameLine(0.0f, 1.0f);
        if (ImGui::Button("1:1")) {
            // A texel a pixel, about the middle of what shows now.
            const float half_x = static_cast<float>(view_width) * 0.5f;
            const float half_y = static_cast<float>(view_height) * 0.5f;
            view.origin_x += half_x * (view.texels_per_pixel - 1.0f);
            view.origin_y += half_y * (view.texels_per_pixel - 1.0f);
            view.texels_per_pixel = 1.0f;
            view.fit = false;
        }
        ImGui::SameLine();
        char text[96];
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(
            "%s", format_text(text, "%.0f%%", 100.0 / static_cast<double>(std::max(view.texels_per_pixel, 1e-6f))));

        // Which values, and which part.
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Range");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(font * 10.0f);
        const float speed = std::max((view.range_max - view.range_min) * 0.005f, 1e-5f);
        ImGui::DragFloatRange2("##range", &view.range_min, &view.range_max, speed, 0.0f, 0.0f, "%.4g", "%.4g");
        ImGui::SetItemTooltip("The values shown from black to white. Drag, or Ctrl+click to type.");
        struct Preset {
            const char* label;
            float from;
            float to;
        };
        constexpr Preset kPresets[] = {{.label = "0..1", .from = 0.0f, .to = 1.0f},
                                       {.label = "0..0.01", .from = 0.0f, .to = 0.01f},
                                       {.label = "0..16", .from = 0.0f, .to = 16.0f},
                                       {.label = "-1..1", .from = -1.0f, .to = 1.0f}};
        for (const Preset& preset : kPresets) {
            ImGui::SameLine(0.0f, 1.0f);
            if (toggle_button(preset.label, view.range_min == preset.from && view.range_max == preset.to)) {
                view.range_min = preset.from;
                view.range_max = preset.to;
            }
        }
        const uint32_t layers = viewed_desc.dimension == rhi::TextureDimension::Texture3D
                                    ? rhi::mip_extent(viewed_desc, view.mip).depth
                                    : viewed_desc.layer_count;
        if (viewed_desc.mip_count > 1) {
            int mip = static_cast<int>(view.mip);
            ImGui::SameLine(0.0f, font);
            ImGui::SetNextItemWidth(font * 6.0f);
            if (ImGui::SliderInt("Mip", &mip, 0, static_cast<int>(viewed_desc.mip_count) - 1)) {
                view.mip = static_cast<uint32_t>(mip);
            }
        }
        if (layers > 1) {
            int layer = static_cast<int>(view.layer);
            ImGui::SameLine(0.0f, font);
            ImGui::SetNextItemWidth(font * 6.0f);
            if (ImGui::SliderInt(viewed_desc.dimension == rhi::TextureDimension::Texture3D ? "Slice" : "Layer", &layer,
                                 0, static_cast<int>(layers) - 1)) {
                view.layer = static_cast<uint32_t>(layer);
            }
        }
    }

    // The texel under the mouse, in columns that stay where they are; its value changes a few times a second, not
    // every frame.
    void draw_probe() {
        const double now = ImGui::GetTime();
        if (probe_value.valid != shown_probe.valid || probe_value.x != shown_probe.x ||
            probe_value.y != shown_probe.y || now - shown_probe_s > 0.2) {
            shown_probe = probe_value;
            shown_probe_s = now;
        }
        ImGui::AlignTextToFramePadding();
        if (!shown_probe.valid) {
            ImGui::TextDisabled("Point at a texel to read it. The wheel zooms, a drag pans, a double-click fits.");
            return;
        }
        const float font = ImGui::GetFontSize();
        char text[64];
        ImGui::TextDisabled("%s", format_text(text, "texel %u, %u", shown_probe.x, shown_probe.y));
        if (is_integer(viewed_desc.format)) {
            ImGui::SameLine(font * 8.5f);
            ImGui::TextUnformatted(format_text(text, "value %.0f", static_cast<double>(shown_probe.value[0])));
            return;
        }
        constexpr const char* kNames[4] = {"R", "G", "B", "A"};
        const uint32_t count = rhi::format_info(viewed_desc.format).depth ? 1 : 4;
        for (uint32_t c = 0; c < count; ++c) {
            ImGui::SameLine((font * 8.5f) + (font * 6.2f * static_cast<float>(c)));
            ImGui::TextDisabled("%s", kNames[c]);
            ImGui::SameLine(0.0f, font * 0.4f);
            ImGui::TextUnformatted(format_text(text, "%.5g", static_cast<double>(shown_probe.value[c])));
        }
    }

    void draw_view() {
        draw_view_controls();
        ImVec2 size = ImGui::GetContentRegionAvail();
        size.y -= ImGui::GetFrameHeightWithSpacing(); // the probe's line, under the picture
        if (size.x < 8.0f || size.y < 8.0f) {
            return;
        }
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 scale = io.DisplayFramebufferScale;
        view_width = static_cast<uint32_t>(std::lround(size.x * scale.x));
        view_height = static_cast<uint32_t>(std::lround(size.y * scale.y));
        const ImVec2 corner = ImGui::GetCursorScreenPos();
        const ImVec2 far_corner(corner.x + size.x, corner.y + size.y);
        ImGui::InvisibleButton("view", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY); // the wheel zooms: it doesn't scroll what's around
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(corner, far_corner, IM_COL32(16, 16, 18, 255));
        const bool has_picture = target.is_valid() && target_state == rhi::ResourceState::ShaderRead && drawn_width > 0;
        if (has_picture && last_problem == ViewProblem::None) {
            // What was drawn, where it was drawn: a panel just resized shows the last picture at its own size.
            const ImVec2 uv(static_cast<float>(drawn_width) / static_cast<float>(target_desc.width),
                            static_cast<float>(drawn_height) / static_cast<float>(target_desc.height));
            const ImVec2 drawn_to(corner.x + (static_cast<float>(drawn_width) / scale.x),
                                  corner.y + (static_cast<float>(drawn_height) / scale.y));
            draw->PushClipRect(corner, far_corner, true);
            draw->AddImage(ImTextureRef(static_cast<ImTextureID>(
                               render::ImGuiRenderer::texture_id(device->texture_index(target)))),
                           corner, drawn_to, ImVec2(0.0f, 0.0f), uv);
            draw->PopClipRect();
        } else if (last_problem != ViewProblem::None) {
            const char* why = problem_text(last_problem);
            const ImVec2 text_size = ImGui::CalcTextSize(why);
            draw->AddText(
                ImVec2(corner.x + ((size.x - text_size.x) * 0.5f), corner.y + ((size.y - text_size.y) * 0.5f)),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), why);
        }
        draw->AddRect(corner, far_corner, ImGui::GetColorU32(ImGuiCol_Border));
        if (hovered) {
            const float x_px = (io.MousePos.x - corner.x) * scale.x;
            const float y_px = (io.MousePos.y - corner.y) * scale.y;
            if (io.MouseWheel != 0.0f) {
                const float zoomed =
                    std::clamp(view.texels_per_pixel * std::pow(0.8f, io.MouseWheel), 1.0f / 64.0f, 64.0f);
                view.origin_x += x_px * (view.texels_per_pixel - zoomed); // the texel under the mouse stays there
                view.origin_y += y_px * (view.texels_per_pixel - zoomed);
                view.texels_per_pixel = zoomed;
                view.fit = false;
            }
            const float texel_x = std::floor(view.origin_x + (x_px * view.texels_per_pixel));
            const float texel_y = std::floor(view.origin_y + (y_px * view.texels_per_pixel));
            if (texel_x >= 0.0f && texel_y >= 0.0f && texel_x < static_cast<float>(viewed_width) &&
                texel_y < static_cast<float>(viewed_height)) {
                probe_wanted = true;
                probe_x = static_cast<uint32_t>(texel_x);
                probe_y = static_cast<uint32_t>(texel_y);
            }
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                view.fit = true;
            }
        }
        if (held && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
            view.origin_x -= io.MouseDelta.x * scale.x * view.texels_per_pixel;
            view.origin_y -= io.MouseDelta.y * scale.y * view.texels_per_pixel;
            view.fit = false;
        }
        draw_probe();
    }

    // The right side: the pass chosen, what it writes and reads, and under that the texture chosen of those.
    void draw_detail() {
        if (!has_selection) {
            centered_note("Choose a pass on the left to see what it writes and reads, and one of its textures to "
                          "look at it as that pass left it.");
            return;
        }
        const PassRow* row = selected_row();
        char text[192];
        char more[96];
        if (row == nullptr) {
            band("Pass", selected_pass.c_str(), nullptr, "not in the frame");
        } else {
            const Timing* time = timing(*row);
            if (!row->ran) {
                band("Pass", row->name.c_str(), nullptr, "culled");
                ImGui::TextDisabled("Nothing reads what it writes, so it didn't run.");
            } else {
                more[0] = '\0';
                if (time != nullptr && shown_frame_ms > 0.0f) {
                    format_text(more, "%.2f ms    %.0f%% of the frame", static_cast<double>(time->shown_average),
                                static_cast<double>(100.0f * time->shown_average / shown_frame_ms));
                } else if (time != nullptr) {
                    format_text(more, "%.2f ms", static_cast<double>(time->shown_average));
                }
                band("Pass", row->name.c_str(), nullptr, more);
                char work[128];
                work_text(row->work, work);
                if (time != nullptr) {
                    ImGui::TextDisabled("%s", format_text(text, "%s%speak %.2f ms", work, work[0] != '\0' ? "    " : "",
                                                          static_cast<double>(time->shown_peak)));
                } else if (work[0] != '\0') {
                    ImGui::TextDisabled("%s", work);
                }
            }
            if (row->use_count > 0) {
                draw_uses(*row);
            }
        }
        // The texture chosen of those, or the first the pass writes.
        if (!has_texture) {
            if (row != nullptr && row->ran) {
                centered_note("This pass uses no textures: there is nothing to look at.");
            }
            return;
        }
        ImGui::Spacing();
        more[0] = '\0';
        if (last_problem == ViewProblem::None && drawn_width > 0) {
            describe(viewed_desc, more);
        }
        band("Showing", selected_texture.empty() ? "(unnamed texture)" : selected_texture.c_str(),
             format_text(text, "as %s left it", selected_pass.c_str()), more);
        draw_view();
    }
};

FrameDebugger FrameDebugger::create(rhi::Device& device, shader::ShaderLibrary& shaders) {
    auto* state =
        ::new (tools_heap().allocate(sizeof(FrameDebuggerState), alignof(FrameDebuggerState))) FrameDebuggerState();
    state->device = &device;
    state->shaders = &shaders;
    return FrameDebugger(state);
}

FrameDebugger::~FrameDebugger() {
    if (state_ == nullptr) {
        return;
    }
    FrameDebuggerState& state = *state_;
    if (state.console != nullptr) {
        state.console->remove(state_);
    }
    if (state.hooked != nullptr) {
        state.hooked->set_pass_hook(nullptr, nullptr);
    }
    state.shaders->destroy(state.view_program);
    state.shaders->destroy(state.probe_program);
    state.shaders->destroy(state.ids_program);
    state.device->destroy(state.target);
    state.device->destroy(state.probe_texture);
    for (const rhi::BufferHandle buffer : state.probe_readback) {
        state.device->destroy(buffer);
    }
    state_->~FrameDebuggerState();
    tools_heap().deallocate(state_, sizeof(FrameDebuggerState), alignof(FrameDebuggerState));
}

FrameDebugger::FrameDebugger(FrameDebugger&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}

FrameDebugger& FrameDebugger::operator=(FrameDebugger&& other) noexcept {
    if (this != &other) {
        const FrameDebugger discarded(std::exchange(state_, std::exchange(other.state_, nullptr)));
    }
    return *this;
}

bool FrameDebugger::open() const {
    return state_->open;
}

void FrameDebugger::set_open(bool open) {
    state_->open = open;
}

void FrameDebugger::draw() {
    FrameDebuggerState& state = *state_;
    if (!state.open) {
        return;
    }
    DE_PROFILE_ZONE("frame debugger");
    if (!state.has_selection && !state.auto_selected) {
        state.select_costliest();
    }
    const float font = ImGui::GetFontSize();
    const ImGuiViewport* screen = ImGui::GetMainViewport();
    const ImVec2 size(std::min(font * 66.0f, screen->WorkSize.x * 0.8f),
                      std::min(font * 38.0f, screen->WorkSize.y * 0.8f));
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(screen->WorkPos.x + ((screen->WorkSize.x - size.x) * 0.5f),
                                   screen->WorkPos.y + ((screen->WorkSize.y - size.y) * 0.5f)),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(font * 34.0f, font * 18.0f), ImVec2(FLT_MAX, FLT_MAX));
    bool open = true;
    if (ImGui::Begin("Frame Debugger", &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        state.draw_toolbar();
        ImGui::Spacing();
        // Left: the frame's passes (or all its textures). Right: the pass chosen, and its texture.
        ImGui::BeginChild("lists", ImVec2(std::max(font * 25.0f, ImGui::GetContentRegionAvail().x * 0.44f), 0.0f),
                          ImGuiChildFlags_ResizeX);
        if (ImGui::BeginTabBar("lists")) {
            if (ImGui::BeginTabItem("Passes")) {
                state.draw_passes();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("All textures")) {
                state.draw_textures();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("detail", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        state.draw_detail();
        ImGui::EndChild();
    }
    ImGui::End();
    state.open = open;
}

void FrameDebugger::begin_frame(render::RenderGraph& graph, rhi::Format ui_format) {
    FrameDebuggerState& state = *state_;
    state.shown = {};
    state.view_added = false;
    state.seen_selected = 0;
    state.last_problem = state.problem;
    state.ui_format = ui_format;
    state.read_probe();
    const bool watching = state.open && state.has_selection && ui_format != rhi::Format::Unknown;
    if (!watching || !state.ensure_programs()) {
        if (state.hooked != nullptr) {
            state.hooked->set_pass_hook(nullptr, nullptr);
            state.hooked = nullptr;
        }
        state.problem = ViewProblem::None;
        return;
    }
    state.problem = ViewProblem::PassMissing; // until the hook finds it
    graph.set_pass_hook(&FrameDebuggerState::pass_added, state_);
    state.hooked = &graph;
}

render::GraphTexture FrameDebugger::shown() const {
    return state_->shown;
}

void FrameDebugger::end_frame(render::RenderGraph& graph) {
    FrameDebuggerState& state = *state_;
    if (!state.open || state.paused) {
        return;
    }
    DE_PROFILE_ZONE("frame debugger take");
    // The times every frame, into their averages; the rows and the numbers shown, a few times a second.
    const uint64_t now = platform::time_ns();
    const bool due = state.passes.empty() || now - state.taken_ns >= kRefreshNs;
    if (due) {
        state.take_rows(graph);
    }
    state.accumulate(graph);
    if (due) {
        state.refresh_shown();
        state.taken_ns = now;
    }
}

void FrameDebugger::select(std::string_view pass, uint32_t occurrence, std::string_view texture) {
    state_->select(pass, occurrence, texture, UINT32_MAX);
}

void FrameDebugger::set_view_size(uint32_t width, uint32_t height) {
    state_->view_width = std::max(width, 1U);
    state_->view_height = std::max(height, 1U);
}

void FrameDebugger::probe_at(uint32_t x, uint32_t y) {
    FrameDebuggerState& state = *state_;
    if (!state.probe_wanted || state.probe_x != x || state.probe_y != y) {
        state.probe_value.valid = false;
    }
    state.probe_wanted = true;
    state.probe_x = x;
    state.probe_y = y;
}

ProbeValue FrameDebugger::probe() const {
    return state_->probe_value;
}

bool FrameDebugger::viewing() const {
    return state_->view_added;
}

const char* FrameDebugger::view_problem() const {
    return problem_text(state_->problem);
}

void FrameDebugger::report(console::Output& out, int32_t pass) const {
    state_->report(out, pass);
}

namespace {

void open_command(void* user, const console::Args& args, console::Output& out) {
    auto& state = *static_cast<FrameDebuggerState*>(user);
    bool open = !state.open;
    if (!args.empty() && !console::Args::parse_bool(args[0], open)) {
        out.error("frame_debugger takes on or off");
        return;
    }
    state.open = open;
    out.info("frame debugger %s", open ? "open" : "closed");
}

// A pass by its number in the list, or by its name (the first of that name).
bool find_pass(const FrameDebuggerState& state, std::string_view word, uint32_t& out) {
    int32_t number = 0;
    if (console::Args::parse_int(word, number)) {
        out = static_cast<uint32_t>(number);
        return number >= 0 && static_cast<size_t>(number) < state.passes.size();
    }
    for (uint32_t p = 0; p < state.passes.size(); ++p) {
        if (state.passes[p].name.view() == word) {
            out = p;
            return true;
        }
    }
    return false;
}

void report_command(void* user, const console::Args& args, console::Output& out) {
    const auto& state = *static_cast<const FrameDebuggerState*>(user);
    if (args.empty()) {
        state.report(out, -1);
        return;
    }
    uint32_t pass = 0;
    if (!find_pass(state, args[0], pass)) {
        out.error("no pass '%.*s' in the frame", static_cast<int>(args[0].size()), args[0].data());
        return;
    }
    state.report(out, static_cast<int32_t>(pass));
}

void select_command(void* user, const console::Args& args, console::Output& out) {
    auto& state = *static_cast<FrameDebuggerState*>(user);
    if (args.empty()) {
        out.error("frame_debugger.select <pass> [texture]: a pass by its number in frame_debugger.report, or by name");
        return;
    }
    const std::string_view texture = args.size() > 1 ? args[1] : std::string_view();
    uint32_t pass = 0;
    int32_t number = 0;
    if (find_pass(state, args[0], pass)) {
        const PassRow& row = state.passes[pass];
        state.select(row.name.view(), row.occurrence, texture, UINT32_MAX);
    } else if (console::Args::parse_int(args[0], number)) {
        out.error("no pass %d in the frame", number);
        return;
    } else {
        state.select(args[0], 0, texture, UINT32_MAX); // by name: it needn't be in the frame yet
    }
    state.open = true;
    out.info("looking at %s%s%.*s", state.selected_pass.c_str(), texture.empty() ? "" : ": ",
             static_cast<int>(texture.size()), texture.data());
}

void probe_command(void* user, const console::Args& args, console::Output& out) {
    auto& state = *static_cast<FrameDebuggerState*>(user);
    if (args.size() >= 2) {
        int32_t x = 0;
        int32_t y = 0;
        if (!console::Args::parse_int(args[0], x) || !console::Args::parse_int(args[1], y) || x < 0 || y < 0) {
            out.error("frame_debugger.probe [x y]: a texel of the texture looked at");
            return;
        }
        state.probe_wanted = true;
        state.probe_x = static_cast<uint32_t>(x);
        state.probe_y = static_cast<uint32_t>(y);
        state.probe_value.valid = false;
        out.info("reading texel %d, %d: ask again in a few frames", x, y);
        return;
    }
    if (!state.probe_value.valid) {
        out.info("no texel read yet%s%s", state.view_added ? "" : ": ",
                 state.view_added ? "" : problem_text(state.problem));
        return;
    }
    out.info("texel %u, %u: %.6g %.6g %.6g %.6g", state.probe_value.x, state.probe_value.y,
             static_cast<double>(state.probe_value.value[0]), static_cast<double>(state.probe_value.value[1]),
             static_cast<double>(state.probe_value.value[2]), static_cast<double>(state.probe_value.value[3]));
}

} // namespace

void FrameDebugger::add_commands(console::Console& console) {
    state_->console = &console;
    console.add_command("frame_debugger", "[on | off]",
                        "Open or close the frame debugger: the frame's passes, their GPU time and work, their "
                        "textures.",
                        &open_command, state_);
    console.add_command("frame_debugger.report", "[pass]",
                        "The frame's passes as text, or one pass's textures and buffers (the debugger must be open).",
                        &report_command, state_);
    console.add_command("frame_debugger.select", "<pass> [texture]",
                        "Look at a pass (its number in the report, or its name) and one of its textures.",
                        &select_command, state_);
    console.add_command("frame_debugger.probe", "[x y]",
                        "Read a texel of the texture looked at; with nothing after it, the last one read.",
                        &probe_command, state_);
}

} // namespace de::tools
