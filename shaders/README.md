# shaders

Every shader in Slang (docs/design.md, Shaders, materials & VFX). The engine compiles them from `src:/shaders/` in dev builds and rebuilds them live when a file changes. The cooker compiles them into `.deshader` files, which ship builds (and dev builds without `src:/`) load instead (`engine/render/shader/README.md`). They refuse to start on `.deshader` files cooked from other sources than they were built from, so after changing anything here, rebuild and re-run the cooker to run those builds.

## Layout

| Path | What |
| --- | --- |
| `scene/scene_common.slang` | What the scene passes share: frame constants, vertex and material layouts, G-buffer packing |
| `scene/shadow_caster.slang` | The sun's shadow casters: depth into one cascade's tile of the shadow atlas, alpha-tested for masked materials |
| `scene/gbuffer.slang` | The G-buffer pass: surfaces into the G-buffer, emissive into the HDR target; `fragment_opaque` (no discard) and `fragment_masked` (alpha test), and `fragment_opaque_fading` and `fragment_masked_fading` for what's fading between LODs. With `EDITOR_IDS` (the editor's, dev builds) one more target takes each pixel's instance; `fragment_selection` draws the editor's selection mask |
| `scene/terrain_common.slang` | Terrain: what its passes share, how a patch's vertices come from the heights, and the morph |
| `scene/terrain_select.slang` | Terrain: which patches draw, each deciding for itself, into a list per view (compute) |
| `scene/terrain.slang`, `scene/terrain_shadow.slang` | Terrain into the G-buffer (one plain look until there are materials), and into a shadow cascade |
| `scene/shadow_mask.slang` | The sun's shadow mask: the cascades resolved per pixel, with a contact-hardening filter |
| `scene/deferred_light.slang` | Deferred lighting (its ambient occluded by GTAO, with a multi-bounce fit and a specular occlusion): the sun (GGX, Lambert, wrap and backlight for translucency) through the shadow mask, hemisphere ambient, the sky |
| `scene/fog_inject.slang`, `scene/fog_integrate.slang` | The froxel grid (compute): each froxel's extinction and light, with fog volumes, the sun's shadows and last frame's light at the same place; then each column integrated front to back |
| `scene/fog.slang` | Fog over the lit image and the sky: the grid as far as it reaches, with the share of a slice with fog volumes really in front of the surface, closed form past it (`common.fog`) |
| `scene/transparent.slang`, `scene/blend_sort.slang` | The transparent pass: what blends, lit as it's drawn (the sun through its cascades, the ambient), fogged, mixed premultiplied over the fogged image, and how much of each pixel it covers, for TAA; and its order: each draw's distance along the view, then its place among the others, farthest first (compute) |
| `scene/forward.slang` | What's drawn after the fog shares: the sun through its shadow cascades at a point (`sun_visibility`) and the fog between the camera and a point (`fog_in_front`) |
| `scene/local_lights.slang`, `scene/light_cull.slang` | Local lights (docs/design.md, Local lights): a point's cluster in the view, what a light sends it (`light_reaching`) and what the lights of its cluster add to a surface (`local_lights`); and the compute pass that writes, for each cluster, a bit per light that reaches it |
| `scene/ao_depth.slang`, `scene/gtao.slang`, `scene/ao_denoise.slang` | Ambient occlusion on the screen (docs/design.md, Global illumination: GTAO): the depth pyramid (depth along the view, averaged along surfaces); the horizons along two slices a pixel and their cosine-weighted visibility in closed form, against what the same slices give open; the edge-aware denoise |
| `scene/path_trace.slang`, `scene/path_tracer.slang`, `scene/path_tracer_show.slang`, `scene/lighting_compare.slang` | The path tracer (docs/design.md, Global illumination: the plan from 2026-11-14): one path of light (`trace_path`: surfaces' glow, the sun and the local lights by rays toward them, diffuse bounces, Russian roulette), the pass adding a path a pixel to the mean, its display, and the compare of the rasterized light against it in stops |
| `scene/rc_common.slang`, `scene/rc_depth.slang`, `scene/radiance_cascades.slang`, `scene/rc_apply.slang` | Radiance cascades (docs/design.md, Global illumination: radiance cascades): the probes' layout, the cascades' depth, one cascade traced through the depth buffer and merged with the one above, and each pixel's light gathered from the first |
| `scene/light_box_bake.slang`, `scene/light_boxes.slang` | Light boxes: the bake (paths from each voxel's middle over the sphere, into first-order harmonics; voxels inside something marked) and the lookup (`light_box_light`: the finest box holding a point, eight voxels weighed by nearness, validity and facing, faded at the box's edge) |
| `scene/trace.slang`, `scene/trace_instances.slang`, `scene/trace_view.slang` | Rays against the scene (docs/design.md, Global illumination), where the GPU has ray queries: the nearest thing a ray meets (`trace_nearest`), whether anything is in its way (`trace_blocked`), and the surface at a hit (`trace_surface`: its place, normal and material, from the instance table, the part table and the geometry pool); the compute pass that writes the top level's instances from the instance table; and the `ray traced` debug view, a ray a pixel. The top level is made from its address (`RaytracingAccelerationStructure(address)`), like a buffer: no descriptor slot |
| `scene/particles_common.slang`, `scene/particles_step.slang`, `scene/particles.slang` | Particles (docs/design.md, VFX foundation): what the two programs share (a particle, an emitter instance, the module types, the random numbers that follow from a particle's seed); one fixed step in compute, a group to a block of the pool (births in their emitter's ring, then the step modules: acceleration, drag, turbulence, wind); and the draw, a quad to a particle (the draw modules and the lighting per particle in the vertex shader, the shape and the soft fades in the fragment shader) |
| `scene/taa.slang` | Temporal anti-aliasing: history reprojected by motion, clipped to the neighbourhood, dropped where object motion or depth says it was about something else; writes the object motion the next frame compares |
| `scene/outline.slang` | The editor's selection outline: an anti-aliased band around the selection's mask over the finished image, fainter where it's hidden |
| `scene/tonemap.slang` | Exposure and a filmic curve to the display, or a G-buffer channel, the shadow mask or the cascades (debug views) |
| `imgui.slang` | Dear ImGui's draw lists (the debug UI) |
| `debug/frame_debugger.slang`, `debug/frame_debugger_ids.slang` | The frame debugger's texture view: a texture of any shape drawn by texels (zoom, pan, a channel or several, a range of values), and the one texel under the mouse as stored; integer textures copied into floats first (compute) |
| `common/` | Modules shared by shaders: `common.bindless` (texture and sampler tables), `common.color`, `common.fog` (the fog medium, its light, its integral along a ray, the froxel slices, fog volumes' density and noise) |
| `tests/` | Shaders the unit tests run: a solid triangle, buffers through pointers, textures by bindless index, storage writes, rays against two instances of a triangle pair, a shader that binds a slot of its own (refused) |

A module's name is its path with dots: `common.color` is `common/color.slang`. Declarations other modules import are `public`.

## Conventions

- Entry points are marked with their stage: `[shader("vertex")] vertex_main`, `[shader("fragment")] fragment_main`, `[shader("compute")] compute_main` (the library's default names). A program can name others: the G-buffer's and the shadow casters' are `fragment_opaque` and `fragment_masked`.
- Push constants are one `[[vk::push_constant]] ConstantBuffer<T>`, at most 128 bytes, matched by a C++ struct where the program is used. Reflection reports each field's offset and size.
- Per-frame structs (`SceneFrame`) are read through their pointer, never copied whole into a local: a copy indexed at run time (by cascade) can land in scratch memory.
- No descriptor slots of their own: buffers are reached through pointers in push constants (`uint* output`), textures and samplers through `import common.bindless;` and indices in push constants. A shader that binds anything else to a slot compiles, but the library rejects it.
- Reach textures through the accessors, never the raw tables: `texture_2d(index)`, `texture_cube(index)`, `storage_2d(index)`, `sampler_state(sampler)`..., or the shorthands (`sample_texture(push.albedo, kSamplerAnisotropicWrap, uv)`). They check the index's table in debug and dev builds (`DE_SHADER_CHECKS`) and clamp it in every build, so a wrong index draws the fallback instead of reading out of bounds.
- A texture's index belongs to the table of its shape: `texture_2d` for 2D, `texture_cube` for cubes... Integer storage textures use `storage_2d_uint`.
- `SV_VertexID` and `SV_InstanceID` leave out a draw's first vertex, vertex offset and first instance. To read vertices from a shared buffer, or find a draw's data by its first instance, use `SV_VulkanVertexID` and `SV_VulkanInstanceID` (rhi `README.md`, Indirect draws).
- Clip space is +Y up, depth 0..1, reverse Z (1 near, 0 far). Front faces are counter-clockwise.
- Matrices are column-major: `mul(matrix, vector)`, the same layout as GLM.
- Names: `snake_case` functions and variables, `PascalCase` structs (docs/design.md, Code conventions).
- Permutations are explicit preprocessor defines (`SKINNED`), kept few. A variant that changes only one stage can be a second entry point instead, as the alpha test is (`fragment_masked`); cooked shaders have no permutations yet.
