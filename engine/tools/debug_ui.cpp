#include "tools/debug_ui.h"

#include "core/allocator.h"
#include "core/assert.h"
#include "core/profile.h"
#include "core/result.h"
#include "platform/imgui_platform.h"
#include "render/imgui/imgui_renderer.h"

#include <imgui.h>

#include <cstddef>
#include <cstring>
#include <new>
#include <utility>

namespace de::tools {

namespace {

core::Allocator& tools_heap() {
    return core::heap_allocator(core::MemoryTag::Tools);
}

// ImGui frees without a size; tagged allocators need it, so each block starts with its size.
constexpr size_t kHeader = alignof(std::max_align_t);

void* imgui_allocate(size_t size, void* /*user_data*/) {
    auto* block = static_cast<std::byte*>(tools_heap().allocate(size + kHeader, kHeader));
    std::memcpy(block, &size, sizeof(size));
    return block + kHeader;
}

void imgui_free(void* pointer, void* /*user_data*/) {
    if (pointer == nullptr) {
        return;
    }
    std::byte* block = static_cast<std::byte*>(pointer) - kHeader;
    size_t size = 0;
    std::memcpy(&size, block, sizeof(size));
    tools_heap().deallocate(block, size + kHeader, kHeader);
}

} // namespace

void create_imgui() {
    DE_ASSERT_MSG(ImGui::GetCurrentContext() == nullptr, "one ImGui context at a time");
    IMGUI_CHECKVERSION();
    ImGui::SetAllocatorFunctions(imgui_allocate, imgui_free);
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // no fopen: layouts will be saved through the VFS by the editor
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
}

void destroy_imgui() {
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::DestroyContext();
    }
}

struct DebugUiState {
    core::Result<render::ImGuiRenderer> renderer = core::fail(core::ErrorCode::Unsupported, "no renderer");
    bool frame_built = false; // a UI frame was built since the last add_pass
};

DebugUi DebugUi::create(platform::Window& window, rhi::Device& device, shader::ShaderLibrary* shaders,
                        rhi::Uploader& uploader) {
    create_imgui();
    platform::imgui_platform_init(window);
    auto* state = ::new (tools_heap().allocate(sizeof(DebugUiState), alignof(DebugUiState))) DebugUiState();
    if (shaders != nullptr) {
        state->renderer = render::ImGuiRenderer::create(device, *shaders, uploader);
    }
    return DebugUi(state);
}

DebugUi::~DebugUi() {
    if (state_ == nullptr) {
        return;
    }
    if (state_->renderer) {
        const render::ImGuiRenderer discarded = std::move(*state_->renderer); // its textures, before the context
    }
    platform::imgui_platform_shutdown();
    destroy_imgui();
    state_->~DebugUiState();
    tools_heap().deallocate(state_, sizeof(DebugUiState), alignof(DebugUiState));
}

DebugUi::DebugUi(DebugUi&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}

DebugUi& DebugUi::operator=(DebugUi&& other) noexcept {
    if (this != &other) {
        const DebugUi discarded(std::exchange(state_, std::exchange(other.state_, nullptr)));
    }
    return *this;
}

bool DebugUi::begin_frame(platform::Window& window, float delta_s) {
    DE_PROFILE_ZONE("debug ui begin");
    if (!state_->renderer) {
        return false; // no renderer: its font atlas would never be made, and NewFrame() requires it
    }
    platform::imgui_platform_new_frame(window, delta_s);
    ImGui::NewFrame();
    return true;
}

void DebugUi::end_frame() {
    DE_PROFILE_ZONE("debug ui end");
    ImGui::Render();
    state_->renderer->update_textures(ImGui::GetDrawData());
    state_->frame_built = true;
}

void DebugUi::add_pass(render::RenderGraph& graph, render::GraphTexture target, rhi::Format format) {
    if (!state_->frame_built) {
        return;
    }
    state_->frame_built = false;
    state_->renderer->add_pass(graph, target, format, ImGui::GetDrawData());
}

} // namespace de::tools
