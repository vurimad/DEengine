# render/imgui

Dear ImGui drawn by the engine's own renderer, not ImGui's Vulkan backend (docs/design.md, Rendering: debug UI).

## Contents

| File | What it gives |
| --- | --- |
| `imgui_renderer.h` | `ImGuiRenderer`: ImGui's draw lists as one render graph pass; the textures ImGui asks for (its font atlas), through the uploader |

## Rules and invariants

- Vertices are read through a buffer pointer and textures by bindless index (`shaders/imgui.slang`): no descriptor sets, the engine's one pipeline layout.
- Each frame, after `ImGui::Render()`: `update_textures()` before `uploader.record()`, then `add_pass()` over the target.
- Colour: ImGui's colours are what a screen is sent (sRGB), and so are the texels of the textures it owns (its font atlas, the editor's pages of pictures), kept in plain 8-bit textures. Drawing into an sRGB target, the shader turns both into light. The engine's own textures shown in the UI (the scene, a preview, the frame debugger's) are light already when sampled, and are left alone: `ImGuiPush::coded_texture` says which a draw has.
- The ImGui context must exist from `create()` until the renderer is destroyed. Move-only.
- Debug and editor tools only; the game's UI is separate (docs/design.md, Game UI).
