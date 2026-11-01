# core

The foundation every other system builds on: the core library (containers, strings, callbacks, `Result`), allocators, the job system, logging and math. See docs/design.md, Core library.

## Contents

| Header | Type | Phase 1 implementation |
| --- | --- | --- |
| `allocator.h` | `Allocator`, `heap_allocator()` | Interface + heap via `operator new` |
| `array.h` | `Array<T>` | `std::vector` underneath |
| `inline_array.h` | `InlineArray<T, N>` | Own |
| `hash_map.h` | `HashMap<K, V>` | `std::unordered_map` underneath |
| `slot_map.h` | `SlotMap<T>`, `SlotHandle<T>` | Own, on `Array` |
| `string_id.h` | `StringId`, `"name"_sid` | Own (FNV-1a 64, constexpr) |
| `function_ref.h` | `FunctionRef<R(Args...)>` | Own |
| `result.h` | `Result<T, E>`, `Error`, `fail()` | Own |
| `hash.h` | `Hash<T>`, `mix64`, `fnv1a_64` | Own |
| `assert.h` | `DE_ASSERT`, `DE_ASSERT_MSG` | Minimal: print and abort |

`core.natvis` gives each type a readable view in the Visual Studio debugger.

## Rules and invariants

- No dependencies on other engine folders. Everything else may depend on `core`.
- Containers built on std expose only our API; no std-only methods leak out, so the implementation can be swapped without touching callers.
- Every container takes an `Allocator&` (defaulting to the heap) and allocates only through it.
- Containers are not copyable: copies are explicit (`assign`). Moves are cheap and leave the source empty.
- Removal names its cost: `remove_swap` is O(1) and reorders, `remove_ordered` is O(n) and keeps order.
- Lookups return a pointer or null; nothing inserts behind your back (`find_or_insert` is explicit).
- Debug and dev builds check bounds and invariants with `DE_ASSERT`; ship builds don't evaluate them.
