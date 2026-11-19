#include "render/shader/shader_compiler.h"

#include "core/allocator.h"
#include "core/hash.h"
#include "core/inline_array.h"
#include "core/log.h"
#include "core/profile.h"
#include "platform/time.h"
#include "platform/vfs.h"
#include "render/shader/shader_cache.h"

#include <slang-com-ptr.h>
#include <slang.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <string_view>
#include <utility>

namespace de::shader {

namespace {

core::Allocator& render_heap() {
    return core::heap_allocator(core::MemoryTag::Render);
}

bool same_uuid(const SlangUUID& a, const SlangUUID& b) {
    return std::memcmp(&a, &b, sizeof(SlangUUID)) == 0;
}

// A file's bytes, handed to Slang. Reference counted the COM way; freed through the render heap.
class VfsBlob final : public ISlangBlob {
public:
    static VfsBlob* create(size_t size) {
        auto* blob = ::new (render_heap().allocate(sizeof(VfsBlob), alignof(VfsBlob))) VfsBlob();
        blob->bytes_.resize(size);
        return blob;
    }

    std::span<std::byte> bytes() { return bytes_.span(); }

    SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(const SlangUUID& uuid, void** out_object) override {
        if (same_uuid(uuid, ISlangUnknown::getTypeGuid()) || same_uuid(uuid, ISlangBlob::getTypeGuid())) {
            addRef();
            *out_object = static_cast<ISlangBlob*>(this);
            return SLANG_OK;
        }
        *out_object = nullptr;
        return SLANG_E_NO_INTERFACE;
    }
    SLANG_NO_THROW uint32_t SLANG_MCALL addRef() override { return refs_.fetch_add(1) + 1; }
    SLANG_NO_THROW uint32_t SLANG_MCALL release() override {
        const uint32_t refs = refs_.fetch_sub(1) - 1;
        if (refs == 0) {
            this->~VfsBlob();
            render_heap().deallocate(this, sizeof(VfsBlob), alignof(VfsBlob));
        }
        return refs;
    }
    SLANG_NO_THROW const void* SLANG_MCALL getBufferPointer() override { return bytes_.data(); }
    SLANG_NO_THROW size_t SLANG_MCALL getBufferSize() override { return bytes_.size(); }

private:
    VfsBlob() = default;
    ~VfsBlob() = default;

    std::atomic<uint32_t> refs_{1};
    core::Array<std::byte> bytes_{render_heap()};
};

// "shaders/x.slang", "./shaders/x.slang", "shaders\x.slang" or "shaders/tests/../x.slang" ->
// "engine:/shaders/x.slang". Absolute paths and paths that climb out of engine:/ are refused.
bool to_vfs_path(const char* path, platform::PathString& out) {
    const std::string_view text(path != nullptr ? path : "");
    if (text.empty() || text.find(':') != std::string_view::npos || text.front() == '/' || text.front() == '\\') {
        return false;
    }
    constexpr std::string_view kPrefix = "engine:/";
    out.assign(kPrefix);
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find_first_of("/\\", start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view segment = text.substr(start, end - start);
        start = end + 1;
        if (segment.empty() || segment == ".") {
            continue;
        }
        if (segment == "..") {
            if (out.size() == kPrefix.size()) {
                return false; // above engine:/
            }
            const size_t parent = out.view().find_last_of('/');
            out.truncate(parent + 1 > kPrefix.size() ? parent : kPrefix.size());
            continue;
        }
        if ((out.size() > kPrefix.size() && !out.append('/')) || !out.append(segment)) {
            return false;
        }
    }
    return out.size() > kPrefix.size();
}

// Slang's view of engine:/: it asks for "shaders/common/color.slang", we read engine:/shaders/common/color.slang
// through the VFS and note the file as a dependency of the compile in progress.
// Owned by the compiler and outliving every session, so its reference count never frees it.
class VfsFileSystem final : public ISlangFileSystem {
public:
    explicit VfsFileSystem(platform::Vfs& vfs) : vfs_(&vfs) {}

    // The compile in progress records the files it read here; null between compiles.
    void set_dependencies(core::Array<ShaderDependency>* dependencies) { dependencies_ = dependencies; }

    SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(const SlangUUID& uuid, void** out_object) override {
        if (same_uuid(uuid, ISlangUnknown::getTypeGuid()) || same_uuid(uuid, ISlangCastable::getTypeGuid()) ||
            same_uuid(uuid, ISlangFileSystem::getTypeGuid())) {
            addRef();
            *out_object = static_cast<ISlangFileSystem*>(this);
            return SLANG_OK;
        }
        *out_object = nullptr;
        return SLANG_E_NO_INTERFACE;
    }
    SLANG_NO_THROW uint32_t SLANG_MCALL addRef() override { return refs_.fetch_add(1) + 1; }
    SLANG_NO_THROW uint32_t SLANG_MCALL release() override { return refs_.fetch_sub(1) - 1; }
    SLANG_NO_THROW void* SLANG_MCALL castAs(const SlangUUID& uuid) override {
        if (same_uuid(uuid, ISlangUnknown::getTypeGuid()) || same_uuid(uuid, ISlangCastable::getTypeGuid()) ||
            same_uuid(uuid, ISlangFileSystem::getTypeGuid())) {
            return static_cast<ISlangFileSystem*>(this);
        }
        return nullptr;
    }

    SLANG_NO_THROW SlangResult SLANG_MCALL loadFile(const char* path, ISlangBlob** out_blob) override {
        *out_blob = nullptr;
        platform::PathString vfs_path;
        if (!to_vfs_path(path, vfs_path)) {
            return SLANG_E_NOT_FOUND;
        }
        // Slang probes several locations per import: a miss is normal, not an error.
        const core::Result<platform::FileInfo> info = vfs_->file_info(vfs_path.c_str());
        if (!info || info->is_directory) {
            return SLANG_E_NOT_FOUND;
        }
        VfsBlob* blob = VfsBlob::create(static_cast<size_t>(info->size));
        const core::Result<size_t> read = vfs_->read_file(vfs_path.c_str(), blob->bytes());
        const bool ok = read.ok() && *read == info->size;
        // A file that exists but couldn't be read (locked, mid-write) is recorded with a zero info,
        // so the next check sees it as changed and the compile is tried again.
        record(vfs_path, ok ? *info : platform::FileInfo{},
               ok ? core::hash_bytes(blob->bytes().data(), blob->bytes().size()) : 0);
        if (!ok) {
            blob->release();
            return SLANG_FAIL;
        }
        *out_blob = blob;
        return SLANG_OK;
    }

private:
    void record(const platform::PathString& path, const platform::FileInfo& info, uint64_t content_hash) {
        if (dependencies_ == nullptr) {
            return;
        }
        for (ShaderDependency& dependency : *dependencies_) {
            if (dependency.path == path.view()) {
                dependency.info = info;
                dependency.content_hash = content_hash;
                return;
            }
        }
        dependencies_->push(ShaderDependency{.path = path, .info = info, .content_hash = content_hash});
    }

    platform::Vfs* vfs_;
    std::atomic<uint32_t> refs_{1};
    core::Array<ShaderDependency>* dependencies_ = nullptr;
};

SlangStage to_slang(rhi::ShaderStage stage) {
    switch (stage) {
    case rhi::ShaderStage::Vertex:
        return SLANG_STAGE_VERTEX;
    case rhi::ShaderStage::Fragment:
        return SLANG_STAGE_FRAGMENT;
    case rhi::ShaderStage::Compute:
        return SLANG_STAGE_COMPUTE;
    case rhi::ShaderStage::Count:
        break;
    }
    return SLANG_STAGE_NONE;
}

// Collects Slang's diagnostics: logged as they come, and kept for the caller.
class Diagnostics {
public:
    explicit Diagnostics(core::Array<char>* out) : out_(out) {}

    void add(ISlangBlob* blob, bool failed) {
        if (blob == nullptr || blob->getBufferSize() == 0) {
            return;
        }
        const std::string_view whole(static_cast<const char*>(blob->getBufferPointer()), blob->getBufferSize());
        constexpr char kNul = 0;
        add_text(whole.substr(0, std::min(whole.find(kNul), whole.size())), failed);
    }

    // Whether Slang said anything, warnings included.
    bool any() const { return any_; }

    void add_text(std::string_view text, bool failed) {
        any_ = true;
        if (failed) {
            DE_LOG_ERROR("shader", "%.*s", static_cast<int>(text.size()), text.data());
        } else {
            DE_LOG_WARN("shader", "%.*s", static_cast<int>(text.size()), text.data());
        }
        if (out_ == nullptr) {
            return;
        }
        if (!out_->empty()) {
            out_->pop(); // the terminator
        }
        for (const char c : text) {
            out_->push(c);
        }
        if (!text.empty() && text.back() != '\n') {
            out_->push('\n');
        }
        out_->push('\0');
    }

private:
    core::Array<char>* out_;
    bool any_ = false;
};

uint32_t to_u32(size_t value) {
    return static_cast<uint32_t>(value);
}

// The program's global parameters (push constants, descriptor slots) and one entry point's own.
ShaderReflection reflect(slang::ProgramLayout* layout, uint32_t entry_index, rhi::ShaderStage stage) {
    ShaderReflection reflection{.stage = stage,
                                .push_constants = core::Array<PushConstantField>(render_heap()),
                                .bindings = core::Array<ResourceBinding>(render_heap())};
    for (unsigned i = 0; i < layout->getParameterCount(); ++i) {
        slang::VariableLayoutReflection* parameter = layout->getParameterByIndex(i);
        const char* name = parameter->getName() != nullptr ? parameter->getName() : "";
        if (parameter->getCategory() == slang::ParameterCategory::PushConstantBuffer) {
            slang::TypeLayoutReflection* block = parameter->getTypeLayout()->getElementTypeLayout();
            reflection.push_constant_bytes = to_u32(block->getSize());
            for (unsigned f = 0; f < block->getFieldCount(); ++f) {
                slang::VariableLayoutReflection* field = block->getFieldByIndex(f);
                reflection.push_constants.push(PushConstantField{
                    .name = ShaderName(field->getName() != nullptr ? field->getName() : ""),
                    .offset = to_u32(field->getOffset()),
                    .size = to_u32(field->getTypeLayout()->getSize()),
                });
            }
        } else {
            reflection.bindings.push(ResourceBinding{
                .name = ShaderName(name),
                .set = parameter->getBindingSpace(),
                .binding = parameter->getBindingIndex(),
            });
        }
    }
    slang::EntryPointReflection* entry = layout->getEntryPointByIndex(entry_index);
    if (entry != nullptr && stage == rhi::ShaderStage::Compute) {
        SlangUInt sizes[3] = {};
        entry->getComputeThreadGroupSize(3, sizes);
        for (int axis = 0; axis < 3; ++axis) {
            reflection.thread_group_size[axis] = static_cast<uint32_t>(sizes[axis]);
        }
    }
    return reflection;
}

bool fits(const char* text) {
    return text != nullptr && std::strlen(text) <= kMaxShaderNameLength;
}

bool from_slang(SlangStage stage, rhi::ShaderStage& out) {
    switch (stage) {
    case SLANG_STAGE_VERTEX:
        out = rhi::ShaderStage::Vertex;
        return true;
    case SLANG_STAGE_FRAGMENT:
        out = rhi::ShaderStage::Fragment;
        return true;
    case SLANG_STAGE_COMPUTE:
        out = rhi::ShaderStage::Compute;
        return true;
    default:
        return false;
    }
}

// Every entry point the module marks with [shader("...")]: what the cooker compiles.
template <typename EntryPoints, typename Names, typename Entries>
bool find_module_entries(slang::IModule* module, EntryPoints& entry_points, Names& names, Entries& entries,
                         Diagnostics& messages) {
    const SlangInt32 count = module->getDefinedEntryPointCount();
    if (count <= 0 || std::cmp_greater(count, kMaxModuleEntries)) {
        messages.add_text(count <= 0 ? "the module has no [shader(\"...\")] entry points"
                                     : "the module has more entry points than a compile takes (32)",
                          true);
        return false;
    }
    for (SlangInt32 i = 0; i < count; ++i) {
        Slang::ComPtr<slang::IEntryPoint>& entry_point = entry_points.emplace();
        if (SLANG_FAILED(module->getDefinedEntryPoint(i, entry_point.writeRef())) || entry_point == nullptr) {
            messages.add_text("an entry point of the module couldn't be read", true);
            return false;
        }
        slang::FunctionReflection* function = entry_point->getFunctionReflection();
        const char* name = function != nullptr ? function->getName() : nullptr;
        Slang::ComPtr<ISlangBlob> diagnostic;
        slang::ProgramLayout* layout = entry_point->getLayout(0, diagnostic.writeRef());
        messages.add(diagnostic, layout == nullptr);
        slang::EntryPointReflection* reflection = layout != nullptr ? layout->getEntryPointByIndex(0) : nullptr;
        rhi::ShaderStage stage = rhi::ShaderStage::Vertex;
        if (name == nullptr || !fits(name) || reflection == nullptr || !from_slang(reflection->getStage(), stage)) {
            messages.add_text("an entry point has no name, a name over 63 characters, or a stage other than vertex, "
                              "fragment or compute",
                              true);
            return false;
        }
        names.push(ShaderName(name));
        entries.push(ShaderEntry{.name = nullptr, .stage = stage});
    }
    return true;
}

// One of Slang's global sessions and what goes with it. A global session is single-threaded, so one compile has it
// at a time; several of them run side by side, each on its own thread for as long as its compile lasts.
struct SlangSession {
    explicit SlangSession(platform::Vfs& vfs) : file_system(vfs) {}

    Slang::ComPtr<slang::IGlobalSession> global;
    SlangProfileID profile = SLANG_PROFILE_UNKNOWN;
    VfsFileSystem file_system; // records what its compile in progress reads
    bool busy = false;         // a compile has it; under CompilerState::sessions_mutex
};

} // namespace

struct CompilerState {
    explicit CompilerState(platform::Vfs& vfs_in) : vfs(&vfs_in) {}

    ShaderCompilerDesc desc;
    platform::Vfs* vfs;
    // The sessions compiles take turns on: the first made with the compiler, the rest as compiles first overlap,
    // up to session_limit. All under sessions_mutex; a compile that finds none free and no room for another waits
    // on session_freed.
    std::mutex sessions_mutex;
    std::condition_variable session_freed;
    core::Array<SlangSession*> sessions{render_heap()};
    uint32_t session_limit = 1;
    uint32_t sessions_being_made = 0;
    const char* compiler_tag = ""; // Slang's build tag: the first session's own text, there as long as it is
    // The cache (shader_cache.h): its files are read and written through the VFS, from any thread.
    platform::PathString cache_folder;           // empty: none
    std::atomic<bool> cache_write_failed{false}; // warned once
    std::atomic<uint32_t> compiled{0};
    std::atomic<uint32_t> from_cache{0};
};

namespace {

// A source's content hash now, and its size and write time. False if it isn't there or can't be read.
// Read and hashed every time, never taken from its size and write time: a file written twice within the file
// clock's step has the same ones for different contents, and a stale program for a fresh source is a shader that
// doesn't match the C++ it shares its layouts with. The sources are a few hundred kilobytes, already in the OS's
// cache.
bool source_now(const CompilerState& state, const platform::PathString& path, platform::FileInfo& info,
                uint64_t& hash) {
    const core::Result<platform::FileInfo> now = state.vfs->file_info(path.c_str());
    if (!now || now->is_directory) {
        return false;
    }
    info = *now;
    const core::Result<core::Array<std::byte>> bytes = state.vfs->read_whole_file(path.c_str(), render_heap());
    if (!bytes) {
        return false;
    }
    hash = core::hash_bytes(bytes->data(), bytes->size());
    return true;
}

// A request's key and file in the cache. False without a cache folder.
bool cache_entry(const CompilerState& state, const CompileRequest& request, ShaderCacheKey& key,
                 platform::PathString& file) {
    if (state.cache_folder.empty()) {
        return false;
    }
    key = shader_cache_key(request, state.desc, state.compiler_tag);
    constexpr char kDigits[] = "0123456789abcdef";
    char name[17] = {};
    for (int i = 0; i < 16; ++i) {
        name[i] = kDigits[(key.file >> (60 - 4 * i)) & 15U];
    }
    file = state.cache_folder;
    return file.append('/') && file.append(std::string_view(name, 16)) && file.append(".shadercache");
}

// A new global session, ready to compile; null if Slang can't make one (logged by the caller that needs it).
SlangSession* make_session(CompilerState& state) {
    auto* session =
        ::new (render_heap().allocate(sizeof(SlangSession), alignof(SlangSession))) SlangSession(*state.vfs);
    if (SLANG_SUCCEEDED(slang::createGlobalSession(session->global.writeRef()))) {
        // Vulkan 1.3 accepts SPIR-V 1.6.
        session->profile = session->global->findProfile("spirv_1_6");
        if (session->profile != SLANG_PROFILE_UNKNOWN) {
            return session;
        }
    }
    session->~SlangSession();
    render_heap().deallocate(session, sizeof(SlangSession), alignof(SlangSession));
    return nullptr;
}

// A session for one compile, its own until it's given back. Waits if they're all taken and there are as many as
// may run at once.
SlangSession* take_session(CompilerState& state) {
    std::unique_lock lock(state.sessions_mutex);
    for (;;) {
        for (SlangSession* session : state.sessions) {
            if (!session->busy) {
                session->busy = true;
                return session;
            }
        }
        if (state.sessions.size() + state.sessions_being_made < state.session_limit) {
            // Another one, made outside the lock: it takes a fraction of a second, and the others go on meanwhile.
            ++state.sessions_being_made;
            lock.unlock();
            SlangSession* made = make_session(state);
            lock.lock();
            --state.sessions_being_made;
            if (made != nullptr) {
                made->busy = true;
                state.sessions.push(made);
                return made;
            }
            // Slang wouldn't make another: as many at once as there are, from now on.
            state.session_limit = static_cast<uint32_t>(state.sessions.size() + state.sessions_being_made);
            DE_LOG_WARN("shader", "Slang couldn't start another session: %u compiles at once from now on",
                        state.session_limit);
            continue;
        }
        state.session_freed.wait(lock);
    }
}

void give_session(CompilerState& state, SlangSession* session) {
    {
        const std::scoped_lock lock(state.sessions_mutex);
        session->busy = false;
    }
    state.session_freed.notify_one();
}

core::Result<void> check_request(const CompileRequest& request) {
    if (!fits(request.module) || request.entries.size() > kMaxShaderEntries ||
        request.defines.size() > kMaxShaderDefines) {
        return core::fail(
            core::ErrorCode::InvalidArgument,
            "a compile needs a module name of at most 63 characters, at most 4 entry points and at most 16 defines");
    }
    for (const ShaderEntry& entry : request.entries) {
        if (entry.name == nullptr || entry.name[0] == '\0' || !fits(entry.name)) {
            return core::fail(core::ErrorCode::InvalidArgument, "entry point names are 1 to 63 characters");
        }
    }
    for (const ShaderDefine& define : request.defines) {
        if (define.name == nullptr || define.name[0] == '\0') {
            return core::fail(core::ErrorCode::InvalidArgument, "a define needs a name");
        }
    }
    return {};
}

// The program from the cache, if it's there and every source it was compiled from is as it was then; dependencies
// then holds those sources as a compile would have recorded them.
bool load_cached(const CompilerState& state, const platform::PathString& path, const ShaderCacheKey& key,
                 CompiledProgram& program, core::Array<ShaderDependency>& dependencies) {
    const core::Result<core::Array<std::byte>> bytes = state.vfs->read_whole_file(path.c_str(), render_heap());
    if (!bytes) {
        return false; // not compiled before: the usual miss
    }
    core::Array<CachedSource> sources(render_heap());
    if (!decode_cached_program(bytes->span(), key, render_heap(), program, sources)) {
        return false; // damaged, or from another version: compiled over
    }
    core::Array<ShaderDependency> read(render_heap());
    read.reserve(sources.size());
    for (const CachedSource& source : sources) {
        ShaderDependency& dependency = read.emplace(ShaderDependency{.path = source.path});
        if (!source_now(state, source.path, dependency.info, dependency.content_hash) ||
            dependency.content_hash != source.content_hash) {
            return false; // a source changed
        }
    }
    for (const ShaderDependency& dependency : read) {
        dependencies.push(dependency);
    }
    return true;
}

void store_cached(CompilerState& state, const platform::PathString& path, const ShaderCacheKey& key,
                  const CompiledProgram& program, std::span<const ShaderDependency> dependencies) {
    for (const ShaderDependency& dependency : dependencies) {
        if (dependency.info.size == 0 && dependency.info.modified_time == 0) {
            return; // a source that couldn't be read: nothing to know it by
        }
    }
    const core::Array<std::byte> bytes = encode_cached_program(key, program, dependencies, render_heap());
    const core::Result<void> written = state.vfs->write_file(path.c_str(), bytes.span());
    if (!written && !state.cache_write_failed.exchange(true, std::memory_order_relaxed)) {
        DE_LOG_WARN("shader", "compiled shaders can't be kept in %s (%s): they'll be compiled again next time",
                    state.cache_folder.c_str(), written.error().message);
    }
}

} // namespace

core::Result<ShaderCompiler> ShaderCompiler::create(platform::Vfs& vfs, const ShaderCompilerDesc& desc) {
    DE_PROFILE_ZONE("shader compiler create");
    const uint64_t start_ns = platform::time_ns();
    auto* state = ::new (render_heap().allocate(sizeof(CompilerState), alignof(CompilerState))) CompilerState(vfs);
    state->desc = desc;
    state->desc.cache_folder = nullptr; // kept as its own copy: the caller's text needn't outlive this call
    ShaderCompiler compiler(state);     // frees the state on every early return below
    if (desc.cache_folder != nullptr && !state->cache_folder.assign(desc.cache_folder)) {
        return core::fail(core::ErrorCode::InvalidArgument, "the shader cache folder's path is too long");
    }
    state->session_limit = std::clamp(desc.parallel_compiles, 1U, kMaxParallelCompiles);
    state->desc.parallel_compiles = state->session_limit;
    // The first session now: it says whether Slang runs at all, and which Slang it is.
    SlangSession* first = make_session(*state);
    if (first == nullptr) {
        DE_LOG_ERROR("shader", "Slang could not start: its runtime library or standard modules are missing, or it "
                               "has no SPIR-V 1.6 profile");
        return core::fail(core::ErrorCode::Unsupported, "the Slang compiler could not start");
    }
    state->sessions.push(first);
    state->compiler_tag = first->global->getBuildTagString();
    DE_LOG_INFO("shader", "Slang %s ready in %.0f ms", state->compiler_tag,
                static_cast<double>(platform::time_ns() - start_ns) / 1e6);
    return compiler;
}

ShaderCompiler::~ShaderCompiler() {
    if (state_ != nullptr) {
        for (SlangSession* session : state_->sessions) {
            session->~SlangSession();
            render_heap().deallocate(session, sizeof(SlangSession), alignof(SlangSession));
        }
        state_->~CompilerState();
        render_heap().deallocate(state_, sizeof(CompilerState), alignof(CompilerState));
    }
}

ShaderCompiler::ShaderCompiler(ShaderCompiler&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}

ShaderCompiler& ShaderCompiler::operator=(ShaderCompiler&& other) noexcept {
    if (this != &other) {
        const ShaderCompiler discarded(std::exchange(state_, std::exchange(other.state_, nullptr)));
    }
    return *this;
}

core::Result<CompiledProgram> ShaderCompiler::read_cached(const CompileRequest& request, CompileReport* report) {
    DE_PROFILE_ZONE("shader read cached");
    if (const core::Result<void> valid = check_request(request); !valid) {
        return core::fail(valid.error());
    }
    CompilerState& state = *state_;
    const uint64_t start_ns = platform::time_ns();
    ShaderCacheKey cache_key;
    platform::PathString cache_file;
    core::Array<ShaderDependency> unreported(render_heap());
    CompiledProgram program;
    if (!cache_entry(state, request, cache_key, cache_file) ||
        !load_cached(state, cache_file, cache_key, program, report != nullptr ? report->dependencies : unreported)) {
        return core::fail(core::ErrorCode::NotFound, "the program isn't in the shader cache as its sources are now");
    }
    state.from_cache.fetch_add(1, std::memory_order_relaxed);
    DE_LOG_DEBUG("shader", "%s (%zu entry points) from the cache in %.1f ms", request.module, program.stages.size(),
                 static_cast<double>(platform::time_ns() - start_ns) / 1e6);
    return program;
}

core::Result<CompiledProgram> ShaderCompiler::compile(const CompileRequest& request, CompileReport* report) {
    DE_PROFILE_ZONE("shader compile");
    if (const core::Result<void> valid = check_request(request); !valid) {
        return core::fail(valid.error());
    }

    CompilerState& state = *state_;
    uint64_t start_ns = platform::time_ns();
    // What the compile reads: the report's, or with a cache and no report, the cache's own.
    core::Array<ShaderDependency> unreported(render_heap());
    ShaderCacheKey cache_key;
    platform::PathString cache_file;
    // Whether this compile's result goes into the cache.
    const bool keep = cache_entry(state, request, cache_key, cache_file);
    core::Array<ShaderDependency>* dependencies =
        report != nullptr ? &report->dependencies : (keep ? &unreported : nullptr);
    if (keep) {
        // Before Slang's lock: a program that's kept doesn't wait behind one that's compiling.
        CompiledProgram program;
        if (load_cached(state, cache_file, cache_key, program, *dependencies)) {
            state.from_cache.fetch_add(1, std::memory_order_relaxed);
            DE_LOG_DEBUG("shader", "%s (%zu entry points) from the cache in %.1f ms", request.module,
                         program.stages.size(), static_cast<double>(platform::time_ns() - start_ns) / 1e6);
            return program;
        }
    }
    // A session of its own until it's done: given back, with nothing left recording into this compile's
    // dependencies, on every way out.
    struct Turn {
        CompilerState& state;
        SlangSession* session;
        ~Turn() {
            session->file_system.set_dependencies(nullptr);
            give_session(state, session);
        }
    } const turn{.state = state, .session = take_session(state)};
    SlangSession& slang_session = *turn.session;
    start_ns = platform::time_ns(); // the compile's own time, not the wait for its turn
    state.compiled.fetch_add(1, std::memory_order_relaxed);
    Diagnostics messages(report != nullptr ? &report->diagnostics : nullptr);
    slang_session.file_system.set_dependencies(dependencies);

    // A fresh session per compile: sessions cache the modules they load, and a reload must see the edits.
    core::InlineArray<slang::CompilerOptionEntry, 4> options(render_heap());
    // 41012: Slang adds the SPIR-V capabilities texture sampling needs, which every Vulkan 1.3 GPU
    // has. Expected, not news.
    options.push({.name = slang::CompilerOptionName::DisableWarnings,
                  .value = {.kind = slang::CompilerOptionValueKind::String, .stringValue0 = "41012"}});
    const slang::TargetDesc target{
        .format = SLANG_SPIRV,
        .profile = slang_session.profile,
    };
    options.push(
        {.name = slang::CompilerOptionName::Optimization,
         .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = SLANG_OPTIMIZATION_LEVEL_DEFAULT}});
    if (state.desc.debug_info) {
        options.push(
            {.name = slang::CompilerOptionName::DebugInformation,
             .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = SLANG_DEBUG_INFO_LEVEL_STANDARD}});
    }
    core::InlineArray<slang::PreprocessorMacroDesc, kMaxShaderDefines + 1> macros(render_heap());
    macros.push({.name = "DE_SHADER_CHECKS", .value = state.desc.checks ? "1" : "0"});
    for (const ShaderDefine& define : request.defines) {
        macros.push({.name = define.name, .value = define.value != nullptr ? define.value : "1"});
    }
    const char* const search_paths[] = {"shaders"};
    slang::SessionDesc session_desc;
    session_desc.targets = &target;
    session_desc.targetCount = 1;
    session_desc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
    session_desc.searchPaths = search_paths;
    session_desc.searchPathCount = 1;
    session_desc.preprocessorMacros = macros.data();
    session_desc.preprocessorMacroCount = static_cast<SlangInt>(macros.size());
    session_desc.fileSystem = &slang_session.file_system;
    session_desc.compilerOptionEntries = options.data();
    session_desc.compilerOptionEntryCount = static_cast<uint32_t>(options.size());

    Slang::ComPtr<slang::ISession> session;
    if (SLANG_FAILED(slang_session.global->createSession(session_desc, session.writeRef()))) {
        return core::fail(core::ErrorCode::Unknown, "a Slang session could not be created");
    }

    // import maps "common.color" to common/color.slang; loadModule takes a path-like name instead.
    ShaderName module_path(request.module);
    for (size_t i = 0; i < module_path.size(); ++i) {
        if (module_path[i] == '.') {
            module_path[i] = '/';
        }
    }
    Slang::ComPtr<ISlangBlob> diagnostic;
    const Slang::ComPtr<slang::IModule> module(session->loadModule(module_path.c_str(), diagnostic.writeRef()));
    messages.add(diagnostic, module == nullptr);
    if (module == nullptr) {
        if (diagnostic == nullptr) {
            messages.add_text("shader module not found under engine:/shaders/", true);
        }
        return core::fail(core::ErrorCode::NotFound, "the shader module failed to load or compile");
    }

    // The module and every entry point, linked into one program: the front end runs once.
    core::InlineArray<Slang::ComPtr<slang::IEntryPoint>, kMaxShaderEntries> entry_points(render_heap());
    core::InlineArray<ShaderEntry, kMaxShaderEntries> entries(render_heap());
    core::InlineArray<ShaderName, kMaxShaderEntries> entry_names(render_heap());
    for (const ShaderEntry& entry : request.entries) {
        Slang::ComPtr<slang::IEntryPoint>& entry_point = entry_points.emplace();
        diagnostic = nullptr;
        const SlangResult found = module->findAndCheckEntryPoint(entry.name, to_slang(entry.stage),
                                                                 entry_point.writeRef(), diagnostic.writeRef());
        messages.add(diagnostic, SLANG_FAILED(found));
        if (SLANG_FAILED(found) || entry_point == nullptr) {
            if (diagnostic == nullptr) {
                messages.add_text("entry point not found in the module", true);
            }
            return core::fail(core::ErrorCode::NotFound, "a shader entry point is missing or invalid");
        }
        entries.push(entry);
        entry_names.push(ShaderName(entry.name));
    }
    if (request.entries.empty() && !find_module_entries(module, entry_points, entry_names, entries, messages)) {
        return core::fail(core::ErrorCode::InvalidArgument, "the module's entry points can't be compiled");
    }
    core::InlineArray<slang::IComponentType*, kMaxShaderEntries + 1> components(render_heap());
    components.push(module);
    for (const Slang::ComPtr<slang::IEntryPoint>& entry_point : entry_points) {
        components.push(entry_point);
    }

    Slang::ComPtr<slang::IComponentType> program;
    diagnostic = nullptr;
    SlangResult status = session->createCompositeComponentType(
        components.data(), static_cast<SlangInt>(components.size()), program.writeRef(), diagnostic.writeRef());
    messages.add(diagnostic, SLANG_FAILED(status));
    Slang::ComPtr<slang::IComponentType> linked;
    if (SLANG_SUCCEEDED(status)) {
        diagnostic = nullptr;
        status = program->link(linked.writeRef(), diagnostic.writeRef());
        messages.add(diagnostic, SLANG_FAILED(status));
    }
    slang::ProgramLayout* layout = nullptr;
    if (SLANG_SUCCEEDED(status)) {
        diagnostic = nullptr;
        layout = linked->getLayout(0, diagnostic.writeRef());
        messages.add(diagnostic, layout == nullptr);
    }
    if (SLANG_FAILED(status) || layout == nullptr) {
        return core::fail(core::ErrorCode::InvalidArgument, "the shader failed to link");
    }

    CompiledProgram result{.stages = core::Array<CompiledStage>(render_heap())};
    result.stages.reserve(entries.size());
    for (uint32_t i = 0; i < entries.size(); ++i) {
        Slang::ComPtr<ISlangBlob> code;
        diagnostic = nullptr;
        status = linked->getEntryPointCode(i, 0, code.writeRef(), diagnostic.writeRef());
        messages.add(diagnostic, SLANG_FAILED(status));
        if (SLANG_FAILED(status) || code == nullptr || code->getBufferSize() % 4 != 0) {
            return core::fail(core::ErrorCode::InvalidArgument, "the shader failed to generate code");
        }
        CompiledStage& stage = result.stages.emplace(CompiledStage{
            .entry = entry_names[i],
            .code = core::Array<uint32_t>(render_heap()),
            .reflection = reflect(layout, i, entries[i].stage),
        });
        stage.code.resize_uninit(code->getBufferSize() / 4);
        std::memcpy(stage.code.data(), code->getBufferPointer(), code->getBufferSize());
    }
    DE_LOG_DEBUG("shader", "compiled %s (%zu entry points) in %.1f ms", request.module, entries.size(),
                 static_cast<double>(platform::time_ns() - start_ns) / 1e6);
    // Kept for next time, unless Slang had something to say: a warning read back from the cache would never be
    // seen again.
    if (keep && !messages.any()) {
        store_cached(state, cache_file, cache_key, result, dependencies->span());
    }
    return result;
}

void ShaderCompiler::release_idle_sessions() {
    CompilerState& state = *state_;
    core::InlineArray<SlangSession*, kMaxParallelCompiles> idle(render_heap());
    {
        const std::scoped_lock lock(state.sessions_mutex);
        // From the end, so the first stays where it is: the one whose text compiler_tag is.
        for (size_t i = state.sessions.size(); i > 1; --i) {
            if (!state.sessions[i - 1]->busy) {
                idle.push(state.sessions[i - 1]);
                state.sessions.remove_swap(i - 1);
            }
        }
    }
    for (SlangSession* session : idle) {
        session->~SlangSession();
        render_heap().deallocate(session, sizeof(SlangSession), alignof(SlangSession));
    }
    if (!idle.empty()) {
        DE_LOG_DEBUG("shader", "let go of %zu Slang sessions", idle.size());
    }
}

uint32_t ShaderCompiler::parallel_compiles() const {
    return state_->desc.parallel_compiles;
}

ShaderCompilerStats ShaderCompiler::stats() const {
    return {.compiled = state_->compiled.load(std::memory_order_relaxed),
            .from_cache = state_->from_cache.load(std::memory_order_relaxed)};
}

} // namespace de::shader
