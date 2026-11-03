# asset

Cooked asset formats (docs/design.md, Asset formats): the files the cooker (`tools/cooker/`) writes and the engine loads in place. No loading or caching here: the asset registry (M0b) builds on it.

## Contents

| File | What it gives |
| --- | --- |
| `asset_id.h` | `Guid` (a source's identity, in its meta) and `AssetId` (what runtime code loads by): the main asset's ID from its GUID, sub-assets' from a name, shaders' from the module name |
| `format.h` | `AssetHeader` (the common header), `OffsetSpan<T>`, `load_in_place<T>()`, `cooked_path()`, content hashes |
| `mesh_format.h` | `.demesh`: 32-byte vertices, 32-bit indices, LODs, parts, material slots, bounds |
| `texture_format.h` | `.detex`: a 2D texture with its mip chain, BCn or RGBA8 |
| `material_format.h` | `.demat`: template, parameters and textures by name; the PBR names the cooker writes |
| `shader_format.h` | `.deshader`: a module's entry points in SPIR-V per permutation, with reflection |

## Rules and invariants

- A cooked file is read into 16-byte aligned memory and used where it lies. Arrays are `OffsetSpan`s (offsets from the start of the file, 16-byte aligned), so nothing is fixed up and a file can be read anywhere.
- `load_in_place<T>()` checks before anything is used: the header (magic, type, format version, platform, size, ID), that every array lies inside the file, and what consumers size or address things by (texture sides at most 32768, mesh indices below the vertex count, SPIR-V's magic word). A fuzz test (`tests/asset/asset_fuzz_test.cpp`) holds it to that. It doesn't hash the file: `verify_content_hash()` is the corruption check.
- Every file starts with `AssetHeader`: magic `DEAS`, type, format version, platform, file size, asset ID, content hash (of every byte after the header) and its dependencies (asset IDs).
- A format change bumps that type's `kFormatVersion`: old files are refused (`Unsupported`) and cooked again, never patched.
- Numbers stored in files (`AssetType`, `TextureFormat`, `ShaderStageKind`...) are never renumbered, only added to.
- Little-endian only (checked at compile time).

## Vertex layout (`MeshVertex`, 32 bytes)

| Bytes | Field | Encoding |
| --- | --- | --- |
| 0-11 | position | 3 float, meters |
| 12-15 | normal | octahedral, two snorm16 (x low, y high) |
| 16-19 | tangent | snorm8 x, y, z, w (w: bitangent sign; bitangent = cross(normal, tangent) * w) |
| 20-27 | uv | 2 float, origin top left |
| 28-31 | color | rgba8 unorm |
