# render/shader

Slang compilation, reflection and hot reload (docs/design.md, Shaders, materials & VFX). Shader sources and their conventions are in `shaders/` at the repository root.

## Contents

| File | What it gives |
| --- | --- |
| `shader_compiler.h` | `ShaderCompiler`: the entry points of a Slang module to SPIR-V in one compile, with reflection (push constant fields, thread group size, descriptor slots) and a report of the files it read and Slang's messages |
| `shader_cache.h` | What a compiled program kept between runs is named by and how its file is laid out; the compiler reads and writes them |
| `shader_library.h` | `ShaderLibrary`: graphics and compute programs with stable handles, their RHI pipelines, and live rebuilds when sources change; `create_cooked()` takes programs from the cooker's `.deshader` files instead. Asked for the same program again (module, entries, defines, pipeline state), it gives the one there is, kept until each who asked has destroyed it: a second renderer costs no compiles and no pipelines |
| `shader_asset.h` | Between the RHI's stages and defines and what `.deshader` files store; `built_source_hash()`, the hash of the shaders this build was compiled from |

## How it works

- Sources are read through the VFS from `src:/shaders/` (Slang's file system is our VFS), so every file a compile touches, imports included, is recorded as a dependency with its size and write time.
- `ShaderLibrary::update()` (once per frame) checks those files every 250 ms. Changed programs queue up and rebuild one at a time as background jobs (`JobSystem::submit_background`: only idle workers run them, never a thread waiting on a counter) while the game keeps rendering with the old pipeline; the new pipeline is swapped in on the thread that renders (the main thread, until there is a render thread), and the old one is destroyed once the GPU is done with it.
- With `ShaderCompilerDesc::cache_folder` set (the engine: `user:/shader_cache`; `--no-shader-cache` turns it off), a compiled program is written there, one file per request (module, entry points, defines, the compiler's options, Slang's version), with the content hash of every source the compile read. The next compile of that request, in this run or a later one, reads the file back if each of those sources still hashes the same, and compiles otherwise: a start compiles only the programs an edit touched (measured on the dev machine: 32 programs, 7.5 s compiled, 0.03 s read back). The sources are hashed each time, never trusted by size and write time. A program that compiled with warnings isn't kept, so its warnings show every time; a failed compile isn't either. A damaged file, or one from another `kShaderCacheVersion`, is compiled over. What it can't see: a new file that shadows an import of an unchanged source (the compile's dependencies are the files it found, not the places it looked); delete the folder then.
- With `ShaderLibraryDesc::background_first_builds` (the engine, for a window someone is at; not for a run that takes pictures or counts its frames, nor for tests), a program's first build doesn't block either: unless the compiler has it cached, it's queued like a rebuild, `create_graphics()` and `create_compute()` return at once, and its pipeline arrives with a later `update()`. `stats().first_builds` counts those still to come (the framework shows it in the window's title and in the picture's corner). They build as many at once as the compiler compiles at once, in the order created, except those created with `build_first` (the UI's, where the wait is shown), which go ahead. Until its pipeline is in, a program has none, as one whose first build failed: every pass checks `pipeline()` already, so the picture fills in as shaders arrive.
- A failed rebuild logs Slang's diagnostics (file, line, column) and keeps the last good version. It isn't retried until a file changes again; a file that was read mid-write or locked counts as changed, so it is.
- A program whose first build fails is kept without a pipeline and watched like the others (its module file included, even if it doesn't exist yet): fixing the shader brings it to life. Check `pipeline()` before drawing.
- Names (module, entries, defines) longer than 63 characters are refused, never cut short.
- Programs are referenced by `ProgramHandle`, which stays valid across rebuilds: look up `pipeline(handle)` when binding.
- Builds without `src:/` (ship) use `ShaderLibrary::create_cooked()`: each program reads its module's `.deshader` from `game:/` (`cooked_path(shader_asset_id(module))`), picks the permutation whose defines match and the entry points by name. The cooker compiles a module's every entry point with the same compiler and checks the pipeline layout the same way. With hot reload on, a re-cooked file is picked up like an edited source, if it's cooked from the sources this build was compiled from.

- `create_cooked()` first compares `game:/shader_sources.hash`, which the cooker writes, with `built_source_hash()` (`check_cooked_shaders()`), and fails with "re-run the cooker" if they differ or the stamp is missing: layouts shared with C++ are matched by hand and reached through pointers, so stale shaders are a GPU crash rather than an error. The framework runs the same check right after the VFS is up, so a refused start opens no window and creates no device, and says which `game:/` it looked in. Compiling from source (`create()`, dev builds with `src:/`) never looks at the stamp.
- Hot reload of cooked files checks the stamp again, when polling and when a build finishes. On a difference or no stamp (a cook is running, for one) the running pipelines stay, with one warning, and what changed comes in once the stamp is right. So re-cooking edited shaders doesn't reach a running engine: that needs a rebuilt one, which compiles from `src:/` anyway if it has it.
- `built_source_hash()` is generated at build time: `tools/shader_hash` runs `asset::shader_sources_hash()`, the cooker's own code, over `shaders/` and writes a source file of this library. An edited `.slang` reruns it (a new, renamed or deleted one through the glob's re-check), a changed hash relinks the executables, and an unchanged one does nothing.

## Rules and invariants

- Up to `ShaderCompilerDesc::parallel_compiles` compiles run at once (the engine: a quarter of the workers, 4 at most, or `--shader-compiles N`; tools: 1), each on a Slang global session of its own, since one is single-threaded. The first session is made with the compiler; the others when that many compiles first overlap, and `ShaderLibrary` lets the compiler drop them again once a burst of builds is over. They run off the thread that renders except for a program's first build without `background_first_builds`. Reading the compiler's cache doesn't wait for a compile in flight.
- What several at once cost: memory. Slang keeps what a compile took, and the process stays that much larger until it ends (measured in the editor, 42 programs from nothing cached: 0.7 GB with everything read from the cache, 1.4 GB after compiling one at a time, 2.0 GB after four at a time). Only a run that compiles many programs pays it; the next start reads them from the cache.
- A fresh Slang session per compile (sessions cache modules, and a rebuild must see the edits), with every entry point of the program in it, so the module is parsed and checked once.
- Every pipeline shares the device's one layout (128 bytes of push constants), so shaders don't need per-program layouts.
- SPIR-V 1.6 for Vulkan 1.3. Debug info is on in debug and dev builds (step through Slang in RenderDoc).
- On Windows, Slang's runtime (`slang*.dll` and `slang-standard-module-*`) is copied next to each executable by `de_deploy_slang_runtime()`; on Linux it's found through the rpath.

## Not yet

- Cooked permutations: modules cook with no defines, so a program with defines has no cooked version yet.
- An OS file watcher instead of polling.
- The cache keeps one file per request, so undoing an edit compiles again; keeping the last few by content would make that a read.
- Generated C++ and Lua parameter structs from reflection.
