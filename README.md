# DEengine

An engine for an open-world action RPG: a large world streamed seamlessly around the player, tight 60 Hz deterministic melee combat, and a deferred, GPU-driven Vulkan renderer.

- Design and decisions: [docs/design.md](docs/design.md)
- Status: M0a (window, core, first pixels)

## Requirements

| Tool | Version |
| --- | --- |
| Windows compiler | Visual Studio 2022 or newer with the C++ workload (MSVC; clang-cl optional) |
| Linux compiler | clang 17+ or gcc 13+ |
| CMake | 3.28+ |
| Ninja | 1.11+ |
| vcpkg | a standalone clone, with `VCPKG_ROOT` pointing to it |
| Git LFS | for binary source assets |

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

Each configure preset has three build and test presets:

| Preset suffix | CMake config | Build kind |
| --- | --- | --- |
| `-debug` | Debug | Debug build: asserts, no optimization |
| `-dev` | RelWithDebInfo | Dev build: optimized, asserts, dev tools |
| `-ship` | Release | Ship build: no asserts, no dev tools |

## Layout

```
engine/    engine code, one folder per system (core, platform, render, world, game, script, editor)
tools/     cooker, validators, packer, dev scripts
shaders/   Slang shaders
content/   source assets, data, Lua
meta/      mirrored meta tree: GUIDs and import settings
tests/     unit, gameplay and benchmark tests
docs/      design doc
```

Folders appear as their milestone tasks land.
