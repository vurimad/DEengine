# tools

Debug and editor tools inside the engine, drawn with Dear ImGui (docs/design.md, Rendering: debug UI).

## Contents

| File | What it gives |
| --- | --- |
| `debug_ui.h` | The ImGui context on the Tools heap (docking on, no ini file), and `DebugUi`: context, platform side and renderer made and torn down together |
| `stats_panel.h` | `StatsPanel`: frame time and history, heap allocations per frame, GPU memory by category, GPU time per pass, the render graph, uploads and the device, where the camera is, to come back to a spot, and, while the cull is frozen, that it is and what it kept for the shadows (F1 in deengine); drawn over the game's picture, see-through, its sections chosen by name (`parse_stats_sections`, `--stats`) |
| `console_ui.h` | `ConsoleUi`: the console window, as Unreal's: ` opens it over the bottom of the game's picture (the editor's Playground, or the window), Enter runs what's typed in the console (`console/console.h`), names complete as they're typed (Tab, the arrows to pick), the arrows walk back through what was typed (kept in `user:/console.history`) |
| `agent_server.h` | `AgentServer`: the agents' door, JSON-RPC 2.0 over TCP on 127.0.0.1 only (port 37017, or the next free one), a request a line: `console.exec {line}` runs commands and answers with what they said, `console.complete {prefix}` lists names. Never blocks; `answer_request` is the protocol alone, for tests. `tools/console.py` (in the repository's `tools/`) is its client |
| `free_camera.h` | `FreeCamera`: a fly camera driven by input actions, placing its entity each frame; `add_free_camera_actions` |
| `camera_script.h` | `CameraPose` (position in doubles, yaw, pitch), `parse_camera_pose`, `find_camera_view` (named views as text), `CameraPath` (waypoints as text, flown by the frame) |
| `screenshot.h` | `Screenshot`: reads a texture back at the end of a frame and makes a PNG of it; `encode_screenshot_png` |

What runs these from the command line is `engine/app/agent_eyes.h`; how to use them, `engine/app/README.md`.

## Rules and invariants

- Dev builds only in the engine's frame (`DE_BUILD_DEV`), except the free camera, which the viewer flies in every build. The library still builds them in ship, for the tests; nothing in ship's `deengine` reaches them, so the linker leaves them out.
- The debug UI's memory is tagged Tools and counted per frame like everything else. Its frame: `begin_frame()`, ImGui windows, `end_frame()` before `uploader.record()`, then `add_pass()` over the backbuffer.
- Without a shader library there is no renderer, and no UI frames are built.
- The free camera reads actions, never device codes; its bindings are in `config/input.bindings`.
