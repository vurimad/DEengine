// The single-header libraries' implementations, built once here with their allocations on the
// Tools heap (docs/design.md, Core library: third-party libraries go through our allocators).

#include "cooker/third_party.h"

#include "core/allocator.h"

#include <meshoptimizer.h>

#include <mutex>

namespace {

void* tools_malloc(size_t size) {
    return de::core::tagged_malloc(de::core::MemoryTag::Tools, size);
}

void* tools_realloc(void* pointer, size_t size) {
    return de::core::tagged_realloc(de::core::MemoryTag::Tools, pointer, size);
}

void tools_free(void* pointer) {
    de::core::tagged_free(de::core::MemoryTag::Tools, pointer);
}

} // namespace

// The libraries' configuration and allocator hooks, named and used as they document.
// NOLINTBEGIN(readability-identifier-naming, bugprone-macro-parentheses)
#define STBI_MALLOC(size) tools_malloc(size)
#define STBI_REALLOC(pointer, size) tools_realloc(pointer, size)
#define STBI_FREE(pointer) tools_free(pointer)
#define STBIR_MALLOC(size, context) ((void)(context), tools_malloc(size))
#define STBIR_FREE(pointer, context) ((void)(context), tools_free(pointer))
#define STBI_NO_STDIO // files come through the VFS
#define STBI_FAILURE_USERMSG
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define CGLTF_IMPLEMENTATION
// NOLINTEND(readability-identifier-naming, bugprone-macro-parentheses)

#include <cgltf.h>
#include <stb_image.h>
#include <stb_image_resize2.h>

namespace de::cooker {

void init_third_party() {
    static std::once_flag once;
    std::call_once(once, [] { meshopt_setAllocator(&tools_malloc, &tools_free); });
}

} // namespace de::cooker
