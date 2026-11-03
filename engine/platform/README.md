# platform

Everything OS-specific: the window, input, files, time, threads and debugger hooks (docs/design.md, Platforms & RHI), plus the VFS on top of the file layer. SDL3 on desktop.

## Contents

| Header | What it gives |
| --- | --- |
| `platform.h` | `Platform`: SDL init; routes core's fatal errors and logs to the debugger |
| `window.h` | `Window`: Vulkan-capable window, drawable size in pixels, resize / minimize / focus / close; Vulkan surface creation for the RHI |
| `events.h` | `pump_events(window, input)`: once per rendered frame |
| `input.h` | `Input`: named actions bound by text, sampled once per tick |
| `vfs.h` | `Vfs`: `game:/` `user:/` `temp:/` `src:/` mounts, path rules, blocking and async reads by priority, atomic writes, file info for hot reload |
| `file.h` | Native files: positional reads, atomic writes, size and write time, folders, the OS user and temp folders |
| `time.h` | `time_ns()`, `sleep_ns()`: the same clock as input timestamps |
| `thread.h` | `Thread` (named, joined on destruction), thread names and ids, core count |
| `debug.h` | Debugger detection, break, and the debugger output log sink |
| `random.h` | Random bytes from the OS: GUIDs for new assets, seeds (not gameplay randomness) |
| `imgui_platform.h` | Dear ImGui's platform side: display size, time step and input events, so SDL stays here |
| `imgui_events.h` | Inside `platform/` only: how `pump_events()` hands each SDL event to ImGui |

## Rules and invariants

- Engine and game code never include OS or SDL headers; only this folder does. The Vulkan RHI backend gets its instance extensions and window surface from `vulkan_instance_extensions()` and `create_vulkan_surface()`, with Vulkan handles passed as plain pointers and integers.
- No platform `#ifdef`: OS-specific code lives in `*_win32.cpp` / `*_linux.cpp`, picked by CMake.
- Game code reads actions, never device codes. Bindings are text (`"key:Space"`, `"gamepad:righttrigger"`, `"mouse:x"`), read from `config/input.bindings` by `Input::bind_text` (a line an action, a binding and an optional scale; lines for actions a build doesn't add are skipped); key names are SDL scancode names, gamepad names SDL gamepad button and axis names.
- Input never loses a tap: presses and releases between two samples both show up in the next sample, with the first press's timestamp for the combat input buffer.
- Losing focus or a gamepad releases everything held.
- `push` and `sample` never allocate.
- The platform tests never initialize SDL video, so they run on headless CI.
- Engine code reads and writes files only through the `Vfs`; `file.h` is for the VFS and tools. Paths are `mount:/lowercase/forward/slashes`.
- `src:/` is the source tree (the repository root: `shaders/`, `content/`, `meta/`), mounted only in dev builds and tools (`VfsDesc::src_root`). Shaders compile from it and the editor writes sources through it.
- `game:/` is read only. Writes to `user:/`, `temp:/` and `src:/` are atomic (temp file, then rename). `temp:/` is emptied at every start; the VFS only ever clears folders it named itself (`DEengine`, `deengine_temp`).
- Async reads go straight into caller memory; queueing, polling and finishing never allocate. Phase 1: blocking positional reads on a small reader pool behind one lock; phase 2 (IoRing / io_uring) keeps the API.
