find_package(SDL3 CONFIG REQUIRED)
find_package(VulkanHeaders CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(vk-bootstrap CONFIG REQUIRED)
# The vcpkg Vulkan headers only: volk must not pick up an installed Vulkan SDK's include folder.
set(VOLK_PULL_IN_VULKAN OFF)
find_package(volk CONFIG REQUIRED)

add_library(de_rhi STATIC
    command_list.h
    device.h
    rhi.cpp
    rhi.h)
add_library(de::rhi ALIAS de_rhi)

# The graphics API backend, picked here like the per-OS platform files. Only Vulkan exists;
# a D3D12 or console backend would be another set of files implementing the same headers.
target_sources(de_rhi PRIVATE
    vulkan/vk_command_list.cpp
    vulkan/vk_device.cpp
    vulkan/vk_device.h
    vulkan/vk_format.cpp
    vulkan/vk_swapchain.cpp
    vulkan/vk_vma.cpp)

set_source_files_properties(vulkan/vk_vma.cpp PROPERTIES COMPILE_DEFINITIONS
    "VMA_IMPLEMENTATION;VMA_STATIC_VULKAN_FUNCTIONS=0;VMA_DYNAMIC_VULKAN_FUNCTIONS=0")

# Vulkan is loaded at runtime through volk: nothing links the loader library.
target_compile_definitions(de_rhi PRIVATE VK_NO_PROTOTYPES)
# Vulkan's C structs are meant to be filled with partial designated initializers (the rest is zero).
# Per source, so the flag lands after the shared warning flags that would turn the warning back on.
set_source_files_properties(
    vulkan/vk_command_list.cpp vulkan/vk_device.cpp vulkan/vk_format.cpp vulkan/vk_swapchain.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:Clang,GNU>:-Wno-missing-field-initializers>")

target_link_libraries(de_rhi
    PUBLIC de::core de::platform de::compile_options
    PRIVATE
        volk::volk
        Vulkan::Headers
        GPUOpen::VulkanMemoryAllocator
        vk-bootstrap::vk-bootstrap
        SDL3::SDL3)
