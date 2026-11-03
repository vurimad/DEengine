# DEengine

An engine for an open-world action RPG: a large world streamed seamlessly around the player, tight 60 Hz deterministic melee combat, and a deferred, GPU-driven Vulkan renderer.

- Design and decisions: [docs/design.md](docs/design.md)

## Status

M0a is done and M0b is under way (task lists in docs/design.md, Roadmap & milestones). What exists today (the system folders' READMEs have their rules):

- Platform layer: SDL3 window and input, files, time, threads; a VFS with `game:/`, `user:/`, `temp:/` and `src:/` mounts and async reads.
- Core library: containers, strings, `Result`, tagged allocators, logging, a work-stealing job system, Tracy zones.
- Cooker: textures, glTF and FBX models, materials, cells, worlds, environments, shaders and `config/`, from `content/` and its `meta/` tree into `cooked/`.
- Asset registry: loads cooked files by ID with their dependencies, reference counted, hot reloaded when they are cooked again.
- World: one flecs world with its phases and a fixed 60 Hz tick, double-precision transforms, cells as assets and entities, render extraction.
- Rendering: a Vulkan 1.3 RHI (bindless, buffers by address, a staging uploader) and a render graph. The deferred renderer draws a G-buffer, the sun and a hemisphere ambient, cascaded sun shadows through a screen-space mask, and a filmic tonemap, with debug views of each channel.
- App: `engine/main.cpp` hands an `Application` to `run_framework`. The Viewer opens a world and flies a free camera through it.
- Input bindings and engine settings are text in `config/`.
- Dev builds: a Dear ImGui debug UI with a frame stats panel.

Not yet: GPU-driven culling and indirect draws, streaming, terrain, Lua, the command API, the editor, the game.

## Requirements

| Tool | Version |
| --- | --- |
| Windows compiler | Visual Studio 2022 or newer with the C++ workload (MSVC; clang-cl optional) |
| Linux compiler | clang 17+ or gcc 13+ |
| CMake | 3.28+ |
| Ninja | 1.11+ |
| vcpkg | a standalone clone, with `VCPKG_ROOT` pointing to it |
| Git LFS | for binary source assets |
| GPU driver | Vulkan 1.3 |
| Linux packages | `libx11-dev libxft-dev libxext-dev libwayland-dev libxkbcommon-dev libegl1-mesa-dev` (SDL3's window backends) |

## Build

Windows (PowerShell):

```powershell
. tools/dev_env.ps1                # Visual Studio x64 environment, keeps your VCPKG_ROOT
cmake --preset msvc                # or clang-cl
cmake --build --preset msvc-dev
ctest --preset msvc-dev
```

Linux:

```sh
cmake --preset linux-clang         # or linux-gcc
cmake --build --preset linux-clang-dev
ctest --preset linux-clang-dev
```

Profile it without the GUI (Windows; needs the Tracy 0.14.1 tools from `winget install wolfpld.tracy`):

```powershell
tools/profile.ps1 -Seconds 10      # prints top zones and plots; CSVs and trace.tracy in build/profiles/
```

Cook assets (`content/` with its `meta/` tree, the shaders and `config/`) into `cooked/`, which the engine mounts as `game:/`. Ship builds have no source tree and draw with these cooked shaders. See `tools/cooker/README.md`.

```sh
build/<preset>/bin/<config>/cooker              # --check validates and writes nothing; --force cooks everything
```

Run the engine. It opens the world named in `config/engine.settings` from the cooked data, so cook first. WASD and Q/E move, the right mouse button looks, Shift goes fast, Escape quits; in dev builds F1 shows the frame stats and F2 steps through the debug views.

```sh
build/<preset>/bin/<config>/deengine            # --frames N exits after N frames; --world ID opens another world
```

Each configure preset has three build and test presets:

| Preset suffix | CMake config | Build kind |
| --- | --- | --- |
| `-debug` | Debug | Debug build: asserts, no optimization |
| `-dev` | RelWithDebInfo | Dev build: optimized, asserts, dev tools |
| `-ship` | Release | Ship build: no asserts, no dev tools |

## Layout

```
engine/    engine code, one folder per system (app, asset, core, platform, render, tools, world)
tools/     the cooker and dev scripts
shaders/   Slang shaders
config/    input bindings and engine settings, as text
content/   source assets and data
meta/      mirrored meta tree: GUIDs and import settings
tests/     unit tests and benchmarks
docs/      design doc
```

Folders appear as their milestone tasks land.
