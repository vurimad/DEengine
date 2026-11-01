# core

The foundation every other system builds on: the core library (containers, strings, callbacks, `Result`), allocators, the job system, logging and math. See docs/design.md, Core library.

## Contents

| Header | Type | Phase 1 implementation |
| --- | --- | --- |
| `allocator.h` | `Allocator`, `heap_allocator(MemoryTag)` | Interface + one heap per tag via `operator new` |
| `memory_tag.h` | `MemoryTag` | Own |
| `memory_stats.h` | `memory_stats()`, heap allocation counters, `NoHeapAllocationScope` | Own (atomics) |
| `arena.h` | `Arena` (linear, mark/rewind, reset) | Own |
| `frame_allocator.h` | `FrameAllocator` (per frame, lock-free, frames in flight) | Own |
| `pool.h` | `Pool` (fixed-size blocks, free list) | Own |
| `log.h` | `DE_LOG_*`, `DE_FATAL`, `LogSink` | Own: printf-style, stderr sink by default |
| `array.h` | `Array<T>` | `std::vector` underneath |
| `inline_array.h` | `InlineArray<T, N>` | Own |
| `hash_map.h` | `HashMap<K, V>` | `std::unordered_map` underneath |
| `slot_map.h` | `SlotMap<T>`, `SlotHandle<T>` | Own, on `Array` |
| `string_id.h` | `StringId`, `"name"_sid` | Own (FNV-1a 64, constexpr) |
| `function_ref.h` | `FunctionRef<R(Args...)>` | Own |
| `result.h` | `Result<T, E>`, `Error`, `fail()` | Own |
| `hash.h` | `Hash<T>`, `mix64`, `fnv1a_64` | Own |
| `assert.h` | `DE_ASSERT`, `DE_VERIFY`, fatal handler | Own: logs, then the fatal handler, then abort |
| `compiler.h` | `DE_PRINTF_FORMAT` | Compiler differences only |

`core.natvis` gives each type a readable view in the Visual Studio debugger.

## Rules and invariants

- No dependencies on other engine folders. Everything else may depend on `core`.
- Containers built on std expose only our API; no std-only methods leak out, so the implementation can be swapped without touching callers.
- Every container takes an `Allocator&` (defaulting to the heap) and allocates only through it.
- Containers are not copyable: copies are explicit (e.g. `Array::assign`). Moves leave the source empty and reusable, and are cheap (except phase 1 `HashMap` on MSVC, which allocates a sentinel node).
- Removal names its cost: `remove_swap` is O(1) and reorders, `remove_ordered` is O(n) and keeps order.
- Lookups return a pointer or null; nothing inserts behind your back (`find_or_insert` is explicit).
- Debug and dev builds check bounds and invariants with `DE_ASSERT`; ship builds don't evaluate them. `DE_VERIFY` checks in every build, for failures that would corrupt memory.
- Every heap byte belongs to a `MemoryTag`. Systems allocate from their tagged heap, an `Arena`, a `Pool` or the frame allocator.
- The heaps, memory stats, logging and the fatal handler are process-wide by design: they are needed everywhere, before any `EngineContext` exists. Platform specifics plug in through hooks (`set_fatal_handler`, `add_log_sink`); core itself has no OS code.
- Hot paths stay off the heap: wrap them in `NoHeapAllocationScope` in tests and in the frame loop.
