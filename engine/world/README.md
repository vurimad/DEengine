# world

The game world (docs/design.md, Core systems; Open world & streaming: world model): one flecs world, its phases and fixed tick, transforms, cells, and the render extraction that hands the renderer what to draw.

## Contents

| File | What it gives |
| --- | --- |
| `world.h` | `World`: the flecs world on the engine's memory, log and jobs; the phases; the fixed 60 Hz tick with interpolation; `render_frame()` |
| `transform.h` | `Transform` (local, double position), `WorldTransform` and `PreviousWorldTransform` (computed), `compose`, `interpolate`, `place` |
| `cell.h` | `Cell`, `InCell`, `create_cell`, `add_to_cell`: cells own their entities and the assets they use |
| `render_extract.h` | `MeshInstance`, `Camera`, `ActiveCamera`, and `RenderFrame`: instances and camera as floats relative to a render origin |
| `flecs_os.h` | flecs on the engine's allocator and log, with its own threads off |

## Rules and invariants

- **One owner per thing, data flowing one way.** The flecs world is the only list of what exists. Nothing mirrors it: the renderer reads `render_frame()`, rebuilt every frame by RenderExtract.
- **Positions are doubles** (`Transform.position`, `WorldTransform.position`); rotation and scale are float. The world never rebases. Render extraction is the only place doubles become floats, relative to a render origin that follows the camera in 1024 m steps (`origin_moves` counts the moves: GPU copies made against the old origin are stale).
- **Transforms:** `Transform` is relative to the entity's `flecs::Parent`, or the world without one, and brings `WorldTransform` and `PreviousWorldTransform` with it. World transforms are computed last in PostSim: roots copy theirs only in tables where `Transform` changed; children (attachments, prefab parts: few) compose their parent's every tick, parents first. `PreviousWorldTransform` is caught up first in Input, only for tables whose world transform changed. Use `place()` to put an entity somewhere at once (spawn, teleport) without it sliding in.
- **Hierarchy is for attachment, not ownership:** placed entities are roots. Deleting a parent deletes its children.
- **Cells:** a cell is an entity with a `Cell` (its coordinates and its hold on its cell asset). Members carry `(InCell, cell)`: exclusive (one cell each), `DontFragment` (cells make no tables), deleted with their cell. Unloading a cell is deleting its entity; an observer releases its asset hold, which releases what the cell's asset depends on. The registry must outlive the world (`WorldDesc::assets`).
- **Entities hold no assets:** `MeshInstance` names a mesh by handle without holding it; its cell does, and outlives it, so the handle can't dangle.
- **Extraction** runs once per frame in RenderExtract: the active camera first (it may move the origin), then every entity with `MeshInstance`, interpolated, rows split across jobs. Static entities skip the slerp. The instance array keeps its capacity: no allocation per frame once the world has settled.

## Not yet

- Spawning cells from cooked `.decell` files, the `.cell` text format and test content (next).
- The streamer that picks cells around the camera, with budgets and hysteresis (M1).
- Entities moving between cells as they cross borders, prefab instancing into cells, lights.
- Persistent GPU instance slots (with GPU-driven rendering), owned by entities through observers.
