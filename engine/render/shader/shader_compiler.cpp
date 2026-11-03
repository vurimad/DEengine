#include "render/shader/shader_compiler.h"

#include "core/allocator.h"
#include "core/inline_array.h"
#include "core/log.h"
#include "core/profile.h"
#include "platform/time.h"
#include "platform/vfs.h"

#include <slang-com-ptr.h>
#include <slang.h>

#include <algorithm>
#include <atomic>
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
// "src:/shaders/x.slang". Absolute paths and paths that climb out of src:/ are refused.
bool to_vfs_path(const char* path, platform::PathString& out) {
    const std::string_view text(path != nullptr ? path : "");
    if (text.empty() || text.find(':') != std::string_view::npos || text.front() == '/' || text.front() == '\\') {
        return false;
    }
    constexpr std::string_view kPrefix = "src:/";
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
                return false; // above src:/
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

// Slang's view of src:/: it asks for "shaders/common/color.slang", we read src:/shaders/common/color.slang
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
        record(vfs_path, ok ? *info : platform::FileInfo{});
        if (!ok) {
            blob->release();
            return SLANG_FAIL;
        }
        *out_blob = blob;
        return SLANG_OK;
    }

private:
    void record(const platform::PathString& path, const platform::FileInfo& info) {
        if (dependencies_ == nullptr) {
            return;
        }
        for (ShaderDependency& dependency : *dependencies_) {
            if (dependency.path == path.view()) {
                dependency.info = info;
                return;
            }
        }
        dependencies_->push(ShaderDependency{.path = path, .info = info});
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

    void add_text(std::string_view text, bool failed) {
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

} // namespace

struct CompilerState {
    explicit CompilerState(platform::Vfs& vfs) : file_system(vfs) {}

    ShaderCompilerDesc desc;
    std::mutex mutex; // one compile at a time: Slang's global session is single-threaded
    Slang::ComPtr<slang::IGlobalSession> global_session;
    SlangProfileID profile = SLANG_PROFILE_UNKNOWN;
    VfsFileSystem file_system;
};

core::Result<ShaderCompiler> ShaderCompiler::create(platform::Vfs& vfs, const ShaderCompilerDesc& desc) {
    DE_PROFILE_ZONE("shader compiler create");
    const uint64_t start_ns = platform::time_ns();
    auto* state = ::new (render_heap().allocate(sizeof(CompilerState), alignof(CompilerState))) CompilerState(vfs);
    state->desc = desc;
    ShaderCompiler compiler(state); // frees the state on every early return below
    if (SLANG_FAILED(slang::createGlobalSession(state->global_session.writeRef()))) {
        DE_LOG_ERROR("shader", "Slang could not start: its runtime library or standard modules are missing");
        return core::fail(core::ErrorCode::Unsupported, "the Slang compiler could not start");
    }
    // Vulkan 1.3 accepts SPIR-V 1.6.
    state->profile = state->global_session->findProfile("spirv_1_6");
    if (state->profile == SLANG_PROFILE_UNKNOWN) {
        return core::fail(core::ErrorCode::Unsupported, "this Slang has no SPIR-V 1.6 profile");
    }
    DE_LOG_INFO("shader", "Slang %s ready in %.0f ms", state->global_session->getBuildTagString(),
                static_cast<double>(platform::time_ns() - start_ns) / 1e6);
    return compiler;
}

ShaderCompiler::~ShaderCompiler() {
    if (state_ != nullptr) {
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

core::Result<CompiledProgram> ShaderCompiler::compile(const CompileRequest& request, CompileReport* report) {
    DE_PROFILE_ZONE("shader compile");
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

    CompilerState& state = *state_;
    const std::scoped_lock lock(state.mutex);
    const uint64_t start_ns = platform::time_ns();
    Diagnostics messages(report != nullptr ? &report->diagnostics : nullptr);
    state.file_system.set_dependencies(report != nullptr ? &report->dependencies : nullptr);
    struct ClearDependencies {
        VfsFileSystem& file_system;
        ~ClearDependencies() { file_system.set_dependencies(nullptr); }
    } const clear_dependencies{state.file_system};

    // A fresh session per compile: sessions cache the modules they load, and a reload must see the edits.
    core::InlineArray<slang::CompilerOptionEntry, 4> options(render_heap());
    // 41012: Slang adds the SPIR-V capabilities texture sampling needs, which every Vulkan 1.3 GPU
    // has. Expected, not news.
    options.push({.name = slang::CompilerOptionName::DisableWarnings,
                  .value = {.kind = slang::CompilerOptionValueKind::String, .stringValue0 = "41012"}});
    const slang::TargetDesc target{
        .format = SLANG_SPIRV,
        .profile = state.profile,
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
    session_desc.fileSystem = &state.file_system;
    session_desc.compilerOptionEntries = options.data();
    session_desc.compilerOptionEntryCount = static_cast<uint32_t>(options.size());

    Slang::ComPtr<slang::ISession> session;
    if (SLANG_FAILED(state.global_session->createSession(session_desc, session.writeRef()))) {
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
            messages.add_text("shader module not found under src:/shaders/", true);
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
    return result;
}

} // namespace de::shader
