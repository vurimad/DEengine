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

bool same_uuid(const SlangUUID& a, const SlangUUID& b) {
    return std::memcmp(&a, &b, sizeof(SlangUUID)) == 0;
}

// A file's bytes, handed to Slang. Reference counted the COM way; freed through the render heap.
class VfsBlob final : public ISlangBlob {
public:
    static VfsBlob* create(size_t size) {
        core::Allocator& allocator = core::heap_allocator(core::MemoryTag::Render);
        auto* blob = ::new (allocator.allocate(sizeof(VfsBlob), alignof(VfsBlob))) VfsBlob(allocator);
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
            core::Allocator& allocator = *allocator_;
            this->~VfsBlob();
            allocator.deallocate(this, sizeof(VfsBlob), alignof(VfsBlob));
        }
        return refs;
    }
    SLANG_NO_THROW const void* SLANG_MCALL getBufferPointer() override { return bytes_.data(); }
    SLANG_NO_THROW size_t SLANG_MCALL getBufferSize() override { return bytes_.size(); }

private:
    explicit VfsBlob(core::Allocator& allocator) : allocator_(&allocator), bytes_(allocator) {}
    ~VfsBlob() = default;

    core::Allocator* allocator_;
    std::atomic<uint32_t> refs_{1};
    core::Array<std::byte> bytes_;
};

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
        if (!read || *read != info->size) {
            blob->release();
            return SLANG_FAIL;
        }
        if (dependencies_ != nullptr) {
            bool known = false;
            for (const ShaderDependency& dependency : *dependencies_) {
                known = known || dependency.path == vfs_path.view();
            }
            if (!known) {
                dependencies_->push(ShaderDependency{.path = vfs_path, .info = *info});
            }
        }
        *out_blob = blob;
        return SLANG_OK;
    }

private:
    // "shaders/x.slang", "./shaders/x.slang" or "shaders\x.slang" -> "src:/shaders/x.slang".
    // Anything absolute or outside src:/ is refused.
    static bool to_vfs_path(const char* path, platform::PathString& out) {
        std::string_view text(path != nullptr ? path : "");
        while (text.starts_with("./") || text.starts_with(".\\")) {
            text.remove_prefix(2);
        }
        if (text.empty() || text.find(':') != std::string_view::npos || text.front() == '/' || text.front() == '\\') {
            return false;
        }
        out.assign("src:/");
        for (const char c : text) {
            if (!out.append(c == '\\' ? '/' : c)) {
                return false;
            }
        }
        return true;
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

// Collects Slang's diagnostics: logged as they come, and appended for the caller.
class Diagnostics {
public:
    explicit Diagnostics(core::Array<char>* out) : out_(out) {}

    void add(ISlangBlob* blob, bool failed) {
        if (blob == nullptr || blob->getBufferSize() == 0) {
            return;
        }
        const auto* text = static_cast<const char*>(blob->getBufferPointer());
        constexpr char kNul = 0;
        const std::string_view whole(text, blob->getBufferSize());
        const size_t length = std::min(whole.find(kNul), whole.size());
        if (failed) {
            DE_LOG_ERROR("shader", "%.*s", static_cast<int>(length), text);
        } else {
            DE_LOG_WARN("shader", "%.*s", static_cast<int>(length), text);
        }
        if (out_ != nullptr) {
            if (!out_->empty() && out_->back() == '\0') {
                out_->pop();
            }
            for (size_t i = 0; i < length; ++i) {
                out_->push(text[i]);
            }
            out_->push('\0');
        }
    }

    void add_text(const char* text) {
        DE_LOG_ERROR("shader", "%s", text);
        if (out_ != nullptr) {
            if (!out_->empty() && out_->back() == '\0') {
                out_->pop();
            }
            for (const char* c = text; *c != '\0'; ++c) {
                out_->push(*c);
            }
            out_->push('\n');
            out_->push('\0');
        }
    }

private:
    core::Array<char>* out_;
};

uint32_t to_u32(size_t value) {
    return static_cast<uint32_t>(value);
}

void reflect(slang::ProgramLayout* layout, ShaderReflection& reflection) {
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
    slang::EntryPointReflection* entry = layout->getEntryPointByIndex(0);
    if (entry != nullptr && reflection.stage == rhi::ShaderStage::Compute) {
        SlangUInt sizes[3] = {};
        entry->getComputeThreadGroupSize(3, sizes);
        for (int axis = 0; axis < 3; ++axis) {
            reflection.thread_group_size[axis] = static_cast<uint32_t>(sizes[axis]);
        }
    }
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
    core::Allocator& allocator = core::heap_allocator(core::MemoryTag::Render);
    auto* state = ::new (allocator.allocate(sizeof(CompilerState), alignof(CompilerState))) CompilerState(vfs);
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
        core::Allocator& allocator = core::heap_allocator(core::MemoryTag::Render);
        state_->~CompilerState();
        allocator.deallocate(state_, sizeof(CompilerState), alignof(CompilerState));
    }
}

ShaderCompiler::ShaderCompiler(ShaderCompiler&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}

ShaderCompiler& ShaderCompiler::operator=(ShaderCompiler&& other) noexcept {
    if (this != &other) {
        const ShaderCompiler discarded(std::exchange(state_, std::exchange(other.state_, nullptr)));
    }
    return *this;
}

core::Result<CompiledShader> ShaderCompiler::compile(const CompileRequest& request, core::Array<char>* diagnostics) {
    DE_PROFILE_ZONE("shader compile");
    if (request.module == nullptr || request.entry == nullptr || request.defines.size() > kMaxShaderDefines) {
        return core::fail(core::ErrorCode::InvalidArgument,
                          "a compile needs a module, an entry point and at most 16 defines");
    }
    CompilerState& state = *state_;
    const std::scoped_lock lock(state.mutex);
    const uint64_t start_ns = platform::time_ns();
    Diagnostics messages(diagnostics);

    CompiledShader result{
        .code = core::Array<uint32_t>(core::heap_allocator(core::MemoryTag::Render)),
        .reflection = {.stage = request.stage,
                       .push_constants = core::Array<PushConstantField>(core::heap_allocator(core::MemoryTag::Render)),
                       .bindings = core::Array<ResourceBinding>(core::heap_allocator(core::MemoryTag::Render))},
        .dependencies = core::Array<ShaderDependency>(core::heap_allocator(core::MemoryTag::Render)),
    };
    state.file_system.set_dependencies(&result.dependencies);
    struct ClearDependencies {
        VfsFileSystem& file_system;
        ~ClearDependencies() { file_system.set_dependencies(nullptr); }
    } const clear_dependencies{state.file_system};

    // A fresh session per compile: sessions cache the modules they load, and a reload must see the edits.
    const slang::TargetDesc target{
        .format = SLANG_SPIRV,
        .profile = state.profile,
    };
    core::InlineArray<slang::CompilerOptionEntry, 4> options(core::heap_allocator(core::MemoryTag::Render));
    options.push(
        {.name = slang::CompilerOptionName::Optimization,
         .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = SLANG_OPTIMIZATION_LEVEL_DEFAULT}});
    if (state.desc.debug_info) {
        options.push(
            {.name = slang::CompilerOptionName::DebugInformation,
             .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = SLANG_DEBUG_INFO_LEVEL_STANDARD}});
    }
    core::InlineArray<slang::PreprocessorMacroDesc, kMaxShaderDefines> macros(
        core::heap_allocator(core::MemoryTag::Render));
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
            messages.add_text("shader module not found under src:/shaders/");
        }
        return core::fail(core::ErrorCode::NotFound, "the shader module failed to load or compile");
    }

    Slang::ComPtr<slang::IEntryPoint> entry_point;
    diagnostic = nullptr;
    const SlangResult found = module->findAndCheckEntryPoint(request.entry, to_slang(request.stage),
                                                             entry_point.writeRef(), diagnostic.writeRef());
    messages.add(diagnostic, SLANG_FAILED(found));
    if (SLANG_FAILED(found) || entry_point == nullptr) {
        if (diagnostic == nullptr) {
            messages.add_text("entry point not found in the module");
        }
        return core::fail(core::ErrorCode::NotFound, "the shader entry point is missing or invalid");
    }

    slang::IComponentType* const components[] = {module, entry_point};
    Slang::ComPtr<slang::IComponentType> program;
    diagnostic = nullptr;
    SlangResult status =
        session->createCompositeComponentType(components, 2, program.writeRef(), diagnostic.writeRef());
    messages.add(diagnostic, SLANG_FAILED(status));
    Slang::ComPtr<slang::IComponentType> linked;
    if (SLANG_SUCCEEDED(status)) {
        diagnostic = nullptr;
        status = program->link(linked.writeRef(), diagnostic.writeRef());
        messages.add(diagnostic, SLANG_FAILED(status));
    }
    Slang::ComPtr<ISlangBlob> code;
    if (SLANG_SUCCEEDED(status)) {
        diagnostic = nullptr;
        status = linked->getEntryPointCode(0, 0, code.writeRef(), diagnostic.writeRef());
        messages.add(diagnostic, SLANG_FAILED(status));
    }
    if (SLANG_FAILED(status) || code == nullptr || code->getBufferSize() % 4 != 0) {
        return core::fail(core::ErrorCode::InvalidArgument, "the shader failed to link or generate code");
    }

    diagnostic = nullptr;
    slang::ProgramLayout* layout = linked->getLayout(0, diagnostic.writeRef());
    messages.add(diagnostic, layout == nullptr);
    if (layout == nullptr) {
        return core::fail(core::ErrorCode::Unknown, "the shader has no reflection");
    }
    reflect(layout, result.reflection);

    result.code.resize_uninit(code->getBufferSize() / 4);
    std::memcpy(result.code.data(), code->getBufferPointer(), code->getBufferSize());
    DE_LOG_DEBUG("shader", "compiled %s:%s (%s) in %.1f ms", request.module, request.entry,
                 rhi::shader_stage_name(request.stage), static_cast<double>(platform::time_ns() - start_ns) / 1e6);
    return result;
}

} // namespace de::shader
