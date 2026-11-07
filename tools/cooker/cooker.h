#pragma once

#include "cooker/common.h"

#include <cstdint>

namespace de::core {
class JobSystem;
}
namespace de::shader {
class ShaderCompiler;
}

namespace de::cooker {

struct CookOptions {
    // Check the source and meta trees, the cooked shaders against their sources, and terrain sources
    // and what's cooked from them, and write nothing (no metas, no cooked files): what CI runs.
    bool check_only = false;
    // Cook every source again, even unchanged ones.
    bool force = false;
    // Recorded in .deshader files: the compiler must have been created with the same settings.
    bool shader_debug_info = false;
    bool shader_checks = false;
    // Cook shaders/tests/ too (the test suite's own modules), which games never load.
    bool test_shaders = false;
    // Cook content/. Off: shaders only, and what was cooked from content/ stays as it is.
    bool content = true;
    // With check_only: off skips comparing the cooked shaders with their sources.
    bool check_shaders = true;
    // For tests: runs when the shader modules are cooked, before they're stamped, as a save mid-cook would.
    void (*before_shader_stamp)(void* context) = nullptr;
    void* before_shader_stamp_context = nullptr;
};

struct CookStats {
    uint32_t cooked = 0;     // sources cooked this run
    uint32_t up_to_date = 0; // skipped: nothing they depend on changed
    uint32_t failed = 0;
    uint32_t removed = 0; // cooked files of deleted sources, or that a source no longer makes
    uint32_t metas_created = 0;
    uint32_t metas_relinked = 0;
};

struct CookReport {
    CookStats stats;
    core::Array<Diagnostic> diagnostics{tools_heap()};

    bool ok() const;
};

// The asset cooker (docs/design.md, Asset formats): turns the source tree (src:/content/, with
// the meta tree in src:/meta/, and src:/shaders/) into cooked files in game:/, named by asset ID.
// - Metas: new sources get one, moved ones are re-linked, broken ones reported (SourceTree).
// - Shaders: every module under shaders/ with entry points (not shaders/tests/) to .deshader, and
//   the stamp of the sources they came from (asset::kShaderStampPath), which the engine compares
//   at startup; a module that fails, or sources that change during the cook, leave no stamp, and
//   then every module cooks again next time. Needs a compiler; without one shaders are skipped.
// - Models (glTF, FBX): one .demesh, a .demat per material and a .detex per embedded image.
// - Materials (.material text): a .demat.
// - Textures: a .detex, with defaults picked by how models and materials use them, then the folder
//   rules, then the meta.
// - Terrains (.terrain text and its 16-bit height image): a .deterrain and a .deheights, both under its ID.
// Incremental: a source whose inputs (bytes, settings, files it reads, cooker version) hash the same
// as last time is skipped; cooked files of deleted sources are removed. The state is
// game:/cook_state.json.
//
// The VFS needs src:/, and game:/ writable unless check_only. Blocking; texture blocks encode on jobs.
CookReport cook(platform::Vfs& vfs, core::JobSystem& jobs, shader::ShaderCompiler* compiler,
                const CookOptions& options = {});

// The report as JSON, for tools and agents (docs/design.md, AI-friendly: machine-readable output).
core::Array<char> report_json(const CookReport& report);

} // namespace de::cooker
