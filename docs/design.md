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
| Terrain | Heightfield clipmap, splat materials | Virtual texturing, sculpt + paint in editor, holes for caves | Runtime deformation, snow/mud trails |
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

Performance target: **120 FPS in the full game**, not just the demo. RTX 3070 / RX 6700 XT class at 1080p native, 1440p with upscaling; 60 FPS floor on RTX 2060 class at low preset. Frame time budget 8.3 ms: game thread ≤ 6 ms, render thread ≤ 6 ms, GPU ≤ 7.5 ms. Memory budget: 12 GB RAM, 8 GB VRAM.

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
- **Change detection by table.** Systems skip tables nothing wrote to (`changed()`); data that changes often and data that rarely does live in different components, since one write marks its whole table. Render extraction is to send only changed transforms to the GPU instance buffer once instances persist on the GPU (GPU-driven v1); until then every instance is written each frame.
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
| `game:/` | Loose cooked files (overriding archives) | `Archives/DATA_*.arc` | Read only |
| `src:/` | The repo's source tree: `content/`, `meta/`, `shaders/` | Not present | Editor, cooker and dev builds (shader hot reload) |
| `user:/` | Saved Games / AppData folder | Same | Read/write: saves, settings, logs, caches |
| `temp:/` | Temp folder | Same | Scratch, cleared on start |

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

**Hot reload.** A file watcher on `src:/` is to trigger the cooker (for now it is run by hand); when cooked output changes in `game:/`, the asset registry reloads it in place and notifies dependents.

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
| Terrain tile | `.deterrain` | Height mips, splat maps, holes, grass density |
| Physics | `.dephys` | Jolt-serialized collision shapes |
| Navmesh tile | `.denav` | Detour tile data |
| Sound | `.desnd` | Opus/Vorbis data + playback metadata |
| Shader | `.deshader` | SPIR-V per permutation + reflection |
| Cloth | `.decloth` | Simulation mesh, constraints, painted limits, render-mesh mapping, LODs |
| Cell / prefab | `.decell` | Binary form of the text scene data |
| World | `.deworld` | A world's cells by grid coordinate (named, not loaded with it), where the player starts, its environment |
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
- Cells are `.cell` (JSON): the cell's grid coordinates and its entities (name, mesh by relative path to a model source, position, rotation, scale, parent by name, static). The `.decell` lists the meshes as dependencies, puts parents before children, and is cooked again when a model it names gets a new ID, not when a model is cooked again.
- Worlds are `.world` (JSON): the environment and the cells by relative path, and the start position and view. The cooker reads each cell's coordinates into the `.deworld` (sorted, one cell per coordinate), so streaming can find cells without loading them; the environment is its one dependency. Environments are `.environment` (JSON): the sun by elevation and azimuth (degrees, clockwise from north, -Z), colors with intensities (1 when left out), exposure and shadow settings, every other one required so a file states the whole look. Editing the light cooks only the environment; moving a cell cooks the world again.
- The cooker keeps its state in `game:/cook_state.json`: per source, the hash of every input (bytes, settings, files it reads, versions) and what it made. Unchanged sources are skipped, outputs of deleted sources removed, and metas of files moved outside the tools re-linked by the last content hash.
- `cooker --check` validates the source and meta trees and writes nothing, for CI.

## Rendering

The renderer is a GPU-driven deferred PBR pipeline built on a render graph, aimed at the Elden Ring look: wide vistas, dense grass, strong atmospheric depth, golden backlight, heavy VFX. Deferred is the right fit here because the open world has huge material variety, many local lights from VFX, and decals everywhere.

**Render graph (frame graph).** Every pass declares what it reads and writes; the graph does the rest. Nothing fancy, just the parts that pay off:

- Passes are lambdas registered each frame with `setup` (declare resources) and `execute` (record commands).
- Transient resources (G-buffer targets, HDR buffers, shadow maps, blur chains, temporary UAVs) are declared by description, not allocated by passes.
- The graph computes each resource's lifetime (first to last use), then a resource allocator places transient resources in shared memory heaps, aliasing any two whose lifetimes don't overlap. Assets are never aliased; only graph resources are.
- Barriers and layout transitions are inserted automatically from the declared reads and writes.
- Passes nothing reads are culled; async-compute passes are scheduled on the compute queue with automatic queue sync.
- Imported resources (swapchain, TAA history, persistent shadow cache) live outside the aliasing pool.
- A debug view shows the graph, lifetimes and heap usage, and can display any intermediate texture.

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
- **Debug.** Immediate mode (dev builds) and a transition log. The graph can be dumped to the log as text.
- **Queues.** Graphics only. Async compute passes come later, on top of the RHI's queue ownership transfers. Uploads stay with the uploader, which lands them at the start of the frame, before the graph's passes.

**GPU-driven geometry.**

- All meshes live in large shared vertex/index buffers; all materials and textures are bindless (descriptor indexing + buffer device address).
- A compute pass culls instances (frustum + Hi-Z occlusion from last frame's depth) and writes indirect draw commands (`vkCmdDrawIndexedIndirectCount`).
- LOD selection on the GPU per instance; distant trees and rocks swap to impostors.
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

**Debug UI.** Dear ImGui (debug and editor tools only) is drawn by the engine's own renderer, not ImGui's Vulkan backend: vertices read through a buffer pointer, textures by bindless index, a render graph pass over the backbuffer, its font atlas through the uploader (ImGui's texture protocol). Its input comes from a small SDL3 feed inside `platform/`, so SDL stays there. Its memory is tagged Tools; it saves no ini file. The frame stats panel (F1) shows frame times, heap allocations per frame, GPU memory by category, the render graph, uploads and the device.

**Hardware ray tracing (optional, RT-ready from day one).** The game must look right with RT off; RT features are passes that replace or improve a raster fallback when the GPU supports them.

- Ready from the start: the RHI exposes acceleration structures (BLAS per mesh LOD0, TLAS rebuilt from the GPU instance buffer each frame), and the bindless setup lets ray shaders reach any material.
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

Built so far (renderer v1): sun shadows, G-buffer, shadow mask, deferred lighting (the sun, a hemisphere ambient, the sky), fog over the scene and the sky (step 9: a froxel grid near the camera with the sun's shadows, the closed form past it), TAA, tonemap, then the debug UI over it. engine/render/scene/README.md has the details.

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

Terrain is a first-class system, not a mesh: a streamed heightfield rendered with GPU-driven clipmaps, shaded through a virtual texture, and covered by GPU-generated grass and foliage.

**Heightfield and streaming.**

- World split into terrain tiles (e.g. 256 m, 1 m height resolution, 16-bit heights) cooked into a mip pyramid.
- Geometry clipmap rings around the camera, rendered as instanced patches; LOD morphing between rings removes cracks and popping.
- Tiles stream by distance with the world cells; far tiles keep only low mips.
- Holes mask for cave entrances and dungeon openings; meshes (cliffs, rocks) blend into terrain with height-based blending.

**Materials and texturing.**

- Splat maps with up to 16 layers per tile (grass, dirt, rock, sand, snow, ash), height-blended.
- Runtime virtual texture (RVT): terrain layers and decals are baked into a page cache near the camera, so far terrain costs one sample, not 16.
- Triplanar mapping on steep slopes; procedural rules (slope, height, curvature) fill layers automatically, painting overrides them.

**Grass fields (GPU-driven, indirect).**

1. A density map per tile (painted or rule-driven) defines grass types and amount.
2. Each frame a compute pass scatters blade or clump instances in tiles around the camera with stable per-tile seeds, so grass never swims.
3. The same pass frustum- and Hi-Z-culls, picks LOD (procedural blades near → grass board cards from your textures mid-distance → terrain color only far), and appends to indirect draw buffers.
4. Grass renders in a few `DrawIndexedIndirectCount` calls, vertex-shader animated by a global wind field.
5. Interaction: a small displacement texture around characters, horses and spells bends grass; fire burns it into ash for a while.

Target: hundreds of thousands of visible blades within \~0.6 ms of GPU time, fading into terrain color past \~80 m.

**Trees and foliage.** Placed instances (painted in the editor or by rules), GPU-culled like all meshes, 3–4 LODs plus octahedral impostors for the far field, wind via vertex animation, subsurface translucency for leaves.

**Editor tools.** Sculpt (raise, smooth, flatten, erode), paint layers, paint grass density and foliage, rule-based scatter with preview. Every brush stroke is a command, so it is undoable and scriptable.

## Sky, atmosphere & weather

The sky is a core system, not a skybox: it drives the lighting, fog, mood and color of the whole world. Everything is physically based and fully dynamic, so time of day and weather change smoothly with no baked lighting to redo.

**Atmosphere.**

- Physically based scattering (Rayleigh, Mie, ozone) using small precomputed LUTs (transmittance, multi-scattering, sky-view, aerial perspective), recomputed only when atmosphere parameters change. Cost: well under 0.5 ms.
- Aerial perspective applied to every pixel, so distant mountains and castles fade into the sky color naturally; this is a large part of the Elden Ring look.
- Artist controls on top of physics: tint, haze density, horizon glow, golden-hour boost, stored per region and per weather state.

**Fog: one medium model, near and far.** Elden Ring's way (study 04): extinction is a constant plus a height fog around a base height, with a bounded exponential profile (thinning upward; below the base, rising only to twice its density, so valleys don't fill without limit), coloured per channel (blue fades first, the sun reddens through haze). Light scattered in: the sun through a colour looking toward it and another looking away (artists' controls in place of one phase parameter), dimmed by the fog column above, plus an ambient colour; a start distance, another for sun haze, and an end distance that is also the sky's. The sky is fogged like everything else, so the horizon takes the haze of the land below it.

- Far range (built, `shaders/common/fog.slang`): the closed-form integral of that medium along each view ray, in a full-screen pass after lighting (about 0.06 ms at 1600 x 900). Settings in the environment asset, hot reloaded.
- Near range, a froxel grid with the same medium, in phases:
  - A (built): a view-aligned grid (a column per 8 pixels, 64 slices spaced exponentially out to `volume_distance_m`). `fog inject` stores each froxel's extinction and light, with the sun's shadows from the cascaded atlas (light shafts); `fog integrate` walks each column front to back with the slice's closed-form integral; the composite reads it at each pixel's depth, then the far range goes on from the grid's edge. About 0.2 ms at 1600 x 900.
  - B: temporal reprojection with per-frame jitter and a filter along depth, for smooth shafts.
  - C: fog volumes artists place (mist banks, fog walls), injected into the grid.
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
- **Waves:** sum of Gerstner waves for lakes and sea plus scrolling normal maps for detail; rivers use flow maps generated from the spline direction. Physics and buoyancy read the same wave function on the CPU.
- **Shading:** depth-based absorption and scattering (clear shallows, dark deep water), refraction from the scene color, fresnel, sky reflection from the sky probe, SSR (and RT reflections when enabled), foam on shorelines, rocks and around characters.
- **Shorelines:** a distance-to-shore field baked per cell drives foam bands, wet sand and gentle shore waves.
- **Interaction:** ripples and wakes from characters, horses and spells via a small simulation texture around the camera; splashes as VFX.
- **Underwater and swamps:** underwater fog and color absorption, optional caustics projected on terrain, poison-swamp variants with emissive and murky parameters.
- **Cost target:** \~0.5 ms with a large lake on screen, rendered in the forward transparent pass.

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

**Large-world precision.** World coordinates in double precision on the CPU, floats relative to a render origin near the camera on the GPU, so a 10+ km world has no jitter.

- Positions are doubles everywhere on the CPU (`Transform.position`); rotation and scale stay float. The simulation never rebases: physics, AI, saves and distances all see one fixed world space. At 60k entities this costs about 2 MB.
- The GPU sees floats relative to a *render origin* that render extraction moves when the camera gets about 1 km away from it. Extraction is the only place doubles become floats, and nothing outside extraction knows the origin exists. Today every instance is written each frame; once instances persist on the GPU (GPU-driven v1), moving the origin re-uploads them once and static instances otherwise upload once, so the origin doesn't get in the way of change-only uploads. Within 1 km of the origin, float precision is about 0.06 mm.

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
- **The renderer owns no entity list.** RenderExtract queries the world each frame and writes what is visible into instance data. When instances become persistent (GPU-driven), the slot lives in a component on the entity, given and freed by observers, so it lives and dies with the entity.
- **Mechanism now, policy later.** M0b loads cells explicitly; M1 adds the streamer that picks cells (ranking, budgets, hysteresis). The cell format and lifetime don't change between the two.
- **Cell grid:** 128 m cells with integer coordinates; dungeons are grids of their own (M1). Cell contents in v1 are a fixed set (name, transform, parent, mesh; lights and cameras next); any reflected component comes with the editor.

**World state.** A flat, saveable set of flags and counters keyed by stable IDs: opened shortcuts, killed bosses, picked-up items, NPC quest stages, discovered graces. Streamed entities consult world state when they spawn, so a looted chest stays looted.

**Checkpoints (graces).** Resting heals, refills flasks, respawns non-boss enemies by marking their spawn records dirty, and saves. Death respawns at the last grace and drops currency at the death spot.

**Saves.** Small binary files with versioned schemas: player, inventory, world-state flags, position, dropped currency. Written to a temp file then renamed. Save on rest, region change, and quit.

## Physics, camera, input & audio

**Physics (Jolt).** Static level collision from simplified meshes; a capsule character controller (Jolt CharacterVirtual) for player and enemies; simple rigid bodies only for breakables and corpses (ragdolls in a later tier); soft bodies for character cloth (see Cloth & secondary motion). Combat hit detection does not use physics bodies; it uses its own swept capsule queries so it stays deterministic and frame-exact.

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
  tools/      # debug UI, stats panel, free camera
config/       # engine settings as text: input bindings, engine.settings (copied into game:/config/)
tools/
  cooker/     # asset cooker, validators, packer
shaders/      # Slang
content/      # src:/ source assets, data, Lua
meta/         # mirrored meta tree: GUIDs + import settings
tests/        # unit, gameplay, benchmarks
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
- [ ] GPU-driven v1: shared vertex/index buffers, compute instance culling, indirect draws
- [ ] Render graph v2: transient memory aliasing, graph and lifetime debug view, a `discard` declaration on uses (so passes whose output is fully overwritten get culled), parallel pass recording (passes split into chunks recorded by jobs on command lists of their own, submitted in order)
- [ ] Render graph v3, before M1 content: async compute passes (a queue per pass, queue ownership transfers and semaphores between queues), barrier grouping by dependency level, then pass reordering where profiling shows bubbles
- [ ] LuaJIT + sol2: bindings for entities, components and commands, script hot reload
- [ ] Command API v1: `entity.create`, `component.set`, undo/redo, local JSON-RPC socket
- [ ] Editor mode v1: outliner, reflection-driven inspector, gizmos, save to text cell files
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
| 2026-10-31 | Terrain clipmaps + GPU-driven indirect grass | Huge fields at low CPU cost |
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
| 2026-11-03 | Cells are assets and entities: members carry a non-fragmenting `(InCell, cell)` that deletes them with the cell; entities hold non-owning asset handles | One owner per thing, no mirrored lists between world, assets and renderer; streaming mechanism built in M0b, policy in M1 |
| 2026-11-03 | `.cell` is JSON with world-space positions for roots; props are static by default (children as their parent); members respawn when hot reload swaps in a re-cooked cell | Readable and diffable by people and agents; most placed things never move; edit, cook and see it in the running game |
| 2026-11-03 | Elden Ring may be studied in full from an owned copy, shader code included (captures and dumps, offline only); own implementations, findings in our words, nothing extracted committed | Its techniques are the reference for the look; studying the real thing beats guessing, and clean implementations keep the engine ours |
| 2026-11-03 | Match Elden Ring's feel, atmosphere and art controls, not its low-level renderer; our own G-buffer, lighting core, shadows and upscaling | Their look comes from art direction on dated tech; a modern, consistent core with their art levers on top looks as good and ages better |
| 2026-11-03 | G-buffer v1: three targets (base color + lighting group, RG16 octahedral normal, roughness/metalness/occlusion/translucency) and emissive into HDR, 20 bytes a pixel | Room for the look's art levers (groups, translucency) at the bandwidth of a lean layout; emissive needs no target of its own |
| 2026-11-03 | The entry point only parses arguments, declares the application and calls `run_framework`; the framework owns the engine and the frame loop, the application (`startup`, `update`) says what to load, does its frame's work and decides when to quit; bindings and settings are text in `config/`, not code | Platform entry, engine and game stay apart; what a build loads and binds is data, so it changes without a rebuild |
| 2026-11-03 | A world is an asset (`.world`: cells, start, environment) and the light is one too (`.environment`), both hot reloaded; the engine opens the world named in `config/engine.settings` | Nothing about a scene is in code; editing the light or the world shows in the running game, and the same assets carry over to the streamer, time of day and weather |
| 2026-11-03 | Sun shadows v1: 4 stable cascades in one atlas, resolved into a screen-space mask (dithered cascade seams, contact-hardening filter), sunlight in shadow tinted toward a shadow color | A mask keeps lighting simple and makes room for temporal filtering; tinted shadows are part of the look; the static cache waits until scenes have enough static casters to need it |
| 2026-11-03 | Until GPU-driven v1 keeps instances on the GPU, render extraction writes every instance each frame; change-only uploads (and the render origin's one re-upload) arrive with it | The simplest correct path first; the per-frame cost is measured (GPU timers, Tracy) before it's optimized |
| 2026-11-03 | Fog v1 is an analytic height fog (Elden Ring's medium: constant plus bounded exponential height fog, coloured extinction, two-colour sun lobe, ambient) in a full-screen pass over the scene and the sky; froxels come next with the same model | Most of the atmosphere for a few hundredths of a millisecond; the froxel fog needs this model and this far range anyway |
| 2026-11-03 | Froxel fog in phases: A the grid with sun shadows, B temporal, C fog volumes, D local lights; 8-pixel columns and 64 slices, on when an environment sets `volume_distance_m` | Each phase is usable alone; 0.2 ms measured for A leaves room in the budget; the closed form stays the far range and the no-grid path |
| 2026-11-03 | Motion vectors: an instance's pose a frame ago comes from the same two ticks it's interpolated between, stepped back by the frame's share of a tick; no history per entity | Stateless and compact, exact for steady motion, none for what's static or place()d; a change of speed is a small one-frame error, which TAA's rejection covers. The camera keeps real history (what was drawn) in the render frame, since a free camera is place()d every frame; an explicit cut drops it |
| 2026-11-04 | TAA resolve: Elden Ring's anti-ghosting layers (nearest-depth motion, neighbourhood box, motion coherence, off-screen) with a depth test added, coherence on object motion (the camera's part taken out, written by the pass and kept) rather than total speed, clipping rather than clamping, Catmull-Rom history, blending in a tonemapped space; history kept by the renderer, on by default | Ghosting is what we refuse to show: each layer covers a case the others can't, and the test exercises each alone. Transparents and particles will need a reactive mask when they arrive |
| 2026-11-04 | Frame pacing: begin_frame waits for the previous frame, and the camera and world move by the mean of the last 12 measured frame times | With two frames allowed to queue on a light load, the loop could settle into two frames back to back then a long wait, 50% of frames late at a good average frame rate, which visibly shook the camera (seen in a blind test, measured from captured frames). Update still overlaps the GPU, so the wait cost 0 to 7% frame rate on heavy GPU loads and none on CPU loads. A measured frame time is noisy even so; the mean is unbiased and adds up to the time measured |
