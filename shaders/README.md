# shaders

Every shader in Slang (docs/design.md, Shaders, materials & VFX). The engine compiles them from `src:/shaders/` in dev builds and rebuilds them live when a file changes; the cooker will turn them into `.deshader` files for ship builds (M0b).

## Layout

| Path | What |
| --- | --- |
| `triangle.slang` | The sandbox triangle (M0a) |
| `textured_quad.slang` | The sandbox's textured quad: a bindless texture (M0a) |
| `common/` | Modules shared by shaders: `common.bindless` (texture and sampler tables), `common.color` |
| `tests/` | Shaders the unit tests run: a solid triangle, buffers through pointers, textures by bindless index |

A module's name is its path with dots: `common.color` is `common/color.slang`. Declarations other modules import are `public`.

## Conventions

- Entry points are marked with their stage: `[shader("vertex")] vertex_main`, `[shader("fragment")] fragment_main`, `[shader("compute")] compute_main` (the library's default names).
- Push constants are one `[[vk::push_constant]] ConstantBuffer<T>`, at most 128 bytes, matched by a C++ struct where the program is used. Reflection reports each field's offset and size.
- No descriptor slots of their own: buffers are reached through pointers in push constants (`uint* output`), textures and samplers through `import common.bindless;` and indices in push constants (`sample_texture(push.albedo, kSamplerAnisotropicWrap, uv)`). A shader that binds anything else to a slot compiles, but the library rejects it.
- A texture's index works in the table of its shape only: `de_textures` for 2D, `de_cube_textures` for cubes...
- Clip space is +Y up, depth 0..1, reverse Z (1 near, 0 far). Front faces are counter-clockwise.
- Matrices are column-major: `mul(matrix, vector)`, the same layout as GLM.
- Names: `snake_case` functions and variables, `PascalCase` structs (docs/design.md, Code conventions).
- Permutations are explicit preprocessor defines (`SKINNED`, `ALPHA_TEST`), kept few.
