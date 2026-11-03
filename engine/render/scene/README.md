# render/scene

The scene on the GPU (docs/design.md, Rendering): GPU copies of the assets the registry has loaded, and the passes that draw what the world extracts each frame: deferred: a G-buffer, a full-screen lighting pass (the sun, a hemisphere ambient, the sky) and a tonemap, with the sun's shadows from cascaded shadow maps resolved into a screen-space mask.

## Contents

| File | What it gives |
| --- | --- |
| `gpu_assets.h` | `GpuAssets`: meshes as vertex and index buffers, textures with their bindless index, materials in one table (`GpuMaterial`), mirrored from the registry's events |
| `scene_renderer.h` | `SceneRenderer`: the passes drawing a `world::RenderFrame`, `SceneLighting` (with the sun's `SceneShadows`, per frame) and `scene_lighting()` (from an environment asset), `SceneView` (debug views); `fit_shadow_cascades`, `camera_view_proj` (infinite reverse-Z, looking down -Z), `invert_4x4` |

## The G-buffer (v1)

| Target | Format | Contents |
| --- | --- | --- |
| 0 | RGBA8 sRGB | base color; alpha: lighting group / 255 (0 until materials carry one) |
| 1 | RG16 unorm | world normal, octahedral, remapped to 0..1 |
| 2 | RGBA8 unorm | roughness, metalness, occlusion, translucency (0 until materials carry one) |
| HDR | RGBA16 float | emissive, written by the G-buffer pass; the lighting is added onto it |
| depth | D32 float | reverse Z |

20 bytes a pixel plus depth. Motion vectors come with temporal anti-aliasing (written in the G-buffer pass, not a pass of their own). `shaders/scene/scene_common.slang` packs and unpacks it.

## The frame

1. `sun shadows`: four cascades in a 2 x 2 depth atlas (`SceneShadows::resolution` each, 2048 by default), every instance drawn into each, without culling. `fit_shadow_cascades` splits the view up to `distance_m` (practical split, `split_lambda`), bounds each slice with a sphere whose size doesn't change as the camera turns, and snaps its center to the texel grid so edges don't swim as the camera moves. Depth is reverse Z toward the sun, and reaches `caster_reach_m` past the sphere for casters outside the view.
2. `gbuffer`: every instance whose mesh is on the GPU, a draw per mesh part. Parts are grouped by kind each frame (one material lookup each): opaque, opaque double-sided, masked, masked double-sided, drawn in that order so masked pixels behind opaque surfaces fail early depth. Opaque parts use a shader with no `discard` (depth is tested before it runs), masked ones the alpha test; double-sided ones turn culling off (dynamic state, one shader for both). The shadow casters use the same groups, with culling off for every kind (thin geometry casts from both sides).
3. `shadow mask`: per pixel, the cascade by distance along the view, with a dithered band handing pixels to the next one (no visible seam; the last cascade fades to lit), the receiver pushed along its normal and toward the sun by about a texel (no acne), then a contact-hardening filter: a blocker search sets the penumbra from how far the occluder is (`penumbra_per_m`, up to `max_penumbra_texels`), 16 Poisson taps rotated per pixel by interleaved gradient noise (another value than the cascade dither's). RG8: sunlight seen, and the cascade (for its debug view). The light pass multiplies the sun by it, toward `SceneLighting::shadow_color` rather than black.
4. `deferred light`: one full-screen pass added onto the HDR target. The sun, through the shadow mask: GGX specular with height-correlated Smith visibility and Schlick Fresnel, Lambert diffuse; translucent surfaces get wrapped diffuse and light seen through them with the sun behind. The ambient: a sky/ground hemisphere for diffuse, and in the reflected direction through a fitted environment BRDF for specular (the placeholder for image-based lighting). Where nothing was drawn, the sky: a gradient to the horizon and the sun's disk.
5. `tonemap`: exposure (`SceneLighting::exposure_ev`) and a filmic curve (the ACES fit) into the target, or a G-buffer channel, the shadow mask or the cascades (`SceneView`, F2 in deengine).

## Rules and invariants

- **The registry owns what's loaded; this mirrors the GPU side.** `GpuAssets` listens to the registry: an asset uploads when it's Ready, again when it's Reloaded, and goes when it's Unloaded. Its maps are keyed by the registry's handles. It must exist before the assets it should mirror load (there's no catch-up), and it stops listening when it goes (`AssetRegistry::remove_listener`).
- **Nothing is drawn before its data is on the GPU.** A mesh becomes drawable in `update()` once the uploader has recorded its buffers (`Uploader::ready(BufferHandle)`); until then `mesh()` is null and the instance is skipped. A material whose textures are still uploading points at default textures (white, a flat normal) and is written again when they're in, so shaders never branch on a missing texture.
- **Material 0 is the default**; a part whose material is missing or failed uses it. Table slots are reused when materials unload; the table holds `GpuAssetsDesc::max_materials` (4096).
- **Reloads replace GPU copies.** A reloaded texture gets a new GPU texture (and bindless index); every material is rewritten, and the old texture stays until the table no longer points at it. A reloaded mesh gets new buffers and is drawable again once they're uploaded. The old resources go when the frames using them are done (the device's deferred destruction).
- **The renderer owns no entity list.** `SceneRenderer::add_passes` reads `world::RenderFrame`: the camera and every instance, relative to the render origin. Per-frame data (view, lighting, instance transforms) goes through a mapped buffer per frame in flight, grown when needed: no allocation per frame once settled.
- **Cameras look down their -Z**; projection is infinite reverse Z (depth = near / distance, cleared to 0, tested Greater).
- Without a camera, while a scene shader is missing (a failed first build), or when the frame's constants can't be written (no per-frame buffer, or a camera transform that can't be inverted), the passes are replaced by one that clears the target.

## Not yet

- A static shadow cache (static casters drawn once, moving ones over them), local-light and cloud shadows, sun visibility for fog, temporal filtering of the mask; image-based lighting from probes, lighting groups with their key and fill lights, the diffuse/specular split (for subsurface), fog and haze, the tonemap curve as a table and grading LUTs, temporal anti-aliasing: the look study's list.
- Shared vertex and index buffers, culling, indirect draws (GPU-driven v1).
- LODs beyond LOD 0, streaming mips.
