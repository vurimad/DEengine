# DEengine — working notes for AI agents

`docs/design.md` is the source of truth for architecture, rules and the decisions log. Read the relevant section before changing a system. If a change contradicts the doc, stop and raise it instead of working around it.

## Build and check

Windows (PowerShell, from the repo root):

```powershell
. tools/dev_env.ps1                       # required: VS x64 env, keeps the standalone VCPKG_ROOT
cmake --preset msvc                       # configure presets: msvc, clang-cl, linux-clang, linux-gcc
cmake --build --preset msvc-debug         # build presets: <configure>-debug | -dev | -ship
ctest --preset msvc-debug
clang-format --dry-run --Werror <files>   # must be clean
clang-tidy -p build/clang-cl <cpp files>  # every finding is an error; skip *_linux.cpp on Windows
build/msvc/bin/Debug/deengine --frames 120  # smoke run: opens the window, exits after 120 frames
```

Before calling work done: it builds warning-free (warnings are errors), tests pass, clang-format and clang-tidy are clean.

## Rules that are easy to break

- No exceptions, no RTTI, no iostreams. `Result` for recoverable errors, `DE_ASSERT` for invariants, `DE_VERIFY` for checks that must survive ship builds.
- Memory: allocate through a tagged allocator (`heap_allocator(MemoryTag::Render)`, an `Arena`, a `Pool`, the frame allocator), never `new`/`malloc` directly.
- Logging: `DE_LOG_INFO("channel", "fmt", ...)` and friends; no `printf` in engine code (log sinks and core's log/assert fallbacks are the only exception).
- Runtime code never uses `std::string`, `std::map`, `std::unordered_map`, `std::list`, `std::function`, `std::shared_ptr`. Use the core library types (docs/design.md, Core library).
- Hot paths: no per-frame heap allocations, no copies of collections (pass `std::span`), no O(N) searches, no per-element locks.
- Naming: types `PascalCase`, functions/variables `snake_case`, private members `snake_case_`, constants `kPascalCase`, macros `DE_UPPER_SNAKE`, namespaces `de::lower`. clang-tidy enforces this.
- Units are meters, seconds, radians, kilograms; anything else carries its unit in the name (`duration_ticks`, `angle_deg`).
- Engine headers use quotes relative to `engine/` (`#include "core/version.h"`); third-party and std use angle brackets.
- No platform `#ifdef` outside `engine/platform/` and RHI backends. No Vulkan types above the RHI. No SDL or OS headers outside `engine/platform/` (RHI backends may use `Window::native_handle()`).
- Input: game code reads actions (`Input::state(action)`), never device codes; bindings are text.
- Profiling: put `DE_PROFILE_ZONE("name")` at the top of every system's per-frame entry point. Tracy viewer 0.14.1 must match the client in vcpkg.
- Threads: parallel work goes through `JobSystem` (`submit` + `JobCounter`, or `parallel_for`); don't create threads for work.

## Git

- Commit messages: imperative, prefixed by system: `render: add Hi-Z build pass`, `core: add SlotMap`.
- Never commit build output, cooked data, `Archives/`, captures (`.rdc`) or profiles.

## Clean IP

- Other games (e.g. Elden Ring) are studied only through frame captures, public talks and observation. Never commit extracted game data, shaders or scripts; write findings in our own words.
- Reference engines: Godot (MIT) may be read and quoted with attribution; Unreal source is for understanding ideas only, never copied.
- Never read, copy or derive from leaked or stolen proprietary source code.
