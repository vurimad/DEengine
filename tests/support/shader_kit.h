#pragma once

#include "core/job_system.h"
#include "platform/vfs.h"
#include "render/rhi/device.h"
#include "render/shader/shader_compiler.h"
#include "render/shader/shader_library.h"
#include "support/sandbox.h"

#include <doctest/doctest.h>

#include <utility>

namespace de::test {

// The shader compiler and a library over the repository's shaders (src:/ is the repo), for GPU
// tests that run programs from shaders/tests/. Hot reload off.
//     const ShaderKit kit(*device);
//     list.bind_pipeline(kit.pipeline(kit.compute("tests.fill_buffer")));
class ShaderKit {
public:
    explicit ShaderKit(rhi::Device& device)
        : vfs_(make_vfs()), compiler_(std::move(shader::ShaderCompiler::create(vfs_)).value()),
          library_(
              std::move(shader::ShaderLibrary::create(device, vfs_, jobs_, compiler_, {.hot_reload = false})).value()) {
    }

    // A compute program from shaders/; the test fails if it doesn't build.
    shader::ProgramHandle compute(const char* module) {
        const auto program = library_.create_compute({.module = module});
        REQUIRE(program.ok());
        REQUIRE(library_.pipeline(*program).is_valid());
        return *program;
    }

    rhi::PipelineHandle pipeline(shader::ProgramHandle program) const { return library_.pipeline(program); }
    shader::ShaderLibrary& library() { return library_; }

private:
    platform::Vfs make_vfs() const {
        const platform::PathString game = sandbox_.game();
        const platform::PathString user = sandbox_.folder("user");
        const platform::PathString temp = sandbox_.folder("temp");
        auto created = platform::Vfs::create({.game_root = game.c_str(),
                                              .user_root = user.c_str(),
                                              .temp_root = temp.c_str(),
                                              .src_root = DE_SOURCE_DIR});
        REQUIRE(created.ok());
        return std::move(created).value();
    }

    Sandbox sandbox_;
    core::JobSystem jobs_{core::JobSystemDesc{.worker_count = 1}};
    platform::Vfs vfs_;
    shader::ShaderCompiler compiler_;
    shader::ShaderLibrary library_;
};

} // namespace de::test
