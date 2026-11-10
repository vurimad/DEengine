// The cooker's command line (docs/design.md, AI-friendly: headless and CLI first).
//   cooker [--project DIR] [--out DIR] [--check] [--force] [--no-shaders] [--shader-debug] [--shader-checks]
//   --project DIR     the project to cook (docs/design.md, Projects): its folder or .deproject file, relative to the
//                     working directory or this repository (default: the build's, projects/sandbox); its sources
//                     are src:/, the engine's shaders engine:/
//   --out DIR         where cooked files go, mounted as game:/ (default: the project's cooked/)
//   --check           check the source and meta trees, that cooked shaders and collision aren't
//                     stale, and terrain sources and what's cooked from them, write nothing (CI);
//                     exit code 1 on errors
//   --force           cook everything again
//   --no-shaders      skip shaders (no Slang compiler needed), in --check too
//   --shader-debug    keep source-level debug info in cooked shaders (RenderDoc, Nsight)
//   --shader-checks   compile shaders with DE_SHADER_CHECKS=1
// Exit code 0 when there are no errors. A JSON report goes to <out>/cook_report.json.

#include "cooker/cooker.h"
#include "core/job_system.h"
#include "core/log.h"
#include "platform/platform.h"
#include "platform/project.h"
#include "platform/thread.h"
#include "platform/vfs.h"
#include "render/shader/shader_compiler.h"

#include <cstdlib>
#include <cstring>

namespace {

const char* flag_value(int argc, char** argv, const char* flag) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) {
            return argv[i + 1];
        }
    }
    return nullptr;
}

bool has_flag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    auto platform = de::platform::Platform::create({.video = false, .gamepad = false});
    if (!platform) {
        return EXIT_FAILURE;
    }
    de::platform::set_current_thread_name("main");
    de::core::JobSystem jobs(de::core::JobSystemDesc{
        .on_worker_start = [](uint32_t /*index*/, const char* name) { de::platform::set_current_thread_name(name); }});

    const char* given = flag_value(argc, argv, "--project");
    given = given != nullptr ? given : DE_DEFAULT_PROJECT;
    const de::core::Result<de::platform::Project> project = de::platform::find_project(given, DE_SOURCE_DIR);
    if (!project) {
        DE_LOG_ERROR("cooker", "project %s: %s", given, project.error().message);
        return EXIT_FAILURE;
    }
    const char* src_root = project->root.c_str();
    const char* out = flag_value(argc, argv, "--out");
    de::platform::PathString game_root(out != nullptr ? out : src_root);
    if (out == nullptr && !game_root.append("/cooked")) {
        return EXIT_FAILURE;
    }
    DE_LOG_INFO("cooker", "project %s: %s", project->name.c_str(), src_root);
    const de::cooker::CookOptions options{
        .check_only = has_flag(argc, argv, "--check"),
        .force = has_flag(argc, argv, "--force"),
        .shader_debug_info = has_flag(argc, argv, "--shader-debug"),
        .shader_checks = has_flag(argc, argv, "--shader-checks"),
        .check_shaders = !has_flag(argc, argv, "--no-shaders"),
    };
    auto vfs = de::platform::Vfs::create({.game_root = game_root.c_str(),
                                          .src_root = src_root,
                                          .engine_root = DE_SOURCE_DIR,
                                          .game_writable = !options.check_only});
    if (!vfs) {
        DE_LOG_ERROR("cooker", "can't mount %s as src:/, %s as engine:/ and %s as game:/", src_root, DE_SOURCE_DIR,
                     game_root.c_str());
        return EXIT_FAILURE;
    }

    de::core::Result<de::shader::ShaderCompiler> compiler =
        de::core::fail(de::core::ErrorCode::Unsupported, "shaders skipped");
    if (!has_flag(argc, argv, "--no-shaders") && !options.check_only) {
        compiler = de::shader::ShaderCompiler::create(
            *vfs, {.debug_info = options.shader_debug_info, .checks = options.shader_checks});
        if (!compiler) {
            DE_LOG_ERROR("cooker", "the shader compiler didn't start (%s): pass --no-shaders to cook without it",
                         compiler.error().message);
            return EXIT_FAILURE;
        }
    }

    const de::cooker::CookReport report = de::cooker::cook(*vfs, jobs, compiler ? &*compiler : nullptr, options);
    if (!options.check_only) {
        const de::core::Array<char> json = de::cooker::report_json(report);
        if (!vfs->write_file("game:/cook_report.json", std::as_bytes(json.span())).ok()) {
            DE_LOG_WARN("cooker", "game:/cook_report.json could not be written");
        }
    }
    return report.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
}
