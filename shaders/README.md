# shaders

Every shader in Slang (docs/design.md, Shaders, materials & VFX). The engine compiles them from `src:/shaders/` in dev builds and rebuilds them live when a file changes. The cooker compiles them into `.deshader` files, which ship builds (and dev builds without `src:/`) load instead (`engine/render/shader/README.md`).

## Layout

| Path | What |
| --- | --- |
| `scene/scene_common.slang` | What the scene passes share: frame constants, vertex and material layouts, G-buffer packing |
| `scene/shadow_caster.slang` | The sun's shadow casters: depth into one cascade's tile of the shadow atlas, alpha-tested for masked materials |
| `scene/gbuffer.slang` | The G-buffer pass: surfaces into the G-buffer, emissive into the HDR target; `fragment_opaque` (no discard) and `fragment_masked` (alpha test) |
| `scene/shadow_mask.slang` | The sun's shadow mask: the cascades resolved per pixel, with a contact-hardening filter |
| `scene/deferred_light.slang` | Deferred lighting: the sun (GGX, Lambert, wrap and backlight for translucency) through the shadow mask, hemisphere ambient, the sky |
| `scene/fog_inject.slang`, `scene/fog_integrate.slang` | The froxel grid (compute): each froxel's extinction and light, with the sun's shadows; then each column integrated front to back |
| `scene/fog.slang` | Fog over the lit image and the sky: the grid as far as it reaches, closed form past it (`common.fog`) |
| `scene/tonemap.slang` | Exposure and a filmic curve to the display, or a G-buffer channel, the shadow mask or the cascades (debug views) |
| `imgui.slang` | Dear ImGui's draw lists (the debug UI) |
| `common/` | Modules shared by shaders: `common.bindless` (texture and sampler tables), `common.color`, `common.fog` (the fog medium, its light, its integral along a ray, the froxel slices) |
| `tests/` | Shaders the unit tests run: a solid triangle, buffers through pointers, textures by bindless index, storage writes, a shader that binds a slot of its own (refused) |

A module's name is its path with dots: `common.color` is `common/color.slang`. Declarations other modules import are `public`.

## Conventions

- Entry points are marked with their stage: `[shader("vertex")] vertex_main`, `[shader("fragment")] fragment_main`, `[shader("compute")] compute_main` (the library's default names). A program can name others: the G-buffer's and the shadow casters' are `fragment_opaque` and `fragment_masked`.
- Push constants are one `[[vk::push_constant]] ConstantBuffer<T>`, at most 128 bytes, matched by a C++ struct where the program is used. Reflection reports each field's offset and size.
- Per-frame structs (`SceneFrame`) are read through their pointer, never copied whole into a local: a copy indexed at run time (by cascade) can land in scratch memory.
- No descriptor slots of their own: buffers are reached through pointers in push constants (`uint* output`), textures and samplers through `import common.bindless;` and indices in push constants. A shader that binds anything else to a slot compiles, but the library rejects it.
- Reach textures through the accessors, never the raw tables: `texture_2d(index)`, `texture_cube(index)`, `storage_2d(index)`, `sampler_state(sampler)`..., or the shorthands (`sample_texture(push.albedo, kSamplerAnisotropicWrap, uv)`). They check the index's table in debug and dev builds (`DE_SHADER_CHECKS`) and clamp it in every build, so a wrong index draws the fallback instead of reading out of bounds.
- A texture's index belongs to the table of its shape: `texture_2d` for 2D, `texture_cube` for cubes... Integer storage textures use `storage_2d_uint`.
- Clip space is +Y up, depth 0..1, reverse Z (1 near, 0 far). Front faces are counter-clockwise.
- Matrices are column-major: `mul(matrix, vector)`, the same layout as GLM.
- Names: `snake_case` functions and variables, `PascalCase` structs (docs/design.md, Code conventions).
- Permutations are explicit preprocessor defines (`SKINNED`), kept few. A variant that changes only one stage can be a second entry point instead, as the alpha test is (`fragment_masked`); cooked shaders have no permutations yet.
