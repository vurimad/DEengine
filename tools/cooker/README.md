# tools/cooker

The asset cooker (docs/design.md, Asset formats): turns the source tree into the cooked files the engine loads. A library (`de::cooker`, so the editor can run it on file changes and tests can drive it) and the `cooker` command line.

```
cooker [--project DIR] [--out DIR] [--check] [--force] [--no-shaders] [--shader-debug] [--shader-checks]
```

| Flag | Meaning |
| --- | --- |
| `--project DIR` | the project to cook, its folder or `.deproject` file, relative to the working folder or the repository (default: the build's, `projects/sandbox`); its sources are `src:/`, the engine's shaders `engine:/` |
| `--out DIR` | where cooked files go, mounted as `game:/` (default: the project's `cooked/`, which the engine mounts when it opens the project) |
| `--check` | check the source and meta trees, report stale cooked shaders and collision, read every terrain source and check what's cooked from it, and write nothing; exit code 1 on errors (CI). With nothing cooked there's nothing stale |
| `--force` | cook every source again |
| `--no-shaders` | skip shaders (no Slang compiler needed), in `--check` too |
| `--shader-debug`, `--shader-checks` | source-level debug info, `DE_SHADER_CHECKS=1` in cooked shaders |

The editor runs it too: after a save, at its Cook button, and by itself when the project's files change (`engine/editor/README.md`). It exits with 0 when there are no errors, and writes a JSON report (counts and every message, with the file it is about) to `<out>/cook_report.json`.

## Contents

| File | What it does |
| --- | --- |
| `cooker.h` | `cook()`: the whole run, and the JSON report |
| `source_tree.h` | `SourceTree`: the source and meta trees matched, metas minted and re-linked, folder rules applied |
| `cook_state.h` | `CookState`: what the last cook made from what (`game:/cook_state.json`) |
| `gltf_import.cpp`, `fbx_import.cpp`, `model.h` | glTF (cgltf) and FBX (ufbx) to one in-memory model |
| `mesh_cooker.h` | `.demesh`: merged, optimized and LOD-chained with meshoptimizer |
| `collision_cooker.h` | `.dephys`: a cooked mesh's collision, through `physics/jolt_shapes.h` |
| `thumbnail_cooker.h` | the pictures the editor's resources show (`asset::thumbnail_asset_id`, 128 x 128 BC7): a model from its cooked mesh and its materials, a texture fitted to the square, a text material as a lit sphere; drawn on the CPU |
| `texture_cooker.h` | `.detex`: mips filtered with stb_image_resize2, then BC1/BC4/BC5 (rgbcx) or BC7 (bc7enc) |
| `material_cooker.h` | `.demat`: from model materials and `.material` text sources |
| `cell_cooker.h` | `.decell`: from `.cell` text sources (the format is described there) |
| `world_cooker.h` | `.deworld` and `.deenv`: from `.world` and `.environment` text sources (the formats are described there) |
| `prefab_cooker.h` | `.deprefab`: from `.prefab` text sources (the format is described there); `entity_transform.h` reads a part's or an entity's position, rotation and scale, `light_source.h` its `"light"` |
| `effect_cooker.h` | `.devfx`: from `.vfx` text sources (the format and every module are described there) |
| `terrain_cooker.h` | `.deterrain` and `.deheights`: the height pyramid, from `.terrain` text sources and their 16-bit height images (the format is described there) |
| `shader_cooker.h` | `.deshader`: every entry point of a Slang module, with reflection |
| `blob_writer.h` | `BlobWriter`: the load-in-place layout and common header of every cooked file |
| `json.h`, `settings.h` | yyjson on the Tools heap; the `texture` and `mesh` settings |
| `common.h` | What every step shares: source paths, `Diagnostics` (the findings, each with its file), the Tools heap |
| `third_party.h` | Routes the libraries with process-wide allocator hooks (meshoptimizer) to the Tools heap |

## Sources, metas and settings

- Sources live under `content/` (lowercase names, forward slashes; uppercase or spaces are reported and skipped). Kinds by extension: `.png .jpg .jpeg .tga .bmp` textures, `.gltf .glb` and `.fbx` models, `.material` text materials, `.cell` cells, `.world` worlds, `.environment` environments, `.terrain` terrains, `.vfx` effects, `.prefab` prefabs. `.bin` files (glTF buffers) are read by their models and `.heightpng` files (16-bit height images) by their terrains, so neither has a meta; other files are ignored with a warning.
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
- A glTF material that transmits light (`KHR_materials_transmission`) and isn't already BLEND becomes one that blends: nothing bends light through a surface yet, so glass is mixed over what's behind it instead, giving up to 85% of its opacity as its transmission goes to 1. The transparent pass draws it (`engine/render/scene/README.md`).
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
| `lod_count` | 1 to 8, LOD 0 included | 8 |
| `lod_ratio` | triangles kept from one LOD to the next | 0.5 |
| `lod_error` | largest error a LOD may add, relative to the mesh's size. The renderer picks a LOD only where its error stays under a pixel, so a large error costs nothing up close | 0.5 |
| `optimize` | vertex cache, overdraw and fetch order | true |
| `collision_triangles` | its `.dephys` is the finest LOD with at most this many triangles | 2048 |
| `wind` | what the vertex colors tell the wind (`shaders/scene/wind.slang`): `keep` the source's; `height`, up the plant, for grass tufts and small plants; `bake`, from its connected pieces (a leaf, a twig, a branch, each about its lowest point; a piece most of the plant's height hardly flutters), for anything with leaves and branches. Red is how far a vertex flutters, green its piece's phase, blue how far that piece is from being the main stem. Baked on the full mesh, so the LOD chain carries it | `keep` |

## What a source cooks to

Cooked files are named by asset ID: `game:/3f/3f8a0c1e5b2d4e61.demesh`. A source's main asset takes the ID of its GUID; the others take IDs derived from it and a name (`asset/asset_id.h`).

| Source | Cooked |
| --- | --- |
| Texture | a `.detex`, and its picture for the editor (fitted to the square, its texels as they are; a texture cooked before pictures existed gets one at the next cook without being encoded again) |
| glTF, FBX | a `.demesh` (the default scene flattened into one mesh, a part per material), a `.dephys` (`collision`: see below), a `.demat` per material (`material/<name>`, or `material/#<index>` for unnamed or repeated names, and names starting with `#`), a `.detex` per embedded image (`image/<n>`, by the image's index in the file, so material order doesn't change it), and its picture for the editor (`thumbnail`, `asset::thumbnail_asset_id`: a 128 x 128 BC7 sRGB `.detex`, one mip, clear round the model; none if there's nothing to see) |
| `.material` | a `.demat`, and its picture (a sphere wearing it; its textures' files are among its inputs, so an edit to one draws it again) |
| `.cell` | a `.decell`: its entities, parents first, and their fog volumes (shape, density, edge, tint, noise, drift, height fade) and water bodies (`"water": {"shape": "ellipse" or "rectangle", "waves": 0 to 1}`), with the meshes of the models they name (relative paths) as its dependencies, and the `.dephys` of those that ask with `"collision": "mesh"` after them |
| `.world` | a `.deworld`: its cells' IDs and coordinates (read from the cells, sorted), its start, its environment and optionally its terrain as its dependencies; cooked again when a cell it lists changes (its coordinate may have), not when the environment or the terrain does |
| `.environment` | a `.deenv`: colors times intensities, the sun's direction from elevation and azimuth, the shadow and fog settings within their limits |
| `.prefab` | a `.deprefab`: its parts flat, those of the prefabs it nests among its own (each named by its path of IDs and placed from this prefab's origin; a prefab inside itself, or more than 8 deep, is an error), its variants as a bit in each part they leave out. The prefabs it nests are read to flatten it, so it cooks again when one changes; the models and effects it names only by their IDs. A cell's entity places one with `"prefab"` (and `"variant"`): the prefab becomes one of the cell's dependencies, and the cell cooks again when the prefab's variants are renamed, not when anything else in it changes |
| `.vfx` | a `.devfx`: each emitter's modules sorted into what runs at birth, at each step and at the draw, in the file's order; its capacity worked out from its rate and longest lifetime, and how far its particles can get from it (`reach_m`) from what launches, pushes and slows them, unless the file says. Every mistake is reported with its emitter and module (`emitter 'flame', module 2 (turbulence): size_m should be...`). A cell's entity names one with `"effect"` (a relative path): it becomes one of the cell's dependencies, after its meshes and their collision |
| `.terrain` | a `.deterrain` (levels, nodes, patch bounds: small, loaded with its world) and a `.deheights` (the blocks, read one at a time), both under its ID |
| `config/**` | copied as is to `game:/config/` when its bytes differ (input bindings and engine settings: what ship builds read) |
| `shaders/**.slang` with `[shader("...")]` entry points (outside comments, spaced any way) | a `.deshader` (ID from the module name, `shader_asset_id("imgui")`); library modules without entry points and `shaders/tests/` are skipped |

## Collision

A `.dephys` is the binary state of a Jolt `MeshShape` (`Shape::SaveBinaryState`), made by `physics::build_mesh_shape`, so the cooker doesn't see Jolt's types or need its own copy of its setup.

- **From the LOD chain,** not a simplification of its own: the finest LOD with at most `collision_triangles` triangles is the closest to what's drawn that collision can afford, and it's already there. With no LOD that small, it's the coarsest, with a warning. The default of 2048 gives 1,000 to 2,000 triangles on a prop, and a plane stays its two. On the boulder that's 1,032 of its 66,122: 16 KB, and on rays straight down 1.6 cm from the full mesh on average, 0.9 cm at the median and 12 cm at the 99th percentile, those being rays just past its outline. Its limit is there for what's walked on: a character's cost goes with the triangles under its capsule (`engine/physics/README.md`), far more than a ray's. Raise it for a big mesh that needs the detail.
- **Welded.** LODs split vertices along uv seams and between materials, so all a LOD's parts go into one list with each position once. Jolt's smooth edges go by shared indices: every split vertex would be an open edge for things to catch on.
- **Reproducible,** like everything: the same mesh makes the same bytes.
- **A mesh with no triangles of any area** has no `.dephys`, with a warning: what's only drawn isn't wrong. A cell that asks for it finds out when it's loaded.
- **Jolt's version** is in the file (`shape_format`) and in the model's input hash, so a new Jolt cooks every model again, and the engine refuses what another wrote. It isn't `JPH_VERSION_ID`, which also changes with asserts and profiling: a debug cooker's files have to work in a dev engine.
- **`--check`** hashes each model as a cook would and compares that with the last cook: that covers its bytes, settings, buffers and GUID. A collision file gone, a model no cook has seen or one deleted are stale too. With no collision cooked at all, there's nothing to be stale.

## Rules and invariants

- The same inputs give the same bytes: no timestamps or paths in cooked files, padding zeroed.
- A source cooks again when anything its outputs depend on changes: its GUID, its bytes, its resolved settings, files it reads (glTF buffers, shader imports, a material's textures for its picture), the cooker's and the formats' versions, or (for models and materials) the IDs of the textures it references, or (for cells) the IDs of the models it places: a model cooked again doesn't cook its cells again. Otherwise it's skipped, as long as its cooked files exist. Cooked files no source makes any more are removed.
- `game:/shader_sources.hash` names the shader sources of everything in `game:/` by `asset::shader_sources_hash()`: every `.slang` under `shaders/` but `shaders/tests/`, by path and content, with CRLF read as LF. It's removed when a cook of the shaders starts and written when every module made it (test modules aside) and the sources hash as they did at the start, so a cook that fails, stops halfway or sees a file saved meanwhile leaves none, and the next one cooks every module. If it can't be removed, no shaders are cooked and the cook fails. It comes from the sources this run read, not from this cooker's own build. The engine refuses cooked shaders whose stamp doesn't match its build (`engine/render/shader/README.md`), and `--check` compares it with the sources as they are now; `.deshader` files without one are stale too.
- A failed source keeps its last cooked files and texture uses, and is tried again next time. `--no-shaders` leaves cooked shaders as they are; `CookOptions::content = false` (shaders only, for tests) leaves what was cooked from `content/` as it is.
- A glTF buffer shorter than the glTF says (a Git LFS pointer that was never pulled) is reported, never read past.
- LODs never move vertices where two material parts meet, so they don't crack along seams.
- A LOD says whether its seams moved (`MeshLod::seams_moved`). A mesh whose vertices are split all over (a scan or a generated model, its uvs in small islands) only simplifies if its seams may move, and then has triangles across two islands: fine drawn small, wrong for whatever reads a colour off its surface up close, as a ray does (`render::trace_lod` takes a finer LOD). It's marked by what happened, not by what was allowed: LOD 0's triangles join its vertices into pieces (an island, a face with normals of its own), and a LOD with a triangle across two pieces is marked. A seam an island has with itself (a sleeve's) isn't caught: it's one piece.
- Files go through the VFS only; third-party libraries allocate on the Tools heap (yyjson, cgltf, ufbx and stb per call, meshoptimizer process-wide).
- Texture blocks encode in parallel on the job system. Shader compiles run one at a time (Slang's global session).
- Each file is read once per run: importers get the bytes the cooker hashed, and shared shader imports are hashed once.
- Mips of color textures are filtered alpha-weighted and sRGB-correct; data textures filter each channel on its own.
- One ID can name two files of different types (a terrain and its heights), so a cook record's outputs, and what's removed as stale, go by ID and type.

## Terrain

`engine/asset/terrain_format.h` is the format; these are the cook's rules.

- A map's size comes from its image: `width_m` is (samples - 1) times `meters_per_pixel`, and the same for depth. Maps needn't be square. `meters_per_pixel` must be 16 divided by a power of 2, down to 0.125, and levels run from 16 m to it (2 m gives 4 levels). Each level's grid is every node touching the map: one ending exactly on a node's edge has no node past it.
- A level's sample is exactly the source sample at that point, unfiltered, so a coarse vertex stands where a fine one does. Past the map's edge it's the nearest source sample, which may not be on the level's grid.
- Samples round to their level's step as whole steps across the level, so neighbours share edges and borders to the bit. A node's `base_steps` is the lowest in its block, border included. A block whose heights don't fit 16 bits above its base fails the cook, naming the level, the worst node, where it is, how far it rises and by how much too far.
- A patch's `min_m` and `max_m` are its samples and the coarser level's samples in its area, rounded outward as floats, without its border: morphing goes from one toward the other, and a coarse sample can lie outside the fine ones by its rounding. Skirts aren't counted: whatever hangs them adds their depth.
- Height images are 16-bit grayscale PNGs (stb's limit: under 2 GB of samples) with the extension `.heightpng`, so they aren't cooked as textures. Anything else is refused: 8 bits, other channel counts, under 2 x 2 samples. The range is required, since a PNG doesn't store one.
- A terrain cooks again when its GUID, its text, its image's bytes or the format's version change, or a cooked file is gone. The image is read once per run, for its hash and its samples. The heights are written first, so a new `.deterrain` is never on disk before the heights it names. Nodes cook on jobs: each writes its own block and patches, so the bytes don't depend on the threads.
- `--check` reads each terrain source and its image's header, and, for what was cooked, compares the source's inputs with the record's, loads the `.deterrain`, and checks the `.deheights` against it: its header, its length and the hash of its bytes. It reports cooked files whose source has gone too.
- `tools/terrain_testmap.py` generates the capacity test map, 16 x 16 km at 2 m with a few steep ranges: what the engine must carry at least. Real maps and that one live in `content/terrain/` and `meta/terrain/`, which aren't committed.

## Not yet

- MikkTSpace tangents (generated tangents use per-triangle UV directions), KHR_texture_transform, Draco and meshopt-compressed glTF, KTX2 and WebP images, glTF scenes as prefabs instead of one flattened mesh.
- FBX: separate roughness and metalness textures packed into one; skinning and animation (with `.deskel` and `.deanim`).
- HDR sources (EXR through tinyexr, BC6H), alpha-coverage-preserving mips for foliage, a quality setting for the BC7 encoder.
- Shader permutations: modules cook with no defines; a list of permutations per module comes with the materials that need them.
- Removing files deleted from `config/` from `game:/config/`; checking config files (`--check`).
- Cooking in parallel across sources, `de asset move`, and the archive packer (which has to carry `game:/shader_sources.hash` along).
- Leaving out collision that nothing asks for; hand-made collision meshes; a material per triangle.
