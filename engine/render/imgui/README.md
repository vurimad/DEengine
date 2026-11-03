# render/imgui

Dear ImGui drawn by the engine's own renderer, not ImGui's Vulkan backend (docs/design.md, Rendering: debug UI).

## Contents

| File | What it gives |
| --- | --- |
| `imgui_renderer.h` | `ImGuiRenderer`: ImGui's draw lists as one render graph pass; the textures ImGui asks for (its font atlas), through the uploader |

## Rules and invariants

- Vertices are read through a buffer pointer and textures by bindless index (`shaders/imgui.slang`): no descriptor sets, the engine's one pipeline layout.
- Each frame, after `ImGui::Render()`: `update_textures()` before `uploader.record()`, then `add_pass()` over the target.
- The ImGui context must exist from `create()` until the renderer is destroyed. Move-only.
- Debug and editor tools only; the game's UI is separate (docs/design.md, Game UI).
