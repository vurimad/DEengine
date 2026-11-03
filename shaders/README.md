# shaders

Every shader in Slang (docs/design.md, Shaders, materials & VFX). The engine compiles them from `src:/shaders/` in dev builds and rebuilds them live when a file changes; the cooker will turn them into `.deshader` files for ship builds (M0b).

## Layout

| Path | What |
| --- | --- |
| `scene/scene_common.slang` | What the scene passes share: frame constants, vertex and material layouts, G-buffer packing |
| `scene/gbuffer.slang` | The G-buffer pass: surfaces into the G-buffer, emissive into the HDR target |
| `scene/deferred_light.slang` | Deferred lighting: the sun (GGX, Lambert, wrap and backlight for translucency), hemisphere ambient, the sky |
| `scene/tonemap.slang` | Exposure and a filmic curve to the display, or a G-buffer channel (debug views) |
| `common/` | Modules shared by shaders: `common.bindless` (texture and sampler tables), `common.color` |
| `tests/` | Shaders the unit tests run: a solid triangle, buffers through pointers, textures by bindless index |

A module's name is its path with dots: `common.color` is `common/color.slang`. Declarations other modules import are `public`.

## Conventions

- Entry points are marked with their stage: `[shader("vertex")] vertex_main`, `[shader("fragment")] fragment_main`, `[shader("compute")] compute_main` (the library's default names).
- Push constants are one `[[vk::push_constant]] ConstantBuffer<T>`, at most 128 bytes, matched by a C++ struct where the program is used. Reflection reports each field's offset and size.
- No descriptor slots of their own: buffers are reached through pointers in push constants (`uint* output`), textures and samplers through `import common.bindless;` and indices in push constants. A shader that binds anything else to a slot compiles, but the library rejects it.
- Reach textures through the accessors, never the raw tables: `texture_2d(index)`, `texture_cube(index)`, `storage_2d(index)`, `sampler_state(sampler)`..., or the shorthands (`sample_texture(push.albedo, kSamplerAnisotropicWrap, uv)`). They check the index's table in debug and dev builds (`DE_SHADER_CHECKS`) and clamp it in every build, so a wrong index draws the fallback instead of reading out of bounds.
- A texture's index belongs to the table of its shape: `texture_2d` for 2D, `texture_cube` for cubes... Integer storage textures use `storage_2d_uint`.
- Clip space is +Y up, depth 0..1, reverse Z (1 near, 0 far). Front faces are counter-clockwise.
- Matrices are column-major: `mul(matrix, vector)`, the same layout as GLM.
- Names: `snake_case` functions and variables, `PascalCase` structs (docs/design.md, Code conventions).
- Permutations are explicit preprocessor defines (`SKINNED`, `ALPHA_TEST`), kept few.
