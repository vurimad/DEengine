# render/scene

The scene on the GPU (docs/design.md, Rendering): GPU copies of the assets the registry has loaded, and the pass that draws what the world extracts each frame. Renderer step 1: the pass is a placeholder (one sun and an ambient term, straight into the target) that step 2 replaces with the G-buffer and deferred lighting; its inputs stay.

## Contents

| File | What it gives |
| --- | --- |
| `gpu_assets.h` | `GpuAssets`: meshes as vertex and index buffers, textures with their bindless index, materials in one table (`GpuMaterial`), mirrored from the registry's events |
| `scene_renderer.h` | `SceneRenderer`: the pass drawing a `world::RenderFrame`; `camera_view_proj` (infinite reverse-Z, looking down -Z) |

## Rules and invariants

- **The registry owns what's loaded; this mirrors the GPU side.** `GpuAssets` listens to the registry: an asset uploads when it's Ready, again when it's Reloaded, and goes when it's Unloaded. Its maps are keyed by the registry's handles. It must exist before the assets it should mirror load (there's no catch-up), and it stops listening when it goes (`AssetRegistry::remove_listener`).
- **Nothing is drawn before its data is on the GPU.** A mesh becomes drawable in `update()` once the uploader has recorded its buffers (`Uploader::ready(BufferHandle)`); until then `mesh()` is null and the instance is skipped. A material whose textures are still uploading points at default textures (white, a flat normal) and is written again when they're in, so shaders never branch on a missing texture.
- **Material 0 is the default**; a part whose material is missing or failed uses it. Table slots are reused when materials unload; the table holds `GpuAssetsDesc::max_materials` (4096).
- **Reloads replace GPU copies.** A reloaded texture gets a new GPU texture (and bindless index); every material is rewritten. A reloaded mesh gets new buffers and is drawable again once they're uploaded. The old resources go when the frames using them are done (the device's deferred destruction).
- **The renderer owns no entity list.** `SceneRenderer::add_pass` reads `world::RenderFrame`: the camera and every instance, relative to the render origin. Per-frame data (view, lighting, instance transforms) goes through a mapped buffer per frame in flight, grown when needed: no allocation per frame once settled.
- **Cameras look down their -Z**; projection is infinite reverse Z (depth = near / distance, cleared to 0, tested Greater).
- Double-sided materials draw with a second pipeline without back-face culling.

## Not yet

- The G-buffer and deferred lighting (step 2), then the lighting, shadows and post chain the look study calls for.
- Shared vertex and index buffers, culling, indirect draws (GPU-driven v1).
- LODs beyond LOD 0, streaming mips, keeping a reloaded texture until its replacement is uploaded (a reload shows defaults for a frame or two).
