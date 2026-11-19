# DEengine Technical Design Doc

Oct 31, 2026 · @Achronovitch

## Vision & pragmatic principles

DEengine exists to ship an open-world, Elden Ring-class action RPG. The engine core stays compact, but the systems that define the genre (streaming, terrain, deferred rendering, VFX, combat) are built to production depth, not as demos.

What the engine is for:

- A large seamless open world streamed around the player, with dense legacy dungeons and caves inside it
- Tight, readable, deterministic melee combat on a fixed 60 Hz simulation
- A painterly, atmospheric look: deferred PBR, volumetrics, huge grass fields, heavy VFX and weather
- Bosses and enemies authored largely in data and Lua on top of C++ systems
- Being equally easy to drive for a human in an editor and for an AI through text and commands

What it deliberately is NOT:

- Not a general-purpose engine: no 2D, no visual scripting, no genre-agnostic abstractions
- Not chasing cutting-edge tech for its own sake: no virtualized geometry; hardware ray tracing only as an optional layer, never a requirement
- Not multiplayer at launch, but the simulation stays deterministic so co-op remains possible

Guiding rules:

1. **Compact core, deep systems.** Few layers and abstractions; depth goes into the systems the player feels and sees.
2. **Streaming is the default.** Every system assumes its data can load and unload at runtime. Nothing assumes a whole level is resident.
3. **GPU-driven where it scales.** Culling, terrain, grass, foliage and particles run on the GPU with indirect draws.
4. **Feel first.** Input latency, hit-stop, animation commitment and frame data come before features.
5. **Text is the source of truth.** Scenes, prefabs, materials, VFX and gameplay data are diffable text files. Binary is only cooked output.
6. **Every action is a command.** The editor, Lua, tests and AI agents all change the world through the same command API.
7. **Every system has a debug view.** If you can't see it on screen, you can't tune it.
8. **Buy the boring parts.** Proven libraries for windowing, physics, audio and import.

## Scope & non-goals

The engine is designed for a full Elden Ring-scale world: at least 16 x 16 km of terrain, with no hard limit on cell count, plus underground maps and many dungeons, since streaming and double-precision world coordinates make size a content question, not an engine one. The first milestone target, a proof rather than the limit, is an open-world vertical slice: about 2 x 2 km of streamed terrain with grass fields and forests, one legacy-dungeon-style interior, open-field enemies, one field boss and one dungeon boss, weather and day/night.

| Area | First playable | Vertical slice | Later |
| --- | --- | --- | --- |
| World | Streamed terrain tiles around the player | 2 x 2 km world, cells + dungeon interior, no loading screens | Multiple regions, underground maps |
| Terrain | Streamed heightfield quadtree, splat materials | Virtual texturing, sculpt + paint in editor, holes for caves | Runtime deformation, snow/mud trails |
| Vegetation | GPU-instanced grass on a patch | GPU-driven indirect grass fields, trees with LOD and impostors, wind | Interactive bending, destructible foliage |
| Rendering | Deferred PBR, sun shadows | Render graph, GPU culling, cascades + local shadows, volumetrics, sky, SSR, TAA | GI probes or DDGI, upscalers |
| VFX | GPU particles | Data-driven emitters, decals, trails, weather, spell and blood effects | Fluid/smoke sims, mesh particles at scale |
| Cloth | Spring bones, one simulated cape on the player | Character cloth on player, enemies and bosses with LOD; wind-driven banners and rags | Self-collision for long robes, torn cloth |
| Combat | Attacks, roll, stamina | Poise, guard, parry, backstab, weapon arts, spells | Ranged, mounted combat |
| Scripting | Lua bindings for entities | Lua for quests, bosses, events, with hot reload | Lua debugger integration |
| Editor | World viewer + inspector | Full world editor (see Editor & tooling) | Collaborative editing |
| Traversal | Run, roll, jump | Mount (Torrent-style), ladders, ledges | Spirit springs, gliding |

Explicit cuts for now: online multiplayer, consoles, mod SDK. Hardware ray tracing is not cut: the renderer is built ready for it and RT features come in as optional passes (see Rendering).

## Tech stack & build

C++20 and Vulkan 1.3 (dynamic rendering, synchronization2, descriptor indexing, buffer device address, draw indirect count). An open-world, GPU-driven deferred renderer needs those features, so Vulkan is the primary backend from day one; OpenGL is dropped.

| Area | Choice | Why |
| --- | --- | --- |
| Language | C++20 core, LuaJIT scripting | Systems in C++, content and logic in Lua |
| ECS | flecs | Hierarchies, relationships, prefabs, queries, pipelines, reflection, REST explorer |
| Graphics API | Vulkan 1.3 | Bindless, BDA, indirect count, async compute |
| Vulkan helpers | volk, VMA, vk-bootstrap | Removes setup and allocation boilerplate |
| Shaders | Slang to SPIR-V, reflection-driven layouts | Modules, generics, hot reload |
| Lua bindings | sol2 | Fast, type-safe C++ bindings |
| Window & input | SDL3 | Window, gamepad, platform glue |
| Math | GLM | Standard |
| Physics | Jolt Physics | Large worlds, streaming bodies, character controller |
| Animation | ozz-animation | Sampling, blending, compression |
| Navigation | Recast/Detour, tiled navmesh | Streams tile by tile with the world |
| Import | ufbx, cgltf, stb\_image, tinyexr, meshoptimizer, BC compressors | glTF in, optimized meshes and LODs out |
| Audio | miniaudio (FMOD later if needed) | 3D audio, streaming |
| Editor UI | Dear ImGui (docking) + ImGuizmo | In-engine editor |
| Data format | JSON for authoring, flatbuffer-style binary for cooked | Diffable source, fast loads |
| Profiling | Tracy, RenderDoc, Nsight | CPU/GPU timelines, frame captures |
| Build | CMake, vcpkg, Ninja, clang-format, clang-tidy | Reproducible |
| Tests | doctest + headless engine runs | Unit and gameplay regression tests |
| Core library | In-house containers, strings, allocators (see Core library) | Predictable memory and cache behavior at scale |

Performance target: **120 FPS in the full game**, not just the demo. RTX 3070 / RX 6700 XT class at 1080p native, 1440p with upscaling; 60 FPS floor on RTX 2060 class at low preset. Frame time budget 8.3 ms: game thread ≤ 6 ms, render thread ≤ 6 ms, GPU ≤ 7.5 ms. Memory budget: 12 GB RAM, 8 GB VRAM. On the development machine (RTX 4080 Laptop, 240 Hz), the target is 240 fps at 1440p: 4.17 ms a frame.

## Platforms & RHI

Decision: Windows and Linux are the target platforms, both first-class from M0 (Linux also covers Steam Deck). PS5 and Xbox are a long-term dream, not a plan, but the engine is layered so a console port would mean new backends, not a rewrite.

**Platform support.**

| Platform | Compiler | Graphics | Status |
| --- | --- | --- | --- |
| Windows 10/11 x64 | MSVC or clang-cl | Vulkan 1.3 | Primary |
| Linux x64 | clang or gcc | Vulkan 1.3 | First-class, built and tested in CI |
| Steam Deck | (Linux build) | Vulkan 1.3 | Bonus target, low preset |
| Xbox Series / PS5 | vendor toolchains | D3D12 / console API | Future, needs dev-kit access |

Both desktop platforms build on every PR; a platform that stops building blocks the merge.

**RHI: a real abstraction, one backend for now.** The RHI is the only place graphics API types exist. Everything above it (render graph, passes, materials, terrain, VFX) uses RHI types only.

- Vulkan is the only backend implemented. No D3D12 or console code until there is a real reason; the abstraction is designed for them, not built for them.
- No Vulkan types, enums or headers leak above the RHI (`VkImage`, `VkFormat`... stay inside `engine/render/rhi/vulkan/`).
- The API is shaped to map cleanly to D3D12 and console APIs: explicit command lists, pipeline state objects, bindless resources by index, explicit queues (graphics, async compute, transfer), barriers expressed as resource states that the render graph emits.
- Shaders are written once in Slang, which can target SPIR-V today and DXIL/HLSL later; no backend-specific shader code in passes.
- Format, feature and limit queries go through RHI capability structs, so passes ask "is ray query available?", never "is this Vulkan?".

**Platform layer.** Everything OS-specific lives in `engine/platform/`: window and input (SDL3 on desktop), file IO backends (positional reads on a small thread pool now; IoRing on Windows and `io_uring` on Linux later), threads, memory paging, timers, crash handling. Game and engine code never include OS headers. A console port would add a platform folder and an RHI backend; nothing else should change.

**Rules that keep ports possible.**

- No platform `#ifdef` outside `platform/` and RHI backends.
- Paths and file names case-sensitive-safe (Linux), lowercase by convention.
- No reliance on desktop-only behavior: memory budgets are explicit, IO is async, saves go through the user mount.
- Input goes through actions, never raw device codes, so console controllers drop in.

## High-level architecture

The engine is four layers, and each layer only calls the one below it. Gameplay runs on a fixed 60 Hz tick on the main thread; rendering is planned to run on its own thread from a snapshot of that tick. There is no render thread yet: the main thread records and submits each frame itself after the ticks, from the same snapshot (`world::RenderFrame`; `app::run_framework`).

&#91;embedded content: engine architecture · 4 layers\]

Game rules never touch Vulkan or SDL directly, so the renderer or platform can change without rewriting combat.

Main loop (game thread; the hand-off to a render thread is the plan):

```cpp
while (running) {
    pollInput();                       // SDL3 events -> input buffer
    accumulator += frameDelta;
    while (accumulator >= TICK) {      // TICK = 1/60 s
        simulate(TICK);                // input, actions, AI, physics, combat
        accumulator -= TICK;
    }
    float alpha = accumulator / TICK;  // interpolate transforms for rendering
    renderThread.submit(buildRenderPacket(alpha));
}
```

Threads: a game thread (input, flecs systems, combat), a render thread (planned; today the game thread records and submits the frame), an async-compute queue for culling and particles (planned), and a job system (work-stealing, fiber-free) that open-world streaming needs from day one: decompression, cooking of streamed cells, animation sampling, navmesh tile loads and physics body creation all run as jobs. flecs systems may run multithreaded where they don't share writes. Audio runs on miniaudio's own thread.

## Core systems

The core is flecs for the world, a job system and allocators for streaming, a cooked-asset pipeline, and Lua as the first-class scripting layer on top of C++ systems.

**flecs as the world model.** All runtime state lives in one flecs world:

- Components are plain structs registered with flecs reflection, so every component is inspectable, serializable and editable without hand-written UI or save code.
- Hierarchies (flecs' non-fragmenting `Parent`, see World data rules) for transforms and attachments; relationships for gameplay links (`TargetOf`, `OwnedBy`, `InCell`).
- Prefabs (`IsA`) for enemy and item archetypes; a spawned enemy is an instance that overrides a few fields.
- Systems run in explicit pipeline phases: Input → PreSim → Combat → Physics → Animation → PostSim → RenderExtract.
- Planned for dev builds: the flecs REST explorer, so humans and AI can query the live world from outside. It isn't on yet: its HTTP server runs on a thread of flecs' own, and the engine turns flecs' threads off (below).

**World data rules.** flecs keeps entities with the same set of components together in one archetype table, one tightly packed column per component; a cached query holds the tables it matches, and a system runs as a flat loop over each table's columns. Everything below keeps the world on that path (checked against flecs 4.1.6):

- **Few tables.** Every distinct component set is a table, and many small tables mean short loops and overhead per table.
  - State that changes every few frames is a field in a hot component (`CombatState.flags`), not a tag: adding or removing a component moves the entity to another table, copying every column. On/off states use the `CanToggle` trait (a bitset; nothing moves).
  - Relationship pairs make a table per target: `(InCell, cell)` across 200 cells would be 200 tables. Hierarchies use flecs' non-fragmenting parent (`EcsParent`); relationships that would fragment are marked `DontFragment` or become an ID in a component. Spatial structure lives in our own grid, not in pairs.
  - Bulky, rarely iterated data (debug info, editor metadata) is `Sparse`: stable pointers, no table moves.
- **Queries, never `get()` in a loop.** Per-frame systems iterate cached queries; `get()` is for one entity, once. Hot components stay small, plain and split from cold data (doc rule 1).
- **Shared, not copied.** Data the same for every instance of a prefab (stats, mesh and material IDs, collision shapes) is marked `(OnInstantiate, Inherit)`: stored once on the prefab, read as one value per table. flecs copies prefab components by default.
- **Change detection by table.** Systems skip tables nothing wrote to (`changed()`); data that changes often and data that rarely does live in different components, since one write marks its whole table. Render extraction sends only what changed to the GPU instance buffer, and a table nothing wrote to costs it next to nothing.
- **Events, not polling.** Observers for reactions, singletons for global state, IDs hashed at load, spatial queries through the grid.
- **Our memory and threads.** flecs' allocations go through our allocators (its OS API hooks, tag World), so they count in the per-frame allocation counter; flecs' own threads stay off (its workers wait on each other at every merge, which a job system can't promise): systems over big tables split their rows with `JobSystem::parallel_for`, one writer per row. Structural changes stay deferred and merge at phase boundaries; Tracy shows each merge.
- **Measured.** A headless world benchmark (about 10k entities in typical shapes: a transform hierarchy, movement, render extraction) runs in CI, and Tracy plots tables and entities per query, so fragmentation is a number, not a guess.

**Memory.** Per-frame linear arenas, pools for hot small objects, a per-cell arena that frees in one shot when a cell unloads, and VMA for all GPU memory. Fixed streaming budgets per category (textures, meshes, physics, audio) with eviction by distance and priority.

**Asset pipeline.**

1. Source: glTF, PNG/EXR, JSON/Lua data, WAV/OGG, Slang shaders.
2. Cooker: vertex-cache-optimized meshes + LOD chains (meshoptimizer), BC7/BC5 textures with mips, ozz animations, baked navmesh tiles, physics shapes.
3. Cooked assets are named by asset ID and carry a content hash; the same cooker runs offline, on file change (with the editor, later), and from the command line.
4. Runtime loads asynchronously via the job system with priorities; GPU uploads go through a staging ring on the transfer queue.

**Ship build: clean, packed, no raw files.** The root holds only the executable, its DLLs and licenses; all game data lives in numbered archives DATA\_000X.arc inside the Archives folder. Source assets never leave the repository.

```
GameName/
  GameName.exe
  *.dll                   # runtime libraries (SDL3, etc.)
  licenses/
    third_party.txt
  Archives/
    DATA_0000.arc         # index: archive table, asset IDs, hashes, version
    DATA_0001.arc         # core: shaders, UI, fonts, player, common VFX
    DATA_0002.arc         # scripts (Lua bytecode)
    DATA_0003.arc         # characters, enemies, bosses
    DATA_0004.arc         # audio
    DATA_0005.arc         # world region 00_00
    DATA_0006.arc         # world region 00_01
    ...
    DATA_00NN.arc         # dungeons, later regions, patches
```

- **Packed by locality.** Everything a streaming region loads together lives in one archive, stored in load order, so streaming is sequential reads. Shared assets go to the core archive; duplicates are removed by content hash. The cooker assigns archive numbers; `DATA_0000.arc` is always the index that maps asset IDs to archive and offset, so code never hard-codes numbers.
- **Compressed and aligned.** zstd (or LZ4 for fast-decompress data), entries 4 KB-aligned for fast async IO.
- **Cooked formats only.** BCn/KTX2 textures, engine-binary meshes and animations, SPIR-V shaders + pipeline cache, Lua bytecode, compressed audio.
- **No raw files, enforced.** A ship validator scans the output and fails the build on any source format (`.gltf`, `.fbx`, `.blend`, `.png`, `.psd`, `.exr`, `.wav`, `.json` authoring files, `.lua` source, `.slang`) or any loose file outside the layout.
- **Nothing user-specific inside.** Saves, settings, logs, crash dumps and shader caches go to the user folder (`Saved Games` / `%LOCALAPPDATA%`), never next to the exe.
- **No debug baggage.** Symbols (`.pdb`) are archived per build ID for crash reports, not shipped; no editor, dev tools or debug UI in the ship binary.
- **Same API, two backends.** A virtual file system reads loose cooked files in dev builds (for hot reload) and the archives in ship builds; game code never knows which.
- **Patch-friendly.** The index lists each archive's hash, so updates replace only changed archives or add new numbered ones that override older entries.

**Lua scripting (first-class).** LuaJIT through sol2, with hot reload. LuaJIT is chosen for speed (JIT compilation, and FFI for zero-overhead calls into plain C++ structs); the cost is the Lua 5.1 dialect, which is fine for gameplay scripts.

- Lua owns: quests, NPC dialogue, boss phase scripts and set pieces, level events, item and spell definitions with behavior, editor tools and automation.
- C++ owns: anything per-frame and heavy, such as combat resolution, physics, animation, AI perception and pathing, streaming and rendering.
- The rule: Lua decides what happens; C++ decides how it is computed. Lua never iterates thousands of entities per frame; it reacts to events and calls C++ systems.
- Lua sees the flecs world through typed bindings and the same command API as the editor.

**Determinism.** Gameplay ticks at a fixed 60 Hz with seeded RNG per system, so replays, regression tests and future co-op all work from input logs.

## Performance rules (non-negotiable)

At 120 FPS the CPU has about 6 ms per thread, so the hot paths follow data-oriented rules strictly. A violation is a bug, the same as a crash.

**1. Cache first.**

- Hot data is contiguous and iterated linearly. flecs archetype storage is already structure-of-arrays; we iterate with queries, never with per-entity `get()` calls in a loop.
- Hot components stay small and plain (POD, no virtuals, no `std::string`, no owning pointers). Split hot and cold data: `Transform`, `Velocity`, `CombatState` are hot; names, debug info and editor metadata are cold components.
- No pointer chasing in inner loops: handles and indices into flat arrays, not linked structures or `std::map` / `std::unordered_map` (use flat hash maps, and only off the hot path).
- Sort work by what it touches (archetype, material, mesh) before processing it.
- Per-thread data is padded to 64-byte cache lines to avoid false sharing.

**2. No copies of count-sensitive data.**

- Never copy entity collections, query results, instance lists or render data. Pass `std::span` / views; functions write into caller-provided output buffers or the frame arena.
- Heavy types delete their copy constructor (`= delete`) so an accidental copy fails to compile; moves are explicit.
- No returning `std::vector` by value from per-frame code; no `auto x = bigThing` where a reference was meant.
- Render extraction writes each visible instance once into a per-frame buffer that is uploaded once, never a copy of the world.
- clang-tidy `performance-*` checks (unnecessary copy, for-range copy, move-const-arg) are errors in CI.

**3. No stupid scans.**

- No O(N) search to find one thing. Lookups go through handles, cached flecs queries, or indexes.
- Spatial questions (perception, lock-on, hit queries, streaming, audio) use a spatial grid or BVH, never "loop over all enemies".
- Event-driven over polling: systems react to change (flecs observers, dirty flags, change detection) instead of re-checking every entity every frame.
- No string lookups at runtime: names are hashed to IDs at load or cook time.
- Every per-frame loop has a known upper bound, and Tracy plots its count.

**4. No per-element locks.**

- Parallelism comes from partitioning data, not from locking it: a job owns a contiguous range and is the only writer to it.
- Structural changes from worker threads go into per-thread command buffers (flecs deferred mode), merged at one sync point per phase.
- Atomics only for counters and append indices; lock-free single-producer or multi-producer queues for streaming results.
- At most one lock per subsystem boundary (e.g. the streaming request queue), never inside a loop over elements.
- Streaming builds new entities off-thread in staging memory, then commits them in one batch on the main thread.

**5. Enforced, not hoped for.**

- Zero heap allocations per frame in steady state, checked by an allocation counter in dev builds.
- Benchmark scenes (dense grass field, 50-enemy fight, fast mounted travel) run headless in CI; a frame-time regression above 5% fails the build.
- Code review checklist: cache layout, copies, scans, locks, allocations.
- Tracy zones on every system with budgets shown on screen; going over budget is visible immediately.

## Core library (std replacement)

The engine uses a small in-house core library for containers, strings, memory and callbacks instead of the std containers. It is a tight, boring library (a few thousand lines), not a full STL rewrite: we replace only what std does badly at scale and keep what std does well.

**Start with a facade, swap later.** To keep the first iterations fast, phase 1 ships our API with std underneath. Engine code only ever names our types, so switching the implementation later touches the library, not the engine.

1. **Phase 1 (M0–M1): thin facade over std.** `Array<T>` wraps `std::vector`, `HashMap` wraps `std::unordered_map`, `FunctionRef` wraps a function pointer + context, allocators forward to `new`/`delete`. Wrappers, not `using` aliases, so the API stays ours: no std-only methods leak into engine code.
2. **Lock the API early.** Only the methods we want long-term are exposed (e.g. `push`, `reserve`, `resize_uninit`, `find` returning a pointer or null). Allocator parameters exist from day one even if phase 1 ignores them.
3. **Phase 2 (from M2, driven by Tracy): swap hot spots.** Replace an implementation only when profiling or the allocation counter points at it, usually `HashMap` and `Array` growth first. One container at a time, each behind its benchmark and tests.
4. **Switchable per container.** A build flag (`CORE_USE_STD_HASHMAP=1`) selects the std or own implementation, so a suspected bug can be bisected by flipping it back.

**Why std is not enough here.** `std::unordered_map` is node-based and scatters entries across memory (cache misses on every lookup). `std::vector` value-initializes on resize, can't relocate with `memcpy`, and ties memory to the global heap. `std::string` and `std::function` allocate silently. Behavior and debug-mode cost differ between MSVC, libstdc++ and libc++, and heavy headers slow compiles.

| Ours | Replaces | What it does differently |
| --- | --- | --- |
| `Array<T>` | `std::vector` | Explicit allocator, no exceptions, `memcpy` relocation for trivial types, uninitialized resize, predictable growth |
| `InlineArray<T, N>` | small vector | First N elements stored inline, heap only beyond N |
| `FixedArray<T, N>` | `std::array` + size | Fixed capacity, never allocates, asserts on overflow |
| `HashMap<K, V>` / `HashSet<K>` | `std::unordered_map/set` | Open addressing, flat storage, one cache line per probe in the common case |
| `SlotMap<T>` | ad-hoc ownership | Dense storage + generational handles, safe stale references |
| `RingBuffer<T>`, `BitSet` | `std::deque`, `std::vector<bool>` | Fixed or growable, no node allocation |
| `StringView`, `StringId`, `FixedString<N>` | `std::string` | Hashed IDs at runtime; strings only at load, in tools and debug |
| `FunctionRef`, `InplaceFunction<Size>` | `std::function` | Non-owning or fixed inline storage, never allocates |
| `Result<T, Error>` | exceptions | Explicit error values, no unwinding |
| Allocators: `Arena`, `Pool`, `FrameAllocator`, `Heap` | global `new` | Every container takes one; memory tagged per system |

**Kept from std** (cheap, well-behaved): `<type_traits>`, `<utility>` (move, forward), `<atomic>`, `<bit>`, `<span>`, `<optional>`, `<algorithm>` on spans (sort, binary search), `<cstring>`, placement new.

**Banned in runtime code** (allowed in offline tools): `std::string`, `std::map`, `std::unordered_map`, `std::list`, `std::function`, `std::shared_ptr`, iostreams, `<regex>`, exceptions, RTTI-based logic.

**Rules for the library itself.**

- Bounds and invariants checked with asserts in debug and dev builds; no checks in release.
- Debug builds stay fast: no iterator debugging layers; a debug build must still run the open world.
- Every allocation is tagged (`Render`, `Streaming`, `Combat`...) and reported in the memory viewer.
- Third-party libraries (flecs, Jolt, VMA, Lua) are routed through our allocators via their hooks.
- Natvis / pretty-printers for every container, so humans see clean values in the debugger.
- Each container has unit tests and a benchmark against its std counterpart; we only keep a replacement that wins.
- Plain, readable code with short headers, so it compiles fast and both humans and AI can read the whole thing.

We considered EASTL; a small own library fits our allocators and rules better and stays fully understood.

## File system & IO

All file access goes through one small virtual file system (VFS) with mount points and an async-first API. Engine code never calls `fopen`, `std::filesystem` or OS file APIs directly; only the platform layer does.

**Mount points.**

| Mount | Dev build | Ship build | Access |
| --- | --- | --- | --- |
| `game:/` | Loose cooked files (overriding archives): the open project's `cooked/` | `Archives/DATA_*.arc` | Read only |
| `src:/` | The open project's sources: `content/`, `meta/`, `config/` | Not present | Editor, cooker and dev builds |
| `engine:/` | The engine's own sources, whatever project is open: `shaders/` | Not present | Cooker and dev builds (shader hot reload) |
| `user:/` | Saved Games / AppData folder | Same | Read/write: saves, settings, logs, caches |
| `temp:/` | Temp folder | Same | Scratch, cleared on start |

**Projects.** The engine is never fused with a game. A game is a project: its own folder under `projects/` (`projects/dreadworld/`), named by the `<name>.deproject` file in it, holding `content/`, `meta/` and `config/` (its sources, `src:/`) and `cooked/` (`game:/`). The engine and the cooker open one with `--project` (its folder or `.deproject` file, relative to the working folder or the repository); without it, dev builds open the build's default, `DE_DEFAULT_PROJECT` (CMake, `projects/sandbox`), and no engine code names a project. Each project cooks on its own: its own cooked data, cook state and cooked shaders, so cleaning or removing one touches nothing else. The repository commits only `projects/sandbox/`, the small test scene the tests and CI use; the game's project is private (real map data, store assets) and kept out of the public repository. Shaders are the engine's (`engine:/shaders/`), the same for every project; a cooker source path under `shaders/` is the engine's, any other the project's.

**Paths and IDs.** Paths are lowercase, forward slashes, relative to a mount (`game:/characters/knight/knight.mesh`). Each asset has a stable 64-bit asset ID derived from the GUID in its meta file (see Asset formats); runtime code loads by ID, and strings exist only in tools, logs and debug views.

**Layered backends.** Each mount is a stack of sources searched top-down: patch archives over base archives in ship; loose cooked files over archives in dev, so a re-cooked asset wins immediately for hot reload. Game code never knows which source answered.

**Async reads (the main API).**

- `fs.read(id, priority, dest) → handle`: reads straight into caller-provided memory (a staging buffer or arena), never into an intermediate copy.
- Priorities: Critical (player and boss now), Near (cells about to activate), Far (distant LODs), Prefetch (predicted from travel direction). Requests are cancelable when the player turns away.
- Completion is polled or delivered to a job; decompression (zstd/LZ4) runs on the job system, then the asset registry finishes setup.
- Blocking reads exist only for startup and tools.

**Archive format (`DATA_000X.arc`).** Header + table of contents sorted by asset ID (binary search), each entry holding archive offset, compressed and raw size, compression type and content hash. Data is stored in 64–256 KB compressed blocks, 4 KB-aligned, so large assets can be read partially (e.g. only the mips that are needed). The TOC is memory-mapped; data is not.

**Platform IO, phased.**

1. Phase 1: a small thread pool doing positional reads (`ReadFile` with offset / `pread`). Simple and good enough for M0–M1.
2. Phase 2: native async batched IO (Windows IoRing or overlapped unbuffered reads, Linux `io_uring`) once streaming profiles show IO overhead.
3. Optional later: GPU decompression for textures (GDeflate), only if CPU decompression becomes the bottleneck.

**Threading.** Speed comes from many reads in flight, not many threads reading. Each stage hands work to the next through lock-free queues:

1. **Requests:** any thread pushes read requests into a lock-free MPSC queue and never blocks.
2. **One IO thread** owns that queue: sorts by priority and archive offset, drops cancelled requests, submits in batches. Phase 1 uses 2–4 blocking-read threads underneath; phase 2 keeps dozens of reads in flight via IoRing / `io_uring` from this one thread.
3. **Decompression and setup** run as jobs on the shared worker pool (zstd blocks, staging flecs entities, physics shapes, navmesh tiles). This is where all cores help.
4. **GPU uploads** go through the transfer queue, batched from a staging ring, so streaming never stalls rendering.
5. **Commit** happens on the main thread: finished assets and cells arrive on a completion queue and activate in one batch per frame, with no locks on live game data.

Avoided by design: a thread per request, systems reading files on their own, per-asset locks, and decompression on the IO thread (it would starve the drive).

**Writes.** Only `user:/` and `temp:/` are writable. Saves and settings use write-to-temp-then-rename so a crash never corrupts them.

**Hot reload.** The editor watches `src:/` and runs the cooker when its files change (built: Cook automatically, below; outside the editor it is run by hand); when cooked output changes in `game:/`, the asset registry reloads it in place and notifies dependents.

**Asset registry v1** (`engine/asset/registry.h`; rules in `engine/asset/README.md`). `load(ref)` holds an asset by ID and type and returns a handle at once; the read lands straight in the asset's memory, is checked on a job, and is committed with the other finished reads once per frame on the main thread, which then loads what its header depends on. States are Loading, Ready (file in, dependencies settled) and Failed (logged; users draw a placeholder). Assets are reference counted and unloaded when nobody holds them. Hot reload polls a few held files per frame (an OS watcher comes later) and swaps changed ones in place, telling listeners. Cooked headers list dependencies with their type, since the type names the file.

**Errors and debugging.** Calls return `Result`. A missing asset shows a loud placeholder (magenta checker mesh, error log with the path) in dev, and a logged fallback in ship. An IO debug view shows throughput, queue depth per priority, latency, and the slowest requests.

**AI and tools.** The editor exposes `fs.list`, `fs.read` and `fs.write` commands on `src:/`, so agents edit source files through the same VFS and its validation as humans.

## Asset formats

The engine never reads `.fbx`, `.png` or any other source format at runtime. Assets live in three layers, and conversion happens in exactly one place: the cooker.

**1. Source files (`content/`, in Git).** `.fbx`, `.gltf`/`.glb`, `.png`, `.psd`, `.exr`, `.tga`, `.wav`, plus our own text sources (materials, cells, prefabs, VFX, sound events, Lua). Source folders stay clean: nothing but the files artists and tools produce.

**2. Meta files (`meta/`, mirrored tree, in Git).** Every source file has a small JSON meta at the same relative path under `meta/`, holding its stable GUID and any import settings that differ from the folder defaults.

```
content/characters/knight/
  knight.fbx
  knight_albedo.png
  knight_normal.png
meta/characters/knight/
  _folder.json            # defaults: PNG -> BC7 + mips, *_normal -> BC5
  knight.fbx.json         # GUID, scale, LOD rules, animations to import
  knight_albedo.png.json  # GUID only
  knight_normal.png.json  # GUID only
```

- **Asset IDs come from the GUID**, not the path, so renaming or moving a file never breaks references.
- **Folder rules** (`_folder.json`) set defaults by type and name suffix, so most per-file metas hold only a GUID.
- **Moves and renames** go through the editor or `de asset move`, which move source and meta together. Files moved outside the tools are caught by the cooker's validator, which re-links orphaned metas by content hash or reports clearly; it never silently mints a new GUID for an existing asset.
- **New files** get a meta created automatically on the next cook.

**Importers** (cooker only): ufbx for FBX, cgltf for glTF, stb\_image and tinyexr for images, our own parsers for text sources. No Assimp.

**3. Cooked DEengine formats (what the engine loads).**

| Asset | Format | Contents |
| --- | --- | --- |
| Mesh | `.demesh` | Optimized vertex/index buffers, LOD chain, bounds, submeshes, material slots |
| Skeleton | `.deskel` | Bone hierarchy, bind pose, sockets |
| Animation | `.deanim` | Compressed ozz tracks, root motion, events |
| Texture | `.detex` | BC1/BC4/BC5/BC7 or BC6H, full mip chain, GPU-ready |
| Material | `.demat` | Shader template, parameters, texture IDs |
| Terrain | `.deterrain`, `.deheights` | A map's pyramid of levels, nodes and patch bounds; its heights in a second file, read a block at a time. Splat maps, holes and grass density to come |
| Physics | `.dephys` | Jolt-serialized collision shapes |
| Navmesh tile | `.denav` | Detour tile data |
| Sound | `.desnd` | Opus/Vorbis data + playback metadata |
| Shader | `.deshader` | SPIR-V per permutation + reflection |
| Cloth | `.decloth` | Simulation mesh, constraints, painted limits, render-mesh mapping, LODs |
| Cell | `.decell` | Binary form of the text scene data: the entities placed in one cell |
| Prefab | `.deprefab` | A composed thing's parts in one flat list, each placed from its origin |
| World | `.deworld` | A world's cells by grid coordinate (named, not loaded with it), where the player starts, its environment and terrain |
| Environment | `.deenv` | How a place is lit: sun, sky and ground ambient, shadow tint, exposure, sun shadows |

**Rules for every cooked format.**

- Common header: magic, format version, asset ID, platform, dependency list (asset IDs), content hash.
- Load-in-place: the file layout is the memory layout; read it, fix up offsets, use it. No parsing step at runtime.
- GPU data is upload-ready and goes straight into staging buffers.
- A format version change triggers a re-cook; old cooked files are never patched.
- Platform variants are possible from the same source (e.g. different texture compression on consoles).
- Text sources are the source of truth; binaries are build output and never committed.

The editor browses source assets but always loads their cooked versions, so previews show exactly what ships.

**Cooker v1** (`tools/cooker/`, formats in `engine/asset/`; details in their READMEs).

- Cooked files live in `game:/` named by asset ID (`game:/3f/3f8a0c1e5b2d4e61.demesh`), since runtime code loads by ID. A source's main asset takes its GUID's ID; others it makes take an ID derived from it and a local name (`material/stone`, `image/0`); shaders take one from their module name, which code already uses.
- A glTF or FBX cooks to one mesh (its default scene flattened, a part per material), a material per material, and a texture per embedded image. Scenes as prefabs and cells come with the prefab and cell formats.
- Texture defaults follow from use: a texture a material samples as a normal map compresses to BC5, color to sRGB BC7, other maps to linear BC7. Folder rules and metas override them (JSON merge patches, root to leaf).
- Our text material source is `.material` (JSON): a template, named parameters and textures by relative path.
- Cells are `.cell` (JSON): the cell's grid coordinates and its entities (name, mesh by relative path to a model source, position, rotation, scale, parent by name, static, and the editor's folder for its hierarchy, which isn't cooked). The `.decell` lists the meshes as dependencies, puts parents before children (each entity keeping its place in the source, for the editor's saving), and is cooked again when a model it names gets a new ID, not when a model is cooked again.
- Worlds are `.world` (JSON): the environment and the cells by relative path, and the start position and view. The cooker reads each cell's coordinates into the `.deworld` (sorted, one cell per coordinate), so streaming can find cells without loading them; the environment and the terrain are its dependencies. Environments are `.environment` (JSON): the sun by elevation and azimuth (degrees, clockwise from north, -Z), colors with intensities (1 when left out), exposure and shadow settings, every other one required so a file states the whole look; an optional `wind` block (where it blows toward, in the sun's degrees, its speed, what a gust adds, a gust's size, turbulence), still air without it. Editing the light cooks only the environment; moving a cell cooks the world again.
- Terrains are `.terrain` (JSON): a 16-bit grayscale height image by relative path, with its own extension (`.heightpng`) so it isn't cooked as a texture, its spacing, the height range its pixels span, and where its first sample lies. It cooks to a small `.deterrain` that loads with its world and a `.deheights` under the same ID, read a block at a time; a cook record's outputs go by ID and type. Land too steep for a node's 16 bits fails the cook, saying where and by how much.
- The cooker keeps its state in `game:/cook_state.json`: per source, the hash of every input (bytes, settings, files it reads, versions) and what it made. Unchanged sources are skipped, outputs of deleted sources removed, and metas of files moved outside the tools re-linked by the last content hash.
- `cooker --check` validates the source and meta trees, reports cooked shaders that are stale, and writes nothing, for CI.
- Cooked shaders and the engine both name the shader sources they came from, by a hash of every `.slang` under `shaders/` but `shaders/tests/`. The build embeds it in the engine; the cooker writes it to `game:/shader_sources.hash` once every module is cooked, and removes it while it works. An engine loading cooked shaders refuses to start on any other hash, or none, before opening a window, and keeps the pipelines it has if a cooked reload doesn't match. Compiling from source doesn't look at it.

## Rendering

The renderer is a GPU-driven deferred PBR pipeline built on a render graph, aimed at the Elden Ring look: wide vistas, dense grass, strong atmospheric depth, golden backlight, heavy VFX. Deferred is the right fit here because the open world has huge material variety, many local lights from VFX, and decals everywhere.

**Render graph (frame graph).** Every pass declares what it reads and writes; the graph does the rest. Nothing fancy, just the parts that pay off:

- Passes are lambdas registered each frame with `setup` (declare resources) and `execute` (record commands).
- Transient resources (G-buffer targets, HDR buffers, shadow maps, blur chains, temporary UAVs) are declared by description, not allocated by passes.
- The graph computes each resource's lifetime (first to last use), then a resource allocator places transient resources in shared memory heaps, aliasing any two whose lifetimes don't overlap. Assets are never aliased; only graph resources are.
- Barriers and layout transitions are inserted automatically from the declared reads and writes.
- Passes nothing reads are culled; async-compute passes are scheduled on the compute queue with automatic queue sync.
- Imported resources (swapchain, TAA history, persistent shadow cache) live outside the aliasing pool.
- A debug view shows the graph, lifetimes and heap usage, and can display any intermediate texture. Its first part is the frame debugger (below): the passes and their textures.

*What we take from other engines* (studied 2026-11-02; findings in our own words, nothing copied):

- **Frostbite FrameGraph** (GDC 2017, Yuriy O'Donnell; the slides are members-only, so this is the talk's public outline): the shape above. Passes are declared every frame with setup and execute, resources are virtual until the graph compiles, passes are culled by counting references back from the outputs, transient memory is aliased, and outside resources are imported.
- **Unreal's Render Dependency Graph** (public documentation only, "Render Dependency Graph in Unreal Engine", UE 5.8; Unreal source is for ideas only):
  - A pass reaches a resource's GPU object only if it declared the resource, and only inside its execute; anything else is an error in dev builds, with the checks compiled out of ship.
  - An *immediate mode* runs each pass as soon as it is added, without culling or reordering, to bisect graph bugs.
  - Results that outlive the frame (TAA history) are *extracted* from one graph and *imported* into the next.
  - Execute lambdas run later, so they never capture stack data by reference; their data lives in the frame's memory.
  - A transition log, filtered by pass and resource, prints every barrier the graph emits.
  - Transient contents are undefined: a debug fill of fresh transients exposes passes that read what they never wrote.
- **Godot's RenderingDeviceGraph** (MIT, `servers/rendering/rendering_device_graph.cpp`). It sits a layer lower than ours: it infers dependencies from each recorded command's resource usage. We take three ideas:
  - **Per-subresource state.** Each resource keeps one state for the whole resource; mip or layer ranges that diverge are tracked as slices, and they are folded back when the whole resource is used again.
  - **Barriers per level.** Commands that don't depend on each other share a level, and each level gets one barrier command, with the stage and access masks of all its barriers merged.
  - **When a barrier is needed.** Godot emits one when the usage changes or a write follows a write. Our read-combinable states refine that: a read after a read in the same layout needs no barrier at all.
- **Granite** (Hans-Kristian Arntzen, "Render graphs and Vulkan — a deep dive", 2017; Granite is MIT):
  - Culling walks back from the frame's output.
  - A read is recorded even when nothing changes, so the next writer waits for it (write after read).
  - Sizes are absolute, relative to the swapchain, or relative to another resource.
  - History resources are never aliased.
  - Reordering passes to put distance between producer and consumer is worthwhile, but a later step.

*Render graph v1 (M0a)*. What we build first, from the above:

- **Passes.** `add_pass(name, setup, execute)`. Setup declares every texture and buffer the pass touches, each with its `ResourceState` and shader stages (subresource ranges allowed), and can create transient resources. Execute records into a graphics `CommandList`, inside a debug label named after the pass. A pass can carry a data struct (`add_pass<Data>`): setup fills it with the handles it creates, execute reads it, and later passes reach those handles through the returned reference (Frostbite's pass data, needed because an execute lambda is built before its setup runs). Closures and data live in the graph's frame arena: no `std::function`, and no stack captures by reference.
- **Resources.**
  - Transient textures and buffers are declared by description, with absolute or swapchain-relative sizes.
  - Imported resources (the swapchain image, anything persistent) come in with their current state and say the state they must leave in.
  - Execute resolves a graph handle to its RHI handle only for resources the pass declared (asserted).
- **Compile.**
  - Passes are culled from the outputs: imported resources that must leave in a state, and passes marked never-cull.
  - Passes run in declaration order; there is no reordering in v1.
  - The state of every subresource range is tracked; before each pass, one barrier batch holds every transition it needs. Reads in a compatible state merge without a barrier.
- **Memory.** Transient resources come from a pool keyed by description and reused across frames. Aliasing in shared heaps is v2, as is the debug view.
- **Debug.** Immediate mode (dev builds) and a transition log. The graph can be dumped to the log as text. Debug views read the frame's graph back (each pass, what it used and what it recorded), and a pass hook lets one add a pass right after the pass it watches: the frame debugger is built on both.
- **Queues.** Graphics only. Async compute passes come later, on top of the RHI's queue ownership transfers. Uploads stay with the uploader, which lands them at the start of the frame, before the graph's passes.

**GPU-driven geometry.**

- All meshes live in large shared vertex/index buffers; all materials and textures are bindless (descriptor indexing + buffer device address).
- A compute pass culls instances (frustum + Hi-Z occlusion from last frame's depth) and writes indirect draw commands (`vkCmdDrawIndexedIndirectCount`).
- LOD selection on the GPU per instance, with a dithered crossfade for the camera; distant trees and rocks swap to impostors.
- CPU work per frame is tiny: upload changed instance data, dispatch culling, issue a handful of indirect draws per material class.

**GPU uploads.** One `rhi::Uploader` moves CPU data into textures and buffers; nothing else records upload copies.

- Requests are checked when queued (exact byte size for the mips, copy usage, mips in range) and refused whole if wrong; accepted data is copied into a staging ring at once, so the caller's memory is free on return.
- Once a frame, `record` issues the queued copies oldest first within a byte budget (16 MiB by default), so a burst of streamed textures spreads over frames instead of stalling one.
- The uploader tracks the state of every mip it fills: low mips can stream in first and the top mip later, each upload starting from the state the last one left. The first upload moves every mip out of Undefined, so a texture's views are valid from then on; mips not streamed in yet are sampled with a mip clamp.
- Staging is reused once the GPU finishes the frame that copied from it; an upload larger than the ring, or one that finds it full, gets a staging buffer of its own.
- Copies run on the transfer queue when the GPU has one, alongside the previous frame's rendering. What they wrote is handed to the graphics queue (a queue ownership release on the transfer list, the matching acquire on the frame's graphics list), and the frame's graphics work waits for them on a timeline semaphore. Uploads land at the start of the frame that records them, before its other graphics work. Copies into new resources run alongside the previous frame; uploads over data the GPU may still read go in a transfer submission that waits for the previous frame, so they cost the graphics queue their copy time (mip streaming should therefore fill new textures rather than overwrite live ones, M1). Without a separate transfer queue (some integrated GPUs, lavapipe in CI) the same uploads run on the graphics queue.
- Mips of textures made at runtime are generated on the GPU (`rhi::generate_mips`, or an upload's `generate_mips`): a filtered blit per mip, sRGB filtered in linear space. Block-compressed and integer formats can't be; their mips come from the cooker, which also owns quality filters (alpha-coverage preservation for foliage, normal-map renormalization).
- sRGB textures with storage usage store through UNORM views of the same memory (GPUs don't write sRGB storage images); shaders encode sRGB themselves there and sample it decoded.
- Planned for streaming (M1): a reserve/commit form, where a decompression job writes straight into staging memory instead of handing a buffer to copy.

**GPU memory accounting.** The device counts every texture and buffer it allocates, by category (render targets, storage textures, sampled textures, GPU, upload and readback buffers), at its real allocation size, plus what waits to be freed. `Device::memory_stats` is cheap enough to poll every frame (for the frame stats panel, M0a task 11), `largest_textures` and `log_memory_report` name what is big, and `begin_frame` warns when a GPU-local heap passes 90% of its budget (once, until it drops below 80% again). The budget is the OS's where the driver has `VK_EXT_memory_budget`, an estimate otherwise. The streaming budgets (M1) build on these counters.

**Debug UI.** Dear ImGui (debug and editor tools only) is drawn by the engine's own renderer, not ImGui's Vulkan backend: vertices read through a buffer pointer, textures by bindless index, a render graph pass over the backbuffer, its font atlas through the uploader (ImGui's texture protocol). Its input comes from a small SDL3 feed inside `platform/`, so SDL stays there. Its memory is tagged Tools; it saves no ini file. The frame stats (F1) show frame times, heap allocations per frame, GPU memory by category, the render graph, uploads and the device, over the game's picture (see-through, taking no clicks; in the editor, over the Playground), their sections chosen with `--stats`.

**Frame debugger.** What the GPU did this frame, pass by pass, while the game runs (F8, the editor's Window menu, `frame_debugger` in the console; dev builds). It captures and replays nothing: the render graph already knows every pass, what it reads and writes and how long it took, so the debugger reads that, live.

- **Passes**, in the order they ran: each one's GPU time (averaged, with its peak) and share of the frame, and what it recorded, counted by the command list as it records: draws, indirect calls and the most draws they can make, dispatches, copies. Culled passes are listed too. Mark keeps the times as they are now, and every pass then shows how far it has moved from that: change a setting, read the difference. The numbers and the list change a few times a second, not every frame, in columns of fixed width, so they can be read; the debugger's own passes are left out of them.
- **Textures:** any texture a pass reads or writes, as that pass left it, before a later pass writes over it or the pool hands it on. The graph calls a hook at the end of every `add_pass`; the debugger answers the pass it watches with a pass of its own that draws the texture into its view. Zoom and pan go by texels, unfiltered; a channel alone or several; a range of values stretched over the display; mips, array layers, cube faces, 3D slices, depth. Integer textures (IDs) are copied into floats by a compute pass first: a fragment shader can't read storage textures. The texel under the mouse is read back as stored.
- **Cost:** closed, nothing: no hook, no passes, nothing copied. Open, the frame has up to four small passes more, listed with the rest.
- **By text**, for agents and scripts: `frame_debugger.report` prints the passes (or one pass's textures and buffers), `frame_debugger.select` chooses a pass and a texture, `frame_debugger.probe` reads a texel; `--frame-debugger` opens it in a picture.

Not in it yet: resource lifetimes and heap use (they come with transient aliasing), buffers' contents, a pass's draws one at a time, pipeline state. RenderDoc stays the tool for the last two.

**Hardware ray tracing (optional, RT-ready from day one).** The game must look right with RT off; RT features are passes that replace or improve a raster fallback when the GPU supports them.

- The RHI has acceleration structures (built 2026-11-12, with GI's first step below): a bottom level per mesh, from the coarsest LOD a ray can't tell from LOD 0 (within 2 cm of it, with every part of it and its uv seams where they were; LOD 0 itself where there's none), a top level rebuilt from the GPU instance buffer each frame, and the bindless setup lets ray shaders reach any material.
- Best-value RT features for this look, in order: RT shadows for the sun and key lights, RT ambient occlusion, RT reflections for water and wet stone, then RT or hybrid diffuse GI.
- Each RT feature is a render-graph pass with a raster fallback (cascaded shadows, GTAO, SSR, probes), toggled per quality preset.
- Grass and foliage stay out of the TLAS (or use simplified proxies) to keep build cost bounded.
- Uses `VK_KHR_ray_query` inside compute/fragment passes first (simpler, fits the deferred pipeline); full ray-tracing pipelines only if a feature needs them.

**Frame passes, in order:**

1. GPU culling (instances, lights) and Hi-Z build
2. Shadows: cascaded sun shadows (4 cascades in an atlas, stable fitting), cached far cascades later; local shadow atlas for torches and spells
3. G-buffer: base color + lighting group ID (RGBA8 sRGB), octahedral normal (RG16), roughness/metalness/occlusion/translucency (RGBA8), emissive straight into the HDR target, motion vectors (RG16F), reverse-Z depth (engine/render/scene/README.md)
4. Terrain and grass write into the same G-buffer
5. Deferred decals (blood, scorch, footprints) modify the G-buffer
6. Sun shadow mask: the cascades resolved per pixel into a screen-space mask with contact-hardening filtering; it reads the G-buffer's depth and normals, so it runs after the G-buffer, not with the shadow maps
7. Async compute: SSAO/GTAO, light clustering, particle simulation
8. Deferred lighting: sun (through the mask, tinted toward a shadow color), clustered local lights, sky and IBL probes, subsurface for skin and foliage
9. Atmosphere and sky (precomputed scattering), volumetric fog and god rays (froxels), clouds
10. Forward pass for transparents, particles, water, hair cards
11. Post: TAA, SSR, bloom, depth of field, motion blur, tonemapping, color grading LUT, film grain
12. UI and editor overlays

Built so far (renderer v1): sun shadows, G-buffer, shadow mask, deferred lighting (the sun, a hemisphere ambient, the sky, local lights through a clustered grid: Local lights, below), fog over the scene and the sky (step 9: a froxel grid near the camera with the sun's shadows, the closed form past it), the transparent pass (step 10: what blends, sorted, below), TAA, tonemap, then the debug UI over it. engine/render/scene/README.md has the details.

**Transparents (built, v1).** What blends (a material's `alpha_mode` of `blend`; glTF's BLEND, and its transmission, which stands in for glass until something bends light) leaves the G-buffer and the shadow maps and is drawn in step 10, after the fog, onto the fogged image:

- **Sorted blending.** The farthest first, each mixed over what's behind it. The cull keeps what blends in a list of its own, a draw each, and a compute pass puts those in order by how far each instance's middle is along the view: every draw against every other, which is nothing for the few hundred see-through things a view holds, and stable. One order per object: two that pass through each other are wrong on one side, and the order flips at once as their middles pass.
- **One blend state: premultiplied.** A surface's own colour counts by its opacity; what it reflects and what it emits count in full. So clear glass still shines, a glow with no opacity only adds light, and both are one pipeline and one indirect call.
- **Lit as it's drawn,** as the deferred pass lights what's solid: the sun through its cascades (one filtered sample), the sky/ground ambient. Seen from both sides.
- **Fog** is applied by the pass itself, since the fog pass has run: the grid as far as it reaches, the closed form past it.
- **No depth, no motion.** It tests against the scene's depth and writes none, so TAA knows nothing of it: a second target adds up how much of each pixel it covers, and TAA trusts its history less there.
- **Not in it yet:** particles (the VFX system brings its own pool and sort, into this pass), refraction and distortion, shadows cast by what blends, picking a see-through thing in the editor by clicking it. For later, if they're needed: weighted blended order-independent mixing for dense soft particles (never pops, approximate), and a low-resolution buffer for big soft smoke (fill cost). Exact order-independent methods (per-pixel lists, moments, depth peeling) cost too much memory and time for the frame budget.

**Local lights (step 1 built: the lights, their culling and their shading; no shadows yet).** A point light or a spot is a thing in the world like any other: an entity with a `LocalLight` (its colour, its intensity, the radius its light ends at, a cone for a spot, a flicker). A cell's entity or a prefab's part places one with `"light": {...}`, so a candle is one prefab: its wax, its flame and the light the flame gives (`engine/render/scene/local_lights.h`, `shaders/scene/local_lights.slang`).

- **What a light is.** `intensity` is what it gives a surface facing it from 1 m, in the sun's units (relative to exposure): a light of 5 lights a wall a metre away as a sun of 5 does. It falls with the square of the distance and is brought smoothly to nothing at `radius_m` (the window in Karis's SIGGRAPH 2013 notes), which is also what it's culled by: the radius is what a light costs. A spot shines along its entity's -Z within `cone_deg` of its axis, fading over the outer `cone_soft` share of that. `flicker` takes up to that share of its light away, by three slow waves that never line up, each light at its own place in them: a function of the world's time, so the same time shows the same light. A light scales with its entity as if the whole thing were made bigger (its radius by the scale, its light by the square), so a prefab placed at 0.7 is lit the same, at 0.7 the size. No physical units until there's auto exposure to make sense of a candle beside the sun.
- **Clustered.** The view is cut into 32 x 18 x 32 clusters: a grid across the picture whatever its size, and slices in depth, each about a quarter thicker than the last, from 0.5 m to 512 m (the first and the last open-ended). Each frame the CPU keeps the lights whose sphere touches the view, from within 100 of their radius (fading over the last fifth of that), the nearest for their size when more than 256 do. A compute pass (`light cull`, a thread a cluster) tests each cluster's box against each of those and writes a bit per light; a lit pixel reads its cluster's words and adds the lights whose bits are set, each worked out as the sun is (GGX and Lambert, the lighting group's share of specular, translucency's wrap). A bit per light, not a list per cluster: nothing overflows where a ring of candles shares a cluster, 21 lights are one word a cluster, and every cluster against every kept light is still thousandths of a millisecond.
- **Where they light.** The deferred light pass; the transparent pass (glass catches them); water, mirrored only (a candle glints on a pond). Not the fog yet (froxel phase D), nor particles.
- **Placing them.** In a `.cell` or a `.prefab`: `"light": {"color": [1, 0.52, 0.2], "intensity": 0.6, "radius_m": 3.5, "flicker": 0.25, "flicker_hz": 9}`, with `cone_deg` and `cone_soft` for a spot. In the editor: Edit > Add Light puts a point light above where the middle of the Playground meets the ground, its values in Properties, saved into its cell (a light edited there is written back whole, in one layout); a prefab's part gets one in the prefab editor (its Light rows, and Studio light off to see the prefab by its own lights); a chosen light shows how far it reaches as rings.
- **Measured (2026-11-12, this machine, 2560 x 1440, offscreen in `tests/render/local_lights_test.cpp`, the GPU shared with other programs, so the spread is wide):** `light cull` 0.009 ms for 21 lights, 0.06 to 0.12 ms for 220. The deferred light pass, 0.09 to 0.30 ms with no lights, was 0.30 to 0.42 ms with 21 candles near the camera (the shrine's), and 0.63 to 0.83 ms with 220 lights of 3 m every 1.25 m, every pixel in reach of a dozen. A measurement on a free GPU is owed.
- **Shadows (built 2026-11-14, uncommitted; GI plan step 2).** The lights that matter most (the best ranked of those lit: nearest for their size) get faces in a local shadow atlas until 24 are used: a point light six (a cube: +x, -x, +y, -y, +z, -z, each a little over 90 degrees so the filter at a tile's edge still reads the face), a spot one (its cone, at most 150 degrees); the rest light without shadows. Each face is a 512 x 512 tile of a 3072 x 2048 depth atlas (6 x 4), drawn with the engine's camera projection (reverse Z, no far plane, near 5 cm) and a shadow view of the GPU cull after the sun's cascades, with its own opaque and masked lists (at most 16,384 records each); a face not used this frame has a cull plane nothing is inside. The pass `local shadows` draws them after the sun's; terrain doesn't cast into them. The deferred light looks a light's shadow up (`local_shadow` in `shaders/scene/local_lights.slang`): the face its way lies through, the point moved off its surface by about a texel there, a 3 x 3 of depth tests with two texels of room. Measured against the path tracer with bounces off (the compare, the probes off): the Cornell box 0.03 stops off on average, 1% of the picture off by over 10%, all at shadow edges (`tests/render/path_tracer_test.cpp` holds a floor, a plate and a light to it). Not yet: shadows in the transparent pass, water and the probes' rays (they light without); terrain casting; caching faces of lights that don't move; their cost measured.
- **Shadows, how big and how steady (reworked 2026-11-14, after the user found them popping with distance; from how Godot sizes its shadow atlas, its source read, and Unreal's docs).** Before, a light's tiles were 512, 256 or 128 texels by its distance over its radius at fixed steps, the nearest taking the atlas first, and a shadow was there or not: from the rc_room's 17 candles only 5 to 7 got shadows, and a small move of the camera snapped shadows between sizes and between candles. Now:
  - *Size by coverage:* how tall the light's reach is on the picture over the picture's height, times 512 texels (`kLocalShadowTexelsPerCoverage`), rounded up to a power of 2 between 128 and 512 (Godot: its slot from its range's share of the screen, a power of 2).
  - *Hysteresis:* a light keeps its tiles' size until another has been wanted for half a second (`kLocalShadowResizeAfterS`; Godot keeps a slot for a time before moving it). Lights are known from frame to frame by where they stand (a moving light starts again each frame: to do when lights have names here).
  - *Room for all first:* in order of coverage (one with a shadow counted as covering half again more), every light that's to have a shadow gets 128-texel tiles while there are faces and cells; then, in the same order, each grows to its size as far as the room left allows. The 17 candles all have shadows (102 faces).
  - *Fading, never popping:* each light's shadow has a strength (`GpuLocalLight::shadow.w`, the shadow lerped toward lit by it) that moves at 2 a second (`kLocalShadowFadePerS`): in when it gets tiles, out as it wants fewer than 64 texels, gone at 32 (Godot and Unreal fade shadows by distance). The face's near plane is a constant in both places now.
  - *Room for every light, and soft edges (2026-11-19, after the user found a lantern's shadow there or not by the way the camera had come).* The shrine has 24 point lights (23 candles and a lantern): 144 faces, and there were 128, so three lights went without a shadow, and which three turned on a ranking that favoured whoever had one (the path the camera took); the sizes too went by rank (4 lights at 512, 1 at 256, 16 at 128), two lights swapping sizes at once whenever their ranks crossed: 37 changes in a 15 second walk round the hall. Now: 192 faces and an atlas of 4096 x 3072 (32 x 24 cells), a point light's tiles 256 at most and a spot's 512, so 32 point lights have what they want whatever the others do (0 changes on the same walk; past 32 the old rules hold). A light that comes into view has its shadow at once, not faded in. And the edge is soft by the light's size (`kLocalLightSourceM`, 3.5 cm, till a light says its own), as Fernando's percentage-closer soft shadows: four looks round the way to the light for what's in the way and how far off it is (none: lit, and that's all), then eight across an edge as wide as the source looks from the point past that thing: sharp where a thing stands on what it shadows, wider the farther the shadow falls. A look is a way out from the light, and what the face holds that way is turned into a distance along it, the same whatever the tile's size (a tile changing size changes how fine an edge can be, nothing else); each look finds the face of the cube its own way lies through, so an edge runs across the cube's seams unbroken, and is asked of the surface's own plane, so there's little room to give and a shadow starts where the thing stands. An edge is never narrower than two texels, so the looks blur a tile's own steps. The looks are turned by a pixel's own turn, and TAA's sample turns them on: grain that TAA smooths. The path tracer's lights have the same size (a disk facing the point), so it's still what the shadows are measured against: the Cornell box 0.026 stops off, 1.1% of the picture off by over 25%.
  - *What it costs, and what was tried for it* (the shrine, 24 lights' shadows over most of the picture, 2560 x 1440, RTX 4080 laptop): the deferred light 1.9 ms where it was 0.6; the cull 1.9 where it was 1.5 (192 views, not 128); drawing the faces 0.6, as before. A look costs by its read of the atlas, not its arithmetic (no shadows 0.4 ms; one read a light 0.6; the looks with no read 0.8; all twelve 1.9): reads spread over a wide edge cost three to four times what the old nine in a block did. Tried and dropped: looks that read four texels at once (six gathers in place of twelve reads: 1.4 ms, and too few places looked at: blocks that jump under TAA, grain without it); every pixel of a frame turned alike (1.6 ms, the same blocks); no shadow for a light that's a small share of the strongest at a point (1.7 ms, and it shows: the far lights' light fills the near one's shadows); no shadow for a light that's a small share of all the light there (two dozen equal lights each fall under any share, and a pillar is left with no shadow at all). And one that looked wrong: every look through the one face the point's own way lies through, the faces seeing 15% past their edges (a tenth cheaper): a wide edge was cut off square along the cube's seams, a Y of straight lines on the wall behind the lantern, which the user saw at once. Kept: a cube's face worked out from the way itself, no matrix read (1.9 from 4.2 as first written), and the looks' places on their disks worked out beforehand.
  - *Tiles kept from frame to frame (2026-11-19; `r.local_shadow_cache`, on).* The atlas is a texture of the renderer's own now, not a frame's, and a light's tiles keep their cells in it while the light is lit and its size holds (`LocalLightsState::cells`; a light that goes, loses its shadow or changes size gives its cells back, and if what's free is too scattered for a new one, everything is laid out again). A light's faces are culled for and drawn again only when what they hold may be wrong (`LocalShadowFace::draw`): its tiles are new, the light itself turned or changed its reach or cone, or something that casts changed within its reach. The scene's renderer says where casters changed (`local_shadow_changes`): the frame carries only the instances that came, went or moved, so each such instance's bounding sphere, where it was and where it is, is a place of change, and so is one whose mesh has only now reached the GPU (it's drawn from that frame; the world coming in redraws what it touches as it lands). When everything is sent again, every tile is drawn. A face not drawn has no cull view (a plane nothing is inside, as an unused face) and no lists, and its tile isn't cleared. In the shrine, where nothing moves: the cull 0.46 ms where it was 1.9, no faces drawn where they were 0.6, the frame's GPU time 9.5 ms where it was 11.1; the pictures along a walk round the hall are the same kept as drawn every frame. Not covered: a mesh whose vertices move by a shader with its instance still (wind): its shadow in a kept tile stands still. Found by the tests on the way: a spot turned in place kept the tile of where it had pointed.
  - *A light says how much of a shadow it casts (2026-11-19).* `"shadow"` in a light (a cell's or a prefab's part's; `LightDesc::shadow`, 0 to 1, 1 unless said): at 0 it lights as before and takes no tiles, no faces and no looks; between, its shadow shows by that much. And a frame has shadows for no more lights than `r.local_shadow_lights` says (the most covering first; as many as there's room for unless said). The shrine's candles stand in threes that cast nearly one shadow, so the tallest of each group casts it and the other two don't (`props/candle_fill.prefab`): 9 lights with shadows of the 24. In its worst view at 2560 x 1440 the deferred light is 1.2 ms where it was 2.15 with all 24 casting (0.4 of it is the lights with no shadows at all), and the frame's GPU time 7.9 ms where it was 9.5. What it gives up shows where a group's own candles shadowed each other and the basin's rim: a little light in the basin and round the candles' feet that the all-casting picture hasn't. Cell format 11 and prefab format 3 (a light is 40 bytes).
  - *Where the shadows' cost stands* (the shrine's worst view, 2560 x 1440, RTX 4080 laptop, nothing moving): the looks about 0.85 ms, the cull's share about 0.1, no faces drawn: about 1 ms, where the day began at 1.6 (hard edges, three lights without, 37 swaps a walk) and stood at 3.5 after the soft edges alone. The aim agreed with the user: 1 ms as a rule, 1.5 at worst.
  - *A caster that moves is tested* (`tests/render/path_tracer_test.cpp`, the floor, the plate and the light, sent only what changed as the world sends it): with nothing changed no face is drawn; the plate moved 0.6 m aside, the light's six faces are drawn that one frame, and its shadow is where the path tracer has it (2.9% of the picture off by over 25%, 0.044 stops).
  - *Still to do, in this order:* a light's own source size; fewer reads a look (a small map of the nearest thing in each part of a face would answer most looks in one read).
- **Not yet, in this order:** lights in the fog; lights an effect carries and drives (VFX pass 5); a source with a size (softer highlights close to it); a budget by distance for thousands of placements; choosing a light by a click in the Playground (it's chosen in the hierarchy).

**Global illumination: the plan from 2026-11-14 (agreed; replaces the probe volume below, removed from the code the same day).** The camera-centred probe volume (below) was built in five steps and then judged by eye. In the Cornell box (tools/worldgen/cornell.py, with a Cycles render of it) it left faces black, corners wrong and patches dark, and each fix (relocation, a view bias, a finer cascade) moved the picture without anyone being able to say whether the light was right. Two things changed the plan: nothing measured whether a layer was correct, and a look at how Elden Ring does it.

- **What Elden Ring does** (an offline RenderDoc capture of an owned copy, 2026-11-14, read by script; findings in our words). No GI is computed at runtime. Bounced light is baked into boxes that nest coarse to fine: in one interior frame, a world layer about 15 km across and 64 m tall at about 536 m a voxel, a region of 6 km at about 100 m, tiles of 1 km at about 12 m, tiles of 256 m at about 4 m, and a room box of 112 x 65 x 51 m at about 4 m; boxes overlap by 3 m and fade into each other over it. Each voxel holds first-order spherical harmonics (four BC7 3D textures a set), and each box two sets and two cubemaps, blended by the hour (0.56 in that frame); colour is scaled per box. A pixel's ambient is one dot product with its normal. Twelve boxes drew in that frame. Three more captures: in Roundtable Hold, nine room-sized boxes (11 to 64 m) at 0.2 to 0.5 m a voxel and no blend by the hour; outdoors, up to 22 boxes, the 256 m tiles at 4 m over open ground and a few hand-placed boxes of 1 to 1.5 m round buildings. So its bounce is fine where it was built by hand (an interior's rooms at a few decimetres) and coarse over open ground, with screen-space AO for what's smaller.
- **The plan: split the light by distance, each part by what's good at it.**
  - *Far field, baked:* nested boxes as Elden Ring's, baked in the editor by our own GPU ray tracer (many rays a voxel, so no noise), down to 0.25 m voxels in room boxes where it matters, as Elden Ring's interiors are (a 50 x 20 x 50 m hall at 0.5 m is about 1.6 MB, at 0.25 m about 13 MB). A voxel the bake finds inside geometry is marked and skipped at lookup, which is what keeps light from leaking. Several baked keys through the day, blended by the hour. At runtime a lookup, about 0.1 ms.
  - *Hardware ray tracing is an option, never a requirement (decided 2026-11-14).* The base below runs on every GPU and is Elden Ring's level: the baked boxes (dense in rooms), GTAO for what's smaller than a voxel, shadow maps for the sun and local lights, cubemaps and screen-space reflections. Where the GPU has ray queries, options are added on top, each its own switch: the near field (below), ray-traced shadows, ray-traced reflections. Without them what's lost is what Elden Ring lacks too: bounce from moving lights and moving things. Ray tracing is required only on the machine that builds the game, for the bake and the reference.
  - *Near field, traced (an option):* at quarter resolution, a short ray (1 to 2 m) a pixel into its hemisphere. A hit takes that surface's live direct light (the sun through a shadow ray, the local lights) and the baked field there; a miss takes the baked field that way. Denoised over frames and space. This is what lights a block red from the wall beside it, and what makes a carried torch or the moving sun bounce up close; only the far, many-bounce part is baked. Estimated from our own rays (a ray and its shadow ray a pixel at 2560 x 1440 measured 0.6 to 0.75 ms): about 0.05 ms at quarter size, plus the denoise. Without ray queries, the base stands alone (GTAO, and screen-space light if it earns its place).
  - *Reflections:* a cubemap per box, darkened where the light there is darker than where it was captured (Elden Ring's trick), then screen-space reflections; ray-traced ones for water and wet stone as an option.
  - *What's given up:* far-field bounce doesn't follow geometry that moves (a door, destruction); the near field still sees it.
- **Each layer is measured before the next, in this order:** (1) a path tracer in the engine, a renderer of its own beside the rasterized one (Rendering Settings > Renderer, `r.path_traced`), with our own rays, materials and lights, adding up frames until it converges, and a compare tool (error per region of the rasterized picture against it, and a heat map), the path tracer proved by a furnace whose answer is known on paper and checked against Cycles; (2) direct light, with shadow maps for local lights, matching the reference with bounces off; (3) the baked boxes, where the Cornell box is meant to come out right in the base (a room box at 0.25 m), with GTAO; (4) reflections (cubemaps, screen space); (5) the ray-traced options, each against the reference: near field, shadows, reflections. Test scenes, simplest first: a furnace box (its answer is known on paper), the Cornell box, the shrine, Sponza.
- **Step 1, built 2026-11-14 (uncommitted): the path tracer and the compare.**
  - *The path tracer* (`render::PathTracer`, `engine/render/scene/path_tracer.h`, `shaders/scene/path_tracer.slang`): a renderer of its own, chosen by Rendering Settings > Renderer or `r.path_traced`. A sample a pixel a frame, each through a new point of the pixel, added to the mean of those before. A path takes each surface's glow, the sun and each local light through a ray toward it (so local lights cast shadows here), with the deferred light's BRDF, then goes on in a cosine-drawn direction scattered by the surface's colour, ended by Russian roulette after three bounces; what leaves into the sky brings the sky's light as the probes take it. Textures at their finest mip. It starts again when the camera, the picture's size, the sun and sky, the local lights or an instance changes (each instance slot's transform and mesh are remembered: the editor sends unchanged ones every few frames), and the frame stats' rays section says how many samples and why it last started again. While it's on, TAA, the probes and GTAO don't run. Not yet: bounces off mirror-like surfaces (only direct highlights), normal maps, translucency, the fog, see-through things, lights culled for being off screen.
  - *Proved by a furnace* (`tests/render/path_tracer_test.cpp`): inside a closed box whose walls glow by 0.5 and scatter half, the light is 0.5 / (1 - 0.5) = 1 on paper. With bounces held to 0, 1 and 2 every pixel is exactly 0.5, 0.75 and 0.875; unheld, 256 samples average 1.0002 over the picture, every pixel within 0.95 to 1.07.
  - *The compare* (`r.compare`, or Rendering Settings > Renderer > Compare with Path Traced): the rasterized picture drawn as always, the path tracer adding up hidden behind it, and a pass (`shaders/scene/lighting_compare.slang`) setting the rasterized light (before the fog) against its mean, per pixel in stops: shown as a heat map (grey agrees, blue too dark, red too bright, full at a stop) and summed into the stats (the mean error, the shares off by 10% and 25%, too dark and too bright). The sky isn't counted. In the furnace with no probes and no ambient, against the path tracer held to no bounces every pixel agrees (0 stops); with bounces every pixel is 0.999 stops too dark (1 on paper).
  - *Checked against Cycles* (2026-11-14): the Cornell box path traced from the world's start, its mean saved by `r.path_tracer_save` (a PFM, linear, in `user:/captures/`) and set against Cycles' render of the same view (`start.exr`, 1920 x 1080, OIDN-denoised; the point light converted as 4 pi times the engine's intensity, in watts) by a script in Blender's Python, ray for ray. The engine's light is 0.975 of Cycles' over the picture (-0.04 stops); away from edges 0.075 stops off on average (median 0.06), 0.6% of pixels off by over 25%. The white surfaces agree; the red and green walls are about 10% darker in the engine. Most likely the specular: Cycles' GGX keeps the light that scatters more than once among a rough surface's facets, ours (as most games') counts once, and on a fully rough dark wall the specular is a large share of what's seen. The rasterized picture uses the same BRDF, so the compare is unaffected; a compensation for it (a small table, after Kulla and Conty, 2017) is a shading change for later.
  - *Step 2, direct light, built 2026-11-14:* shadows for local lights (Local lights, above). The compare with the probes off and the path tracer held to no bounces (then `r.gi 0`; `r.ao 0`, `r.path_tracer_bounces 0`, `r.compare 1`): the Cornell box 0.03 stops off on average, 1% off by over 10%, at shadow edges. The compare's mean now counts a pixel as 2 stops off at most: a shadow's edge black in one picture and dimly lit in the other was ten stops, and a few such pixels drowned the rest.
  - *First reading, the Cornell box with the probe volume as it stands:* 0.27 stops off on average, 40% of the picture off by over 10%, 19% by over 25%; 17% too dark (block faces, corners: the bounce the probes miss) and 23% too bright (where the light's shadows fall: local lights cast none in the rasterized picture). The baseline later layers are measured against.
- **Step 3a, built 2026-11-14 (uncommitted): light boxes, baked and read.**
  - *A box* (`render::LightBoxes`, `engine/render/scene/light_boxes.h`; `LightBoxDesc`: its corners, a voxel's size, its fade): four RGBA16F 3D maps per box, first-order spherical harmonics as Elden Ring keeps them, put so that the light a surface facing n gathers, over pi, is a + b . n (what `hemisphere()` gives). Alpha of the first map says whether the voxel is inside something.
  - *The bake* (`shaders/scene/light_box_bake.slang`, compute, a thread a voxel): paths from the voxel's middle in directions over the whole sphere, through the path tracer's own code (`shaders/scene/path_trace.slang`, `trace_path`: same materials, lights and sky), all light but a light's direct light. 1,024 rays a voxel, 32 at a time, about a million paths a frame, so a box sharpens over the frames after it's placed. A voxel whose rays meet backs more than a quarter of the time is inside something and kept out of the lookup.
  - *The lookup* (`shaders/scene/light_boxes.slang`, `light_box_light`): the finest box holding a point, its eight voxels round a point half a voxel off the surface, by nearness, validity and how far each is in front of the surface; faded to the flat ambient over the box's fade (2 voxels unless said). The deferred light takes it for the diffuse and mirror ambient in place of the hemisphere; water and the fog's ambient read it too.
  - *Placing one, for now by console:* `r.light_box X Y Z X Y Z [VOXEL] [FADE]`, `r.light_box clear`, `r.light_box rebake`; `r.light_boxes 0` compares with the flat ambient; Rendering Settings > Light Boxes in the editor. Not saved yet (3b).
  - *Measured:* the furnace with a box 0.045 stops off; the Cornell box at 0.125 m voxels 0.19 stops off on average, 5% of the picture too dark by over 10% and 25% too bright (in the local light's shadows, where first-order harmonics can't hold light that changes sharply with direction; Elden Ring's format has the same limit) and blotchy from the bake's noise. Next: denoise the bake (Elden Ring's tools have such kernels), then 3b (placing and saving boxes, nested over a world) and 3c (keys through the day).
- **The probe volume (the first plan, below) was removed from the code on 2026-11-14,** on the user's word, once the path tracer could measure it: its probes, passes, shaders (`gi*.slang`), tests, `r.gi`, `r.gi_relocate`, `r.gi_probes`, the probe view and its stats. What stays from that plan: rays against the scene (`TraceScene`, step 1), GTAO (step 5), and the water's use of the light where it stands (now the light boxes'). Where no light box holds a point, the ambient is the flat hemisphere again.
- **Other kinds of game.** The GI method is the project's choice (its `config/`), never named by the engine; both feed the same slot in the light pass. For a top-down action RPG (Path of Exile's kind: generated levels, hundreds of moving spell lights, a camera that shows nearly everything from above), the planned second method is screen-space radiance cascades (Sannikov; what Path of Exile 2 uses, by its developers' account), fully dynamic and unbaked, built when there's such a project to judge it by. Shared by every game: the reference and compare tool, direct light, AO and reflections.
- **Radiance cascades (started 2026-11-14, on the user's word: built ahead of the light boxes' next steps).** The GI for top-down games (Path of Exile's kind), followed from Sannikov's paper ("Radiance Cascades: A Novel Approach to Calculating Global Illumination", its screen-space probes) and his ExileCon 2023 and 2026 talks, written in our own words and code.
  - *What Path of Exile 2 does* (the talks): probes on the screen in cascades, rays marched through the depth buffer, merged from the farthest cascade down, traced again every frame with nothing kept from the last (lights come and go at once, no ghosting), a fixed cost of 1 to 2 ms whatever is on screen; every glowing pixel lights the scene for free. What's off the screen or behind what it shows isn't seen: a fixed top-down camera makes that acceptable. In 2026 a world-space version (sparse radiance cascades, probes on visible surfaces at a constant size on the screen, about 5 ms) needs hardware ray tracing and isn't shipped; the paper "Split Radiance Cascades" (Freeman, Sannikov, Pitt-Francis, arXiv 2607.20384) describes it. That one is a later option for us: we trace rays already.
  - *Rebuilt from nothing on 2026-11-18, on the user's word* ("delete all GI work and start from scratch"): whether the engine is kept for the game turns on this. The first build (2026-11-14) kept each probe's light over an octahedral map of the whole sphere, marched each ray in 12 steps in the world, and had its finest probes 8 pixels apart: it lit what stood next to a glow and little else. What replaces it follows what the 2023 talk's "cascades encoding" slide shows of the method itself: a probe's directions are a thin strip, few one way and doubling the other.
  - *The method* (`shaders/scene/rc_common.slang`; `render::RadianceCascades`, `engine/render/scene/radiance_cascades.h`): a direction is an azimuth (a direction on the screen) and an elevation (4 bins of equal solid angle, from toward the camera to away from it). Every ray along one azimuth crosses the same pixels, so one march along the screen serves every elevation, as horizon-based occlusion does. Cascade 0's probes are the pixels of the GI picture (the screen at half its size each way, below), with 4 azimuths and a reach of 2 of its pixels; each next cascade has probes twice as far apart each way, twice the azimuths and an interval twice as long and as far out (the paper's penumbra condition), so each holds half the texels of the one before; 9 cascades cross a 1600 x 900 screen's 800 x 450 GI picture. What a slice keeps isn't a hit per ray but what's covered: a horizon (everything below a cos, starting at the probe's own tangent plane and rising as what stands on it does) with how near what it rose to is, one floating piece above it, and per bin the light of what covered it. So a flat floor never shadows itself and nothing flickers as a thing crosses a bin.
  - *Each frame:* `rc glow` (the luminance of the HDR target before the deferred light adds to it: what surfaces emit, kept at the GI picture's size), `rc pyramid` (the GI picture: depth, the surface's normal from the depth with the share of its light that it emits, and the lit picture, each with up to 10 mips: a march's sample reads the mip as coarse as its samples are apart, so a small bright thing is never stepped over and a far cascade's march is tens of samples, not hundreds), then a `radiance cascade` pass per stored cascade, the topmost first (`shaders/scene/radiance_cascades.slang`): each thread a probe and a stored azimuth (the mean of two of the cascade's own, the paper's pre-averaging), marched and merged with the cascade above. A cascade of few probes (under 65536 threads) is wide: a thread one azimuth, and every azimuth stored, the cascade below taking the mean of each two as it reads them; such a pass can't fill the GPU and takes as long as one thread does, so half the work a thread is half the time. Then `rc gather` (`shaders/scene/rc_gather.slang`): cascade 0 marched for each of the GI picture's pixels and merged with cascade 1; what's kept is the light's first two moments over directions, each colour's apart (how much arrives in all, and how much times the way it comes from), each part of each wedge in closed form: the sky's light through what's open, the light of what covers the rest. Last `rc apply` (`shaders/scene/rc_apply.slang`), the only one of them at the screen's size (so it reads little: the four depths in one gather, the four pixels' moments by two filtered samples a colour): for each pixel the four GI pixels round it, each counting by nearness, by how near the pixel is to the plane it stands on and by how alike the normals are, and from their moments the light along the pixel's own normal (the material's normal map and all), times the material's occlusion, added as the diffuse ambient, and with it the specular ambient, from the same moments (below); the deferred light leaves both out while they're on, and GTAO doesn't run (the cascades see what's near themselves).
  - *The GI picture, and why moments* (`r.rc_scale`; 2026-11-18, for the cost): everything but the glow and `rc apply` works on the screen at 1 / scale of its size each way. A pixel of it is one of the screen's for its depth and its surface (a probe stands on a real surface, not a mean of two) and the mean of its block of the screen for its light (a spark a pixel wide is never missed). Left at 0, the scale is the most that keeps the GI picture 360 pixels high or more: 2 up to 900p, 3 at 1080p, 4 from 1440p. Measured, the light is as right with the GI picture at a quarter of a 2560 x 1440 screen as at a third, a half or the whole (below), and as steady as the camera moves (pictures half a pixel apart differ no more at 2 than at 1). What tells isn't the GI picture's size but how far apart a far march's samples are on the screen: at 16 of the screen's pixels every scale measures alike, and at 24 to 32 (what 8 of the GI picture's pixels came to at a third and a quarter, before the cap was a length on the screen) the sun-lit room was 0.03 stops farther off. What a smaller GI picture does give up is the finest creases, a GI pixel wide. What makes a smaller picture do is keeping the moments rather than the light on the GI pixel's own surface: nothing arrives from behind the surface a probe stands on, so for a normal that is that surface's the light is exactly moment 1 . n (the integral of the radiance times the cosine), and each of the screen's pixels takes it with its own normal. For a normal turned well away from that surface's (borrowed across a corner) it goes over to the first two spherical harmonics, moment 0 / 4 + (moment 1 . n) / 2. Tried first and dropped: the spherical harmonics for every pixel, which put the lone wall from 0.07 stops off to 0.13 (they count light from near the horizon for too much and light from overhead for too little).
  - *What surfaces mirror* (the specular ambient; 2026-11-19): from the same moments, in `rc apply`, in place of the flat sky's. It was the flat sky's until then, and in a hall with a roof every smooth thing had a blue sheen at its edges (the shrine's candles): the sky, mirrored by wax that couldn't see it, and bounced on by the cascades too, since the deferred light had already put it in the picture they march. Now the light along the mirrored way r is the plainest light that has the moments: over the hemisphere above a surface of normal n, light a + b . w has moment 0 = 2 pi a + pi b . n and moment 1 = pi a n + (2 pi / 3) b, so a = (2 / pi) (moment 0 - 1.5 moment 1 . n) and the light along r is a (1 - 1.5 n . r) + (3 / (2 pi)) moment 1 . r. An even light comes out even whichever way r points; the first two spherical harmonics, which take the dark under the surface for part of the light, would halve it at a grazing r, where a surface mirrors most (they're still what a normal turned away from the gathered surface gets, as for the diffuse light). A rough surface mirrors a wider lobe, which takes less of the light's lean: b . r counts by 1 - roughness / 3, so the roughest gets the diffuse light's own formula. Then the same environment BRDF, ground share and material occlusion the flat one had. It is a broad light, no picture of what's mirrored (sharp reflections want rays of their own); what it has is the place's light: its colour, the way it comes from, what hides it. On open ground under the even sky it comes to 0.85 of the flat sky's (the gather's four wide wedges cut the hemisphere roughly; the diffuse light, moment 1 . n alone, is within 3%); the path tracer has no specular ambient, so there's nothing truer to set it against yet. Cost at 2560 x 1440: `rc apply` 0.02 ms more, the deferred light 0.05 less (it skips its whole ambient, the light boxes' too).
  - *Past the picture's edge* (2026-11-19): a march that leaves the picture takes the surface its line crosses the edge on to go on as its own plane (`rc_past_edge`, `rc_common`): a probe standing in front of that plane has it hiding everything under the plane's way through the slice, with that surface's light; a probe on the plane or behind it is hidden nothing, and sky at the edge goes on as sky. Before, nothing past the edge was known and that was taken for open, which let the sky in from under the screen: a wall's face near the picture's foot was lit from below through where the ground goes on unseen. And only sometimes: when no probe of a far cascade stood on that face it borrowed the light of probes on the ground beside it (`rc_upper`), which do take the ground to go on, 0.3 stops darker. Which of the two a face got turned on a pixel's worth of where a far cascade's few probes fell, so under TAA's sub-pixel offsets the whole face jumped between them, twice in sixteen frames (the user saw it as flicker; the measuring room's near wall, 153 and 190 of 255). With the edge taken to go on both ways agree. What it costs in truth: a thing that ends just past the edge is taken to go on.
  - *More than one bounce* (`r.rc_bounces`, on; 2026-11-19): `rc gather` keeps, beside the moments, what each of the GI picture's surfaces sends on of the light gathered there (its block's mean base colour, less its metalness, times its material's occlusion, times moment 1 along the probe's normal over pi), in a texture kept to the next frame (two, turn about); the next frame's `rc pyramid` adds it to the lit picture's light, read where the surface was on the screen then (the last frame's view; nothing kept counts after a cut, a resize or a frame not drawn). So a frame adds a bounce and a still picture holds them all after a few; what has bounced lights what moves a frame late. Without it a surface lit only by the sky or by bounced light sends nothing on, and with the edge no longer letting the sky in that showed: the wall's face had no light off the sky-lit ground at all. The loop can't run away: each turn multiplies by a surface's colour and by the share of a hemisphere that what's on the screen fills.
  - *Under TAA's offsets* (2026-11-19): the cascades stand on pixels, so a sub-pixel offset can change which surface a probe stands on, and the picture with it. `r.jitter N` (with `r.taa 0`) holds the Nth of TAA's sixteen offsets, so a still shows it: over the sixteen, in the measuring room at 1600 x 900, the share of pixels whose brightness (blurred over 5 x 5, so a silhouette moving a pixel counts little) ranges over more than 8 of 255 was 9.8% and is 5.0% (0.3% with no cascades), over more than 16, 6.6% and 1.1%; under TAA itself, pixels that change by more than 8 at some point in eight frames, 6.4% and 1.4%, and in the shrine seen from above 0.12%. What's left is on faces narrower than a far cascade's probes are apart (the pillars' sides): the same borrowing, at a smaller size.
  - *How open to the sky a place is* (`open_sky`, `shaders/scene/sky.slang`; 2026-11-19): fog, lit particles, water and what's see-through take the sky's ambient as under an open sky, and the cascades fill what the screen can't answer with the sky: none of them sees what's above a place. In the shrine that was most of the picture: the surfaces as the cascades lit them were dark and warm (the far wall 0.0005 in the HDR target) and the fog laid 0.03 to 0.045 of the sky's blue over all of it, which the auto exposure then brightened (the user asked why the hall was still blue). So a place says how far it's closed to the sky, 0 to 1. The world's environment says it for the whole world (`"under_roof"` in the .environment: a dungeon is 1), and a fog volume says it for its own place over its soft edge (`"under_roof"` in its `fog_volume`, which may then have no density: a place, no fog): a hall with a roof in an open world, a courtyard in a roofed one. The frame's constants carry the world's (`SceneFrame::sky_color.w`) and the volumes that say it, first among the fog volumes (`sky_volume_count`, `fog_volumes`); `open_sky` at a point is the world's, then each such volume's by how far inside it the point is. `rc gather` asks it for each of the GI picture's surfaces, half a meter in front of the surface (a wall is thinner than a volume's edge is soft, and its two faces stand in different places); the fog's froxels, particles, water and see-through things ask it where they are. Only while the cascades light the picture: the flat ambient they stand in for knows no roof either. `r.rc_sky` still multiplies it all. The shrine's hall is one round volume, a sphere about the dome's centre that ends inside the dome's shell (8.45 m, a soft edge of 0.12): inside, a dark hall with the door's light and the candles' pools; outside and from above, as before. (First drawn to the wall's outer face, 8.1 m with an edge of 0.4, it left the dome's ribs half open: a rib's side asks about the sky half a meter sideways, which landed in the sphere's soft edge, and under a dark hall's exposure the ribs shone white; the user saw it looking up. A volume has to hold the surfaces it roofs with room to spare, which is one more reason to work roofs out and not draw them.) What it isn't: worked out from what stands above a place (a map of the roofs from above would need no volumes), and it doesn't let the sky seen through a door on the screen light the hall (what's open in a slice is all taken by the probe's own place).
  - *The shadow's tint under a roof* (2026-11-19): shadowed sunlight keeps the environment's shadow colour (the look's painterly shadows), and it kept it in a hall too: a sun shining dimly through the walls. The diffuse part hardly shows, but its highlight does: on a bumpy wall seen edge on, every bump whose normal mirrors the sun's way to the camera glinted blue-white, and the auto exposure of a dark hall made specks of them (the user's picture, looking up the shrine's wall; found by reading the editor's camera with `camera.where` and taking the same view apart term by term: the specks were in the picture before `rc apply` added anything). The tint is the sky's doing, so it's kept by how open to the sky the place is (`open_sky`, asked half a meter in front of the surface as the depth shows it: a bump's own normal can lean out of the hall), in the deferred light, on particles and on what's see-through. On the way, `rc apply` got a third rank: where none of the four GI pixels round a pixel is near its surface or close by (the same bumpy wall edge on: its normals turn from the surface the GI pixels stand on, and those are meters apart along it), any that stand on a surface count, by nearness on the screen, before the flat ambient does.
  - *The merge, and what it took to get right* (each found by the compare below): the four probes of the cascade above round a probe count by nearness, by how near they stand to its plane (hardly at all behind it) and by how alike their surfaces face, and where none are alike, by how far away they are; with none, what's past the probe's own interval is taken as open (a wall's top isn't darkened by the floor at its foot). What a probe of the cascade above saw is set where this probe would see it, from how near it was (nearer a wall, the wall stands taller: without it corners are too bright), and its light is laid in the bins where this probe sees it. Its own ground (the plane of the surface it stands on) hides something from this probe only if this probe is in front of that plane. A thing that glows is taken to glow from its unseen sides too; a thing lit by a light is dark on them.
  - *Thickness:* seen from above, what stands on the ground is solid down to it, so by default the picture is taken as a height field (`r.rc_thickness 100`): smooth, right for walls and what stands on the floor, but a tall thin thing (a pillar) shadows too wide a strip behind it. Under a meter, light passes behind such things (the floating piece), but with patches where pieces meet: not yet fit to use, and what a camera along the ground would need.
  - *Switching and tuning it:* Rendering Settings > Radiance Cascades, or `r.radiance_cascades`. `r.rc_scale` (0: by the screen's height), `r.rc_azimuths` (4), `r.rc_interval` (2 of the GI picture's pixels), `r.rc_max_step` (16 of the screen's pixels between a march's samples at most: a far cascade's march reads the mip as coarse as its samples are apart, and past 16 its depth is too blurred, 0.03 stops off at 32 and 0.25 to 0.3 past 64), `r.rc_march_steps` (32 samples a march at most, the samples standing farther apart past that: only the topmost cascades' are that long, and the light measures the same as with no cap; at 16 the sun-lit room is 0.005 to 0.01 stops farther off, for 0.03 ms), `r.rc_cascades` (10 at most), `r.rc_thickness`, `r.rc_tolerance`, `r.rc_sky` and `r.rc_bounce` (what each kind of light is multiplied by; how far a place is closed to the sky is its environment's and its fog volumes' to say, `under_roof`), `r.rc_bounces` (light bounces more than once; 0: once, to compare with the path tracer held to one bounce), `r.rc_white_sky` (with `r.view indirect_light`: what's open, as Path of Exile 2's "light only" slides show it), `r.rc_material_ao`. Off by default.
  - *Measured against the path tracer held to one bounce* (what the cascades stand for: `r.path_tracer_bounces 1`, `r.compare 1`, with `r.specular_ambient 0` and `r.rc_material_ao 0`, which the path tracer has neither of), 2026-11-18, mean stops off. The four measuring scenes (`tools/worldgen/rc_plain.py`: the rc_room's hall on wide ground, in plain colours, nothing textured or flickering, under an even sky, under a low sun, lit only by what glows; and one long wall on the ground under the even sky), as shipped: at 1600 x 900 (the GI picture at a half) 0.22, 0.16, 0.59 and 0.073; at 1920 x 1080 (a third) 0.21, 0.14, 0.57 and 0.082; at 2560 x 1440 (a quarter) 0.21, 0.145, 0.57 and 0.087. The flat ambient with GTAO, at 1600 x 900: 0.77, 0.35 and 0.89. Before the GI picture went to a quarter at 2560 x 1440 (a third, its samples 24 pixels apart): 0.21, 0.155, 0.635 and 0.083. The first build of the day, everything at the screen's size and gathered in closed form round each pixel's normal, at 1600 x 900: 0.21, 0.12, 0.64 and 0.07. The crowded scene the cost is timed in (`rc_stress_sun`: blocks half a meter apart over the whole screen, every seventh glowing; deep narrow gaps, hard for anything on the screen) at 2560 x 1440: 0.69, and 0.72 before. The numbers move by about 0.01 with where the far cascades' few probes happen to fall, so differences that size between settings mean nothing. What's left in the room is mostly what the picture doesn't show: the hidden inner face of the wall nearest the camera, the backs of pillars.
  - *Measured again, 2026-11-19* (1600 x 900, the same four scenes; with the edge taken to go on, more than one bounce and the specular from the moments). Against the path tracer with all its bounces (16), which is what the picture is meant to be: 0.35, 0.26, 0.82 and 0.12 stops off, where the build of the day before was 0.38, 0.38, 0.94 and 0.18. Held to one bounce against the path tracer held to one (`r.rc_bounces 0`): 0.29, 0.14, 0.59 and 0.12, where it was 0.22, 0.16, 0.59 and 0.073: the even sky and the lone wall are farther off that way because the path tracer's one bounce has the sky's light off the ground in it, which the sky let in at the edge used to stand in for. What has bounced is still too little, about half of what the path tracer's later bounces add in the room (the room is 0.29 stops dark all over against 16 bounces; with `r.rc_bounce 2` it's 0.07): what the screen doesn't show sends nothing on (a wall's hidden face, a pillar's back), a surface seen edge on counts for less, and each turn of the loop loses that again. `r.rc_bounce` is left at 1; raising it is a look, not a measure. In the shrine with the sky's factor at 0 the picture is close to the path tracer's: a dark hall, the door's light across the floor, the candles' pools.
  - *Cost* (dev build, RTX 4080 laptop; the passes' GPU times as `--stats` logs them for a scripted run, the scene the crowded one, `rc_stress_sun`, since a screen with sky in it costs less: 0.3 ms for a screen of sky). At 2560 x 1440, the game's target: `rc glow` 0.04 ms, `rc pyramid` 0.08, the eight `radiance cascade` passes 0.52 together (from cascade 1 up: 0.17, 0.10, 0.07, 0.06, 0.05, 0.04, 0.03, 0.01), `rc gather` 0.31, `rc apply` 0.24: 1.18 ms, and 1.13 by the frame's own time with them on and off. At 1920 x 1080 about 1.2, at 3840 x 2160 2.6 to 2.7; at 1600 x 900 about 1.4, though there the frame is the CPU's and the GPU's clock sinks, so that number is soft. Path of Exile 2 reports 1 to 2 ms "depending on your hardware". At 2560 x 1440 it was 1.80 ms (1.71 by the frame's time) before the GI picture went from a third to a quarter of the screen with the march's widest step a length on the screen; with the GI picture the same size the passes' code is 3 to 4% faster than it was, no more. The day's first build was 5.1 ms at 1600 x 900, everything at the screen's size.
  - *Cost, 2026-11-19* (the same scene and size, the two builds by turns, three rounds; the laptop ran slower that day, the build of the day before measuring 1.13 to 1.30 ms): the GI's passes 0.08 to 0.19 ms more (the cascades 0.03 to 0.09: the march's code for the edge; `rc apply` 0.04 to 0.09: the specular; the gather 0.01 to 0.03: what's sent on), the deferred light 0.05 to 0.09 less (no ambient of its own): 0.02 to 0.09 ms more over all, 1.43 to 1.55 against 1.34 to 1.49. The 1 ms asked for is that much farther off.
  - *Where the time goes, and what didn't help* (2560 x 1440, the GI picture a quarter, each part left out in turn): reading the cascade above in the merge 0.19 ms, laying its light in this probe's bins 0.16, the marches 0.24, the gather's integral 0.05, `rc apply` 0.25, the pyramid and the glow 0.13, the rest (a probe's own setup, finding the four probes above, the writes) about 0.2. Tried and dropped, each measured: laying the cascade above's light once a merge, from the four probes' edges put together, in place of once a probe (no faster, no righter); a probe's four elevations as a square of texels so a merge reads one place in memory (the cascade passes 15% slower); fewer cascades with the topmost marching to the picture's edge (no cheaper, the lone wall 0.075 to 0.10 stops off); a cap of 16 or 8 samples a march (0.03 ms for 0.01 stops); taking a thing a probe stands past as hiding nothing in its slice (no nearer the path tracer). Kept, though each is worth little: the gather's arcsines halved (along_up_to rises with its cos, so held inside a bin it's the cos's own held between the bin's), cascade 0's samples read unfiltered (they fall on pixels' middles), a height field's columns not given a back, the glow kept at the GI picture's size and the pyramid's first mip reading a block's light by filtered samples, `rc apply` reading less. And one fault found on the way: a mip of a size that isn't even was read as if it covered the whole picture, so a far sample landed beside where it was meant (by up to 6 pixels at mip 3 of an 854-wide picture).
  - *Timing it on a laptop:* a GPU that was idle takes about four seconds to bring its clock up (780 MHz to 1755 here), so a short run times the climb, not the pass: `--stats` logs the mean of the run's last whole window of 512 frames, a run is 3000 frames, and two builds are set against each other by turns, the old one kept as a second executable with its shaders copied in before each of its runs. Before that, differences of 0.3 ms between runs were the clock's. The timestamps themselves cost about 0.06 ms a frame over all its passes (`--no-gpu-timing` runs without them).
  - *The test scenes:* `tools/worldgen/rc_room.py`, a 20 x 12 m stone hall open to a night sky, seen from above (Poly Haven's cobblestone_floor_06 and castle_wall_slates, the shrine's props, a rune and a crystal that glow with no light), and `tools/worldgen/rc_plain.py`, the measuring room and the lone wall, each in the private project.
  - *Next, in order:* how open a place is from what stands above it, not from volumes placed by hand (a map of the roofs seen from above), and the sky seen through a door lighting the hall; the shimmer left on faces narrower than a far cascade's probes are apart, and the patches on thin things and with a small thickness (one fault: a surface with no probe of its own up there); the bounced light that's missing (what the screen doesn't show sends nothing on: a guess at a hidden face's light from the faces beside it); sharp reflections for smooth things (the cascades' are broad); what's transparent (water, glass, particles) and the fog take the flat sky's ambient by how open their place is, not the place's own light; the cost, now about 1.3 ms at 2560 x 1440 against the 1 ms asked for (the march's edge code runs for every march; `rc apply` could go into the deferred light's own pass now that the last frame's light is kept); a GPU weaker than the one it was measured on; the project's config choosing it (the doc's rule; a switch for now).

**Global illumination, the first plan (agreed 2026-11-12, built 2026-11-12 to 11-14; replaced by the plan above, its probe volume removed from the code 2026-11-14: kept here as a record of what was tried and measured).** What the shrine shows without it: direct sun through the door, and everything else lit by the same flat sky colour indoors as out; no bounce, no darkness, and local lights that light only what they reach. The user chose the approach from a presentation of it and its alternative ("i agree on that gi proposal, lets try it").

- **What picks the approach.** Time of day and weather change, so nothing is baked. The world is 20 x 9 km and streamed. The frame is 4.17 ms on the dev machine (240 fps at 2K), 8.3 ms on the 120 fps class. Ray tracing is optional. One person and agents: no bake pipeline to tend.
- **The approach: one probe volume for diffuse indirect light.** A grid of probes in the world, centred on the camera, in nested cascades of doubling spacing (fine near, coarse far). Each probe keeps the light arriving at it from every direction (a small octahedral map of irradiance) and how far it sees each way (a map of distance and its square), so a surface asks only the probes that can see it and light doesn't leak through walls. The method of Majercik et al. (DDGI, 2019, 2021), written here from the papers.
- **How probes are filled.** A few probes' worth of rays each frame: where a ray lands, the surface's colour times the light falling on it (the sun through a shadow ray, the local lights, and the probes' own light from last frame, which is what makes more than one bounce) comes back to the probe, blended into what it had. The update is spread over frames: how many rays a frame is the cost, and a dial.
- **What rays are traced against.** First: the scene's meshes, by hardware ray queries (`VK_KHR_ray_query`) against acceleration structures built from the geometry pool. The first test scene is an interior made of meshes, and every GPU class this doc names has ray queries. Second: the terrain's heightfield, marched in the shader, for the open world and for GPUs without ray queries. Whether a GPU without ray queries gets anything more than terrain and sky indoors is open: "must look right with RT off" will need an answer there (a coarse signed distance field of the level is the usual one).
- **With it:** ambient occlusion in screen space (GTAO) for the contact-scale detail probes can't see (step 5, below).
- **Budget:** 0.5 ms a frame at 240 fps as a first limit, to be measured.
- **The order it's built in,** each step seen and measured before the next: (1) acceleration structures in the RHI and a top level over the scene's instances, with a debug view that draws what rays hit; (2) one cascade of probes lighting the shrine: traced, shaded by the sun, local lights and sky, applied in the deferred light in place of the flat ambient; (3) more than one bounce, distance maps against leaks, probes that sleep; (4) cascades that follow the camera; (5) GTAO; (6) the terrain's heightfield. Steps 1 to 5 are built (below).
- **Step 1, built 2026-11-12:** rays against the scene's meshes, and a view of what they meet.
  - *The RHI:* bottom-level and top-level acceleration structures (`create_bottom_level`, `create_top_level`, `build_bottom_level`, `build_top_level`, `acceleration_address`), only with `Capabilities::ray_query`. A shader makes its top level from an address, as it reaches a buffer: no descriptor slot.
  - *`render::TraceScene`* (`engine/render/scene/trace_scene.h`), which the game's renderer has where the GPU has ray queries (`SceneRendererDesc::ray_tracing`; a preview's doesn't): a bottom level for each mesh an instance uses, built once from the geometry pool, 16 a frame at most, and let go when no instance uses the mesh, it's unloaded or cooked again. One top level over every instance slot, built whole each frame from instances a compute pass writes from the renderer's own instance table, so nothing is per instance on the CPU.
  - *Which LOD rays see:* the coarsest within 2 cm of LOD 0 that has all its parts and kept its seams. A LOD whose seams moved as it was simplified has triangles across uv islands: drawn small that doesn't show, but a ray reads the surface's colour off it. The cooker says which those are (`MeshLod::seams_moved`, mesh format 3). The statue in the shrine, a generated model with its uvs in pieces, is traced at LOD 0 for that.
  - *Shaders* trace through `scene.trace` (`trace_nearest`, `trace_blocked`, and `trace_surface`: the hit's position, normal, base colour, emissive, roughness and metalness, its material's maps read 3 mips down). `SceneFrame::trace` points at the frame's scene for rays.
  - *The view* `ray traced` (F2, `r.view ray_traced`, the Playground's view list): a ray a pixel from the camera, the surface it meets lit by the sun through a shadow ray and by the sky. Where it differs from the lit view in shape or place, rays and the picture disagree about the scene.
  - *Not in it yet:* everything is solid to a ray (what's cut out or blends stops it like stone); the terrain, grass, water and particles aren't there; the top level holds every instance, near or far (a world's wants only what's near the camera, and foliage out of it or as proxies); bottom levels aren't compacted; a mesh of more than 64 parts is traced by its first 64.
  - *What it costs, first numbers* (`tests/render/trace_scene_test.cpp`'s benchmark, 2560 x 1440 offscreen, validation on, the GPU shared with a game being recorded: to be measured again alone): the top level built whole each frame, 0.07 ms over 64 instances, 0.13 ms over 4,096, 0.19 ms over 32,761; writing its instances, under 0.01 ms; a ray a pixel with its shadow ray and three texture reads (the debug view: 3.7 million pixels), 0.9 to 1.1 ms, against bottom levels of 36 triangles, so a deep mesh will cost more a ray. The shrine: 11 meshes of 39,500 triangles, 49 instance slots. Against the 0.5 ms meant for GI, the top level's build is the fixed part; a few hundred thousand rays a frame fit beside it.
- **Step 2, built 2026-11-12:** one volume of probes, lighting what's inside it in place of the flat ambient.
  - *The volume* (`render::GiProbes`, `engine/render/scene/gi_probes.h`): 24 x 12 x 24 probes 1.5 m apart, 36 m square and 18 m high, round the camera (a third of its height below it, two thirds above), so a hall is inside it whole from anywhere in it. It moves a whole spacing at a time and its probes wrap round it: a probe that stands for a cell of the world keeps its place in the atlas while the volume holds that cell, so moving copies nothing and only the probes that came in start again. Probes stand in the middle of the world's cells, so a floor or wall laid on whole meters doesn't run through them.
  - *A probe's map:* 6 x 6 texels of an octahedral map of directions with a border of one (8 x 8 in an RGBA16F atlas of 2304 x 192), holding the light gathered round each direction by the cosine, over pi: what `hemisphere()` gave, so the two swap for each other. Alpha is how much the probe counts: 0 for one inside something (more than a quarter of its rays met the back of a surface).
  - *Each frame, three compute passes:* `gi probes` (those that came in start again; the rest age), `gi trace` (64 rays a probe, evenly over the sphere, turned as a whole by a new random turn each frame: where one lands, the surface's own glow and its colour times the sun's light, through a shadow ray, and the local lights that reach the volume, up to 64, without shadows; where it meets nothing, the sky), `gi blend` (the rays gathered into the map: an even mean over a probe's first fills, then 3% of the new each fill).
  - *The sky a ray sees* is the frame's flat ambient, as light from that way, shaped so that under an open sky a probe gives a surface exactly what the flat ambient gave it. So the tuned outdoor look is unchanged, and indoors only what gets in is there.
  - *Reading it* (`shaders/scene/gi.slang`, `gi_light`): the eight probes round a point a quarter spacing off the surface, weighed by nearness, by whether they're on the surface's side of it, and by their alpha; over the volume's outermost cell it fades to the flat ambient. The deferred light takes it for the diffuse ambient and, round the mirror direction, for the specular ambient (the first of the reflection layers below: an interior stops mirroring the sky).
  - *Water* takes where it stands too (found in the shrine's basin, which glowed white seen from above: its own colour was lit by the open sky and by a sun only the terrain could shadow, and what it mirrored and couldn't find on the screen was the sky): its own colour and its foam are lit by the probes' light there in place of the sky's, the sun on it goes through the sun's cascades as well as the terrain's baked shadows, and the sky it falls back to mirroring is dimmed by the share of open sky the probes see that way. Under an open sky all three are what they were. What it mirrors of the hall itself is still only what's on the screen: the reflection layers below.
  - *Seeing and comparing it:* the view `indirect light` (the ambient falling on each surface, whatever its colour); `r.gi 0` in the console, or Rendering Settings > Global Illumination in the editor, turns it off; the frame stats' `rays` section counts probes, rays and lights.
  - *What it shows in the shrine:* the hall is dark where it was evenly lit; the sun's patch by the door lights the wall and floor round it, each group of candles the stone round it, the sky comes in at the door. It is darker than it will be: one bounce only (a surface lit only by the ambient sends nothing on), and the exposure is still the outdoor one.
  - *What it costs, first numbers* (the GPU shared, as above): in the shrine at the editor's window, rays 0.13 ms, maps 0.16 ms, the deferred light 0.07 ms more; in `tests/render/gi_probes_test.cpp`'s benchmark (a hall of five walls, 21 candles, 2560 x 1440), rays 0.08 ms, maps 0.08 ms, the deferred light 0.08 ms more. With the top level's build that is at or under the 0.5 ms meant for it, every probe filled every frame; `GiProbes::set_phases` fills each one frame in several.
  - *Not in it yet, and what shows for it:* shadows for the local lights at what a ray meets; what's see-through and particles are lit by the flat ambient still; fireflies from small bright things aren't clamped.
- **Step 3, built 2026-11-12:** more than one bounce, probes that know how far they see, and probes that sleep.
  - *More than one bounce:* where a ray lands, the surface also scatters the light that has already bounced there: the probes' own, as they stood last frame (the flat ambient past the volume). Each frame adds a bounce to what the maps hold, so a wall the sun never reaches lights the floor beside it.
  - *How far a probe sees* (`shaders/scene/gi_see.slang`): a second atlas, laid out as the first with maps twice as fine (14 x 14 texels and a border, RGBA16F, 4608 x 384), holds for each direction the mean distance the probe's rays went and the mean of its square, gathered sharply round the direction (the cosine's 32nd power). A surface takes a probe's light by how likely the probe is to see it: fully if it's no farther than the probe sees that way, else by Chebyshev's bound on the spread, cubed (the papers' test, as variance shadow maps use it). Light no longer comes through a wall from probes on its other side; where no probe round a point sees it, the point is dark, not lit by the flat ambient. Tried first at the light's own 6 x 6 with a wider lobe: too coarse, a wall 2 cm thick leaked at a slant.
  - *Inside or not is said at once:* a probe with more than a quarter of its rays meeting the back of something stops counting that frame (it was blended in before, and a probe that came to be inside a floor kept lighting it for seconds); it counts again when under 15% do.
  - *Sleeping:* a probe lights only surfaces within a cell of it, so one with no surface within 1.75 spacings shoots no rays: after eight fills in a row that found nothing it sleeps, and looks round one turn in sixteen (a sixteenth of the sleepers each frame, not all at once); one that finds something starts its mean again. In the shrine most of the volume is air or stone. The probes a frame fills are listed and counted on the GPU (`gi probes`), and the passes that fill them are dispatched by that count: seen first without it, in a world of terrain alone where every probe sleeps, the passes still cost 0.18 ms launching threads that had nothing to do.
  - *The lookup* (`gi_light`) reads what it needs of the frame once, wraps a probe's place without a division, and skips probes that could count for next to nothing.
  - *What it shows in the shrine:* the same dark hall, now lit evenly where it's lit (the walls and the dome by the candles' light off the floor, the floor by the door's sky), with no patches. It reads as very dark because the exposure is still the one for daylight: brightened seven times, the picture of the ambient alone is a hall lit by candles. Auto exposure is what's missing, not light.
  - *What it costs, first numbers* (the GPU shared with a game being recorded, so high): in the shrine at the editor's window, the top level 0.17 ms, rays 0.15 ms, maps 0.08 ms, distances 0.24 ms before they were gathered one fill in four; in the benchmark (2560 x 1440, the hall of five walls), rays 0.03 to 0.04 ms, maps 0.02 ms, distances 0.04 ms, and the deferred light 0.3 ms more than without probes, of which the test of what each probe sees is 0.2 ms. That last is the largest part now, and over what GI was meant to cost: reading the probes at half the picture's size and spreading that by depth and normal is the known answer, not built.
  - *Measured again with the GPU to itself* (2026-11-12, late; the laptop's GPU clocks down under so light a load, so the deferred light's figures move by half from run to run): in the benchmark hall, 910 of the 6,912 probes awake, 58,240 rays a frame: keeping 0.003 ms, rays 0.03 ms, maps 0.013 ms, distances 0.025 ms, 0.07 ms for the probes in all; the top level 0.03 to 0.04 ms; the deferred light about 0.2 ms more than without probes. The rays' benchmark: the top level 0.05 ms over 64 instances, 0.13 ms over 4,096, 0.20 ms over 32,761; a ray a pixel with its shadow ray 0.6 to 0.75 ms.
  - *Not in it yet:* the half-size read above; the probes' sight for what moves fast (a door is known a few frames late).
- **Step 4, built 2026-11-13:** cascades that follow the camera. Before it, flying a few metres up in the shrine took the floor out of the one volume, and it went back to the flat sky ambient.
  - *Four cascades,* alike but for their spacing, each twice that of the one inside it: 1.5, 3, 6 and 12 m, so 36 m square and 18 m high for the finest, 288 m and 144 m for the coarsest, each a third below the camera and two thirds above, each moving a whole cell of its own at a time. 27,648 probes in all; one light atlas and one distance atlas, each cascade's tiles a band of rows below the last (the distances now RG16F: a mean and a mean of squares).
  - *A point takes the finest cascade that holds it,* and over that one's outermost cell blends to the next coarser one, which holds it well inside. So light leaving one cascade is the next one's, never the flat ambient's, until it leaves the last, where it fades to the flat ambient as before. A pixel in the finest cascade's interior reads one cascade, as before; only the hand-over band reads two.
  - *Refill:* the finest cascade's turn comes every frame, the next every other, then every fourth and eighth: a coarser one lights what's farther, and changes more slowly to the eye. Its probes' sleeping and sight distances go by its own spacing. The lights the probes' hits are lit by are those that reach the coarsest cascade.
  - *What a coarse cascade can't do:* see into a space smaller than its spacing. A probe 12 m from its neighbours doesn't fit into a 3 m gap under a ceiling, so from far enough away such a gap is lit as none of its probes see it: dark. Found by a test, and kept as it is: the screen-space layer (the alternative below, as a near-detail layer) is what would add it back.
  - *Tests* (`tests/render/gi_probes_test.cpp`): the cascades' places nest; a stone floor under a 120 m roof is as dark from 30 m up, where only the coarsest holds it, as from 4 m (4 and 6 against the flat ambient's 117).
  - *Cost* (benchmark hall, GPU to itself but clocking down): 1,371 of 27,648 probes awake, the probe passes about 0.13 ms in all, the top level 0.04 ms, the deferred light 0.4 ms more than without probes (it was 0.2: the hand-over band and the larger atlases; the half-size read is still the answer). Memory: the light atlas 14 MB, the distances 28 MB.
- **Step 5, built 2026-11-13:** ambient occlusion on the screen, GTAO: how open the hemisphere above each pixel's surface is, for the near and small things the probes are too coarse to see (a step's foot, the gap under a crate). Jimenez et al., "Practical Realtime Strategies for Accurate Indirect Occlusion" (2016), written from the paper (`engine/render/scene/gtao.h`, `shaders/scene/gtao.slang`).
  - *The method:* per pixel, two slices of the hemisphere (planes through the view direction, along a screen direction each); along each, both ways, the highest horizon within 0.8 m in the world, found in the depth buffer in 6 steps a side spread quadratically (more near the pixel). The normal is projected into the slice, the horizons kept to the surface's side, and the visible arc between them integrated in closed form by the cosine: the paper's integral, so the result is the cosine-weighted visibility diffuse light sees, not a count of samples.
  - *Normalised by the open value:* the closed form averages to 1 for an open surface only over every slice direction; over a pixel's two it comes to more than 1 in some and less in others, and storing it clips the first. Each pixel's is divided by what its own slices give with nothing in the way: an open surface reads exactly open at any slant, with no noise. Found by a test: a floor seen at 6 degrees read 7% shut before it.
  - *Samples fade out* toward the radius (from 40% of it), so far things don't occlude and an occlusion's edge fades rather than cuts. The radius on the screen is held to a tenth of the picture's height.
  - *A depth pyramid* (`ao depth`, compute): depth along the view in meters, then 4 mips, each an average of the four below it where they lie on a surface, and only the nearest where they straddle an edge. Farther samples read coarser mips, at the mip texel's middle (a mip's depth stands for that point).
  - *Noise and denoise:* each pixel's slices turn by interleaved gradient noise and its steps start a noise-sized way in, so neighbours hold the other directions; `ao denoise` (twice, its taps 1 and then 2 pixels apart) averages each pixel with those of its 3 x 3 that lie on its surface (by depth against the surface's slope there). With TAA on, the noise also moves each frame and TAA averages it.
  - *Where it applies:* the deferred light's ambient only, never the sun or the local lights (their shadows are their own): the darker of it and the material's own occlusion (both see the same crease at their own scales; multiplied, it would be dark twice), then the paper's multi-bounce fit by the surface's colour (a bright crease is less dark: light bounces inside it), for the diffuse ambient; for the ambient reflection, the specular occlusion of Lagarde and de Rousiers (2014) from it, the angle and the roughness. Not on water, what's see-through or particles yet.
  - *Seeing and comparing it:* the view `ambient occlusion`; `r.ao 0` or Rendering Settings > Ambient Occlusion turn it off. `SceneRenderer::set_ambient_occlusion_settings` takes the radius, slices and steps.
  - *Tests* (`tests/render/gtao_test.cpp`): open ground reads open, and so does a floor seen at a 6 degree slant; a right-angle crease is about half shut at its line (166 of 255, 5 cm from it), less with distance, open past the radius, and the same 30 cm up the wall as 30 cm out on the floor within 5% (the screen sees the two at different slants); the lit picture is darker in the crease with only an ambient, and the same with only the sun.
  - *Not in it yet:* bent normals (the open direction, for the ambient's lookup and for specular occlusion by a cone), a thickness heuristic for thin things in front of the background, its cost measured.
- **The alternative, not chosen:** radiance cascades on screen for near and mid light with a coarse world volume behind them (Sannikov; Path of Exile 2): sharper near detail and many lights cheaply, but what's off screen is missing and it costs more in 3D. It stays open as a later layer for near detail.
- **Ruled out:** baked lightmaps (the sky changes); ray-traced GI per pixel (the frame budget); a traced surface cache with a final gather, as the largest engines have (too much to build and tend); the flat ambient (what's there).

**Reflections (planned with GI, 2026-11-12).** GI as above is diffuse light: it doesn't make reflections. What's there now is a march along one mirror ray in the water's shader only, reading the picture from before the water was drawn: no solid surface has reflections (they take a two-colour sky and ground), thin things are stepped over, flames and glass can't be in it, and there's no roughness. The plan is layers, each a fallback for the one above:

- **Rough surfaces:** their reflection from the GI probes (a wide lobe is nearly what a probe holds), so an interior stops reflecting the sky. Built with GI's step 2: the specular ambient reads the probes round the mirror direction, whatever the roughness, which is too soft for anything smooth.
- **One screen-space pass for every surface,** replacing the water's own: traced through a depth pyramid (nothing thin is skipped), wider with roughness, filtered over frames, reading last frame's finished picture so flames and glass are in it.
- **Ray-traced reflections where there are ray queries,** for water and wet stone: the only way to reflect what's off screen. A small step once GI traces rays against the scene.
- **The sky's reflection probe** (Sky lighting) as the fallback outdoors.

GPU budget for 120 FPS at 1080p (≈7.5 ms total, RT off):

| Pass | Budget |
| --- | --- |
| Culling + Hi-Z | 0.4 ms |
| Shadows (cached far cascades) | 1.3 ms |
| G-buffer (incl. terrain + grass) | 2.0 ms |
| Lighting + AO | 1.2 ms |
| Atmosphere + volumetrics | 0.8 ms |
| Transparents + VFX | 0.8 ms |
| Post + TAA/upscaler | 1.0 ms |

**Look study: Elden Ring (part 1, 2026-11-03).** From its shaders, studied from an owned copy (names, buffer layouts and the disassembled sun-light, tonemapping and grading passes), in our own words. Not yet checked against a frame capture: what runs each frame, and the render-target formats, still need one.

*What we take from it, and what we don't.* Their look comes mostly from art direction (artists tuning lighting groups, shadow tints, fog, haze and grading per area, time and weather), not from advanced rendering: much of the low-level tech is dated (ad-hoc local lights, grading on SDR, tens of thousands of shader permutations, an unused GI system). Their low-level rendering isn't a model to follow. What we study and match is the feel, the atmosphere, and the art controls that produce them; the G-buffer, lighting core, shadows, anti-aliasing and upscaling are our own modern designs (one physically consistent core, few uber-shaders, bindless), with those art controls on top.

*What their renderer does:*

- **Deferred, tile-based lighting.** Lights accumulate per tile, with variants by light type and count; a composite pass combines them. Forward variants of every material exist for transparents, and every material has G-buffer, velocity, depth and shadow variants.
- **G-buffer:** five color targets plus depth. As the sun pass reads it: a world normal stored as plain xyz remapped to 0..1; metalness; a translucency amount; base color; and an 8-bit **lighting group ID** in alpha. Position comes from depth and the inverse view-projection.
- **Shading is metal-roughness** (F0 from 0.04 to the base color by metalness), with two additions that matter for the look:
  - **wrapped diffuse** for translucent materials (light wraps past the terminator, for foliage, cloth and skin);
  - **backlight transmission**: a tight lobe toward the sun, seen through the surface, weighted by how much it faces away. This gives the glowing rims of leaves, hair and cloth against the sun: the golden backlight.
- **Lighting groups.** The ID indexes a table of parameters per group, including **two directional lights** (a key and a fill, each with a direction and color). Areas and material classes can be lit differently by the same sun: an art-direction lever behind the painterly look.
- **Tinted shadows.** The sun's shadow, from a screen-space mask over cascades (4 cascades, plus a second shadow map packed into an atlas for a region near the camera, and a cloud shadow variant), is blended toward a shadow color rather than black.
- **Atmosphere is a large system:** froxel volumetric fog with injected density and local lights, shadowed by the sun, plus separate fog volumes. Indirect light comes from box-projected environment-map regions blended across areas, and a GI volume system with several techniques (which ones ship at runtime is for a capture to tell).
- **Post chain:** auto exposure from adapted luminance (center-weighted), bright pass and bloom, a tonemap curve baked into a table from selectable curves (filmic, Reinhard, a sensitometric film curve), a color matrix and chromatic aberration, then **3D LUT grading with LUTs blended together** (by area and time of day, presumably), TAA or FXAA, motion blur and a lens-simulation depth of field.

*What it changes for us (renderer v1 and after):*

- G-buffer v1 keeps the planned octahedral normals (more precision for the same bits), and adds a **translucency amount** and an **8-bit lighting group ID** (in base color's alpha; there is no material ID channel), so the group lever exists from the start. Both are written as 0 until materials carry them.
- Lighting v1: metal-roughness GGX for the sun plus a placeholder ambient; materials flagged translucent get wrapped diffuse and backlight transmission (cheap, and central to foliage and cloth). Shadows get a **shadow color** parameter from the first shadow pass.
- Lighting groups (a key and fill light per group, from a table indexed by the G-buffer ID): designed in v1 (the ID is written), used from M1 with the lighting presets.
- Post v1: fixed exposure, then the tonemap curve as a 1D table (a filmic curve to start, others swappable), then a 3D LUT hook (identity at first). Auto exposure, bloom and blended LUTs follow. Grading always happens after tonemapping, on the LUT. Built so far: fixed exposure and a filmic curve computed in the shader (the ACES fit); the table and the LUT hook are next.

The simulation stays at a fixed 60 Hz tick; the renderer interpolates transforms between ticks, so the game renders at 120+ FPS while combat frame data stays deterministic. Input is polled every rendered frame and timestamped into the buffer to cut latency. Raising the tick to 120 Hz is possible later (frame data would be authored in seconds, not frames) but doubles simulation cost.

## Terrain & vegetation

Terrain is a first-class system, not a mesh: a streamed heightfield rendered as a GPU-driven quadtree, shaded through a virtual texture, and covered by GPU-generated grass and foliage.

**Heightfield and streaming.**

- A map is any size, read from its source; the engine must carry at least 16 x 16 km. It's cooked into a pyramid of nodes of 129 x 129 samples plus a border, 16-bit, with a fixed height step per level and a whole-number base per node, so neighbours on a level share edges to the bit. The coarsest nodes are 2,048 m across at 16 m spacing, and each level halves that. The first map comes at 2 m. The aim is 1 m everywhere and 0.5 m where authored, refined by the cooker.
- A GPU-driven quadtree: a compute pass walks a flat table of patches (32 x 32 squares, 4 x 4 to a node), and each one decides for itself from the camera's distance, its own height bounds and what's loaded. It's the same choice for the camera and every shadow cascade, so they agree on the ground. Vertices morph toward the coarser level before a swap so nothing pops, and skirts cover what morphing can't: a neighbour two levels away while its data loads, and the edge of the map.
- Terrain streams from the first version, on its own rings around the camera, not with the world cells: terrain never holds up a cell, and land needs no cell. Heights live in a file read a block at a time. The coarsest level is always loaded. A patch splits only once its children are on the GPU, so what's drawn is always a whole, valid surface, whatever has arrived.
- On the CPU, the ground's height comes from the same heights file (`world::TerrainHeights`): the finest level's block under a point, between its four samples, read when first asked about and a few kept. It's for tools and the editor (placing on the ground). Physics reads it the streamed way: Jolt height fields a node of the finest level each (256 m at 2 m spacing), read and built on the job system around cameras, characters and moving bodies, waited for under what stands or falls on them, and let go when nothing needs them (engine/physics/README.md, The ground). Scattering will read it the same way.
- Holes mask for cave entrances and dungeon openings; meshes (cliffs, rocks) blend into terrain with height-based blending.

**Materials and texturing.**

- Splat maps with up to 16 layers per tile (grass, dirt, rock, sand, snow, ash), height-blended.
- Runtime virtual texture (RVT): terrain layers and decals are baked into a page cache near the camera, so far terrain costs one sample, not 16.
- Triplanar mapping on steep slopes; procedural rules (slope, height, curvature) fill layers automatically, painting overrides them.
- A color map over the whole map (the look's `color_map`, as World Creator exports it): far off (400 m) the ground's color is the map's; up close the layers' textures keep their detail and are tinted so their average is the map's color (by each texture's mean, its smallest mip), as far as `color_map_strength` says, so nothing marks where one gives way to the other. Grass keeps its own color. Layers read from the sides on steep ground with `"projection": "sides"`, which the World Creator import sets: from above only, a cliff is its texture stretched down it.
- The ground shades by a normal map over the whole map (the look's `normals`), baked from the float source at twice the heights' density, 1 m for 2 m heights, not by the mesh's normals: far off, where the quadtree's mesh is coarse, it still has every rib and gully, mipped to what a pixel covers. The layers' normals and slope rules build on it; grass grows by the mesh's. Until it's in, the mesh's. At 1 m a 16 x 16 km map is 256 MiB of BC5, so it will stream like the heights, with the RVT.

**Grass fields (GPU-driven, indirect).**

1. A density map per tile (painted or rule-driven) defines grass types and amount.
2. Each frame a compute pass scatters blade or clump instances in tiles around the camera with stable per-tile seeds, so grass never swims.
3. The same pass frustum- and Hi-Z-culls, picks LOD (procedural blades near → grass board cards from your textures mid-distance → terrain color only far), and appends to indirect draw buffers.
4. Grass renders in a few `DrawIndexedIndirectCount` calls, vertex-shader animated by a global wind field.
5. Interaction: a small displacement texture around characters, horses and spells bends grass; fire burns it into ash for a while.

Target: hundreds of thousands of visible blades within \~0.6 ms of GPU time, fading into terrain color past \~80 m.

**Trees and foliage.** Placed instances (painted in the editor or by rules), GPU-culled like all meshes, 3–4 LODs plus octahedral impostors for the far field, wind via vertex animation, subsurface translucency for leaves.

**Wind (built, not yet seen moving).** One field over the world, `shaders/scene/wind.slang`, set by the environment (`render::SceneWind`): a steady push one way, gusts (patches of faster air, mostly lull, that travel with the wind) and turbulence (swirls a third the size, turning about over 32 s, that bend it off its way and change its strength). Everything that moves reads the same field, so grass and plants agree, and asks for last frame's wind too, for its motion vector: TAA would smear motion it isn't told of. Times wrap at 256 s and every rhythm makes a whole number of beats in that, so nothing jumps at the wrap; the gust field repeats every 64 gusts and the renderer wraps its offset in double precision, so it stays put in the world however long it runs and wherever the render origin is.

- Grass leans over toward the wind, more the harder it blows and never flat (80% of its height at most), bending along its length without stretching; up close it also bobs, out of step by its yaw, dying out from 20 to 55 m, so far off only gusts rolling across the field show.
- Meshes move when their material says so: `wind_bend` (meters a point 10 m up moves in a 10 m/s wind) and `wind_flutter` (meters at full vertex red in an 8 m/s wind), material parameters, so the format didn't change. A plant leans and rocks as a whole about its own root, more the higher up (the square of height), and its leaves flutter as far as the vertex color's red says, out of step by its green, fading out from 40 to 90 m. The shadow pass moves the same way.
- Vertex colors for it come from the source, or the cooker: a mesh's `wind` setting bakes them from its connected pieces (a leaf, a twig, a branch, each about its lowest point, the main stem hardly fluttering) or up its height (`tools/cooker/README.md`), on the full mesh, so the LOD chain carries them. That covers study 10's section 9 items 5 to 7 in a smaller form: no pivot stream (the whole plant turns about its root, its pieces about themselves by their colors), and Megascans' own `_wind` attribute isn't read yet.
- Still to come: impostors and card clusters swaying, interaction (characters, horses and spells bending grass), local wind (spells, wings), weather changing it over time, a wind texture around the camera if evaluating the field per vertex costs too much.

**Editor tools.** Sculpt (raise, smooth, flatten, erode), paint layers, paint grass density and foliage, rule-based scatter with preview. Every brush stroke is a command, so it is undoable and scriptable.

## Sky, atmosphere & weather

The sky is a core system, not a skybox: it drives the lighting, fog, mood and color of the whole world. Everything is physically based and fully dynamic, so time of day and weather change smoothly with no baked lighting to redo.

**Atmosphere.**

- Physically based scattering (Rayleigh, Mie, ozone) using small precomputed LUTs (transmittance, multi-scattering, sky-view, aerial perspective), recomputed only when atmosphere parameters change. Cost: well under 0.5 ms.
- Aerial perspective applied to every pixel, so distant mountains and castles fade into the sky color naturally; this is a large part of the Elden Ring look.
- Artist controls on top of physics: tint, haze density, horizon glow, golden-hour boost, stored per region and per weather state.

**Fog: one medium model, near and far.** Elden Ring's way (study 04): extinction is a constant plus a height fog around a base height, with a bounded exponential profile (thinning upward; below the base, rising only to twice its density, so valleys don't fill without limit), coloured per channel (blue fades first, the sun reddens through haze). Light scattered in: the sun through a colour looking toward it and another looking away (artists' controls in place of one phase parameter), dimmed by the fog column above, plus an ambient colour; a start distance, another for sun haze, and an end distance that is also the sky's. The sky is fogged like everything else, so the horizon takes the haze of the land below it. An environment can also thin the height fog as the camera comes down into it (`low_camera`), which isn't physical and is there on purpose: real height fog is thick from inside and looks thinner from above, the reverse of what reads well, with low land foggy from afar and clear once you're there. The user chose this over a far start distance alone, by eye, on their map.

- Far range (built, `shaders/common/fog.slang`): the closed-form integral of that medium along each view ray, in a full-screen pass after lighting (about 0.06 ms at 1600 x 900). Settings in the environment asset, hot reloaded.
- Near range, a froxel grid with the same medium, in phases:
  - A (built): a view-aligned grid (a column per 8 pixels up to 1080p and wider past it, so about 240 x 135 columns whatever the picture's size: since 2026-11-19, below; 64 slices spaced exponentially out to `volume_distance_m`). `fog inject` stores each froxel's extinction and light, with the sun's shadows from the cascaded atlas (light shafts); `fog integrate` walks each column front to back with the slice's closed-form integral; the composite reads it at each pixel's depth, then the far range goes on from the grid's edge. About 0.2 ms at 1600 x 900.
  - B (built): the light of each froxel blends into last frame's at the same place in the world, its samples moving around inside the froxel each frame, for smooth shafts. A filter along depth was planned too; in two trial scenes it made no measurable difference, so it isn't built. Extinction is exact, so it takes no history: fog never lags a change of density. No history after a cut, a frame not drawn, or a new reach or size.
  - C (built): fog volumes artists place (mist banks, fog walls), boxes and ellipsoids with a soft edge, a density and a tint, injected into the grid, so they're lit, shadowed and reprojected like the rest. The nearest 64 in view and reach each frame, with a mask per slice so a froxel tests only those at its depth. They need the grid, and fade out at its reach; their soft edge and the grid's columns limit how sharp they can look. A pixel whose surface is inside a slice with volumes takes the share of the slice really in front of it. The grid moves with the camera and a volume doesn't, so far off, where a slice is as thick as the volume, it still shimmers a little as you move. Noise eats into them, in three sizes that drift with the world's time, filtered by how far apart a ray's points are, so it doesn't crawl at a distance; a height fade thins their tops. A height map for their shape is next. A raymarch at screen resolution, stepping from where the ray enters the volume, removes the cause, for when that isn't enough.
  - D: local lights in the fog (torch glow), once the engine has local lights.

**Volumetric clouds.**

- Raymarched clouds from a 2D weather map (coverage, type, density) plus 3D noise textures, rendered at reduced resolution and reprojected over frames.
- Cloud shadows on terrain, light shafts through gaps, sunsets lighting cloud undersides.
- Low far-field layer of cheap cloud cards for distant overcast and storm walls.

**Sun, moon and stars.** Sun and moon positions from time of day and a latitude setting; moon phases; star field and a few scripted celestial objects (a special moon, a giant tree glow) driven by Lua for story moments.

**Time of day.** A single `time_of_day` value drives sun direction, sky and ambient color, fog density and color, cloud lighting, exposure, color-grading LUT blend, and which lights and VFX are active (torches light up at dusk). Day length is configurable; time can be frozen for areas or events.

**Sky lighting.** The sky and sun are captured into an ambient irradiance probe and a reflection cubemap, updated incrementally (one face or mip per frame) so ambient light always matches the sky without spikes. Local IBL probes in interiors blend with it.

**Weather.** Weather states are data files (clear, overcast, rain, storm, fog, ash, snow) with smooth transitions:

| Drives | Example |
| --- | --- |
| Clouds | coverage, type, wind speed |
| Fog | density, height falloff, color |
| Precipitation | GPU rain/snow particles, splashes, streaks on lens |
| Surfaces | wetness (darker albedo, lower roughness, puddles), snow cover |
| Wind | grass and tree motion, cloth, cloud and particle drift |
| Lighting | sun intensity, sky tint, lightning flashes |
| Audio | rain beds, thunder, wind layers |

Weather can be per region (always a storm on one mountain), scheduled, or forced by Lua for bosses and story beats.

**Editor.** A sky panel with a time-of-day scrubber, weather blending preview, and per-region overrides; all of it previewable in the live world.

## Water

Water is procedural and stylizable: lakes, rivers, swamps and the sea share one water shader with different parameters, placed as water bodies in the editor.

- **Water bodies:** lakes and sea are flat surfaces with a height; rivers are splines with width and flow speed, carving into terrain automatically. Each body streams with its cells.
- **Waves:** the sea's from an FFT ocean: a wind-driven spectrum (the environment's wind) evolved and transformed on the GPU each frame, in three tile sizes so nothing repeats, displacing a mesh that's finest under the camera, with foam where the surface folds. Lakes use a few Gerstner waves; rivers use flow maps generated from the spline direction. Physics and buoyancy read the same waves on the CPU.
- **Shading:** depth-based absorption and scattering (clear shallows, dark deep water), refraction from the scene color, fresnel, sky reflection from the sky probe, SSR (and RT reflections when enabled), foam on shorelines, rocks and around characters.
- **Shorelines:** a distance-to-shore field baked per cell drives foam bands, wet sand and gentle shore waves.
- **The surf, from a film (on trial, 2026-11-11):** a strip of beach running out to sea is simulated offline, in one
  dimension at a few centimeters a cell (`tools/worldgen/surf_film.py`: the shallow water equations, a set of waves
  coming in as the swell does where it's about to break), and baked into a texture: across it the place across the
  shore, down it the time, round a loop of a few waves; in it the surface's height, the depth where the water is
  thin, the foam (made where bores break, carried by the flow) and the surface's slope. The water plays it back
  along every shore (`shaders/scene/surf_film.slang`): a place's film is by its distance to the waterline, which the
  shore map holds to the centimeter, at a time that runs a little ahead or behind from one stretch of shore to the
  next, so fronts bend. Everything that varies it (how far ahead, how large, which stretches are breaking) goes by the
  place along the shore and the time only, never by the distance to the shore or by which wave is there: varied
  across the shore, the film is stretched in one place and squeezed in the next, and a foam trail's soft end,
  squeezed, is a hard outline round a patch of foam (tried, and seen). It's one system at every distance (no near and far to join), nothing is stepped while the
  game runs and nothing is made of cells, and the pace comes from the physics: bores slow and bunch as it shallows,
  the run-up reaches and drains as a thin sheet, the sets surge. One film serves any wave height: lengths scale by
  the height, times by its square root. Where its water is thin the sheet lies on the ground as the shore map has it,
  and ends by the film's own depth, not where the drawn ground cuts it, so the waterline doesn't move with the
  terrain's level of detail. The shore's material carries the film as its base color. The bake's seabed is its own (1 in 22, with a steeper face above the waterline), not the world's: on a
  seabed as flat as 1 in 40 the waves break 90 m out and reach the sand ankle-high, and each run-up hasn't drained
  before the next. So out to sea the water takes the film's height about the sea's level (its troughs held to a share
  of the water really there), and the film's depth over the real ground only on its beach, where the two meet at the
  waterline. The bake also keeps how fast the film's water is moving across the shore (a third texture, the material's
  occlusion), and the water slides its foam's pattern along with it, out to sea or in (two readings of the pattern,
  each slid for a second or two from a start of its own and faded as it ends), so foam rides in behind a bore and is
  drawn back out by the backwash; where it's foam that's seen, the motion written for the frames' smoothing is the
  foam's. The way it slides is the distance to the waterline's slope over several meters: the shore map's own way
  to land turns in a step at every texel's edge. The breaker's shape is the bake's own: the shallow water equations take a swell straight from a round hump to
  a bore and never make a wave rear up, so where the waves break the tool makes them higher, runs their tops ahead
  of their bases and sharpens their crests over a hollow (by rule, where Guerrilla shaped theirs by hand), and the
  water shades such a wave's front face as deep water and lets light through its crest. What it gives up: waves don't
  answer rocks or coves on their own (they follow the shoreline's shape), and a wave can't curl over (the film holds
  heights; sideways movement, as Guerrilla's baked cross-section has, is a later step). `r.surf_film 0` shows the
  simulated surf below instead, to compare; whichever loses goes.
- **The surf, simulated:** near the camera, the shore's water is a shallow water simulation on the GPU: a grid of cells a
  window around the camera (512 cells of 0.5 m, 256 m across, moving with it), the seabed read from the terrain, the
  sea's waves coming in at the window's seaward edges. The shallow water equations hold where the water is shallow next
  to its waves' length, which is the surf zone and the beach, and they give what can't be drawn convincingly: waves
  slowing and steepening as the bottom rises, bending to meet the shore, breaking into bores where it's shallow for
  their height, each part of a crest where its own depth says, and running up the real slope and draining back (cells
  go wet and dry). Foam is an amount on the same grid, made where the water loses energy (bores, plunging) and carried
  by the water's own velocity, so it gathers into lines where the flow converges and opens into holes where it spreads:
  the web of real surf. The water's color takes its scattering from how stirred the simulation says it is. Past the
  window, a far shore keeps the band surf drawn from the shore map. The two are the same waves where one hands over
  to the other: the band surf's formula (`shaders/scene/shore_waves.slang`, shared) is what the simulation is drawn
  toward round its window's edge, wholly at the edge and less inward, over the stretch where the water fades from one
  to the other. The simulation is held to the formula in two more places, where the shallow water equations alone
  fall short: the swell before it breaks (they'd steepen it into a bore far too early), and how deep the water stands at
  the waterline (a bore that has crossed a wide flat surf zone has little left to run up with, so the run-up's height
  and timing are the formula's swash, the same as the far shore's, while where it goes on the ground, its flow and
  its foam are the simulation's). Steps: the simulation and the surface it moves (built); the waves fed in and matched to the band surf
  at the window's edge (built); foam carried by the flow (built: made where the surface is steep or a thin sheet runs
  fast, carried from last frame's, gone in a few seconds); wet sand from where the water went.
- **Interaction:** ripples and wakes from characters, horses and spells via a small simulation texture around the camera; splashes as VFX.
- **Underwater and swamps:** underwater fog and color absorption, optional caustics projected on terrain, poison-swamp variants with emissive and murky parameters.
- **Cost target:** \~0.5 ms with a large lake on screen, rendered in the forward transparent pass.
- **Built so far:** a world can have a sea (`"sea": {"level_m": 0}` in the .world). Its surface is drawn after the light pass and before the fog, onto the lit image with its depth and motion, from a copy of what's behind it: seen through it as far as absorption and scattering let it be, the water's own color where it's deep, the sky and a soft sun glitter reflected by Fresnel, the land's baked shadows on it (and, since 2026-11-12, the sun's cascades and the probes' light where it stands: Global illumination, step 2). The mesh is 12 square levels around the camera, 0.5 m squares at the middle doubling outward to about 49 km, each snapped to twice its spacing so it doesn't swim, its vertices morphing into the next level's toward its edge so levels meet without a crack (engine/render/scene/water.h). The waves are the FFT ocean's (engine/render/scene/ocean.h): tiles of 640, 101 and 17.3 m at 256 texels, from the environment's `sea` (swell height, period and direction, fetch, choppiness) and its wind, JONSWAP spectra spread about their ways, every frequency rounded to repeat in the wind's 256 s; displacement and slopes with mips, the slopes' lost variance as roughness (the sun's glitter spreads far off), foam where the surface folds. The shore: a shore map the generator bakes over the terrain (the exact distance to land, in centimeters over 16 bits, and depth, uncompressed and filtered by the shader; the way to land is the distance's slope: tools/worldgen/shore.py, named by the world's `sea`). The open sea's waves grow as the bottom shallows and die at the waterline; surf runs in toward the land in three trains a little apart in period, length and way, so crests bunch, cross and leave uneven gaps, the swell coming in sets with lulls between, each crest running only so far along the shore and its line wandering off the shore's, sharp-crested over wide troughs, white where it breaks and against cliffs, each train leaving its own foam that thins seaward; whitecaps and the water's edge foam, never solid: soft patches pocked with bubbles' holes of two sizes that open it up as it thins, streaked along the wind, over water churned milky green-blue; light glows through crest faces seen against the sun; the ground is wet just above the sea. On the dev laptop at 2560 x 1440, close over the surf with water filling the screen: the ocean's compute 0.08 ms, the surface 0.44, the copy behind it 0.10. Reflections are screen-space: the reflected ray marched through the depth drawn before the water in 40 steps that grow with distance, then narrowed down, the sky standing in where it leaves the screen or is hidden, both hazed by the fog along the reflected ray. Foam comes from a tiling foam texture when the world's sea names one (`"foam"`: content/water/sea_foam.material, CC0 from ambientCG), read at 18 and 5 m, streaked along the wind for whitecaps and along the crests for surf, cut by coverage up close and taken in proportion where its mips blur it; noise stands in without one. Lakes and ponds: a cell's entity with `"water": {"shape": "ellipse" or "rectangle", "waves": 0 to 1}` is still water at the entity's height, as wide as its scale along x and z (half extents), turned by its yaw (`world::WaterBody`). Each is drawn with the sea's shader and mesh: the levels that reach it, at its height, their vertices held inside its box (the squares outside fold onto its edge and draw nothing), an ellipse's corners cut per pixel. Its shore is wherever the ground rises through it, which the depth test and the water's depth show: no shore map, no surf, a thin broken line at the waterline. Its waves are a share of the ocean's (its `waves` of the short tiles, its square of the 640 m swell), not the few Gerstner waves this section plans: one shader and one set of waves to start; separate lake waves when a lake needs its own wind or shape. Fresh water's color: it takes the blue sooner than the sea's, so it deepens to dark green-brown. tools/worldgen/waterworks.py makes a small test land with a lake, a pond, a cliff, a river valley and a sea. Not yet: waves bending toward the shore (the surf runs straight in), breaking waves that curl, spray, underwater, rivers, a lake filling its basin by itself, saving a lake's water values from the editor, a clean timing of reflections (measured only with another program loading the GPU).

## Content strategy: procedural first

The first worlds are generated, not hand-modeled: the engine must produce a convincing open world (terrain, grass fields, water, sky) from rules and a handful of textures. Your own assets and AI-generated assets come on top once the procedural base looks right.

**Procedural layers, in build order:**

1. **Terrain:** heightmaps from layered noise (fBm, ridged noise for mountains) plus hydraulic and thermal erosion, run in the cooker or as an editor tool. Rules then assign material layers by slope, height, curvature and erosion flow (rock on cliffs, dirt in channels, grass on gentle slopes).
2. **Grass fields:** blade geometry generated procedurally on the GPU (curved blades with width, bend and color variation per instance), so no grass meshes need to be modeled. Your grass board textures form a second layer of cards for mid-distance and variety (tall grass, flowers, weeds), and terrain color carries the far field.
3. **Trees, rocks and props:** scattered by rules (slope, density masks, distance from roads and water), using placeholder or provided meshes until final art arrives.
4. **Water:** lakes fill terrain basins automatically, rivers follow erosion flow paths as splines.
5. **Sky and weather:** fully procedural from day one (see Sky, atmosphere & weather).

**Rules, not baked results.** Generation is deterministic from seeds and rule files, so it can be re-run when rules change. Hand edits (sculpting, painting, placement) are stored as layers on top of the generated result and survive regeneration.

**Then your assets and AI-generated assets.**

- Your assets: hero meshes, characters, textures and grass boards, imported through the normal glTF/texture pipeline.
- AI-generated assets (textures, materials, concept-to-mesh, variations): go through the same import and validation as any asset, with a `source` tag in metadata so they can be filtered, reviewed and replaced later.
- Every asset passes the cooker's validators (scale in meters, texture sizes, triangle budgets, LODs, naming) whoever made it.

## Shaders, materials & VFX

This is where the game's identity lives, so iteration speed matters most: every shader, material and effect hot-reloads in under a second while the game runs.

**Shader system.**

- Shaders written in Slang, split into modules: common lighting, G-buffer packing, wind, dissolve, noise library, terrain sampling.
- Permutations kept small and explicit (skinned, alpha-tested, wind, dissolve); compiled in parallel, cached by hash, reloaded live.
- The cache (built, `engine/render/shader/shader_cache.h`): dev builds keep each compiled program in `user:/shader_cache`, named by its request and checked against the content hash of every source its compile read, so a start compiles only what an edit touched. What isn't cached compiles several programs at once (a Slang session each, 4 at most), in the background. With nothing cached (a first start, a new Slang), a window someone is at compiles its programs in the background and stays responsive, the picture filling in as they arrive and the wait shown in its corner; runs that take pictures or count frames compile before their first frame.
- No descriptor layouts per shader: every pipeline shares one layout (push constants, bindless tables), and reflection checks each program against it. Reflection is also to generate C++/Lua-visible parameter structs, so no hand-written binding code.
- A shader playground panel: pick a material, tweak parameters, see it on a test sphere, character or terrain patch.

**Materials.** No node graph. Materials are text files choosing a shader "template" (standard PBR, foliage, skin, cloth, hair, emissive rune, glass, water) plus parameters and textures. Text materials are easy to diff, batch-edit and generate, for both humans and AI. Custom looks are new templates written in Slang.

Anything a node graph can do, Slang code can do: graph nodes are just functions. The shader library covers the usual node set as plain functions (noise, UV ops, triplanar, flow maps, parallax, fresnel, blend modes, color ops, time and wind), so new looks are a few lines of code, not a new tool. A graph can be added later as another way in that compiles to the same templates, if non-coding artists join. VFX uses an ordered module stack per emitter (not a graph either), and particles render with material templates (particle unlit, soft additive, distortion), so one shader system serves both.

**Live preview (a first-class feature).** Iteration on looks lives or dies on preview, so materials, shaders and VFX get a real-time, accurate preview:

- **Same renderer, not a fake.** Preview viewports render with the real deferred pipeline, lighting, post and tonemapping, so what you see is what ships.
- **Instant feedback.** Parameter changes apply next frame with no recompile; shader edits recompile in the background (target < 1 s), keep showing the last good version, and report errors inline with file and line.
- **Preview scenes.** Sphere, cube, plane, character, terrain patch, grass patch, and "in world" (edit while looking at the real spot in the open world).
- **Lighting presets.** Day, golden hour, night, overcast, rain, inside a dungeon with torches, plus a custom sun angle and time of day slider, so a look is checked under every condition.
- **Debug views.** Albedo, normals, roughness/metal, emissive, overdraw, shader cost (instruction count and per-pixel GPU time heatmap), mip level, wireframe.
- **VFX controls.** Play, pause, step frame, scrub timeline, loop, 0.1x to 2x speed, restart on edit, bounding boxes, particle counts and GPU cost per emitter, triggered on a dummy character to test hits and spells.
- **Side by side.** A/B compare two parameter sets or two versions, and snapshot to compare with a reference capture (e.g. a RenderDoc image of the target look).
- **AI-accessible.** Previews can be driven and captured through the command API (`preview.set`, `preview.capture`), so an AI can iterate on a material and check its own result.

**Signature effects for the genre:**

- Dissolve and spawn-in (golden ash, spirit summons), erosion on death
- Emissive runes and glowing weapon buffs with animated flow maps
- Golden "grace" light, god rays, fog banks, swirling ash and petals
- Blood and sparks: hit particles plus deferred decals on hit surfaces
- Spell effects: trails, ribbons, distortion and heat haze, light emitters that cast into the scene
- Weather: rain with wet surfaces (darker albedo, lower roughness, puddles), fog, wind gusts, storm lightning
- Day/night cycle driving sky, sun, fog density and color grading

**VFX system.**

- GPU particles: emission, simulation, sorting and culling in compute; rendered as billboards, ribbons, trails or meshes via indirect draws.
- Modules: spawn shape, velocity, curl noise, gravity, depth-buffer collision, color/size over life, flipbooks, attached lights.
- Effects are text files composed of emitters, decals, lights, sounds and camera shake, triggered by animation events or Lua.
- Budget: a fixed particle pool (e.g. 1M) with priority, so a boss fight never starves the player's own effects.

**VFX: the order it's built in.** One pass at a time, each finished and measured before the next: (1) the foundation, below; (2) the VFX viewer (built: Editor & tooling, The effect viewer): one effect alone in the real renderer, with play, pause, step, scrub, speed, loop, restart on edit, bounds, particle counts per emitter and the GPU cost of what's shown; (2b) the VFX editor (not built): making and changing effects in that window, emitters, modules, numbers and curves, instead of in text; (3) looks: textures and flipbooks, lit smoke, a per-particle sort for dense smoke, heat haze; (4) gameplay: one-shot bursts, effects attached to moving things, the pool's priorities; (5) shapes: ribbons, trails, mesh particles, and lights attached to effects (with local lights).

**VFX foundation (pass 1, built).** In the shrine: a flame and its sparks on each of 21 candles (one prefab, placed 21 times) and dust motes in the hall's air, 22 effects and 43 emitters, about 2,050 particles alive in 78 of the pool's 16,384 blocks. Measured in the editor's Playground at 2180 x 1180 (2026-11-12): `particles step` 0.011 ms on the frames it runs, `particles` 0.028 ms. The CPU works per emitter, the GPU per particle: nothing on the CPU ever touches a particle. `engine/render/scene/README.md` (step 7b) has the details; its tests hold it to what's said here: the same steps leave the same particles at 30, 60 and 240 fps, and an emitter that was out of view has, once it's back, exactly the particles it would have had.

- **Effect files.** A `.vfx` source is JSON, cooked to `.devfx`. An effect is a list of emitters. An emitter says how many particles a second, how long each lives, and holds an ordered stack of modules, each a small record with a type:
  - *where a particle starts:* `sphere`, `disc`, `box` (a point without one);
  - *how it starts moving:* `velocity`, `cone_velocity`, `random_velocity`;
  - *what moves it, every step, in the file's order:* `acceleration` (gravity, lift), `drag`, `turbulence` (a field that swirls without sources or sinks), `wind` (the environment's);
  - *how it looks over its life:* `size`, `size_over_life`, `color`, `color_over_life`, `opacity_over_life`, `rotation`, `stretch` (along its motion).

  Its `look` is one of a few shapes drawn in the shader (`glow`, `disc`, `puff`, `spark`), how bright, how much it covers what's behind it (0: it only adds light), whether the sun and sky light it, and how softly it meets a surface. Textures and flipbooks come with pass 3: nothing here needs a picture.
- **A module stack, read by the GPU.** The cooker turns each emitter's stack into three runs of words: what runs when a particle is born, what runs every step, what runs when it's drawn. The compute and vertex shaders walk them: a loop over records with a switch on the type. A new module is a new case, not a new file format or a new shader per effect, and two of the same kind (two turbulences of different sizes) just work. The asset's `validate` walks the same runs, so a corrupt file can't send a shader past its end.
- **One pool.** 1,048,576 particles in one buffer made once (32 bytes each: position, age, velocity, a seed; 32 MiB), in blocks of 64. An emitter instance holds a run of blocks, as many as its `capacity` (worked out from its rate and longest lifetime when the file doesn't say), and fills it as a ring: its Nth particle goes in slot N modulo the capacity. No free lists, no counters shared between threads, and a particle's slot and seed follow from its number, so a run is repeatable. A full pool starts no new instance, and says so in the stats; priorities are pass 4.
- **Fixed steps, drawn in between.** The simulation steps 60 times a second whatever the frame rate, and the picture is drawn from the last step carried forward by each particle's velocity. At 240 fps that's a step every fourth frame, a quarter of the compute; at 30 fps two a frame. Because a step is always the same length and every random number comes from a particle's seed, the same effect gives the same particles every time: what pause, step, scrub and restart in the viewer rest on.
- **Each effect has its own clock.** An effect in the world carries an age that follows the world's time from a moment it was set at, at its own rate (`world::EffectClock`: since when, how old then, how fast). The particle system keeps no clock of its own: each frame an effect's age says how many steps its emitters should have taken, and they take what they're short of (at most 64 a frame, undrawn until they're there; the world's own time never runs ahead by more than a few ticks a frame, so what's in view and playing owes a few at most). Short by more than their longest-lived particle lasts, they skip to one lifetime before that age and step from there, which leaves exactly the particles stepping all the way would have; ahead of it (the clock was set back), they start from nothing the same way. So an effect's particles are what its age gives, however it got there: held still (rate 0), slowed, set back or far ahead, it shows what playing to that age would have left. The default clock is as old as the world: a candle in a cell has always been burning, whenever its cell came in, and no state is kept for it while it's away. What starts at a moment (a burst, an impact: pass 4) is set to that moment. The viewer's pause, step, speed, scrub and restart are this clock and nothing else, and so is a picture of an effect at a settled age.
- **Spawning.** For each step the CPU says how many of an emitter's particles are born in it (from its rate, as whole numbers of its own clock). The compute pass gives each its place in the step: born part-way through, at the emitter's place part-way through its move, and moved on for what's left of the step. A fast emitter leaves an even trail, not clumps.
- **Space.** `world`: particles are left behind as the emitter moves, kept relative to where the instance started (an anchor in the render steps instances use), so the render origin can move with nothing rewritten. `local`: they move with the emitter.
- **What's skipped.** An instance past its effect's `draw_distance_m` gives its blocks back. One outside the view keeps them but isn't stepped or drawn. Back in reach or in view, it's behind its clock and catches up as above: it has the particles it would have had. `prewarm_s` is steps on top of its age: what an effect that starts now (age 0) starts with.
- **Drawing.** After the see-through meshes, in a pass of its own (`particles`), onto the same two targets: the fogged image, premultiplied, and the coverage TAA reads. One instanced draw for every particle in view, a quad each facing the camera. Instances are drawn farthest first (ordered on the CPU: they're few); within one, in the ring's order. Fog, and for `lit` looks the sun through its shadow cascades and the sky, are worked out once per particle in the vertex shader. The fragment shader draws the shape, fades it where it nears a surface (the scene's depth) and the camera, and that's all.
- **Not in pass 1:** a per-particle sort (dense smoke seen from every side wants one: pass 3), particles ordered among see-through meshes (a flame behind glass draws over it), collisions, lights, ribbons, bursts.
- **Placing.** A cell's entity names an effect (`"effect": "fire/candle.vfx"`), as it names a mesh. It moves with its entity; a re-cooked effect carries on at its age, as the new file has it. In the editor a `.vfx` is dragged into the world like a model. Something made of several (a candle: its model and its flame) is a prefab, placed as one, not a pair of entities a generator or a person keeps together by hand.

**Using Elden Ring as reference.** Everything a copy we own exposes may be studied: RenderDoc frame captures (passes, render-target formats, G-buffer contents, timings), its shader code (as captures show it, or dumped with modding tools), FromSoftware's public talks, community analysis and observation. Captures and dumps are taken offline only, never by tampering with anti-cheat online.

- Study is for understanding techniques. We write our own implementations, shaped to our pipeline, and never copy or transcribe their shader code: that keeps the engine clean legally and every effect ours.
- Findings go into this document in our own words, as techniques and the decisions they lead to.
- Nothing extracted from the game (data, shaders, scripts, captures) is ever committed.

## Combat & character systems

Combat is frame-data driven: every action is a timeline of tagged frame windows authored in data, evaluated on the fixed 60 Hz tick. Animation plays the motion; the action timeline decides what is true (can be hit, can cancel, deals damage).

**Action timeline.** Each action (attack, roll, flask, parry) has phases: startup, active, recovery. Windows on the timeline toggle flags:

| Window | Effect |
| --- | --- |
| Hitbox active | Weapon hitbox sweeps and can deal damage |
| Invincible | Hurtboxes ignored (roll i-frames, \~13 frames at 60 Hz) |
| Hyper armor | Poise damage can't interrupt |
| Cancel: roll | Player may cancel into a roll |
| Cancel: combo | Next attack input is accepted |
| Input buffer | Inputs pressed early are queued (\~10 frames) |
| Stamina regen locked | No regen during action |
| Parry window | Incoming hits trigger a parry |

Example attack data:

```json
{
  "id": "longsword_r1_1",
  "anim": "ls_light_01",
  "length": 42,
  "stamina": 18,
  "damage": { "physical": 110 },
  "poise_damage": 25,
  "windows": [
    { "type": "hitbox", "from": 14, "to": 19, "shape": "blade" },
    { "type": "cancel_combo", "from": 22, "to": 34, "next": "longsword_r1_2" },
    { "type": "cancel_roll", "from": 26, "to": 42 },
    { "type": "input_buffer", "from": 10, "to": 42 }
  ]
}
```

**Hit detection.** Weapons use swept capsules between the previous and current tick so fast swings never tunnel. Characters have a few hurtbox capsules on bones (head, torso, limbs). Each attack instance keeps a hit list so one swing hits a target once.

**Hit resolution order:** invincibility → parry → guard (stamina and guard-break) → damage → poise damage → stagger/knockback reaction → hit-stop (2–6 frames on both sides) → effects (sound, spark, camera shake).

**Resources.** HP, stamina (regen delay \~0.6 s after spending), poise (resets after a short delay), equip load affecting roll type.

**Lock-on.** Target selection by screen-space distance and angle; switching by stick flick; lock breaks on distance or line-of-sight loss for \~1 s.

**Root motion.** Attacks and rolls move the character through root motion fed into the physics character controller, so animation and collision always agree. Enemies have a small rotation-tracking window during startup only, which is what makes rolling to the side readable and fair.

**Fairness rules.** Every enemy attack has readable startup, no hitbox outside its visible weapon, and a consistent punish window. These are checked in the combat debug view, not by feel alone.

## Animation

Animation serves combat: the action system picks what plays, and animation only handles how it blends. Built on ozz-animation for sampling, blending and compression.

- **Two layers of control.** Locomotion uses a blend tree (idle, walk, run, strafe in 8 directions while locked on). Actions (attacks, rolls, hit reactions) are one-shot clips that override locomotion and are triggered by the action timeline, never by the animation graph itself.
- **Root motion** is extracted at cook time and applied on the gameplay tick, then sent through the character controller.
- **Animation events** (footsteps, swing whooshes, VFX spawns) are authored in the same data file as the frame windows, so there is one timeline per action.
- **Blending:** short crossfades (4–8 frames) into actions; attacks start nearly instantly to keep input responsive.
- **Upper/lower body masking** for flask drinking while walking.
- **Foot IK** with two-bone IK on uneven ground, applied after sampling (vertical slice tier).
- **Skinning** on the GPU with dual-quaternion or linear blend skinning; up to 4 bone weights per vertex.
- **Debug view:** scrub any action frame by frame in-engine, with hitboxes and windows overlaid.

## Cloth & secondary motion

Cloth is a primary part of the look, not late polish: capes, robes, tabards, skirts, sashes, tattered rags, banners and hair give every silhouette its motion, and a character with stiff cloth reads as unfinished. Cloth is planned into characters, the asset pipeline and the frame budget from the start.

**What moves, and how.**

| Kind | Examples | Simulation |
| --- | --- | --- |
| Hero cloth | player, boss and NPC capes, robes, skirts, tabards | CPU, position-based, full body collision, near the camera |
| Crowd cloth | enemies in groups | same solver, fewer iterations and particles, aggressive LOD |
| Environment cloth | banners, flags, sails, curtains, hanging rags | GPU compute, wind driven, simple colliders, hundreds at once |
| Secondary motion | hair cards, braids, belts, pouches, chains, tails | spring bones (jiggle chains) on the CPU |
| Far away | everything above beyond its LOD range | vertex-shader wind sway, no simulation |

**Solver.** Extended position-based dynamics (XPBD) on a simulation mesh: a coarse particle mesh per garment (about 100–600 particles) that drives the render mesh through a precomputed mapping. Character cloth starts on Jolt's soft bodies, which are XPBD-based, collide with the Jolt world and support skinned constraints, so there is one physics library. If profiling or feel demands more control, an in-house solver replaces it behind the same cloth component. Environment cloth uses our own small XPBD compute pass, since it needs no physics world.

**What this genre needs from cloth.**

- **Skinned limits.** Every particle has a maximum distance from its animated (skinned) position, painted per vertex: zero at the collar and belt, generous at the hem. Cloth never leaves the body, whatever the animation does.
- **Tethers.** Long-range attachment constraints keep cloth from stretching, even with few iterations.
- **Inertia control.** Rolls, backsteps, jump attacks, mounting and teleports must not whip cloth across the screen or through the body. Cloth simulates relative to the character, inheriting a tunable fraction of world motion (linear and angular), with velocity clamps, and resets on teleports and camera cuts.
- **Collision.** Capsules and spheres authored with the skeleton (legs, hips, torso, arms) plus the ground; no self-collision by default (too costly), optional for long robes.
- **Wind.** Driven by the weather system (global wind and gusts) plus local wind fields from spells, explosions and wing beats.
- **Animation control.** Actions can pull cloth toward its animated pose for authored moments (a cape flourish, a cutscene) through a cloth weight window on the action timeline.
- **Stable timing.** Fixed simulation steps with substeps, interpolated for rendering like every other transform.

**Cosmetic only.** Cloth never affects hitboxes, combat, AI or saves. It can run at its own rate, drop to a cheaper LOD or skip a frame without touching the deterministic simulation or co-op sync.

**Data.** Cloth is authored on the garment mesh in the DCC as vertex attributes (max distance, stiffness, which vertices are pinned), with folder rules supplying defaults. The cooker builds a `.decloth` per garment: simulation mesh, constraints, painted limits, the mapping to render vertices, and LODs. Collision shapes live in the skeleton (`.deskel`). Tuning values (stiffness, damping, inertia, wind response) live in text so they diff and hot reload.

**Threading and budget.**

- Character cloth runs as jobs after animation sampling, one job per garment, results written straight into the skinning input; environment cloth is a compute pass on the async compute queue.
- LOD by screen size: full detail near the camera, fewer iterations and particles in the middle distance, then a blend to plain skinning and vertex wind. Off-screen characters don't simulate and reset when they come back into view.
- Budget at 120 FPS: at most 0.6 ms of CPU across workers for about 6 hero/boss garments and 20 crowd garments, at most 0.2 ms of GPU for environment cloth.
- Tracy zones per garment, and a cloth debug view: simulation mesh, constraints, colliders, painted limits, LOD level.

**Tools.** A cloth panel in the editor tunes a garment live while an action loops in the preview scenes (scrub frame by frame, slow motion, the lighting and wind presets), and the same values are reachable through the command API.

**Sequencing.** Spring bones and character cloth arrive with the first animated character, since a moving silhouette is part of judging animation; environment cloth arrives with the weather system. The vertical slice is not complete without both.

## AI & enemies

Enemies use a hierarchical state machine plus weighted attack selection. Behavior trees and utility AI are overkill for Souls-like enemies, whose fun comes from readable, repeatable patterns.

**States:** Idle/Patrol → Alert → Approach → Combat (Strafe, Attack, Recover) → Return to spawn. Plus reaction states driven by combat: Stagger, Parried, Backstabbed, Dead.

**Perception:** sight cone with line-of-sight raycast, hearing radius triggered by player actions (running, rolling, attacking), and an aggro leash distance from the spawn point.

**Attack selection.** Each enemy lists attacks with conditions and weights:

| Field | Example |
| --- | --- |
| Range | 1.5–3.0 m |
| Angle to target | within 45° |
| Weight | 3 |
| Cooldown | 4 s |
| Phase | 2 only |
| Follow-ups | combo\_02 (60%), retreat (40%) |

The AI picks from valid attacks by weight, which gives variety without randomness feeling unfair.

**Bosses** use the same system plus phases (switch at HP thresholds with a transition action), arena triggers (fog gate locks the room), and phase logic and set pieces scripted in Lua.

**Navigation:** Recast/Detour navmesh baked per world tile offline and streamed with the cells; crowd avoidance only for small groups. Enemies only path within their leash zone.

**Reset:** every enemy has a spawn record; resting at a checkpoint respawns all non-boss enemies from these records.

## Open world & streaming

The world is one seamless space streamed around the player in a grid of cells, with no loading screens outside fast travel. Streaming is the foundation every other system plugs into.

**World partition.**

- The world is a grid of cells (e.g. 128 m) grouped into larger terrain tiles. Each cell is a text file listing the entities placed in it.
- Each entity type declares a streaming distance class: terrain and landmarks (far, \~2 km+ as LODs), large props (\~500 m), gameplay entities and enemies (\~150 m), small detail (\~60 m).
- Legacy dungeons and caves are sub-worlds with their own cell grids, entered seamlessly through portals, and unloaded when far away.
- Far field: a cooked HLOD per region (merged, simplified meshes + impostors) so distant castles and the Erdtree-style landmarks are always visible.

**Streaming pipeline.**

1. Each frame the streamer ranks cells by distance, view direction and travel speed (mounted travel predicts ahead).
2. Load requests become jobs: read, decompress, create flecs entities from prefabs, create Jolt bodies, register navmesh tiles, upload GPU data.
3. Entities activate only when all their dependencies are resident; the game never waits on IO.
4. Unload reverses the order and frees the cell arena in one shot.
5. Budgets per category with hysteresis prevent load/unload thrashing at cell borders.

**Large-world precision.** World coordinates in double precision on the CPU, and on the GPU floats relative to a render origin near the camera, reached from fixed point for instances, so a 16+ km world has no jitter.

- Positions are doubles everywhere on the CPU (`Transform.position`); rotation and scale stay float. The simulation never rebases: physics, AI, saves and distances all see one fixed world space. At 60k entities this costs about 2 MB.
- The GPU works in floats relative to a *render origin* that render extraction moves when the camera gets about 1 km away from it. Doubles only become anything else in extraction, and in the renderer for the origin itself and the shadow grid fixed to the world. Within 1 km of the origin, precision is about 0.06 mm.
- Instances stay on the GPU, so they hold no origin: each keeps its world position in whole steps of 1/16384 m (0.06 mm), and shaders take the origin off it. Moving the origin sends nothing. The steps wrap at 32 bits and so does the subtraction, so the world has no limit: only what's drawn has to be within 131 km of the origin, and extraction logs anything sent past that. Rotation and scale stay float.

**World model: one owner per thing, data flowing one way.** No system keeps its own copy of what exists in the world; the flecs world is the only list.

```
.cell (text) --cooker--> .decell asset --registry--> cell entity --spawn--> entities --extract--> GPU
```

- **A cell is an asset.** Its cooked `.decell` lists every asset its entities use as dependencies, so the registry makes it Ready only when they're resident: the activation rule above, with nothing added.
- **A cell is an entity.** When the cell asset is Ready, its entities are created in one batch on the main thread at the start of a tick, each placed (no slide in), with an `(InCell, cell)` pair. `InCell` is `DontFragment` (cells don't create tables) with `(OnDeleteTarget, Delete)`: unloading a cell is deleting its entity, which deletes its members; the cell component's hooks release its asset, which releases what it held. No lists, no manual cleanup.
- **A cell's members match its loaded file.** The cell entity remembers which revision of its asset it spawned from; when hot reload swaps in a re-cooked one, its members are spawned again from it, before the next tick or extraction. Editing a `.cell` and cooking it updates the running game.
- **Static is the default for placed things,** and a child is as static as its parent. Static children of static parents spawn as roots at their world transform: nothing in that chain moves, so they cost nothing per tick.
- **Lifetime and transforms are separate.** Placed entities are roots with world-space transforms; the non-fragmenting `Parent` hierarchy is for things actually attached (a sword in a hand, a prefab's parts), and world transforms are computed down it once per tick.
- **Entities hold non-owning asset handles.** A `MeshInstance` names its mesh by handle without a reference: the cell holds the references, and a member never outlives its cell, so a handle can't dangle.
- **The renderer owns no entity list.** RenderExtract queries the world each frame and sends what changed into instance data that persists on the GPU. The slot lives in a component on the entity, given and freed by observers, so it lives and dies with the entity.
- **Mechanism now, policy later.** M0b loads cells explicitly; M1 adds the streamer that picks cells (ranking, budgets, hysteresis). The cell format and lifetime don't change between the two.
- **Prefabs** (step 1 built; `D:/ERStudy/study/13-prefabs-and-streaming.md` has the research behind it). Something made of several things (a candle: its model and its flame; a house; a camp) is defined once in a `.prefab` text file and placed many times. What every large shipped world does, and what's done here:
  - **The definition and the placement are separate, and the placement is small.** A `.prefab` lists parts under IDs of their own: each a mesh (with or without collision), an effect, a light (Rendering, Local lights), or another prefab, placed from the prefab's origin. A cell's entity places one in a single line (`"prefab": "../props/candle.prefab"`, with its own position, turn and scale), whatever the prefab holds. A cell says where a prefab is, never what's in it.
  - **Nesting is an authoring idea.** The cooker flattens the prefabs a prefab holds into its own list (a cycle or more than 8 deep is an error), each part named by its path of IDs (`candle_table/flame`) and placed from the outer prefab's origin. Nothing at run time walks a tree.
  - **Expanded when the cell loads.** A prefab is one of its cell's dependencies; when the cell spawns, each placement gets an entity for each part, made in the same batch from the cooked prefab's flat list. So editing a prefab cooks that one file (and the prefabs that hold it), not the cells that place it, and the running game makes the placements' parts again from the new one. A cell cooks again only when a prefab's variants are renamed, since those are all a cell names.
  - **Parts aren't children.** The parts of a placement that doesn't move are roots where they stand, static, costing nothing per tick. A placement that moves carries its parts with `flecs::Parent`, as anything attached is. A part knows its placement and the next part; the placement knows its first. A placement's parts go with it.
  - **Variants and nothing looser.** A variant is the same prefab with named parts left out (`"unlit": {"flame": null}`); a placement asks for one by name. No per-instance property overrides: every engine that has them free-form reports them as its main source of trouble. Parts have stable IDs from the start, so a narrower kind of override can be added without changing files already written, and one that names a part that's gone is a cook error, never skipped.
  - **Identity.** A part's ID is a hash of its path of part IDs, the same every cook. With an ID written into the placement when it's made (to come with saves; `id` is kept for it), that's a part's identity in the world.
  - **In the editor** a placement is one thing: one row, one gizmo, one line saved. Clicking any part picks the whole. A `.prefab` is dragged from the resources like a model. Chosen, its properties open with what it's made from: the prefab's picture and file (a link that shows it in Resources), the variant it asks for (a list of the prefab's own; changing it is a command, its parts made again at once, and a save writes `"variant"` into its entry or takes it out), its parts by name, greyed where the variant leaves them out, and Edit Prefab. A thing placed from a model or an effect shows that file the same way. The prefab itself is made and changed in the prefab editor (Editor & tooling).
  - **flecs prefabs (`IsA`) are for kinds** (enemy and item types: a few hundred, with shared stats), not for these. Read in flecs 4.1.6's source: every flecs prefab is a table, and every part of one another table, so thousands of level-art prefabs would be tens of thousands of tables, and parts made as children would be composed every tick.
  - **Measured (2026-11-12, ship build, this machine):** a cell of 2,000 placements of four parts (10,000 entities) spawns in 12.7 ms and is deleted in 3.1 ms: 6.4 ms per thousand placements. Nothing for a hall of candles; three frames at 240 fps for a cell that dense, so such cells need their spawning spread over frames before they exist. While they stand they cost 0.16 ms a frame, most of it the 8,000 effects listed for the renderer every frame, which wants to become "only what changed" as instances are.
  - **Step 2, only on numbers.** Parts are entities now. Static visual parts could instead be rows written straight into the GPU instance buffer, with entities only for what has logic: the same cooked data to another place. That would break "the flecs world is the only list of what exists", so it waits for the spawn benchmark (`tests/world/scene_test.cpp`) and a world large enough to say it's needed.
  - **Later:** a prefab bigger than a cell (a castle) dissolved by the cooker into the cells its parts fall in, with a cooked proxy on a coarser grid; a budget by distance for the effects and lights of thousands of placements; a block form for dense dressing; fog and sounds as parts; replacing what a prefab was made from in the world with a placement of it, as one command (it needs the new prefab cooked first).
- **Cell grid:** 128 m cells with integer coordinates; dungeons are grids of their own (M1). Cell contents in v1 are a fixed set (name, transform, parent, mesh, collision, effect, light, prefab, fog, water; cameras next); any reflected component comes with the editor.
- **Nobody files things into cells by hand.** Cells are how the world is stored and streamed, not how it's authored. A designer places things in the world; the editor files each into the cell its position falls in (its origin, divided by 128 m, rounded down), makes the cell's file the first time something lands there and adds it to the world, and moves the entity to the next cell's file when it's moved across a border (one command, undoable). Saving writes only the cells that changed. A group placed as one (a prefab) goes into the cell of its origin. Exceptions: big things (a castle, a bridge) are stored in the cell of their origin but stream by their own bounds, so they show from far away; what must always exist (game rules, weather volumes, world-wide lights) lives in an always-loaded layer that never streams; trees, bushes, grass and scattered rocks aren't entities in cells at all but instance lists per terrain tile, painted or scattered by rules (Terrain: trees and foliage). The hierarchy shows the world by name and the designer's own folders; cells show on the cell map.

**World state.** A flat, saveable set of flags and counters keyed by stable IDs: opened shortcuts, killed bosses, picked-up items, NPC quest stages, discovered graces. Streamed entities consult world state when they spawn, so a looted chest stays looted.

**Checkpoints (graces).** Resting heals, refills flasks, respawns non-boss enemies by marking their spawn records dirty, and saves. Death respawns at the last grace and drops currency at the death spot.

**Saves.** Small binary files with versioned schemas: player, inventory, world-state flags, position, dropped currency. Written to a temp file then renamed. Save on rest, region change, and quit.

## Physics, camera, input & audio

**Physics (Jolt).** Static level collision from simplified meshes; a capsule character controller (Jolt CharacterVirtual) for player and enemies; simple rigid bodies only for breakables and corpses (ragdolls in a later tier); soft bodies for character cloth (see Cloth & secondary motion). Combat hit detection does not use physics bodies; it uses its own swept capsule queries so it stays deterministic and frame-exact.

Static collision is authored on a cell's entity with `"collision": "mesh"`, and cooked from a mesh's LOD chain into a `.dephys` beside its `.demesh`: the finest LOD under a limit on triangles (`collision_triangles`, 2048 by default), welded by position, as a Jolt `MeshShape`'s binary state. A cell depends on its `.dephys` as it does on its meshes, so it's Ready only when they're loaded, and its bodies come and go with its members. All instances of a mesh share one restored shape, with a `ScaledShape` around it per distinct scale.

**Camera.** A third-person orbit camera with collision (sphere cast back from the pivot), smoothed follow, and a lock-on mode that frames player and target. Camera shake is data-driven per hit type. Camera pull-in near walls must never hide an enemy's startup frames.

**Input.** SDL3 gamepad first, keyboard and mouse second. Inputs are mapped to actions (attack\_light, roll, use\_item), sampled once per tick, and pushed into the input buffer. Code adds actions; their bindings are text, `config/input.bindings` (read from `src:/` in dev builds, from `game:/config/` in ship), then the player's `user:/input.bindings`, whose lines replace the defaults of the actions they name. Target input-to-action latency: 3 ticks or less (about 50 ms) plus render latency.

**Audio.** miniaudio with 3D positional sources, distance attenuation and a few reverb zones per area. Sounds are triggered by animation events and hit resolution. Mix buses: music, SFX, ambience, UI. Boss music changes with phase.

## Game UI

Decision: RmlUi for all game UI (HUD, menus, inventory, dialogue, map). It is a small MIT-licensed C++ library with its own layout engine; its syntax looks like HTML/CSS (`.rml` for structure, `.rcss` for style) but it is not a browser: no JavaScript, no web stack. Dear ImGui stays for the editor and debug tools only.

**Integration.**

- RmlUi's render interface is implemented on our Vulkan RHI (textured quads, scissor/clip) and runs as one render-graph pass after post-processing.
- Text through our own font atlas: FreeType + signed-distance-field fonts, sharp at any size, with fallback fonts for localization.
- UI logic in C++ and Lua (RmlUi's Lua support matches our scripting choice).
- `.rml` / `.rcss` files hot-reload while the game runs and cook into the archives like any asset.

**Data binding, not polling.** UI elements bind to data models (`player.health`, `player.stamina`, `inventory.items`, `boss.health`). C++ marks a model dirty when values change; the UI updates only then.

**Animation.** Transitions and keyframe animations with easing on transform, opacity and color: draining health bars with a delayed damage chunk, menu fades and slides, pickup popups, the death screen fade, boss name and health bar intros, focus highlights. Effects CSS-style animation can't express (burning runes, dissolves) use a hook that renders an element with one of our material templates.

**Gamepad first.** Focus order, D-pad and stick navigation, controller-specific button glyphs switched automatically, full keyboard and mouse support second.

**Performance rules.**

- Budget: ≤ 0.3 ms CPU and ≤ 0.2 ms GPU per frame.
- Animate transform, opacity and color only; never change layout properties (size, margins, element count) every frame.
- Element counts stay in the hundreds; large lists (inventory) are virtualized.

## Audio

Decision: miniaudio as the low-level backend, with a small DEengine audio system on top. Sound data is text (events, buses, music states), hot-reloads and is authored by humans and AI alike. Steam Audio can be added later for advanced spatialization; FMOD is the exit path if a sound designer joins.

**Sound events.** Data files referenced by ID from code, Lua and animation events:

```json
{
  "id": "footstep_heavy",
  "bus": "sfx",
  "variations": ["step_01", "step_02", "step_03", "step_04"],
  "switch": "surface",
  "surfaces": { "stone": "step_stone", "grass": "step_grass", "water": "step_water" },
  "volume_db": [-3, 0],
  "pitch": [0.95, 1.05],
  "cooldown_ms": 60,
  "max_instances": 4,
  "attenuation": "medium"
}
```

**Mixer.** Buses: master → music, sfx, ambience, dialogue, ui. Snapshots apply mix changes: music ducks under dialogue, low-pass on the death screen, muffled mix underwater.

**3D audio.** Distance attenuation curves per event, occlusion via Jolt raycasts (a boss behind a wall sounds muffled), doppler off by default.

**Reverb zones.** Dungeon, cave, cathedral, forest, open field, placed as volumes in the editor and cross-faded at their edges.

**Ambience.** Layered beds (wind, rain, birds, insects, distant thunder) driven by the sky system's weather and time of day, plus point emitters (torches, waterfalls, rivers) that stream with their cells.

**Music.** A state machine (explore, combat, boss phase 1/2/3, victory, grace) with crossfades and musical stems, transitions on beat or bar when needed, controlled from Lua.

**Streaming and memory.** Music, dialogue and long ambience stream from the archives; short sounds load into memory with their cell or character. Compressed as Vorbis or Opus in the archives.

**Performance rules.**

- Hard voice limit (\~64 real voices), with virtual voices beyond it; the quietest and farthest sounds are culled first.
- Mixing runs on its own audio thread; the game sends commands through a lock-free queue, never locks.
- Event lookups by hashed ID, no string lookups at runtime.

## Editor & tooling

Decision: a real editor, but as a mode of the engine executable, not a separate application. An open world can't be built without terrain sculpting, foliage painting, cell visualization and in-place placement, so "no editor" is off the table. A separate editor app (Unreal-style) would double the code to maintain, so we don't build one either.

**How it works.**

- `DEengine.exe --editor` runs the same runtime, renderer and streaming as the game, with ImGui docking panels on top. Press Play to simulate in place from the camera position; Stop restores the world.
- The editor never mutates data directly: every change is a command (create entity, set component, sculpt stroke, paint grass) with undo/redo and a text form.
- Panels are generated from flecs reflection: any registered component gets an inspector for free.
- Edits save to the text source files (cells, prefabs, materials, VFX), never to cooked data. The cooker rebuilds affected assets in the background.

**Without an agent (built).** The engine is driven by agents, and everything an agent does for someone has to be within that person's own reach too, or they're stuck when no agent is there:

- **Starting.** `Editor.cmd` at the repository's root, by a double click: `deengine --editor` with no project named opens the project it had open last (kept in the user's data folder), at the world that project had open last.
- **Opening.** File > Open World lists the project's `.world` files by name, and File > Open Project the projects beside the engine. Another project opens in an editor of its own and this one closes: what's mounted doesn't change while the engine runs.
- **Cooking.** The status bar has Cook (Ctrl+B, `editor.cook`): the whole project, in the background; what it comes to shows beside it, and every error it found is in the Output. Next to it, **Cook automatically** (on by default, kept between runs): the editor looks at the project's sources every second and a half (names, sizes and times, on a background job) and cooks once they have changed and stopped changing, so a model dropped into `content/`, a generator's output or a file edited by hand shows up by itself. It cooks once at startup too: what's seen is what the sources say.
- **The frame rate limit** is in Preferences (none, 30, 60, 120, 240), kept between runs; `--fps-cap` says otherwise for one run, and isn't kept.

And the other way round: what a person opens by a menu, an agent opens by name (`editor.open_world`, `editor.open_project`, `editor.cook`, `editor.cook_auto`; with no name the first two list what there is).

Still an agent's alone: running the world generators (`tools/worldgen/`), building the engine, taking pictures.

**Built so far (editor v1, dev builds):** `deengine --editor` runs the viewer's world with docked panels, in the user's own editor design (its theme, Source Sans 3, its property rows and resource browser): the hierarchy (the cell map in a tab beside it) over the properties on the left, the game's view (the Playground, the scene drawn into a texture at the panel's size, under a toolbar of tools, axes, snapping, camera speed and view) on the right; Resources and the engine's output in drawers that come up from a bottom bar (as Unreal's, Ctrl+Space for the resources), beside a console command line. A click in the Playground selects the mesh under it, exactly (an instance ID per pixel from the G-buffer), and the selection is outlined around its real shape, fainter where it's hidden; the move, rotate and scale handles (as Unreal Engine 4's: arrows, plane corners and a free-move sphere; full rings on a sphere, their near halves bands, showing the angle turned and the snap steps on the dragged ring; cubes and corner lines) change the selection, at its pivot or its geometry's centre, each drag one command, with snapping. The hierarchy lists what's placed by name (names from the cell files), each with what's attached to it; the cell map shows the grid from above, the loaded cells, the camera and the selection. The properties panel draws every reflected component from flecs meta, a Transform's rotation as angles; each edit is a `component.set` command with undo and redo (Ctrl+Z, Ctrl+Y), one drag one command, and an edited field's reset arrow puts back its saved value; what the engine computes stays hidden until asked for. The resources browse `content/` through `src:/`, search every file, and open a world with a double-click. The camera flies only while the mouse is over the Playground; F frames the selected entity; the layout is kept in `user:/editor/layout.ini` (engine/editor/README.md). File > Save (Ctrl+S, or `editor.save` in the console) writes the edited transforms and fog volume values into the cells' `.cell` sources, relative to each entity's parent in its file, rewriting only the values that changed so comments, key order and layout stay and a save diffs as its edits; it refuses a file that changed since it was cooked. It then runs the cooker in the background (cells, not shaders), and hot reload spawns the saved cells' members again: the selection and undo follow them by their place in the source, which each cooked entity keeps. Something moved into another cell's square is filed into that cell's file in the same world, with what's under it (renamed if its name is taken there, its mesh's path made from the new folder); a cell with no file gets one, next to the one it left, and the world lists it. Closing the window, Quit or opening another world with unsaved edits asks first (Save, Don't Save, Cancel). A model dragged from the resources onto the Playground is placed where the mouse ray meets the nearer of the terrain and something solid (or the level of the world's zero off them), Several can be chosen (Ctrl toggles, Shift adds, a hierarchy's Shift a range), dragged together as one command. Ctrl+D duplicates the selection (named apart at once: a trailing number counts up), End drops it onto the ground below it (its lowest point, on the terrain or something solid) and Delete takes it away with what's attached to it, each a command: undo and redo make and take away entities again, the history following the new ones; a save writes what was made into the file of the cell it stands in (named apart from what's there) and takes what was deleted out of its file, what its file hung on it becoming a root where it stands. The hierarchy shows the designer's folders, each entity's `"folder"` in its cell's file (the cooker leaves it out): a click on one chooses what's in it, they're renamed, nested and emptied by drag and drop or the right button, each a command. Surface snapping keeps what's moved across the ground on it; camera bookmarks (Ctrl+1 to 9 keeps one, 1 to 9 goes back, per world); View > Cell Grid draws the cells' borders and coordinates on the ground; Resources shows each model, texture and material as a picture of itself (a model from its mesh, a texture fitted to the square, a material as a lit sphere), which the cooker draws with the source (a 128 px BC7 sub-asset) and the editor reads into a few shared, capped pages; an effect and a prefab as the renderer draws them (Preview scenes, below); it renames, moves (by drag and drop onto folders) and deletes (to the recycle bin, after saying who still names it) files and folders, each taking its meta along and rewriting every text source that names it. Not yet: the always-loaded layer, Play and Stop.

**Preview scenes (built: the pictures and the effect viewer).** A thing shown by itself is shown by the engine's own renderer, in a scene of its own: a second small world (`app::PreviewScene`: a flecs world, a `SceneRenderer` and a render graph of its own, drawing into one texture), which shares what's loaded with the game (the registry, the GPU's meshes, textures and materials) and nothing else. What a flame looks like is what its particles do, and a prefab is its meshes, materials and effects together: a second way of drawing them (a CPU rasterizer in the cooker, as models' pictures use) would drift from the first, and couldn't simulate particles at all. An application draws its previews in `Application::draw`, between the frame's start and the frame's own graph, so the UI shows the texture the same frame; pictures come back from the GPU a few frames later without the frame waiting for them. Unreal and Unity do the same (a preview world per editor viewport and for thumbnails); later it's also a character in a menu.

- **Pictures of effects and prefabs** (`editor/preview_pictures.h`). One at a time: loaded, put in the scene alone, waited on until everything it shows is on the GPU and its particles have caught up, then measured, framed, shot and kept. An effect is held at an age where it has settled (as long as its longest-lived particle lasts, 20 s at most), its clock at rate 0, so the picture is the same every time. Nothing but a picture knows what particles cover (their sizes and stretch are worked out in the shader), so the framing is measured: a shot on black, the box of what's brighter than nothing (with the meshes' own corners added: a dark mesh on black measures as nothing), the camera moved and brought in until that box fills four fifths of the picture, three more shots at most. Then the shot that's kept: at four times the size, brought down by 4 x 4 boxes averaged as light, under one studio light (a warm key from the upper left, a cool sky, no fog, still air) on a dark backdrop, from the side the cooker draws models from. The camera keeps between two limits and the lens takes up the rest: an effect isn't drawn from past its draw distance, so a big one gets a wider lens instead of a camera far away; and particles fade out within 0.4 m of a camera (a game rule: a flame doesn't fill the screen as the camera passes through it), so a 3 cm flame is seen from half a metre through a long lens, not from 7 cm.
- **It waits for the GPU, not for the queue.** `GpuAssets::mesh()` says a mesh can be drawn as soon as its entry in the mesh table is on its way, which is right for a renderer planning draw lists. But uploads go oldest first, 16 MiB a frame, and while a world is coming in the entry lands many frames later: until then the cull finds nothing. A picture taken in between lacks the mesh for good (found the hard way: a candle's picture with only its flame, and only at editor start). So a picture waits for `mesh_on_gpu()` and `material_on_gpu()` (the entry's upload recorded), and for the renderer's own cull to report a draw. A test backs uploads up on purpose and checks the mesh is in the picture.
- **Kept between runs.** A drawn picture is a file in `user:/editor/pictures`, with the list of what it was drawn from: every asset down its dependencies, each with the size and write time of the file its contents were read from (`AssetRegistry::file_info`). It's shown again without drawing while each of those files is still that file (a stat each, asked once per refresh however many pictures share a texture); a cook that changes any of them, or a new `Thumbnails::kPictureLook`, draws it again. A folder of prefabs seen before costs no asset loads at all. A picture shot before everything was there (a material that never finished) is shown for the session and drawn again the next.
- **The backdrop.** `SceneLighting::backdrop`: one colour at the top of the picture and another at the bottom, instead of the sky, whose horizon would be a line through a thing seen from above.
- **Cost.** The scene is made when the first picture is asked for and let go after about 900 frames with none. Making it took 150 ms while every renderer compiled and built its own programs; the shader library now gives the same program to everyone who asks for the same thing (one pipeline, counted, gone when the last lets go), and it takes 5 ms (2026-11-12, dev build: 4.1 ms its world, 0.7 ms its renderer and graph). Its particle pool is the usual 32 MiB. Each picture takes 12 to 20 frames, most of them waiting for texels to come back; the game's frames carry on meanwhile.
- **Shown as they are.** The UI drew its own textures (the pages of pictures) as if their texels were light, when they're colours as a screen is sent them: every picture in Resources was lighter than its file. The UI shader now turns them into light, as it does ImGui's own colours (`render/imgui/README.md`).

**The effect viewer (VFX pass 2, built).** A double click on a `.vfx` in Resources (or View in its properties, or `editor.view_effect PATH`) opens it in a window of its own, a tab beside the Playground the first time: the effect alone in a preview scene, by the engine's renderer, under a clock the viewer owns (`editor/effect_viewer.h`).

- **Time is an age.** The viewer keeps the effect's age and hands it to the effect each frame as a clock that stands still at that age (`EffectClock`, rate 0), so there is one mechanism for everything the bar under the picture does: play (the age moves on by the frame's time at the chosen speed, 0.1x to 2x), hold, a step either way (to the simulation's next or last 1/60 s step, exactly), dragging the time, back to the start, and a loop that turns at the bar's length. An effect that plays on past the bar's length stretches the bar to what it has played, so its whole history stays under the hand. An emitter starts settled unless its file says otherwise (`prewarm_s`, a lifetime by default), in the viewer as in the game: age 0 is what a new instance shows. The particles are what the age gives, so going back shows what was there, to the bit: a step back is the picture before the step on.
- **Dragging never empties the picture.** Set back, an emitter starts over and takes the steps to its new age: all of them in the frame, up to 1,024 (`ParticleSystemDesc::catch_up_steps`; the game keeps 64, and shows an emitter only once it has caught up). That's 17 seconds of the longest-lived particle; a 4 s smoke is 241 small compute passes in the frame the hand moved.
- **The camera** turns about the effect (drag), slides (middle button or Shift), and comes in and out (wheel) by the pictures' rule: no nearer than particles show whole from, no farther than they're drawn from, the lens taking up the rest. Framed on how far the emitters may reach (`reach_m`), which needs no measuring; F or a double click frames it again.
- **What it's made of.** Beside the picture: the effect's particles and pool blocks, and each emitter's rate and how many of its particles are alive of the most it may have (worked out from its births, as the renderer does). An emitter can be left out of the picture (`EffectInstance::hidden_emitters`: it goes on being simulated), or shown alone.
- **Cost, as it's seen.** The GPU time of the scene's `particles step` and `particles` passes: with the effect alone in it, that is the effect's cost at the window's size. Hiding the others gives one emitter's draw. Per-emitter step cost isn't split out: every emitter's blocks go through one dispatch a step, which is what makes it cheap. Measured in the viewer (2026-11-12, its picture about 1500 x 1060, the dev build beside a game being recorded): the candle flame, 15 particles, steps in 0.006 ms and draws in 0.014 ms; the dust motes, 1,760 particles in 36 blocks, draw in 0.013 ms.
- **Looking.** A ground grid whose spacing follows the zoom; the emitters' reach as rings; a studio's light on a dark, grey or light backdrop (an effect that only adds light and one that covers read very differently on each), or the open world's light; wind from still to 20 m/s.
- **Editing.** The effect is its `.vfx` text; a cook (automatic when the file is saved, if that's on) brings the new one in, and the viewer starts it over or lets it carry on at its age, as chosen.
- **Not yet: making effects in it.** The viewer only shows. Nothing in the editor makes a new effect, adds or removes an emitter, adds, orders or removes its modules, or changes a number or a curve: that is all done in the `.vfx` text. The VFX editor has to do all of it in this window, on the effect as it plays, with undo, and save it back to the text (the user, 2026-11-12: "no tools to create vfx myself ... this needs to be done also"). It comes after the prefab editor.
- **Numbers that can be read.** They refresh four times a second, in columns that don't move (`tools`' rule for anything read rather than watched).

**The prefab editor (built).** A double click on a `.prefab` in Resources (or Edit Prefab in a placement's properties, or `editor.edit_prefab PATH`) opens it in a window of its own, a tab beside the Playground the first time: the prefab alone in a preview scene, with its parts listed beside it (`editor/prefab_editor.h`). The user, 2026-11-12, on what was there before: a placed candle's properties didn't say which prefab it was, and nothing in the editor could make or change one.

- **It edits the document, and shows it as it stands.** What's open is the `.prefab` text, held as parts and variants (`editor/prefab_document.h`), not the cooked prefab. The scene is made from the document: an entity for each part, with the cooked model or effect its file cooks to; a part that's another prefab is a placement of that prefab, whose parts the preview world makes as any world does, attached to it so they go where the handles take it. So a change shows the frame it's made, before any save or cook, and only what a part names has to be cooked. A part whose file isn't there or isn't cooked is still a part, listed under Problems with what the cooker would refuse (`PrefabDocument::check`: two parts of one ID, a part with nothing in it, a body without a model, a scale of zero, a variant leaving out what isn't there).
- **Parts.** They come in from Resources: a model, an effect or another prefab dragged onto the picture (placed where the mouse is on the ground through the origin) or onto the side (at the origin), or Add Selected File. A part's ID is made from the file's name (`brass_bowl`, numbered if taken). Chosen in the list or by a click in the picture (the renderer's own pick, as the Playground's), it has the Playground's handles (move, turn, scale; Q W E R; the prefab's axes or its own) and an outline, and its values under the list: its ID, what it's made from (each a link that shows the file in Resources, replaced by dropping another on its row), Solid (a static body in the model's shape), position, rotation in degrees and scale. A part is a model, an effect or both; or another prefab, with which of that prefab's variants it places. Delete, Ctrl+D (a copy where it stands) and a rename are in the list's menu.
- **Variants.** Listed under the parts; one can be shown, and then the picture leaves out what it leaves out and the list of parts gets a Kept column to tick. With a variant shown, a part that's another prefab lists that prefab's own parts too, each kept or left out by its path (`table/candle/flame`).
- **One layout, written whole.** A save writes the file in one layout: a part a line, their objects lined up, the comment the file opens with kept. A cell's file is patched value by value because people lay those out by hand in hundreds of lines; a prefab is small, and from its first save it's the editor's to lay out. A file that says something the editor wouldn't write back (a key it doesn't know) isn't opened: it's mended in text first, never half-kept. What's written reads back as the same document (tested), and cooks (tested).
- **Undo is the text.** Each change is a step: the document's text at that step, 200 of them. Ctrl+Z and Ctrl+Y in the window are the prefab's, not the world's (the window's own shortcuts while it has the keyboard, as are Ctrl+S, Delete and F). A drag of the handles or of a number is one step, taken when it's let go. The handles write to the part's entity through a command stack of the window's own, and the document takes what they did.
- **Saving.** Save (Ctrl+S in the window) writes the file; the editor then cooks (by itself with Cook automatically on), and every placement in the world is made again from the new prefab by hot reload. Closing the window, opening another prefab or quitting with changes unsaved asks first.
- **New prefabs.** Resources' New Prefab (the right button on a folder or in it) asks its name and opens it empty: it isn't a file until its first save, since a prefab with nothing in it doesn't cook. Edit > Make Prefab from Selection makes one of what's chosen in the world: each chosen thing with a model, an effect or a prefab becomes a part, placed from the one the handles are on, in the folder of what the first is made from. What was chosen stays as it is.
- **The camera** is the effect viewer's (`editor/orbit_camera.h`): the right button turns it (or Alt and the left), the middle one or Shift slides it, the wheel comes closer; F frames the chosen part, or all of it. Until it's taken hold of it keeps the whole prefab in view as its parts load.
- **For agents and tests:** `editor.new_prefab PATH`, `editor.prefab_from_selection PATH`, and `editor.prefab` with add, remove, duplicate, rename, move, rotate, scale, select, variant, delete_variant, keep, show, undo, redo, save, frame and close; alone, it lists what the prefab holds.
- **Not yet.** A prefab changed outside the editor while it's open there isn't read again (the editor's copy wins at the next save). A nested prefab's own parts can't be moved from the outer one (they're that prefab's). Fog and sounds aren't parts yet (Prefabs, Later).

**Core panels (vertical slice):** world outliner and cell map, inspector, asset browser, terrain sculpt/paint, foliage and grass painting, prefab editor, material and shader playground, VFX editor with live preview, Lua console, render-graph viewer, streaming and memory viewer.

**Debug views:** combat (hitboxes, hurtboxes, frame windows, poise), AI (state, perception, chosen attack), physics shapes, navmesh tiles, streaming cells and budgets, GPU culling stats, per-pass timings.

**What stays outside:** Blender (or Houdini) for meshes and animation, Substance or similar for textures, Tracy and RenderDoc for profiling. The engine imports their output; it doesn't try to replace them.

## AI-friendly by design

An AI agent and a human should be able to do the same things through the same doors. A human clicks in the editor; an AI calls the same commands as text. Neither gets a privileged path.

**1. Everything authored is readable text.** Cells, prefabs, materials, VFX, attacks, enemies, items, quests and Lua scripts are text with JSON schemas. An AI can read, diff, generate and validate them; a human can review the diff in git.

**2. One command API for everything.** Every editor action is a named command with typed arguments, e.g. `entity.create`, `component.set`, `terrain.sculpt`, `grass.paint`, `vfx.spawn`, `camera.goto`, `screenshot.capture`. The editor UI, Lua, tests and AI all call it. Every command is undoable and logged.

**3. An engine server for agents.** In dev builds the engine exposes the command API over a local JSON-RPC socket, wrapped as an MCP server, plus the flecs REST explorer for live queries. An agent like Claude can:

- Query the live world ("which enemies are within 50 m of the player?")
- Make edits and see the result immediately
- Take screenshots from any camera and read back profiler stats
- Run a headless simulation or replay and read the results

**4. Headless and CLI first.** `DEengine --headless --run tests/boss_margit.lua` runs gameplay without a window. The cooker, validators and tests are command-line tools with machine-readable output, so an AI can verify its own changes.

**5. Self-describing engine.** Components, commands and Lua bindings generate their own reference docs from reflection. Error messages say what was expected and where. Each system folder has a short README describing its rules and invariants.

**6. Code conventions that help both.** Small files with one responsibility, explicit names over clever ones, no macro magic in public APIs, invariants checked with asserts, and a test next to every gameplay rule.

**7. Guardrails.** Agents work through commands with validation and undo; destructive operations (deleting cells, overwriting assets) require confirmation in the editor. Every agent change is a normal git diff a human reviews.

## Code conventions

One style everywhere, enforced by clang-format and clang-tidy in CI, so humans and AI write code that looks the same. Data is plain structs; behavior is functions and systems over that data.

**Naming.**

| Thing | Style | Example |
| --- | --- | --- |
| Types, structs, components | PascalCase | `CombatState`, `Health` |
| Functions, methods | snake\_case, verb first | `apply_damage`, `load_cell` |
| Variables, parameters, fields | snake\_case | `hit_count`, `poise_damage` |
| Private class members | snake\_case + trailing `_` | `capacity_` |
| Constants, constexpr | `k` + PascalCase | `kMaxParticles`, `kTickRate` |
| Enums | `enum class` PascalCase values | `AttackPhase::Active` |
| Macros | `DE_` + UPPER\_SNAKE | `DE_ASSERT`, `DE_PROFILE_ZONE` |
| Namespaces | short lowercase | `de::render`, `de::world` |
| Files | snake\_case, one main type per file | `combat_system.cpp`, `hash_map.h` |
| Shaders (Slang) | snake\_case functions, PascalCase structs | `sample_terrain`, `GBufferData` |
| Lua | snake\_case functions, PascalCase modules | `Boss.on_phase_change` |
| Data IDs and keys | snake\_case | `longsword_r1_1`, `poise_damage` |
| Commands | `domain.verb` | `entity.create`, `terrain.sculpt` |

**Units in names.** Meters, seconds, radians, kilograms by default. Anything else carries its unit: `duration_ticks`, `angle_deg`, `size_bytes`, `distance_km`. World space is right-handed, Y-up, matching glTF.

**C++ rules.**

- C++20 subset: no exceptions, no RTTI, no iostreams; `Result` for recoverable errors, `DE_ASSERT` for invariants.
- Plain structs for data; classes only for things that guard invariants (containers, RHI objects, allocators).
- No raw `new`/`delete` outside allocators; ownership through handles or a single owner, no `shared_ptr`.
- No hidden globals or singletons: engine services are reached through an explicit `Engine` (`app/framework.h`) passed in, or through the flecs world.
- `const` by default; `auto` only when the type is obvious from the line; `enum class` only.
- Templates kept simple: concepts are fine, deep metaprogramming is not.
- Headers: `#pragma once`, minimal includes, forward declarations, no `using namespace` in headers.
- Functions short enough to read in one screen; early returns over nesting.
- Comments explain why, not what; each system folder has a README with its rules and invariants.

**Formatting** (clang-format): 4-space indent, 120 columns, braces on the same line, pointer and reference bound to the type (`Entity* e`, `const Mesh& mesh`), sorted includes (own header, engine, third-party, std).

**Source layout.**

```
engine/
  main.cpp    # the entry point: arguments, the application, run_framework
  app/        # the framework (engine systems, frame loop) and the applications it runs
  core/       # core library, allocators, jobs, logging, math
  asset/      # cooked formats (load in place), asset IDs
  platform/   # SDL3, file IO, threads, OS glue
  render/     # RHI, render graph, passes, materials
  world/      # streaming, terrain, vegetation, cells
  game/       # combat, AI, player, items, saves
  script/     # Lua bindings
  editor/     # editor mode, panels, commands
  tools/      # debug UI, stats panel, free camera, camera scripts and screenshots
tools/
  cooker/     # asset cooker, validators, packer
  image_diff/ # compares two PNGs, with a mask and a threshold
shaders/      # Slang (engine:/shaders/)
tests/        # unit, gameplay, benchmarks
projects/     # games, each apart from the engine and from each other (File system & IO, Projects)
  sandbox/    # the committed test scene: sandbox.deproject, content/, meta/, config/, cooked/ (not committed)
  dreadworld/ # the game: private, not in this repository
    dreadworld.deproject
    content/  # src:/content/: source assets, data, Lua
    meta/     # mirrored meta tree: GUIDs + import settings
    config/   # its settings as text: engine.settings (its start world), input bindings, camera views and paths
              # (copied into game:/config/)
    cooked/   # game:/ in dev builds
```

**Example** (component + system):

```cpp
namespace de::game {

struct Stamina {
    float current = 100.0f;
    float max = 100.0f;
    float regen_per_sec = 45.0f;
    int32_t regen_delay_ticks = 0;
};

void register_stamina_system(flecs::world& world) {
    world.system<Stamina>("update_stamina")
        .kind(phase::PostSim)
        .each([](Stamina& stamina) {
            if (stamina.regen_delay_ticks > 0) {
                --stamina.regen_delay_ticks;
                return;
            }
            stamina.current = min(stamina.max, stamina.current + stamina.regen_per_sec * kTickSeconds);
        });
}

} // namespace de::game
```

## Version control

Decision: Git + Git LFS (GitHub or self-hosted Gitea). It fits a two-person team where one member is an AI: branches, pull requests, review and CI all work natively with Git and Claude Code, and the text-first data (cells, prefabs, materials, VFX, Lua) diffs and merges cleanly.

**Repositories.**

- `deengine`: engine, editor, tools, shaders, scripts, text data and small binary assets.
- `deengine-content` (only when raw art grows large): heavy working files (`.blend`, `.psd`, high-res sources), so cloning the code stays fast.

**Large files.**

- `.gitattributes` from day one: `.png`, `.exr`, `.tga`, `.psd`, `.blend`, `.fbx`, `.glb`, `.wav`, `.ogg`, `.ktx2` go to LFS.
- LFS file locking on unmergeable files (`.blend`, `.psd`), so two people never overwrite each other's work.
- Planned: a pre-commit hook that rejects any file over 1 MB not tracked by LFS. It isn't set up yet.

**Never committed.** Cooked data, `Archives/`, build output, shader caches, editor user settings. The cooker rebuilds them; CI caches cooked data keyed by content hash.

**Workflow.**

- Short-lived branches, pull requests into `main`, squash merges.
- CI on every PR: debug and ship builds with the unit tests on Windows and Linux, `cooker --check`, clang-format and clang-tidy. Headless gameplay tests, benchmark scenes and the ship-layout validator join as they land.
- `main` always builds and runs; tagged releases map to milestones (`m0`, `m1`...).
- Commit messages: imperative, prefixed by system (`render: add Hi-Z build pass`).

**Watch out for.** LFS storage and bandwidth quotas on hosted plans (self-hosting avoids them).

**Exit path.** If the team grows to several artists or raw art reaches hundreds of GB, migrate content to Perforce; the engine repo can stay on Git.

## Roadmap & milestones

The plan is five milestones, each ending in something playable and a gate that must pass before moving on. Streaming and the world come first (M1), because every later system has to live inside it.

&#91;embedded content: roadmap · 5 milestones with exit gates\]

No dates yet: we set them once M0 shows how fast we move together. A milestone that slips gets cut down to its gate, never extended with new features.

### M0 task list

M0 is split in two so something runs early. Tasks are ordered: each builds on the ones above and is testable on its own. Archives, terrain and streaming wait for M1.

**M0a: window, core, first pixels.** Gate: Windows and Linux CI green; a triangle and a textured quad drawn through the RHI and render graph; shader hot reload; Tracy shows every system.

- [x] Repo setup: CMake presets, vcpkg manifest, `.gitattributes` for LFS, clang-format and clang-tidy, CI building Windows and Linux
- [x] Core library facade: `Array`, `InlineArray`, `HashMap`, `SlotMap`, `StringId`, `FunctionRef`, `Result` (std underneath)
- [x] Allocators: heap, arena, frame, pool, with tags and the per-frame allocation counter; `DE_ASSERT` and logging
- [x] Platform layer: SDL3 window, input actions, timers, threads
- [x] Job system (work-stealing) and Tracy zones
- [x] VFS: `game:/`, `user:/`, `temp:/` mounts (and `src:/` in dev builds), loose-file backend, async reads on a small thread pool
- [x] Vulkan RHI: device, VMA, swapchain, command lists, sync2, dynamic rendering, capability struct; GPU uploads through a staging ring (transfer queue when there is one)
- [x] Slang compilation, reflection and shader hot reload
- [x] Bindless setup: textures and samplers by index, buffers by device address
- [x] Render graph v1: pass setup/execute, transient resources, automatic barriers, unused-pass culling
- [x] Dear ImGui as a graph pass, with a frame stats panel

**M0b: engine skeleton.** Gate: a cooked glTF scene with textures, rendered deferred through the graph at 120 FPS, editable in editor mode, Lua and asset hot reload working, one headless test passing in CI.

- [x] flecs world: pipeline phases, component reflection, fixed 60 Hz tick with render interpolation; double-precision transforms, cells as assets and entities
- [x] Meta tree and cooker v1: GUIDs, folder rules, ufbx/cgltf/stb import, `.demesh`, `.detex`, `.demat`, `.decell`, `.deworld`, `.deenv`, `.deshader`, common header, load-in-place
- [x] Asset registry: load by ID through VFS and jobs, reference counting, hot reload of cooked assets
- [x] Deferred renderer v1: G-buffer, sun light with cascaded shadows and a screen-space mask, hemisphere ambient as the placeholder for IBL, filmic tonemapping, G-buffer debug views, free camera
- [x] App framework: `engine/main.cpp` calls `run_framework` with an application (the viewer opens a world); input bindings and engine settings as text in `config/`
- [x] GPU-driven v1: shared vertex/index buffers, compute instance culling per view with LOD selection, indirect draws, instances that persist on the GPU. Hi-Z waits for real terrain
- [ ] Render graph v2: transient memory aliasing, a lifetime and heap debug view (the graph's passes and textures are in the frame debugger already), a `discard` declaration on uses (so passes whose output is fully overwritten get culled), parallel pass recording (passes split into chunks recorded by jobs on command lists of their own, submitted in order)
- [ ] Render graph v3, before M1 content: async compute passes (a queue per pass, queue ownership transfers and semaphores between queues), barrier grouping by dependency level, then pass reordering where profiling shows bubbles
- [ ] LuaJIT + sol2: bindings for entities, components and commands, script hot reload
- [ ] Command API v1: `entity.create`, `component.set`, undo/redo, local JSON-RPC socket (started: the console's commands and variables, `--exec`, and the socket on 127.0.0.1 with `console.exec` and `console.complete`; `tools/console.py`)
- [ ] Editor mode v1: outliner, reflection-driven inspector, gizmos, save to text cell files (started: the docked shell, the hierarchy, the inspector with undo, the resource browser, saving transforms into their cells' files)
- [x] doctest suite in CI, GPU tests included (lavapipe on Linux), with a headless world benchmark (10k entities)
- [ ] Headless mode (`--headless`) and one benchmark scene in CI

## Open questions & decisions log

Open questions for you:

- [x] Version control for large assets: Git LFS or Perforce?
- [x] Game UI (HUD, menus, inventory): RmlUi or a small custom UI?
- [x] Platforms: Windows only, or Linux / Steam Deck too?
- [x] Audio: miniaudio enough, or FMOD / Wwise?
- [x] Split M0 into a concrete task list

Decisions so far:

| Date | Decision | Reason |
| --- | --- | --- |
| 2026-10-31 | Open world, streaming-first | Elden Ring-class game; every system assumes streamed data |
| 2026-10-31 | flecs instead of EnTT | Relationships, prefabs, reflection, REST explorer |
| 2026-10-31 | Deferred, GPU-driven, bindless renderer on Vulkan 1.3 | Material variety, many lights, decals, scale |
| 2026-10-31 | Render graph with transient resource aliasing | Easy passes, automatic barriers, VRAM reuse |
| 2026-10-31 | Lua as first-class scripting; heavy logic stays in C++ | Fast iteration without per-frame cost |
| 2026-10-31 | Terrain clipmaps + GPU-driven indirect grass (terrain superseded on 2026-11-06: quadtree, below) | Huge fields at low CPU cost |
| 2026-10-31 | AI-friendly: text sources, command API, MCP server | AI and humans use the same doors |
| 2026-10-31 | Elden Ring shaders used as reference only (superseded on 2026-11-03: studied in full, below) | Own implementations, clean IP |
| 2026-10-31 | 120 FPS target for the full game; 60 Hz fixed simulation with interpolation | Smooth, responsive feel; deterministic frame data |
| 2026-10-31 | Own core library API; std underneath first, own implementations swapped in when profiling demands | std allocates silently and scales poorly; we keep only what wins benchmarks |
| 2026-11-01 | Slang for all shaders | Modules, generics, reflection, good Vulkan support |
| 2026-11-01 | LuaJIT as the scripting runtime | Fastest option; FFI for cheap C++ calls |
| 2026-11-01 | Editor is a mode of the engine executable (confirmed) | Practical: one runtime, real preview, no second app |
| 2026-11-01 | Procedural content first (terrain, grass, water, sky), then own and AI-generated assets | A convincing world early without waiting on art |
| 2026-11-01 | Co-op is a later goal; simulation stays strictly deterministic | Keeps lockstep or rollback co-op possible without a rewrite |
| 2026-11-01 | Git + Git LFS for version control | AI-native workflow, text data diffs well; Perforce kept as exit path |
| 2026-11-01 | RmlUi for game UI; Dear ImGui for tools only | Free, light, text-based, data binding, Lua support, good animations |
| 2026-11-01 | Windows + Linux first-class; consoles later via a clean RHI and platform layer | Ship on desktop now, keep PS5/Xbox ports a backend job, not a rewrite |
| 2026-11-01 | miniaudio + own audio layer; Steam Audio optional; FMOD as exit path | Free, cross-platform, text-authored sound data for humans and AI |
| 2026-11-01 | Own cooked formats (.demesh, .detex...); source + mirrored meta tree with GUIDs | Load-in-place speed, clean source folders, rename-safe references |
| 2026-11-02 | Clip space +Y up with depth 0..1 and reverse Z on every backend | One convention for Vulkan, D3D12 and consoles; depth precision for long view distances |
| 2026-11-02 | Cloth is a primary system: XPBD character cloth (Jolt soft bodies first), GPU environment cloth, spring bones | Moving capes, robes and banners are central to the look; cosmetic only, so determinism is untouched |
| 2026-11-02 | Cooker libraries: yyjson (JSON), cgltf, ufbx, stb_image(_resize2), meshoptimizer, bc7enc_rdo (BC1-BC7), xxHash; ufbx and bc7enc_rdo through overlay vcpkg ports | Small, fast, permissive, with allocator hooks; no Assimp, no closed encoders |
| 2026-11-02 | Cooked files named by asset ID in game:/, not by source path | Runtime loads by ID; one source makes several assets; the archive packer works by ID anyway |
| 2026-11-03 | Double-precision world positions on the CPU; floats relative to a render origin on the GPU | No rebasing of simulation state; about 2 MB at 60k entities; change-only uploads keep working |
| 2026-11-06 | Jolt is built in double precision with cross-platform determinism on, from an overlay of vcpkg's port, as a static library behind `de::physics`, with no Jolt types above it | Positions are doubles and the simulation stays deterministic, and both change Jolt's types and code, so they can't be chosen at run time; the same world steps to the same bits on any number of threads, tested |
| 2026-11-06 | Static collision is a `.dephys` per model: a Jolt `MeshShape` from the finest LOD of at most 2048 triangles (a `mesh` setting), welded by position, its bytes refused by any other Jolt version; one restored shape per mesh, a `ScaledShape` per distinct scale | The chain's already there and a coarser shape costs little accuracy: the boulder's 1,032 triangles lie 1.6 cm from its 66,122 on average, in 16 KB rather than 1 MB. Triangles under a capsule are what characters pay for: 200 of them cost about 11 ms a tick on these and about 430 ms on the full mesh. A thousand instances add about 300 bytes each |
| 2026-11-06 | The GPU cull merges everything of one mesh part in one view and pipeline into one instanced draw, counting first and writing records second | A draw's cost was what limited 100k objects, not triangles; a draw per object stays behind a switch for GPUs slow at very small instances. How much time this saves still has to be measured |
| 2026-11-06 | A cascade draws only casters whose shadow can land on what reads it: the part of the view it serves, swept toward the sun | Half the shadow draws go (28,603 to 14,054 in a mixed scene of 100k, 186,378 to 65,668 in a grid of 100k quads), and tests find the same picture; a cache of far cascades will need all that its map could hold instead |
| 2026-11-06 | Instances keep world positions as wrapping 32-bit fixed point, in place of floats relative to the render origin | Re-sending every instance when the origin moved cost one frame about 3.7 ms of CPU and 0.7 ms of GPU at 100k, most of a 240 fps frame; now it sends nothing, the world still has no limit, and precision within 1 km stays 0.06 mm |
| 2026-11-03 | Cells are assets and entities: members carry a non-fragmenting `(InCell, cell)` that deletes them with the cell; entities hold non-owning asset handles | One owner per thing, no mirrored lists between world, assets and renderer; streaming mechanism built in M0b, policy in M1 |
| 2026-11-03 | `.cell` is JSON with world-space positions for roots; props are static by default (children as their parent); members respawn when hot reload swaps in a re-cooked cell | Readable and diffable by people and agents; most placed things never move; edit, cook and see it in the running game |
| 2026-11-03 | Elden Ring may be studied in full from an owned copy, shader code included (captures and dumps, offline only); own implementations, findings in our words, nothing extracted committed | Its techniques are the reference for the look; studying the real thing beats guessing, and clean implementations keep the engine ours |
| 2026-11-03 | Match Elden Ring's feel, atmosphere and art controls, not its low-level renderer; our own G-buffer, lighting core, shadows and upscaling | Their look comes from art direction on dated tech; a modern, consistent core with their art levers on top looks as good and ages better |
| 2026-11-03 | G-buffer v1: three targets (base color + lighting group, RG16 octahedral normal, roughness/metalness/occlusion/translucency) and emissive into HDR, 20 bytes a pixel | Room for the look's art levers (groups, translucency) at the bandwidth of a lean layout; emissive needs no target of its own |
| 2026-11-03 | The entry point only parses arguments, declares the application and calls `run_framework`; the framework owns the engine and the frame loop, the application (`startup`, `update`) says what to load, does its frame's work and decides when to quit; bindings and settings are text in `config/`, not code | Platform entry, engine and game stay apart; what a build loads and binds is data, so it changes without a rebuild |
| 2026-11-03 | A world is an asset (`.world`: cells, start, environment) and the light is one too (`.environment`), both hot reloaded; the engine opens the world named in `config/engine.settings` | Nothing about a scene is in code; editing the light or the world shows in the running game, and the same assets carry over to the streamer, time of day and weather |
| 2026-11-03 | Sun shadows v1: 4 stable cascades in one atlas, resolved into a screen-space mask (dithered cascade seams, contact-hardening filter), sunlight in shadow tinted toward a shadow color | A mask keeps lighting simple and makes room for temporal filtering; tinted shadows are part of the look; the static cache waits until scenes have enough static casters to need it |
| 2026-11-03 | Until GPU-driven v1 keeps instances on the GPU, render extraction writes every instance each frame; change-only uploads arrive with it | The simplest correct path first; the per-frame cost is measured (GPU timers, Tracy) before it's optimized |
| 2026-11-05 | Instances persist on the GPU and only changes are sent: a slot per entity in a fixed buffer, a stable slot per mesh, list sizes from counts per mesh | 100k static objects cost about 2.5 ms of CPU a frame before; that scaled with the world, not with what changes |
| 2026-11-03 | Fog v1 is an analytic height fog (Elden Ring's medium: constant plus bounded exponential height fog, coloured extinction, two-colour sun lobe, ambient) in a full-screen pass over the scene and the sky; froxels come next with the same model | Most of the atmosphere for a few hundredths of a millisecond; the froxel fog needs this model and this far range anyway |
| 2026-11-03 | Froxel fog in phases: A the grid with sun shadows, B temporal, C fog volumes, D local lights; 8-pixel columns and 64 slices, on when an environment sets `volume_distance_m` | Each phase is usable alone; 0.2 ms measured for A leaves room in the budget; the closed form stays the far range and the no-grid path |
| 2026-11-03 | Motion vectors: an instance's pose a frame ago comes from the same two ticks it's interpolated between, stepped back by the frame's share of a tick; no history per entity | Stateless and compact, exact for steady motion, none for what's static or place()d; a change of speed is a small one-frame error, which TAA's rejection covers. The camera keeps real history (what was drawn) in the render frame, since a free camera is place()d every frame; an explicit cut drops it |
| 2026-11-04 | TAA resolve: Elden Ring's anti-ghosting layers (nearest-depth motion, neighbourhood box, motion coherence, off-screen) with a depth test added, coherence on object motion (the camera's part taken out, written by the pass and kept) rather than total speed, clipping rather than clamping, Catmull-Rom history, blending in a tonemapped space; history kept by the renderer, on by default | Ghosting is what we refuse to show: each layer covers a case the others can't, and the test exercises each alone. Transparents and particles will need a reactive mask when they arrive |
| 2026-11-04 | Frame pacing: the camera and world move by whole refresh intervals while the frame rate holds the refresh rate (FrameClock, the idea from Godot); frames keep overlapping the GPU | A run could settle into two frames back to back then a long wait, averaging the refresh rate, and moving by that clock visibly shook the camera. Waiting for the previous frame cured it too, but serializes recording and the GPU, which the frame budget can't afford; the display shows frames on its intervals anyway. The loop's rhythm itself stays, so mouse look still follows it; pacing from real present timing can fix that if it's seen to matter |
| 2026-11-05 | Fog volumes stay in the froxel grid: 3 points through the ray's span inside each volume for extinction, and the fog pass takes the share of a slice really in front of a pixel's surface; noise and shape next, a per-pixel raymarch only if that isn't enough | Backing away from a mist bank on the ground, at about 94 m, the wave measured 7.9%; with this 2.6%, as low as mist off the ground (2.7%): what's left is the grid's. More slices cost more and helped less (128: 4.7%). Elden Ring's grid path reads one filtered sample at the pixel's depth; its mist also has moving noise and a raymarched path |
| 2026-11-06 | One hash of all shader sources decides whether cooked shaders fit an engine, compared at startup; any edit counts, comments too, line endings don't. The build gets it from a small tool running the cooker's own code | Layouts shared with C++ are matched by hand and reached through pointers, so a mismatch was a GPU crash (`VK_ERROR_DEVICE_LOST`) in ship builds, which load cooked shaders. A hash per module would need the build to know each one's imports; asking for a cook after a comment edit costs a few seconds, a crash costs far more. The stamp comes from the sources the cooker read, not the cooker's own build, which may be older |
| 2026-11-06 | LODs crossfade by a screen-space dither, from the error alone: a LOD comes in at the error limit and takes over within a share of it; a separate list and pipeline for what's fading; shadows keep cuts for now | The user requires a fade ("fade is a MUST"). No state per instance, so still instances cost the CPU nothing and the fade works on the GPU cull as it stands. Fading draws twice and loses early depth only where it happens. Alpha blending would need sorting, and time-based fades need history per object per view |
| 2026-11-06 | Terrain is a GPU-driven quadtree with morphing and skirts, not clipmap rings; streaming is in from the first version (the user's call), on its own rings; a map's size comes from the map, with 16 x 16 km the least the engine must carry | Clipmap detail ignores height above the ground and spends the same on flat land and mountains, and Far Cry 5 shipped this shape. Untested here: morphing with skirts, and what it costs on our GPU |
| 2026-11-06 | Terrain starts before the M0b gate is met (editor, Lua, headless): a deliberate departure, on the user's word. Screenshot and camera flags stand in for agent eyes until then | The look target needs real land to judge, and the rest of the gate doesn't block it |
| 2026-11-07 | A terrain step is 1/64 of its level's spacing, not 1/128: a node holds a rise of 8 to 1, and steeper land fails the cook | The first real map's cliffs are deliberate and rise 1,081 m across a 2 m node, past the 1,024 m that 1/128 holds, and would fail harder at the 1 m we aim for. It costs a step of 31 mm at 2 m (the source's own is 24 mm), and baked normals come from the float source anyway |
| 2026-11-07 | Agent eyes, dev builds only, until the command API: flags place the free camera (doubles from the text to the entity), take a PNG of the final image, step the frame clock by a fixed amount and fly a path; a run that takes pictures listens to no input, hides the debug UI and starts with the scene on the GPU; `image_diff` (C++ on stb_image, in `tools/`) compares two PNGs; stb_image_write comes into the dev engine for it, and ship links none of this | An agent could not see the engine at all. Stills of one view at one frame count were identical across every comparison made at 96 frames, in Debug and dev, with validation and vsync on or off, so baselines are per GPU and driver; 96 frames is where TAA and the froxel history stop mattering in the test world (engine/app/README.md has the numbers) |
| 2026-11-07 | Wind: one field, evaluated where it's used (grass and plant vertex shaders, the shadow pass), times wrapped at 256 s with every rhythm a whole number of beats in it, plant motion by material parameters and vertex colors the cooker can bake from connected pieces | The user asked for it before trees and foliage shading. One field keeps everything moving together; evaluating it per vertex needs no new pass, and costs a few noise reads where plants are; wrapping without a jump needs nothing per frame; parameters and colors changed no format but the environment's |
| 2026-11-07 | Height fog can thin with a low camera, by a setting per environment, and a key in dev builds turns it off to compare | Low land should read as foggy from above and clear from inside, which one fog of one thickness per height can't do; the user judged it by eye on their map |
| 2026-11-08 | Terrain shades by a baked normal map over the whole map, at twice the heights' density, rather than by the mesh's normals | Far mountains, drawn by coarse levels, shaded as smooth planes: their normals came from vertices 16 to 64 m apart. It's one BC5 read a pixel, and the real mountains' 1 m survey gives it detail the 2 m heights can't hold. Its memory grows with the map, so it will stream with the RVT |
| 2026-11-08 | The sea's waves come from an FFT ocean (a wind spectrum on the GPU in three tile sizes), not a sum of Gerstner waves | The user asked for an advanced water system and chose it: a sum of a few dozen waves looks regular and repeats on a close look, where thousands from a real spectrum don't, for about 0.2 to 0.3 ms of compute. Gerstner waves stay for lakes and shore swash |
| 2026-11-11 | On trial beside the simulated surf: the surf as a film baked offline from a one-dimensional simulation and played back by distance to the waterline, at every distance | The simulated surf needed its near water joined to the band surf past its window, showed its half-meter cells at the waterline, and had to be held to the band surf's formula in three places to look right, at which point it added little of its own. Shipped games bake or draw their shore waves rather than simulate them as the game runs (Guerrilla's Horizon Forbidden West talk, SIGGRAPH 2022; War Thunder's shore waves by distance to shore). Offline and in one dimension the same equations are cheap at centimeters a cell. The user compares the two in the editor; the loser is removed |
| 2026-11-11 | The surf near the camera is a shallow water simulation on the GPU (a 256 m window of 0.5 m cells, the seabed from the terrain, the sea's waves fed in at its edges), with foam carried by its flow; not Gerstner waves, and not the band surf drawn from the shore map | The user compared the band surf with footage of real shores and asked for what the physics gives rather than more drawing: shoaling, refraction, bores breaking where their own depth says, run-up and drainage, and foam in lines and holes where the flow converges and spreads all come out of the equations. The band surf stays for shores past the window. Its cost is to be measured against the 240 fps budget before it goes further |
| 2026-11-08 | The editor's view of the game is the scene drawn into a texture at its panel's size, shown as an image in a docked panel | A panel that docks, resizes and sits among the others is how the user works; rendering at the panel's size costs nothing over the window's |
| 2026-11-08 | The editor files placed things into cells by position (and moves them across cell borders), with an always-loaded layer and big things streaming by their bounds; the hierarchy shows the world by name, cells only on the cell map | Cells are a streaming detail: a designer thinking in cells would be doing the engine's bookkeeping, and gets it wrong at borders. The user asked who puts props in cells |
| 2026-11-08 | An edited field's reset arrow puts back what it was before this session's edits (its saved value), not the component's default | A placed object's default position is the world's origin: a reset there threw things across the map. Arrows only on edited fields also keep the panel quiet |
| 2026-11-08 | In the editor the camera moves as Unity's scene view (fly with the right button held, wheel zoom, middle-drag pan, Alt-drag orbit and zoom, eased framing); Q W E R pick the tools; handles and picking are our own (`editor/gizmo.h`), not ImGuizmo | Unreal's convention: the keys can't fly the camera and pick tools at once. Our own handles fit the engine's double-precision positions and its commands, and needed no library that may not follow ImGui 1.93 |
| 2026-11-10 | Games are projects under `projects/`, apart from the engine: `src:/` is the open project's sources (`content/`, `meta/`, `config/`), `game:/` its `cooked/`, and a new `engine:/` the engine's shaders; `--project` picks one, CMake's `DE_DEFAULT_PROJECT` is only a default. The meta tree stays mirrored, per project | Everything was one folder: engine code, every map's content, and one cooked pool of every world ever cooked. A project cooks, cleans and versions on its own (the game's private, the test scene public), and the engine names none |
| 2026-11-09 | A console as Unreal's (` over the game's picture, `stat fps` and the rest, `r.*` variables, names completed as they're typed, `--exec`), its commands the same text from three doors: the window, the command line and agents over the dev-build JSON-RPC socket on 127.0.0.1 (`console.exec`, `console.complete`; `tools/console.py`). The socket is the command API's, ahead of `entity.create` and the rest | A person and an agent change a running engine the same way, and every line that runs is in the log. Text first because that's what both type; typed commands grow from it. Loopback only and dev builds only: nothing off the machine, and ship has no door |
| 2026-11-09 | The editor picks with an instance ID per pixel: in dev builds, while the editor asks, the G-buffer's programs are built with `EDITOR_IDS` and write each pixel's instance slot (plus one) to an R32 target; a click copies that one texel back. The selection's outline comes from a mask of its meshes drawn again alone (LOD 0, without TAA's jitter, tested against the scene's depth for what's hidden), with an anti-aliased band around it after the tonemap | Exact to the mesh and its alpha test, and the same for anything the G-buffer draws, at no cost outside the editor (ship builds have neither). A ray against bounding boxes picks the wrong thing whenever boxes overlap. Drawing the selection again keeps its outline whole where it's hidden, which a stencil from the G-buffer can't |
| 2026-11-09 | The editor saves by editing a `.cell`'s text in place: only the values that changed are rewritten, entries found by their place in the source, which each cooked entity keeps; the cooker then runs in the background, and the editor follows the members hot reload spawns again | Cells are written by hand and read in diffs: re-serializing would drop comments and layout and make every save a rewrite. Names can be empty, so the place in the source is the key, and a file edited since its cook is refused rather than written into the wrong entries |
| 2026-11-09 | Saving files a root moved into another cell's square into that cell's file in the same world, with what names it as a parent; a cell with no file gets one beside the file it left, and the world lists it; the files things move into are written before those they leave | The design's rule (things are filed by position, nobody files them by hand) without a separate step; one world's cells can share a folder with another's, so the world, not the folder, says which file is the cell there; written in that order, a failed write can't leave something in neither file |
| 2026-11-09 | What the editor makes (placed, duplicated, deleted and put back after a save) is an entity carrying its own `.cell` entry until a save files it and its cell comes back cooked with it; deleting is a command that keeps a record to make the entity again | An entity can't be a member of a cell whose file doesn't have it yet, and deleting must undo after its cell has been cooked again: the entry text keeps everything the file said (fog, collision, keys the editor doesn't know), and the record makes a new entity whose ID the history follows |
| 2026-11-11 | The frame debugger reads the render graph live (its passes, their GPU times, what each recorded) and shows a texture through a pass added right after the one watched, instead of capturing a frame and replaying it | The graph already holds what a capture would have to rebuild, so live it costs a few small passes, needs no second tool, and shows the frame as it changes while a setting is turned. RenderDoc stays for single draws and pipeline state |
| 2026-11-12 | The transparent pass is sorted blending: what blends is drawn after the fog, the farthest first, premultiplied (one blend state for glass and glows), ordered per object on the GPU. Weighted blended mixing and a low-resolution buffer are add-ons for later; exact order-independent methods are out | It's the cheapest method that's right for most of what the game draws (fire and sparks need no order at all, glass panes rarely cross), one state means one indirect call, and its failures (objects that cross, popping) are the ones the add-ons fix where they show. Exact methods cost memory per layer per pixel |
| 2026-11-12 | What an agent does for someone, they can do themselves: a launcher, the last project and world opened again, worlds and projects in the File menu, a Cook button, and cooking by itself when the project's files change (found by looking at names, sizes and times every 1.5 s, not by OS notifications) | The owner was stuck whenever no agent was there to start the editor or run the cooker. Polling is one portable piece of code, costs a few milliseconds off the frame's thread, and can't miss a change the way a dropped notification can; switching project restarts the editor because mounts are fixed for a run |
| 2026-11-12 | VFX foundation: the simulation steps at a fixed 60 Hz and is drawn carried forward by velocity; an emitter instance holds a run of the pool as a ring, and every random number comes from a particle's number; modules are records the shaders interpret; the CPU decides per emitter (how many are born, what's in view, the draw order), the GPU per particle | The viewer has to pause, step, scrub and restart exactly, which needs the same particles every time: a variable step and free lists filled by racing threads give different ones each run. A fixed step also costs a quarter of the compute at 240 fps. Interpreted modules keep the doc's ordered stack without a shader per effect. Rejected: one buffer per emitter (Godot's way: thousands of small buffers and draws), dead and alive lists with atomic counters (order differs run to run), a sort of every particle in pass 1 (not needed for flames, embers and motes; it comes with dense smoke) |
| 2026-11-12 | Each effect has its own clock (an age that follows the world's time at its own rate), and the particle system none: emitters take the steps their effect's age says they're short of, skipping to a lifetime before it when they're far behind or ahead | The viewer's pause, step, speed, scrub and restart, a picture at a settled age, an effect that starts at a moment and time slowed for one thing all need an effect's time to be its own; one global clock gives none of them. As old as the world by default, an ambient effect needs no state while its cell is away and comes back as if it never left. Rejected: stopping or scaling the preview world's time (the camera and everything else would stop with it, and it can't jump), a start time per emitter kept by the renderer (what it had: lost whenever the emitter left reach) |
| 2026-11-12 | The effect viewer owns the effect's age and gives it to the effect as a clock standing still at that age; play, hold, step, drag and loop are all "another age"; an emitter set back catches up within the frame (1,024 steps in the viewer's scene) | One mechanism, exact both ways in time, and nothing in the simulation knows it's being viewed. Rejected: stepping the simulation from the UI (a second way to advance it, and no way back), recording frames to scrub through (memory, and stale the moment the file changes), showing an emitter only once caught up as the game does (an empty picture under the hand) |
| 2026-11-12 | Previews are drawn by the engine's renderer in a second small world sharing the loaded assets (`app::PreviewScene`); the pictures of effects and prefabs in Resources come from it, framed by measuring a shot, and are kept as files trusted while the files they were drawn from are unchanged | One renderer means a picture can't differ from the game, and particles can't be drawn any other way. A world of its own keeps previews out of the game's world, its streaming and its saves. Measuring the picture is the one thing that knows what particles cover. Rejected: a CPU rasterizer in the cooker for prefabs (a second lighting model, no effects), drawing previews in the game's world far away (they'd be saved, streamed and lit by its sky), no cache (every folder of prefabs would load all its meshes and textures each run) |
| 2026-11-12 | Global illumination is one camera-centred, cascaded probe volume (irradiance and distance maps per probe, after DDGI), filled by hardware ray queries against the scene's meshes first and the terrain's heightfield second, with GTAO; 0.5 ms at 240 fps as its first limit. Reflections are layers after it: probes for rough surfaces, one screen-space pass for all surfaces, ray-traced where ray queries exist, the sky probe outdoors | The user's choice from a presentation of this and the alternative. World-space probes are stable, see what's off screen, and spread their cost over frames; nothing is baked. Meshes before terrain because the first scene is an interior and the named GPU classes all have ray queries. Rejected: screen-space radiance cascades as the base (off-screen light missing; kept as a later near-detail layer), baked light, per-pixel ray-traced GI, a traced surface cache |
| 2026-11-12 | Rays trace a mesh's coarsest LOD within 2 cm of LOD 0 that kept all its parts and its seams, not LOD 0 always (what this doc said before); the cooker marks the LODs whose seams moved (`MeshLod::seams_moved`, mesh format 3) | Fewer triangles to build and to walk on meshes that simplify well, with nothing a bounce or a reflection can tell. Found on the shrine's statue: its 2 cm LOD had triangles across uv islands, and rays read scrambled colours off it. Marked by what happened (a triangle joining two pieces of the surface that LOD 0 keeps apart), not by what the simplifier was allowed. Not caught: a seam a uv island has with itself (a sleeve's), which is one piece |
| 2026-11-12 | Local lights: entities with a `LocalLight`, placed by a cell's entity or a prefab's part; culled into a 32 x 18 x 32 grid of the view with a bit per light per cluster, by a compute pass over the lights the CPU kept (256 at most); intensity in the sun's exposure-relative units, inverse square with a window to its radius | The design's clustered deferred lighting, in its smallest form that scales: a bit per light can't overflow where lights crowd (a ring of candles), 3D clusters serve what's see-through and later the fog, and brute force over a few hundred CPU-culled lights costs thousandths of a millisecond. A light as a part makes a candle one prefab. Rejected: a list of indices per cluster (it overflows, and what's dropped pops); 2D tiles only (nothing for transparents or fog); a light only as a property of an effect (a lantern has no effect: effect-driven lights come with VFX pass 5, on top of these); physical units now (a candle beside the sun means nothing without auto exposure) |
| 2026-11-12 | The prefab editor edits the `.prefab` text as a document and shows it from that document, each part an entity with what its file cooks to; it writes the file whole in one layout; undo keeps the document's text at each step | A change shows the frame it's made, with no cook in the loop, and what's shown is by the engine's renderer. A prefab is small, so one layout written whole is simpler and safer than patching text (a cell's file is patched because people lay hundreds of lines out by hand), and text steps make undo exact with no command per kind of change. Rejected: editing the cooked prefab and cooking on every change (seconds per drag, and nothing to show while a part is half made); patching the text in place as cells are (far more code for a file the editor owns from its first save); undo through the world editor's commands (they name entities of the game's world, and Ctrl+Z would undo the wrong thing) |
| 2026-11-12 | Prefabs: a `.prefab` cooked to one flat list of parts (nesting resolved by the cooker), a cell holding one small record per placement, parts expanded into entities when the cell loads; variants that leave parts out and no free-form overrides; flecs `IsA` prefabs kept for enemy and item kinds | Researched first (study 13): shipped open worlds separate definition from placement and resolve composition before the game runs; free-form overrides are their documented pain; flecs 4.1.6 makes a table per prefab and per part, which level art can't afford. Expanding at load rather than flattening into cells keeps cells small and makes a prefab edit cook one file. Parts stay entities until a benchmark says static ones should become GPU rows: that step contradicts the world model and isn't taken on an estimate |
| 2026-11-14 | Global illumination is split by distance: a far field baked into nested boxes in the editor by our own GPU ray tracer (Elden Ring's structure, finer where it matters, voxels inside geometry skipped, keys through the day), a near field of short ray-traced gathers per pixel at quarter size that read live direct light, and reflections as layers; each layer measured against a path-traced reference in the engine before the next. The GI method is chosen per project; screen-space radiance cascades are the planned second method, for top-down games | The camera-centred probe volume of 2026-11-12, judged by eye, left a Cornell box black and patchy, and fixes moved the picture without saying whether it was right. A capture of Elden Ring showed its bounce is entirely baked, at a few decimetres in hand-built interiors and 4 m over open ground, with AO for what's smaller; traced near-field gathers add what a bake can't (live light, moving things) for little (our own measured ray cost). Hardware ray tracing stays optional for players (the base is the bake, GTAO and shadow maps; the near field, ray-traced shadows and reflections are options on top), required only where the game is built. "Nothing is baked" is dropped: a GPU bake in the editor takes seconds a box and the day is covered by keys. Rejected: keeping the live probes and patching them (no measure of correctness; they stay as a fallback far field if baking proves a chore), per-pixel path-traced GI (the frame budget), screen-space radiance cascades for the open world (light off screen missing) |
| 2026-11-14 | The camera-centred probe volume is removed from the code, not kept beside the light boxes | The path tracer showed it wrong in ways no fix could be measured to cure; one ambient path keeps the light pass and its tests simple, and the flat hemisphere covers what no box holds until boxes cover a world |
| 2026-11-14 | Screen-space radiance cascades are built now, ahead of the light boxes' next steps, following Sannikov's paper and talks rather than improvising; a renderer switch until a project's config chooses its GI | The user asked for the top-down GI first; a published method with a shipped game behind it is measured against the path tracer, not invented |
| 2026-11-18 | Screen-space radiance cascades are rebuilt from nothing: directions as azimuths on the screen and a few elevation bins, one march along the screen for every elevation, horizons and covered shares instead of hits, cascade 0 a probe a pixel; a height field by default; each step set against the path tracer held to one bounce | The user asked for it ("delete all GI work and start from scratch"): keeping the engine for the game turns on getting Path of Exile 2's GI right. The 2023 talk's encoding slide shows probes as thin strips, few directions one way and doubling the other, which is what makes each cascade half the cost of the one before. Measured, the first build lit only what stood by a glow; this one is 0.12 to 0.21 stops from the path tracer where the flat ambient is 0.35 to 0.77. A finite thickness (light passing behind pillars) is kept but not the default: its pieces leave patches |
| 2026-11-18 | The radiance cascades work on a GI picture smaller than the screen (half its size each way up to 1080p, a third at 1440p, a quarter at 2160p) and keep the light's moments over directions, which each of the screen's pixels takes with its own normal; the far cascades' marches read coarser mips | They cost 5.1 ms at 1600 x 900 where Path of Exile 2 reports 1 to 2, and the user wouldn't keep the engine at that. Now 1.2 ms (0.8 at 720p to 2.6 at 2160p), as near the path tracer as before (0.21, 0.15 and 0.075 stops off in the measuring scenes; at full size 0.21, 0.13 and 0.077) and no less steady as the camera moves. Spherical harmonics alone lost by walls (0.13), so a normal that is the probe's surface's takes the exact integral, moment 1 . n |
| 2026-11-18 | The GI picture is the smallest that's still 360 pixels high (a quarter of the screen each way from 1440p, a third at 1080p), and a march's widest step is 16 of the screen's pixels, not 8 of the GI picture's | The user asked for 1 ms at 2560 x 1440, the game's target, with the light no worse and the cost fixed (nothing that's only quick because a scene is simple). Measured against the path tracer, what the light's accuracy turns on is how far apart the far marches' samples are on the screen, not how many pixels the GI picture has: at 16 pixels a quarter measures as a third, a half and the whole do. So 1.80 ms became 1.18 (1.13 by the frame's time) with every measuring scene as near the path tracer as before or nearer. Short of 1 ms: with the GI picture's size held, a day of tightening the passes' code gave 3 to 4%, and what's left is spread thin (docs: where the time goes) |
| 2026-11-19 | With the radiance cascades on, what surfaces mirror (the specular ambient) comes from the cascades' moments in `rc apply`; the deferred light adds no ambient at all then | The user, looking at the shrine with the sky's factor at 0 (a hall with a roof), saw a blue sheen on the candles: the flat sky, mirrored indoors, which the cascades also bounced on because it was already in the picture they march. From the moments the mirrored light is the place's own, costs nothing more over the two passes together, and a single-bounce picture holds only direct light and glow again |
| 2026-11-19 | A radiance cascade's march that leaves the picture takes the surface at the edge to go on as its plane; before, past the edge was open sky | The user saw a wall flicker in the measuring room. Its face had two answers, 0.3 stops apart, and which it got turned on whether a far cascade's probe stood on it: with one, the sky came in from under the screen; without, it borrowed the light of probes on the ground, for which the ground went on. TAA's sub-pixel offsets flipped it twice in sixteen frames, and a moving camera would too. Taking the edge to go on makes the two agree (the face 161 to 163 of 255 over nine frames, where it went between 154 and 190), and stops the sky lighting things from below the screen |
| 2026-11-19 | The radiance cascades' light bounces more than once: what each surface sent on last frame of the light gathered is met by this frame's marches | With the sky no longer let in at the edge, what it had stood in for was missing: light off the sky-lit ground, which a single bounce of direct light doesn't have. Kept a frame and read where the surface was, it costs a texture's read and write at the GI picture's size, and measures nearer the path tracer with all its bounces than the build before did on all four scenes (0.35, 0.26, 0.82, 0.12 stops against 0.38, 0.38, 0.94, 0.18). The price is a frame's lag on what has bounced |
| 2026-11-19 | While the radiance cascades are on, the factor they give the sky is also the share of the sky's ambient that fog, lit particles, water and see-through things take | The user asked why the shrine was still blue with the cascades lighting it as a hall (the sky's factor at 0). Captured pass by pass, the lit surfaces were warm and dark and the fog pass laid the open sky's blue over them, fifty times the far wall's own light. One factor for all of it makes a roofed hall read as one: door light, candle pools, no veil. By place is still to do |
| 2026-11-19 | How far a place is closed to the sky is the world's to say: `under_roof` in its environment, and in a fog volume for that volume's place; the console's factor only multiplies it | The shrine only looked like a hall with `r.rc_sky 0` typed, and that was one number for the whole view: wrong for the ground outside the door. An environment's value covers a world that is all dungeon; a volume covers a hall in an open world, with nothing new to place or edit (a fog volume already has a place, a shape and a soft edge, and the editor already shows and saves it). A map of the roofs from above would need no placing at all, but it is another view to cull and draw, in the sun shadows' machinery: later |
| 2026-11-19 | Local lights' shadows: room for 32 point lights at one steady size (192 faces, a 4096 x 3072 atlas, 256 texels a point light's face), and soft edges by the light's size (percentage-closer soft shadows, by ways out from the light) | The user had found them popping for a month, by no distance that made sense, and harsh. Measured: the shrine's 24 lights needed 144 faces of the 128 there were, so three went without and which three turned on the camera's path; sizes went by rank and swapped at once (37 changes in a 15 second walk, none now). A hard 3 x 3 edge at 128 texels is the harshness; an edge as wide as the source looks is what a candle's shadow is. It costs 1.3 ms more in the shrine's worst view, all of it reads of the atlas; not drawing still lights' tiles again is what should pay for it |
| 2026-11-19 | The local shadow atlas is kept from frame to frame: a light's tiles are culled for and drawn again only when something that casts changed within its reach, or the light itself did | Culling for 192 faces and drawing 144 of them was 2.5 ms of the shrine's frame, every frame, for candles and walls that never move. The frame already says which instances changed, so where casters changed costs a loop over those. 1.9 ms back in the shrine, more than the soft edges cost; what's left is the looks |
| 2026-11-19 | A light says how much of a shadow it casts (`"shadow"`, 0 to 1), and a frame's shadowed lights can be capped (`r.local_shadow_lights`); in the shrine one candle of each group casts | With tiles kept, what a shadowed light costs is its looks, a pixel at a time, for every light that reaches the pixel: 24 overlapping casters were 1.75 ms of them. The user's own way of putting it: keep the lights, let few cast. Nine casters are 0.85 ms and the hall reads the same but for a little fill in the basin |
| 2026-11-19 | The fog's grid is about 240 x 135 columns whatever the picture's size (a column 8 pixels up to 1080p, 11 at 1440p, 16 at 2160p); a froxel with last frame's light takes its light at two places, not four; how open to the sky it is, is asked once a froxel | With the shadows and the GI near their aims, `fog inject` was the frame's largest pass: 1.6 to 1.9 ms of the shrine's 7.9 at 2560 x 1440, where a column of 8 pixels makes as many froxels as the picture has pixels (3.7 million), each lit at four places by every light of its cluster, the sun through its shadow map and, since that morning, the sky volumes. Each part timed by leaving it out: two places for four 0.8 ms, the lights 0.6, the sun 0.5, the sky's openness a place 0.45. Now 0.6 ms, the frame 6.65; the pictures are the same to within 0.2 of 255 on average, and as steady under TAA |
