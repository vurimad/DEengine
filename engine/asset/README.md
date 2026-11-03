# asset

Cooked asset formats (docs/design.md, Asset formats): the files the cooker (`tools/cooker/`) writes and the engine loads in place, and the registry that loads them by ID.

## Contents

| File | What it gives |
| --- | --- |
| `asset_id.h` | `Guid` (a source's identity, in its meta) and `AssetId` (what runtime code loads by): the main asset's ID from its GUID, sub-assets' from a name, shaders' from the module name |
| `format.h` | `AssetHeader` (the common header), `OffsetSpan<T>`, `load_in_place<T>()`, `cooked_path()`, content hashes |
| `mesh_format.h` | `.demesh`: 32-byte vertices, 32-bit indices, LODs, parts, material slots, bounds |
| `texture_format.h` | `.detex`: a 2D texture with its mip chain, BCn or RGBA8 |
| `material_format.h` | `.demat`: template, parameters and textures by name; the PBR names the cooker writes |
| `shader_format.h` | `.deshader`: a module's entry points in SPIR-V per permutation, with reflection |
| `registry.h` | `AssetRegistry`: load by ID with dependencies, reference counts, states and events, hot reload |

## Rules and invariants

- A cooked file is read into 16-byte aligned memory and used where it lies. Arrays are `OffsetSpan`s (offsets from the start of the file, 16-byte aligned), so nothing is fixed up and a file can be read anywhere.
- `load_in_place<T>()` checks before anything is used: the header (magic, type, format version, platform, size, ID), that every array lies inside the file, and what consumers size or address things by (texture sides at most 32768, mesh indices below the vertex count, SPIR-V's magic word). A fuzz test (`tests/asset/asset_fuzz_test.cpp`) holds it to that. It doesn't hash the file: `verify_content_hash()` is the corruption check.
- Every file starts with `AssetHeader`: magic `DEAS`, type, format version, platform, file size, asset ID, content hash (of every byte after the header) and its dependencies (asset ID and type, which names the file).
- A format change bumps that type's `kFormatVersion`: old files are refused (`Unsupported`) and cooked again, never patched.
- Numbers stored in files (`AssetType`, `TextureFormat`, `ShaderStageKind`...) are never renumbered, only added to.
- Little-endian only (checked at compile time).

## The registry

- `load(ref)` holds an asset and returns its handle at once; the read goes straight into the asset's memory (VFS, with the IO priority asked for), is checked with `load_in_place` on a job, and is committed by `update()` with the other finished reads, once per frame, on the registry's thread. Its header's dependencies are then loaded and held for it.
- States: `Loading`, then `Ready` once its file is in and every dependency is `Ready` or `Failed`; `Failed` when the file is missing, unreadable, corrupt or of another type, with an error naming the asset and its file. A failed dependency doesn't fail what uses it: the renderer draws a placeholder.
- Every `load()` needs a `release()`. An asset nobody holds is unloaded at the next `update()`, and lets go of its dependencies, which may go too. One let go while its read is in flight goes once the read is in.
- Listeners hear `Ready`, `Failed`, `Reloaded` and `Unloaded`, in order, from `update()`. Pointers from `get()` and `file()` stay valid until an `update()` reports `Reloaded` or `Unloaded` for that asset.
- Hot reload (dev builds): each `update()` checks a few held files (round robin) for a new size or write time. A changed file is read and checked in the background and swapped in place: the handle stays, the contents and dependencies change (new ones held before old ones let go). A broken version is logged and the loaded one kept; a failed asset whose file appears is loaded.
- An idle `update()` allocates nothing; finished reads cross from jobs through one lock and two queues that trade places.
- Not yet: OS file watching instead of polling, budgets and eviction by priority (M1 streaming), raising the priority of a read already queued, archives.

## Vertex layout (`MeshVertex`, 32 bytes)

| Bytes | Field | Encoding |
| --- | --- | --- |
| 0-11 | position | 3 float, meters |
| 12-15 | normal | octahedral, two snorm16 (x low, y high) |
| 16-19 | tangent | snorm8 x, y, z, w (w: bitangent sign; bitangent = cross(normal, tangent) * w) |
| 20-27 | uv | 2 float, origin top left |
| 28-31 | color | rgba8 unorm |
