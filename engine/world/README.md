# world

The game world (docs/design.md, Core systems; Open world & streaming: world model): one flecs world, its phases and fixed tick, transforms, cells, and the render extraction that hands the renderer what to draw.

## Contents

| File | What it gives |
| --- | --- |
| `world.h` | `World`: the flecs world on the engine's memory, log and jobs; the phases; the fixed 60 Hz tick with interpolation; `render_frame()` |
| `transform.h` | `Transform` (local, double position), `WorldTransform` and `PreviousWorldTransform` (computed), `StaticTransform`, `compose`, `interpolate`, `place`, `attach`, `detach` |
| `cell.h` | `Cell`, `InCell`, `load_cell` (spawns a cooked cell's members once it's loaded), `create_cell`, `add_to_cell`: cells own their entities and the assets they use |
| `render_extract.h` | `MeshInstance`, `Camera`, `ActiveCamera`, and `RenderFrame`: instances and camera as floats relative to a render origin |
| `flecs_os.h` | flecs on the engine's allocator and log, with its own threads off |

## Rules and invariants

- **One owner per thing, data flowing one way.** The flecs world is the only list of what exists. Nothing mirrors it: the renderer reads `render_frame()`, rebuilt every frame by RenderExtract.
- **Positions are doubles** (`Transform.position`, `WorldTransform.position`); rotation and scale are float. The world never rebases. Render extraction is the only place doubles become floats, relative to a render origin that follows the camera in 1024 m steps (`origin_moves` counts the moves: GPU copies made against the old origin are stale).
- **Transforms:** `Transform` is relative to the entity's parent if that parent has a transform, and to the world otherwise (a name scope or a grouping entity makes a root, in either hierarchy). It brings `WorldTransform` and `PreviousWorldTransform` with it. World transforms are computed by one system in a phase of its own after PostSim, so they follow every game system, whenever it was made: roots copy theirs only in tables where `Transform` changed; children (attachments, prefab parts: few) compose their parent's every tick, parents first. `PreviousWorldTransform` is caught up first in Input, only for tables whose world transform changed.
- **Moving things at once:** `place()` (spawn, teleport) sets the world and previous transforms of the entity and of everything attached to it, so nothing streaks across the screen. `attach()` and `detach()` keep an entity where it is: its `Transform` is recomputed against its new parent (or the world).
- **Static entities** carry `StaticTransform`: tables of their own, so a moving neighbour with the same components doesn't make them look changed (change detection is per table), and they cost nothing per tick.
- **Hierarchy is for attachment, not ownership:** placed entities are roots, attachments use `flecs::Parent`. Deleting a parent deletes its children.
- **Cells:** a cell is an entity with a `Cell` (its coordinates and its hold on its cell asset). Members carry `(InCell, cell)`: exclusive (one cell each), `DontFragment` (cells make no tables), deleted with their cell. Unloading a cell is deleting its entity. A `Cell` is exactly one hold, kept by its hooks: removing it (or deleting the entity) deletes the members and releases the hold, which releases what the cell's asset depends on; replacing it releases the old hold. It can't be copied. The registry must outlive the world (`WorldDesc::assets`).
- **Spawning:** `load_cell()` loads a `.decell` through the world's registry and makes its entity. Once the cell and its meshes are Ready, its members are made in one deferred batch (one table move each), by `de::SpawnCells` first in Input or `de::SpawnCellsBeforeExtract` before RenderExtract, whichever runs first: a frame may run no tick, and must never extract members whose meshes a reload let go. Transforms are composed down the cell's hierarchy and set with their previous ones; `MeshInstance` comes from the cell's dependencies (looked up by reference, not position). Moving children keep `flecs::Parent`; static ones (their parents are static too) become roots at their world transform, so they cost nothing per tick. `CellContents` holds the registry revision the members came from: when hot reload swaps in a re-cooked cell, they're made again. A failed cell stays empty.
- **Time:** systems take the fixed step from `it.delta_time()`; count ticks with `World::tick_count()`. flecs' own timers (`interval`, `rate`, tick sources) and its world time don't fit our two pipelines: the timers' systems live in phases the pipelines don't run, and its world time counts ticks and frames both.
- **Moving an entity:** write `Transform` with `set()` (or call `modified()` after `get_mut()`): world transforms are recomputed only in tables marked changed.
- **Entities hold no assets:** `MeshInstance` names a mesh by handle without holding it; its cell does, and outlives it, so the handle can't dangle.
- **Extraction** runs once per frame in RenderExtract: the active camera first (it may move the origin), then every entity with `MeshInstance`, interpolated, with its pose a frame ago for motion vectors (the same two ticks stepped back by the frame's share of a tick, through the tick before if the frame was longer; a hitch counts no more than the ticks that ran; nothing per entity is kept). The camera's previous view is what was drawn, kept in the frame and moved to its origin, since a free camera is `place()`d every frame and its ticks don't say; `ActiveCamera::cut`, or another camera entity, drops it for a frame. Then: every table is cut into 512-row chunks, run as one parallel job over all of them, whatever the tables' sizes. Unmoved entities skip the slerp. The instance and chunk arrays keep their capacity: no allocation per frame once the world has settled.

## Not yet

- Spreading a big cell's spawning over frames (a budget per frame); names on entities (they're in the `.decell`, for the editor).
- The streamer that picks cells around the camera, with budgets and hysteresis (M1).
- Entities moving between cells as they cross borders, prefab instancing into cells, lights.
- Persistent GPU instance slots (with GPU-driven rendering), owned by entities through observers.
