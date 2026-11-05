#include "render/imgui/imgui_renderer.h"

#include "core/allocator.h"
#include "core/array.h"
#include "core/log.h"
#include "core/profile.h"
#include "render/rhi/device.h"
#include "render/rhi/upload.h"
#include "render/shader/shader_library.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <span>
#include <utility>

namespace de::render {

namespace {

core::Allocator& render_heap() {
    return core::heap_allocator(core::MemoryTag::Render);
}

// Matches ImGuiPush in shaders/imgui.slang.
struct ImGuiPush {
    uint64_t vertices = 0;
    float scale[2] = {};
    float translate[2] = {};
    uint32_t texture = 0;
    uint32_t sampler = 0;
    uint32_t srgb_target = 0;
    uint32_t vertex_base = 0; // the draw's first vertex in the buffer (SV_VertexID is its index)
};

static_assert(sizeof(ImDrawVert) == 20, "shaders/imgui.slang reads 20-byte vertices");
static_assert(sizeof(ImDrawIdx) == 2, "the index buffer is bound as 16-bit");

// A texture ImGui asked for. Its ImTextureID carries the slot (high 32 bits, + 1) and the
// bindless index (low 32 bits); engine textures shown with ImGui::Image() have no slot.
struct OwnedTexture {
    rhi::TextureHandle handle;
    bool live = false;
};

struct FrameBuffers {
    rhi::BufferHandle vertices;
    rhi::BufferHandle indices;
    uint64_t vertex_bytes = 0;
    uint64_t index_bytes = 0;
};

uint32_t slot_of(ImTextureID id) {
    return static_cast<uint32_t>(id >> 32U);
}

uint32_t bindless_of(ImTextureID id) {
    return static_cast<uint32_t>(id & 0xFFFFFFFFU);
}

} // namespace

struct ImGuiRendererState {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
    rhi::Uploader* uploader = nullptr;
    shader::ProgramHandle program;
    rhi::Format program_format = rhi::Format::Unknown;
    core::Array<OwnedTexture> textures{render_heap()};
    core::Array<uint32_t> free_slots{render_heap()};
    FrameBuffers frames[rhi::kFramesInFlight];
    core::Array<std::byte> converted{render_heap()}; // an alpha-only atlas as RGBA8
    uint32_t failed_textures = 0;

    // Room for `bytes` in a mapped buffer of the current frame, grown by half again when short.
    bool reserve(rhi::BufferHandle& buffer, uint64_t& capacity, uint64_t bytes, rhi::BufferUsage usage,
                 const char* name) {
        if (bytes <= capacity) {
            return true;
        }
        device->destroy(buffer); // freed once the frames that used it are done
        const uint64_t size = std::max<uint64_t>(bytes + (bytes / 2), uint64_t{64} * 1024);
        const auto created = device->create_buffer(
            {.size = size, .usage = usage, .memory = rhi::MemoryLocation::Upload, .debug_name = name});
        if (!created) {
            buffer = {};
            capacity = 0;
            return false;
        }
        buffer = *created;
        capacity = size;
        return true;
    }

    // The texture's pixels as RGBA8 (ImGui may keep an alpha-only atlas), the whole texture each
    // time: ImGui only adds pixels where none were used before, and updates are rare (new glyphs).
    // A partial upload would need regions in the uploader. False if it couldn't be queued.
    bool upload(rhi::TextureHandle handle, ImTextureData& texture) {
        const auto pixels = static_cast<size_t>(texture.Width) * static_cast<size_t>(texture.Height);
        std::span<const std::byte> bytes;
        if (texture.Format == ImTextureFormat_RGBA32) {
            bytes = std::span(static_cast<const std::byte*>(texture.GetPixels()), pixels * 4);
        } else {
            converted.resize(pixels * 4); // kept between uploads: no allocation once it's big enough
            const auto* alpha = static_cast<const uint8_t*>(texture.GetPixels());
            for (size_t i = 0; i < pixels; ++i) {
                converted[(i * 4) + 0] = std::byte{255};
                converted[(i * 4) + 1] = std::byte{255};
                converted[(i * 4) + 2] = std::byte{255};
                converted[(i * 4) + 3] = static_cast<std::byte>(alpha[i]);
            }
            bytes = converted.span();
        }
        const auto queued = uploader->upload_texture(
            handle, bytes, {.after = rhi::ResourceState::ShaderRead, .after_stages = rhi::ShaderStages::Fragment});
        if (!queued && ++failed_textures <= 4) {
            DE_LOG_ERROR("render", "imgui texture upload: %s", queued.error().message);
        }
        return queued.ok();
    }

    void create(ImTextureData& texture) {
        const auto handle = device->create_texture({.width = static_cast<uint32_t>(texture.Width),
                                                    .height = static_cast<uint32_t>(texture.Height),
                                                    .format = rhi::Format::RGBA8Unorm,
                                                    .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst,
                                                    .debug_name = "imgui texture"});
        if (!handle) {
            if (++failed_textures <= 4) {
                DE_LOG_ERROR("render", "imgui texture %dx%d: %s", texture.Width, texture.Height,
                             handle.error().message);
            }
            return; // stays WantCreate: tried again next frame
        }
        uint32_t slot = 0;
        if (!free_slots.empty()) {
            slot = free_slots[free_slots.size() - 1];
            free_slots.pop();
        } else {
            slot = static_cast<uint32_t>(textures.size());
            textures.push({});
        }
        if (!upload(*handle, texture)) {
            device->destroy(*handle);
            free_slots.push(slot);
            return; // stays WantCreate: tried again next frame
        }
        textures[slot] = OwnedTexture{.handle = *handle, .live = true};
        texture.SetTexID((static_cast<uint64_t>(slot + 1) << 32U) | device->texture_index(*handle));
        texture.SetStatus(ImTextureStatus_OK);
    }

    void destroy(ImTextureData& texture) {
        const uint32_t slot = slot_of(texture.TexID);
        if (slot != 0 && slot - 1 < textures.size() && textures[slot - 1].live) {
            device->destroy(textures[slot - 1].handle); // freed once the frames using it are done
            textures[slot - 1] = {};
            free_slots.push(slot - 1);
        }
        texture.SetTexID(ImTextureID_Invalid);
        texture.SetStatus(ImTextureStatus_Destroyed);
    }

    // Whether a draw may sample this texture now: an engine texture always (the caller's job), an
    // ImGui one once its first upload has landed.
    bool drawable(ImTextureID id) const {
        const uint32_t slot = slot_of(id);
        if (slot == 0) {
            return id != ImTextureID_Invalid;
        }
        return slot - 1 < textures.size() && textures[slot - 1].live && uploader->ready(textures[slot - 1].handle);
    }
};

core::Result<ImGuiRenderer> ImGuiRenderer::create(rhi::Device& device, shader::ShaderLibrary& shaders,
                                                  rhi::Uploader& uploader) {
    if (ImGui::GetCurrentContext() == nullptr) {
        return core::fail(core::ErrorCode::InvalidArgument, "the ImGui renderer needs an ImGui context");
    }
    auto* state =
        ::new (render_heap().allocate(sizeof(ImGuiRendererState), alignof(ImGuiRendererState))) ImGuiRendererState();
    state->device = &device;
    state->shaders = &shaders;
    state->uploader = &uploader;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "deengine_rhi";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    platform_io.Renderer_TextureMaxWidth = static_cast<int>(device.capabilities().max_texture_size_2d);
    platform_io.Renderer_TextureMaxHeight = static_cast<int>(device.capabilities().max_texture_size_2d);
    return ImGuiRenderer(state);
}

ImGuiRenderer::~ImGuiRenderer() {
    if (state_ == nullptr) {
        return;
    }
    if (ImGui::GetCurrentContext() != nullptr) {
        // ImGui keeps its textures' pixels; they are made again if another renderer comes.
        for (ImTextureData* texture : ImGui::GetPlatformIO().Textures) {
            if (texture->RefCount == 1 && texture->Status != ImTextureStatus_Destroyed) {
                state_->destroy(*texture);
            }
        }
        ImGuiIO& io = ImGui::GetIO();
        io.BackendRendererName = nullptr;
        io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    }
    for (const OwnedTexture& texture : state_->textures) {
        if (texture.live) {
            state_->device->destroy(texture.handle);
        }
    }
    for (const FrameBuffers& frame : state_->frames) {
        state_->device->destroy(frame.vertices);
        state_->device->destroy(frame.indices);
    }
    if (state_->program.is_valid()) {
        state_->shaders->destroy(state_->program);
    }
    state_->~ImGuiRendererState();
    render_heap().deallocate(state_, sizeof(ImGuiRendererState), alignof(ImGuiRendererState));
}

ImGuiRenderer::ImGuiRenderer(ImGuiRenderer&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}

ImGuiRenderer& ImGuiRenderer::operator=(ImGuiRenderer&& other) noexcept {
    if (this != &other) {
        const ImGuiRenderer discarded(std::exchange(state_, std::exchange(other.state_, nullptr)));
    }
    return *this;
}

void ImGuiRenderer::update_textures(ImDrawData* draw_data) {
    DE_PROFILE_ZONE("imgui textures");
    if (draw_data == nullptr || draw_data->Textures == nullptr) {
        return;
    }
    for (ImTextureData* texture : *draw_data->Textures) {
        switch (texture->Status) {
        case ImTextureStatus_WantCreate:
            state_->create(*texture);
            break;
        case ImTextureStatus_WantUpdates: {
            const uint32_t slot = slot_of(texture->TexID);
            const bool owned = slot != 0 && slot - 1 < state_->textures.size() && state_->textures[slot - 1].live;
            if (!owned || state_->upload(state_->textures[slot - 1].handle, *texture)) {
                texture->SetStatus(ImTextureStatus_OK); // else stays WantUpdates: tried again next frame
            }
            break;
        }
        case ImTextureStatus_WantDestroy:
            state_->destroy(*texture);
            break;
        default:
            break;
        }
    }
}

void ImGuiRenderer::add_pass(RenderGraph& graph, GraphTexture target, rhi::Format format, const ImDrawData* draw_data) {
    DE_PROFILE_ZONE("imgui pass");
    ImGuiRendererState& state = *state_;
    if (draw_data == nullptr || draw_data->TotalVtxCount == 0 || draw_data->DisplaySize.x <= 0.0f ||
        draw_data->DisplaySize.y <= 0.0f) {
        return;
    }
    // The pipeline matches the target's format; made again when it changes (a new swapchain).
    if (format != state.program_format) {
        if (state.program.is_valid()) {
            state.shaders->destroy(state.program);
        }
        rhi::GraphicsPipelineDesc pipeline{.color_count = 1, .cull = rhi::CullMode::None, .debug_name = "imgui"};
        pipeline.color_formats[0] = format;
        pipeline.blend[0] = rhi::BlendMode::Alpha;
        const auto program = state.shaders->create_graphics({.module = "imgui", .state = pipeline});
        state.program = program.ok() ? *program : shader::ProgramHandle{};
        state.program_format = format;
    }
    const rhi::PipelineHandle pipeline = state.shaders->pipeline(state.program);
    if (!pipeline.is_valid()) {
        return; // still compiling, or broken: the log says which
    }

    // This frame's vertices and indices, all lists one after another.
    FrameBuffers& frame = state.frames[state.device->frame_index() % rhi::kFramesInFlight];
    const uint64_t vertex_bytes = uint64_t{sizeof(ImDrawVert)} * static_cast<uint64_t>(draw_data->TotalVtxCount);
    const uint64_t index_bytes = uint64_t{sizeof(ImDrawIdx)} * static_cast<uint64_t>(draw_data->TotalIdxCount);
    if (!state.reserve(frame.vertices, frame.vertex_bytes, vertex_bytes, rhi::BufferUsage::Storage, "imgui vertices") ||
        !state.reserve(frame.indices, frame.index_bytes, index_bytes, rhi::BufferUsage::Index, "imgui indices")) {
        return;
    }
    std::byte* vertices = state.device->mapped(frame.vertices).data();
    std::byte* indices = state.device->mapped(frame.indices).data();
    for (const ImDrawList* list : draw_data->CmdLists) {
        const size_t list_vertex_bytes = sizeof(ImDrawVert) * static_cast<size_t>(list->VtxBuffer.Size);
        const size_t list_index_bytes = sizeof(ImDrawIdx) * static_cast<size_t>(list->IdxBuffer.Size);
        std::memcpy(vertices, list->VtxBuffer.Data, list_vertex_bytes);
        std::memcpy(indices, list->IdxBuffer.Data, list_index_bytes);
        vertices += list_vertex_bytes;
        indices += list_index_bytes;
    }
    state.device->flush_mapped(frame.vertices, 0, vertex_bytes);
    state.device->flush_mapped(frame.indices, 0, index_bytes);

    struct PassData {
        GraphTexture target;
        const ImDrawData* draw_data = nullptr;
        const ImGuiRendererState* state = nullptr;
        rhi::PipelineHandle pipeline;
        rhi::BufferHandle indices;
        uint64_t vertices = 0;
        bool srgb = false;
    };
    graph.add_pass<PassData>(
        "imgui",
        [&](PassBuilder& pass, PassData& data) {
            pass.use(target, rhi::ResourceState::RenderTarget);
            data = PassData{.target = target,
                            .draw_data = draw_data,
                            .state = &state,
                            .pipeline = pipeline,
                            .indices = frame.indices,
                            .vertices = state.device->buffer_address(frame.vertices),
                            .srgb = rhi::format_info(format).srgb};
        },
        [](const PassData& data, PassContext& context) {
            const ImDrawData& draw = *data.draw_data;
            const rhi::TextureDesc& target_desc = context.desc(data.target);
            const rhi::ColorAttachment color{.texture = context.texture(data.target), .load = rhi::LoadOp::Load};
            context.list.begin_rendering({.colors = std::span(&color, 1)});
            context.list.bind_pipeline(data.pipeline);
            context.list.bind_index_buffer(data.indices, 0, rhi::IndexType::Uint16);

            // Points (y down, from DisplayPos) to clip space (y up); clip rects to pixels.
            const ImVec2 origin = draw.DisplayPos;
            const ImVec2 scale = draw.FramebufferScale;
            ImGuiPush push{.vertices = data.vertices,
                           .scale = {2.0f / draw.DisplaySize.x, -2.0f / draw.DisplaySize.y},
                           .translate = {-1.0f - (origin.x * 2.0f / draw.DisplaySize.x),
                                         1.0f + (origin.y * 2.0f / draw.DisplaySize.y)},
                           .sampler = rhi::sampler_index(rhi::DefaultSampler::LinearClamp),
                           .srgb_target = data.srgb ? 1U : 0U};
            uint32_t vertex_base = 0;
            uint32_t index_base = 0;
            for (const ImDrawList* list : draw.CmdLists) {
                for (const ImDrawCmd& command : list->CmdBuffer) {
                    if (command.UserCallback != nullptr) {
                        continue; // no callbacks in the engine's UI
                    }
                    const float left = std::max((command.ClipRect.x - origin.x) * scale.x, 0.0f);
                    const float top = std::max((command.ClipRect.y - origin.y) * scale.y, 0.0f);
                    const float right =
                        std::min((command.ClipRect.z - origin.x) * scale.x, static_cast<float>(target_desc.width));
                    const float bottom =
                        std::min((command.ClipRect.w - origin.y) * scale.y, static_cast<float>(target_desc.height));
                    // Not GetTexID(): it asserts for a texture whose creation failed (retried
                    // next frame); such draws are skipped instead.
                    const ImTextureID texture =
                        command.TexRef._TexData != nullptr ? command.TexRef._TexData->TexID : command.TexRef._TexID;
                    if (right <= left || bottom <= top || !data.state->drawable(texture)) {
                        continue;
                    }
                    context.list.set_scissor({.x = static_cast<int32_t>(left),
                                              .y = static_cast<int32_t>(top),
                                              .width = static_cast<uint32_t>(right - left),
                                              .height = static_cast<uint32_t>(bottom - top)});
                    push.texture = bindless_of(texture);
                    // The vertex base goes through push constants, not the draw's vertex offset:
                    // SV_VertexID leaves that offset out.
                    push.vertex_base = command.VtxOffset + vertex_base;
                    context.list.push_constants(push);
                    context.list.draw_indexed(command.ElemCount, 1, command.IdxOffset + index_base, 0);
                }
                vertex_base += static_cast<uint32_t>(list->VtxBuffer.Size);
                index_base += static_cast<uint32_t>(list->IdxBuffer.Size);
            }
            context.list.end_rendering();
        });
}

} // namespace de::render
