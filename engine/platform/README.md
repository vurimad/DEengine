# platform

Everything OS-specific: the window, input, time, threads and debugger hooks (docs/design.md, Platforms & RHI). SDL3 on desktop. File IO backends arrive with the VFS task.

## Contents

| Header | What it gives |
| --- | --- |
| `platform.h` | `Platform`: SDL init; routes core's fatal errors and logs to the debugger |
| `window.h` | `Window`: Vulkan-capable window, drawable size in pixels, resize / minimize / focus / close |
| `events.h` | `pump_events(window, input)`: once per rendered frame |
| `input.h` | `Input`: named actions bound by text, sampled once per tick |
| `time.h` | `time_ns()`, `sleep_ns()`: the same clock as input timestamps |
| `thread.h` | `Thread` (named, joined on destruction), thread names and ids, core count |
| `debug.h` | Debugger detection, break, and the debugger output log sink |

## Rules and invariants

- Engine and game code never include OS or SDL headers; only this folder (and RHI backends, for surface creation through `Window::native_handle()`) does.
- No platform `#ifdef`: OS-specific code lives in `*_win32.cpp` / `*_linux.cpp`, picked by CMake.
- Game code reads actions, never device codes. Bindings are text (`"key:Space"`, `"gamepad:righttrigger"`, `"mouse:x"`) so they can live in data files; key names are SDL scancode names, gamepad names SDL gamepad button and axis names.
- Input never loses a tap: presses and releases between two samples both show up in the next sample, with the first press's timestamp for the combat input buffer.
- Losing focus or a gamepad releases everything held.
- `push` and `sample` never allocate.
- The platform tests never initialize SDL video, so they run on headless CI.
