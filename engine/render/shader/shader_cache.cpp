#include "render/shader/shader_cache.h"

#include "core/hash.h"

#include <cstring>
#include <string_view>
#include <type_traits>

namespace de::shader {

namespace {

constexpr uint32_t kMagic = 0x43534544; // "DESC"
// What a file may hold at most: more is a damaged one, not a program.
constexpr uint32_t kMostSources = 4096;
constexpr uint32_t kMostFields = 1024;
constexpr uint32_t kMostWords = 16U << 20;
constexpr uint64_t kCheckSeed = 0x9e3779b97f4a7c15ULL;

// Hashes a run of values and texts, each text with its length so "ab", "c" isn't "a", "bc".
class Hasher {
public:
    explicit Hasher(uint64_t seed) : hash_(seed) {}

    template <typename T>
    void value(T v) {
        static_assert(std::is_trivially_copyable_v<T>);
        hash_ = core::hash_bytes(&v, sizeof(v), hash_);
    }
    void text(std::string_view t) {
        value(static_cast<uint64_t>(t.size()));
        hash_ = core::hash_bytes(t.data(), t.size(), hash_);
    }
    uint64_t result() const { return hash_; }

private:
    uint64_t hash_;
};

uint64_t key_hash(const CompileRequest& request, const ShaderCompilerDesc& desc, const char* compiler_tag,
                  uint64_t seed) {
    Hasher hasher(seed);
    hasher.value(kShaderCacheVersion);
    hasher.text(compiler_tag != nullptr ? compiler_tag : "");
    hasher.value(static_cast<uint8_t>(desc.debug_info));
    hasher.value(static_cast<uint8_t>(desc.checks));
    hasher.text(request.module != nullptr ? request.module : "");
    // No entries named means every entry point of the module: another program than one that names them all.
    hasher.value(static_cast<uint32_t>(request.entries.size()));
    for (const ShaderEntry& entry : request.entries) {
        hasher.text(entry.name != nullptr ? entry.name : "");
        hasher.value(static_cast<uint32_t>(entry.stage));
    }
    hasher.value(static_cast<uint32_t>(request.defines.size()));
    for (const ShaderDefine& define : request.defines) {
        hasher.text(define.name != nullptr ? define.name : "");
        hasher.text(define.value != nullptr ? define.value : "1");
    }
    return hasher.result();
}

class Writer {
public:
    explicit Writer(core::Array<std::byte>& out) : out_(&out) {}

    void bytes(const void* data, size_t size) {
        const size_t at = out_->size();
        out_->resize_uninit(at + size);
        if (size != 0) {
            std::memcpy(out_->data() + at, data, size);
        }
    }
    template <typename T>
    void value(T v) {
        static_assert(std::is_trivially_copyable_v<T>);
        bytes(&v, sizeof(v));
    }
    void text(std::string_view t) {
        value(static_cast<uint32_t>(t.size()));
        bytes(t.data(), t.size());
    }

private:
    core::Array<std::byte>* out_;
};

// Reads what Writer wrote. Past the end, or a text too long for where it goes, it fails and stays failed: check ok()
// once, at the end.
class Reader {
public:
    explicit Reader(std::span<const std::byte> in) : in_(in) {}

    bool ok() const { return ok_; }
    size_t left() const { return in_.size() - at_; }

    bool bytes(void* out, size_t size) {
        if (!ok_ || size > left()) {
            ok_ = false;
            return false;
        }
        if (size != 0) {
            std::memcpy(out, in_.data() + at_, size);
        }
        at_ += size;
        return true;
    }
    template <typename T>
    T value() {
        static_assert(std::is_trivially_copyable_v<T>);
        T v{};
        bytes(&v, sizeof(v));
        return v;
    }
    // A count of things that each take at least least_bytes of what's left: more than can be there is a failure.
    uint32_t count(uint32_t most, size_t least_bytes) {
        const auto n = value<uint32_t>();
        if (!ok_ || n > most || static_cast<size_t>(n) * least_bytes > left()) {
            ok_ = false;
            return 0;
        }
        return n;
    }
    template <size_t N>
    void text(core::FixedString<N>& out) {
        const auto size = value<uint32_t>();
        if (!ok_ || size > N || size > left()) {
            ok_ = false;
            return;
        }
        out.assign({reinterpret_cast<const char*>(in_.data() + at_), size});
        at_ += size;
    }

private:
    std::span<const std::byte> in_;
    size_t at_ = 0;
    bool ok_ = true;
};

} // namespace

ShaderCacheKey shader_cache_key(const CompileRequest& request, const ShaderCompilerDesc& desc,
                                const char* compiler_tag) {
    return {.file = key_hash(request, desc, compiler_tag, 0),
            .check = key_hash(request, desc, compiler_tag, kCheckSeed)};
}

core::Array<std::byte> encode_cached_program(const ShaderCacheKey& key, const CompiledProgram& program,
                                             std::span<const ShaderDependency> dependencies,
                                             core::Allocator& allocator) {
    core::Array<std::byte> out(allocator);
    Writer writer(out);
    writer.value(kMagic);
    writer.value(kShaderCacheVersion);
    writer.value(key.file);
    writer.value(key.check);
    writer.value(static_cast<uint32_t>(dependencies.size()));
    for (const ShaderDependency& dependency : dependencies) {
        writer.text(dependency.path.view());
        writer.value(dependency.content_hash);
    }
    writer.value(static_cast<uint32_t>(program.stages.size()));
    for (const CompiledStage& stage : program.stages) {
        const ShaderReflection& reflection = stage.reflection;
        writer.text(stage.entry.view());
        writer.value(static_cast<uint32_t>(reflection.stage));
        writer.value(reflection.push_constant_bytes);
        for (const uint32_t size : reflection.thread_group_size) {
            writer.value(size);
        }
        writer.value(static_cast<uint32_t>(reflection.push_constants.size()));
        for (const PushConstantField& field : reflection.push_constants) {
            writer.text(field.name.view());
            writer.value(field.offset);
            writer.value(field.size);
        }
        writer.value(static_cast<uint32_t>(reflection.bindings.size()));
        for (const ResourceBinding& binding : reflection.bindings) {
            writer.text(binding.name.view());
            writer.value(binding.set);
            writer.value(binding.binding);
        }
        writer.value(static_cast<uint32_t>(stage.code.size()));
        writer.bytes(stage.code.data(), stage.code.size() * sizeof(uint32_t));
    }
    // Of everything before it: a file cut short or changed on disk doesn't pass for a program.
    writer.value(core::hash_bytes(out.data(), out.size()));
    return out;
}

bool decode_cached_program(std::span<const std::byte> bytes, const ShaderCacheKey& key, core::Allocator& allocator,
                           CompiledProgram& program, core::Array<CachedSource>& sources) {
    if (bytes.size() < sizeof(uint64_t)) {
        return false;
    }
    const size_t body = bytes.size() - sizeof(uint64_t);
    uint64_t stored = 0;
    std::memcpy(&stored, bytes.data() + body, sizeof(stored));
    if (stored != core::hash_bytes(bytes.data(), body)) {
        return false;
    }
    Reader reader(bytes.first(body));
    if (reader.value<uint32_t>() != kMagic || reader.value<uint32_t>() != kShaderCacheVersion ||
        reader.value<uint64_t>() != key.file || reader.value<uint64_t>() != key.check || !reader.ok()) {
        return false;
    }
    sources = core::Array<CachedSource>(allocator);
    const uint32_t source_count = reader.count(kMostSources, sizeof(uint32_t) + sizeof(uint64_t));
    sources.reserve(source_count);
    for (uint32_t i = 0; i < source_count; ++i) {
        CachedSource& source = sources.emplace();
        reader.text(source.path);
        source.content_hash = reader.value<uint64_t>();
    }
    program = CompiledProgram{.stages = core::Array<CompiledStage>(allocator)};
    const uint32_t stage_count = reader.count(static_cast<uint32_t>(kMaxModuleEntries), 8 * sizeof(uint32_t));
    program.stages.reserve(stage_count);
    for (uint32_t i = 0; i < stage_count && reader.ok(); ++i) {
        CompiledStage& stage = program.stages.emplace(CompiledStage{
            .entry = {},
            .code = core::Array<uint32_t>(allocator),
            .reflection = {.push_constants = core::Array<PushConstantField>(allocator),
                           .bindings = core::Array<ResourceBinding>(allocator)},
        });
        ShaderReflection& reflection = stage.reflection;
        reader.text(stage.entry);
        const auto stage_value = reader.value<uint32_t>();
        if (stage_value >= static_cast<uint32_t>(rhi::ShaderStage::Count)) {
            return false;
        }
        reflection.stage = static_cast<rhi::ShaderStage>(stage_value);
        reflection.push_constant_bytes = reader.value<uint32_t>();
        for (uint32_t& size : reflection.thread_group_size) {
            size = reader.value<uint32_t>();
        }
        const uint32_t field_count = reader.count(kMostFields, 3 * sizeof(uint32_t));
        reflection.push_constants.reserve(field_count);
        for (uint32_t f = 0; f < field_count; ++f) {
            PushConstantField& field = reflection.push_constants.emplace();
            reader.text(field.name);
            field.offset = reader.value<uint32_t>();
            field.size = reader.value<uint32_t>();
        }
        const uint32_t binding_count = reader.count(kMostFields, 3 * sizeof(uint32_t));
        reflection.bindings.reserve(binding_count);
        for (uint32_t b = 0; b < binding_count; ++b) {
            ResourceBinding& binding = reflection.bindings.emplace();
            reader.text(binding.name);
            binding.set = reader.value<uint32_t>();
            binding.binding = reader.value<uint32_t>();
        }
        const uint32_t word_count = reader.count(kMostWords, sizeof(uint32_t));
        stage.code.resize_uninit(word_count);
        reader.bytes(stage.code.data(), static_cast<size_t>(word_count) * sizeof(uint32_t));
    }
    // All of it, and nothing after it.
    return reader.ok() && reader.left() == 0 && !program.stages.empty();
}

} // namespace de::shader
