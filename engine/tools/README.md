# tools

Debug and editor tools inside the engine, drawn with Dear ImGui (docs/design.md, Rendering: debug UI).

## Contents

| File | What it gives |
| --- | --- |
| `debug_ui.h` | The ImGui context on the Tools heap (docking on, no ini file), and `DebugUi`: context, platform side and renderer made and torn down together |
| `stats_panel.h` | `StatsPanel`: frame time and history, heap allocations per frame, GPU memory by category, the render graph, uploads and the device (F1 in deengine) |
| `free_camera.h` | `FreeCamera`: a fly camera driven by input actions, placing its entity each frame; `add_free_camera_actions` |

## Rules and invariants

- Dev builds only in the engine's frame (`DE_BUILD_DEV`), except the free camera, which the viewer flies in every build.
- The debug UI's memory is tagged Tools and counted per frame like everything else. Its frame: `begin_frame()`, ImGui windows, `end_frame()` before `uploader.record()`, then `add_pass()` over the backbuffer.
- Without a shader library there is no renderer, and no UI frames are built.
- The free camera reads actions, never device codes; its bindings are in `config/input.bindings`.
