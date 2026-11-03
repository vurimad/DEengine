# tools/cooker

The asset cooker (docs/design.md, Asset formats): turns the source tree into the cooked files the engine loads. A library (`de::cooker`, so the editor can run it on file changes and tests can drive it) and the `cooker` command line.

```
cooker [--src DIR] [--out DIR] [--check] [--force] [--no-shaders] [--shader-debug] [--shader-checks]
```

| Flag | Meaning |
| --- | --- |
| `--src DIR` | the repository root, mounted as `src:/` (default: this repository) |
| `--out DIR` | where cooked files go, mounted as `game:/` (default: `<src>/cooked`, which the engine mounts by default) |
| `--check` | check the source and meta trees and write nothing; exit code 1 on errors (CI) |
| `--force` | cook every source again |
| `--no-shaders` | skip shaders (no Slang compiler needed) |
| `--shader-debug`, `--shader-checks` | source-level debug info, `DE_SHADER_CHECKS=1` in cooked shaders |

It exits with 0 when there are no errors, and writes a JSON report (counts and every message, with the file it is about) to `<out>/cook_report.json`.

## Contents

| File | What it does |
| --- | --- |
| `cooker.h` | `cook()`: the whole run, and the JSON report |
| `source_tree.h` | `SourceTree`: the source and meta trees matched, metas minted and re-linked, folder rules applied |
| `cook_state.h` | `CookState`: what the last cook made from what (`game:/cook_state.json`) |
| `gltf_import.cpp`, `fbx_import.cpp`, `model.h` | glTF (cgltf) and FBX (ufbx) to one in-memory model |
| `mesh_cooker.h` | `.demesh`: merged, optimized and LOD-chained with meshoptimizer |
| `texture_cooker.h` | `.detex`: mips filtered with stb_image_resize2, then BC1/BC4/BC5 (rgbcx) or BC7 (bc7enc) |
| `material_cooker.h` | `.demat`: from model materials and `.material` text sources |
| `cell_cooker.h` | `.decell`: from `.cell` text sources (the format is described there) |
| `shader_cooker.h` | `.deshader`: every entry point of a Slang module, with reflection |
| `blob_writer.h` | `BlobWriter`: the load-in-place layout and common header of every cooked file |
| `json.h`, `settings.h` | yyjson on the Tools heap; the `texture` and `mesh` settings |

## Sources, metas and settings

- Sources live under `content/` (lowercase names, forward slashes; uppercase or spaces are reported and skipped). Kinds by extension: `.png .jpg .jpeg .tga .bmp` textures, `.gltf .glb` and `.fbx` models, `.material` text materials, `.cell` cells. `.bin` files (glTF buffers) are read by their models; other files are ignored with a warning.
- Every source has a meta at the same path under `meta/`, plus `.json`: `content/rocks/rock.png` has `meta/rocks/rock.png.json`. A meta holds the source's GUID, and the settings sections (`texture`, `mesh`) that differ from the folder rules. New sources get a meta with a fresh GUID on the next cook. Metas are committed with their sources.
- A source moved without its meta is found again by content (the last cook's hash) and its meta moved after it. A meta whose source can't be found is reported, and a new file that may be that source gets no GUID until it's settled: a GUID is never silently replaced, so references never break.
- Two metas with one GUID (a copied meta): the file the last cook knew by that GUID keeps it, and the other is reported; deleting that one's meta gives it a GUID of its own. Deleting a meta re-cooks its source under the new ID and removes the old ID's files.
- A source left out for its meta (unreadable, a copied GUID, not yet minted) keeps what was cooked from it until it's sorted out.
- Folder rules, `meta/**/_folder.json`, set defaults for a folder and everything below it:

  ```json
  {"rules": [{"match": "*_normal.*", "texture": {"compression": "bc5", "srgb": false, "normal_map": true}}]}
  ```

  `match` is a pattern on the file name (`*` any run, `?` one character). Settings are layered as JSON merge patches: built-in defaults, then the rules from the root down (each file's in order), then the meta's own section. Sections must be objects (anything else would replace every layer below it); a rules file that isn't `{"rules": [...]}` with a `match` per rule is reported and ignored.
- A model's embedded images take their defaults from use, then the `texture` section of the model's own meta. Folder rules don't apply to them: their patterns describe file names, and the model's name says nothing about its images.
- Texture defaults come from how models and materials use a texture: base color and emissive are sRGB BC7, normal maps BC5, other maps linear BC7. Folder rules and metas override them.

| `texture` setting | Values | Default |
| --- | --- | --- |
| `compression` | `bc7`, `bc5`, `bc4`, `bc1`, `none` | from the use, else `bc7` |
| `srgb` | true, false | from the use, else true |
| `mips` | true, false | true |
| `normal_map` | true, false: renormalize mips | from the use |
| `max_size` | largest side in texels, 0 for no limit | 0 |

| `mesh` setting | Values | Default |
| --- | --- | --- |
| `scale` | multiplier on top of the importer's conversion to meters | 1 |
| `lod_count` | 1 to 8, LOD 0 included | 4 |
| `lod_ratio` | triangles kept from one LOD to the next | 0.5 |
| `lod_error` | largest error a LOD may add, relative to the mesh's size | 0.02 |
| `optimize` | vertex cache, overdraw and fetch order | true |

## What a source cooks to

Cooked files are named by asset ID: `game:/3f/3f8a0c1e5b2d4e61.demesh`. A source's main asset takes the ID of its GUID; the others take IDs derived from it and a name (`asset/asset_id.h`).

| Source | Cooked |
| --- | --- |
| Texture | a `.detex` |
| glTF, FBX | a `.demesh` (the default scene flattened into one mesh, a part per material), a `.demat` per material (`material/<name>`, or `material/#<index>` for unnamed or repeated names, and names starting with `#`), a `.detex` per embedded image (`image/<n>`, by the image's index in the file, so material order doesn't change it) |
| `.material` | a `.demat` |
| `.cell` | a `.decell`: its entities, parents first, with the meshes of the models they name (relative paths) as its dependencies |
| `shaders/**.slang` with `[shader("...")]` entry points (outside comments, spaced any way) | a `.deshader` (ID from the module name, `shader_asset_id("imgui")`); library modules without entry points and `shaders/tests/` are skipped |

## Rules and invariants

- The same inputs give the same bytes: no timestamps or paths in cooked files, padding zeroed.
- A source cooks again when anything its outputs depend on changes: its GUID, its bytes, its resolved settings, files it reads (glTF buffers, shader imports), the cooker's and the formats' versions, or (for models and materials) the IDs of the textures it references, or (for cells) the IDs of the models it places: a model cooked again doesn't cook its cells again. Otherwise it's skipped, as long as its cooked files exist. Cooked files no source makes any more are removed.
- A failed source keeps its last cooked files and texture uses, and is tried again next time. `--no-shaders` leaves cooked shaders as they are; `CookOptions::content = false` (shaders only, for tests) leaves what was cooked from `content/` as it is.
- A glTF buffer shorter than the glTF says (a Git LFS pointer that was never pulled) is reported, never read past.
- LODs never move vertices where two material parts meet, so they don't crack along seams.
- Files go through the VFS only; third-party libraries allocate on the Tools heap (yyjson, cgltf, ufbx and stb per call, meshoptimizer process-wide).
- Texture blocks encode in parallel on the job system. Shader compiles run one at a time (Slang's global session).
- Each file is read once per run: importers get the bytes the cooker hashed, and shared shader imports are hashed once.
- Mips of color textures are filtered alpha-weighted and sRGB-correct; data textures filter each channel on its own.

## Not yet

- MikkTSpace tangents (generated tangents use per-triangle UV directions), KHR_texture_transform, Draco and meshopt-compressed glTF, KTX2 and WebP images, glTF scenes as prefabs instead of one flattened mesh.
- FBX: separate roughness and metalness textures packed into one; skinning and animation (with `.deskel` and `.deanim`).
- HDR sources (EXR through tinyexr, BC6H), alpha-coverage-preserving mips for foliage, a quality setting for the BC7 encoder.
- Shader permutations: modules cook with no defines; a list of permutations per module comes with the materials that need them.
- Running on file change (the editor), cooking in parallel across sources, `de asset move`, and the archive packer.
