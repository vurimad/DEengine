#include "tools/debug_ui.h"

#include "core/allocator.h"
#include "core/assert.h"

#include <imgui.h>

#include <cstddef>
#include <cstring>

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

} // namespace de::tools
