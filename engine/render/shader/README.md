# render/shader

Slang compilation, reflection and hot reload (docs/design.md, Shaders, materials & VFX). Shader sources and their conventions are in `shaders/` at the repository root.

## Contents

| File | What it gives |
| --- | --- |
| `shader_compiler.h` | `ShaderCompiler`: the entry points of a Slang module to SPIR-V in one compile, with reflection (push constant fields, thread group size, descriptor slots) and a report of the files it read and Slang's messages |
| `shader_library.h` | `ShaderLibrary`: graphics and compute programs with stable handles, their RHI pipelines, and live rebuilds when sources change; `create_cooked()` takes programs from the cooker's `.deshader` files instead |
| `shader_asset.h` | Between the RHI's stages and defines and what `.deshader` files store; `built_source_hash()`, the hash of the shaders this build was compiled from |

## How it works

- Sources are read through the VFS from `src:/shaders/` (Slang's file system is our VFS), so every file a compile touches, imports included, is recorded as a dependency with its size and write time.
- `ShaderLibrary::update()` (once per frame) checks those files every 250 ms. Changed programs queue up and rebuild one at a time as background jobs (`JobSystem::submit_background`: only idle workers run them, never a thread waiting on a counter) while the game keeps rendering with the old pipeline; the new pipeline is swapped in on the thread that renders (the main thread, until there is a render thread), and the old one is destroyed once the GPU is done with it.
- A failed rebuild logs Slang's diagnostics (file, line, column) and keeps the last good version. It isn't retried until a file changes again; a file that was read mid-write or locked counts as changed, so it is.
- A program whose first build fails is kept without a pipeline and watched like the others (its module file included, even if it doesn't exist yet): fixing the shader brings it to life. Check `pipeline()` before drawing.
- Names (module, entries, defines) longer than 63 characters are refused, never cut short.
- Programs are referenced by `ProgramHandle`, which stays valid across rebuilds: look up `pipeline(handle)` when binding.
- Builds without `src:/` (ship) use `ShaderLibrary::create_cooked()`: each program reads its module's `.deshader` from `game:/` (`cooked_path(shader_asset_id(module))`), picks the permutation whose defines match and the entry points by name. The cooker compiles a module's every entry point with the same compiler and checks the pipeline layout the same way. With hot reload on, a re-cooked file is picked up like an edited source.

- `create_cooked()` first compares `game:/shader_sources.hash`, which the cooker writes, with `built_source_hash()` (`check_cooked_shaders()`), and fails with "re-run the cooker" if they differ or the stamp is missing: layouts shared with C++ are matched by hand and reached through pointers, so stale shaders are a GPU crash rather than an error. The engine doesn't start then. Compiling from source (`create()`, dev builds with `src:/`) never looks at the stamp. Hot reload of cooked files doesn't either, once started: what is re-cooked while running is taken as it is, like an edited source.
- `built_source_hash()` is generated at build time: `tools/shader_hash` runs `asset::shader_sources_hash()`, the cooker's own code, over `shaders/` and writes a source file of this library. An edited `.slang` reruns it (a new, renamed or deleted one through the glob's re-check), a changed hash relinks the executables, and an unchanged one does nothing.

## Rules and invariants

- Compiles run one at a time (Slang's global session is single-threaded), off the thread that renders except for a program's first build.
- A fresh Slang session per compile (sessions cache modules, and a rebuild must see the edits), with every entry point of the program in it, so the module is parsed and checked once.
- Every pipeline shares the device's one layout (128 bytes of push constants), so shaders don't need per-program layouts.
- SPIR-V 1.6 for Vulkan 1.3. Debug info is on in debug and dev builds (step through Slang in RenderDoc).
- On Windows, Slang's runtime (`slang*.dll` and `slang-standard-module-*`) is copied next to each executable by `de_deploy_slang_runtime()`; on Linux it's found through the rpath.

## Not yet

- Cooked permutations: modules cook with no defines, so a program with defines has no cooked version yet.
- A disk cache of compiled shaders, parallel compiles (a Slang global session per worker), and an OS file watcher instead of polling.
- Generated C++ and Lua parameter structs from reflection.
