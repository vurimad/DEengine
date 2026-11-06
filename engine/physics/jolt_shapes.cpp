#include "physics/jolt_shapes.h"

#include "core/allocator.h"
#include "core/assert.h"
#include "core/log.h"

// Jolt.h has to come first: it sets up what the others need.
// clang-format off
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/StreamOut.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/RegisterTypes.h>
// clang-format on

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

namespace de::physics {

namespace {

using core::MemoryTag;

// ---- Jolt's memory, logs and asserts ------------------------------------------------------------------------------

void* allocate_hook(size_t size) {
    return core::tagged_malloc(MemoryTag::Physics, size);
}

void* reallocate_hook(void* block, [[maybe_unused]] size_t old_size, size_t new_size) {
    return core::tagged_realloc(MemoryTag::Physics, block, new_size);
}

void free_hook(void* block) {
    core::tagged_free(MemoryTag::Physics, block);
}

// Over-aligned, with where the block really starts kept just before what's handed out.
void* aligned_allocate_hook(size_t size, size_t alignment) {
    const size_t needed = size + alignment + sizeof(void*);
    auto* const block = static_cast<std::byte*>(core::tagged_malloc(MemoryTag::Physics, needed));
    auto* const aligned = block + sizeof(void*) + core::alignment_padding(block + sizeof(void*), alignment);
    reinterpret_cast<void**>(aligned)[-1] = block;
    return aligned;
}

void aligned_free_hook(void* block) {
    if (block != nullptr) {
        core::tagged_free(MemoryTag::Physics, static_cast<void**>(block)[-1]);
    }
}

// NOLINTNEXTLINE(modernize-avoid-variadic-functions,cert-dcl50-cpp): the shape Jolt's hook has
void trace_hook(const char* format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    DE_LOG_INFO("physics", "%s", line);
}

#ifdef JPH_ENABLE_ASSERTS
bool assert_hook(const char* expression, const char* message, const char* file, JPH::uint line) {
    DE_LOG_ERROR("physics", "Jolt: %s (%s), %s:%u", expression, message != nullptr ? message : "", file, line);
    return true; // stop here
}
#endif

// Jolt's global setup, while anything uses it.
int jolt_users = 0;
alignas(JPH::Factory) std::byte factory_storage[sizeof(JPH::Factory)];

// Jolt, for as long as this lasts.
class JoltUse {
public:
    JoltUse() { acquire_jolt(); }
    ~JoltUse() { release_jolt(); }
    JoltUse(const JoltUse&) = delete;
    JoltUse& operator=(const JoltUse&) = delete;
    JoltUse(JoltUse&&) = delete;
    JoltUse& operator=(JoltUse&&) = delete;
};

// Jolt's own stream goes through iostreams, which the engine doesn't use.
class BytesOut final : public JPH::StreamOut {
public:
    explicit BytesOut(core::Array<std::byte>& out) : out_(out) {}
    void WriteBytes(const void* data, size_t count) override {
        const size_t at = out_.size();
        out_.resize(at + count);
        std::memcpy(out_.data() + at, data, count);
    }
    bool IsFailed() const override { return false; }

private:
    core::Array<std::byte>& out_;
};

} // namespace

void acquire_jolt() {
    if (jolt_users++ > 0) {
        return;
    }
    JPH::Allocate = allocate_hook;
    JPH::Reallocate = reallocate_hook;
    JPH::Free = free_hook;
    JPH::AlignedAllocate = aligned_allocate_hook;
    JPH::AlignedFree = aligned_free_hook;
    JPH::Trace = trace_hook;
#ifdef JPH_ENABLE_ASSERTS
    JPH::AssertFailed = assert_hook;
#endif
    JPH::Factory::sInstance = ::new (static_cast<void*>(factory_storage)) JPH::Factory();
    JPH::RegisterTypes();
}

void release_jolt() {
    if (--jolt_users > 0) {
        return;
    }
    JPH::UnregisterTypes();
    JPH::Factory::sInstance->~Factory();
    JPH::Factory::sInstance = nullptr;
}

uint64_t mesh_shape_format() {
    constexpr uint64_t kMajor = JPH_VERSION_MAJOR;
    constexpr uint64_t kMinor = JPH_VERSION_MINOR;
    constexpr uint64_t kPatch = JPH_VERSION_PATCH;
    constexpr uint64_t kDoubles = JPH_VERSION_FEATURE_BIT_1;
    constexpr uint64_t kDeterministic = JPH_VERSION_FEATURE_BIT_2;
    return (kMajor << 16U) | (kMinor << 8U) | kPatch | (kDoubles << 24U) | (kDeterministic << 25U);
}

core::Result<uint32_t> build_mesh_shape(std::span<const float> positions, std::span<const uint32_t> indices,
                                        core::Array<std::byte>& out) {
    DE_ASSERT(positions.size() % 3 == 0 && indices.size() % 3 == 0);
    const JoltUse jolt;
    JPH::VertexList vertices;
    vertices.reserve(positions.size() / 3);
    for (size_t i = 0; i + 2 < positions.size(); i += 3) {
        vertices.emplace_back(positions[i], positions[i + 1], positions[i + 2]);
    }
    JPH::IndexedTriangleList triangles;
    triangles.reserve(indices.size() / 3);
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        triangles.emplace_back(indices[i], indices[i + 1], indices[i + 2]);
    }
    const JPH::Ref<JPH::MeshShapeSettings> settings =
        new JPH::MeshShapeSettings(std::move(vertices), std::move(triangles));
    if (settings->mIndexedTriangles.empty()) {
        return core::fail(core::ErrorCode::InvalidArgument, "it has no triangles with any area");
    }
    const JPH::ShapeSettings::ShapeResult shape = settings->Create();
    if (shape.HasError()) {
        DE_LOG_ERROR("physics", "a mesh shape: %s", shape.GetError().c_str());
        return core::fail(core::ErrorCode::InvalidArgument, "Jolt won't make a mesh shape of it");
    }
    BytesOut stream(out);
    shape.Get()->SaveBinaryState(stream);
    return static_cast<uint32_t>(settings->mIndexedTriangles.size());
}

} // namespace de::physics
