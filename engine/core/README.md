# core

The foundation every other system builds on: the core library (containers, strings, callbacks, `Result`), allocators, the job system, logging and math. See docs/design.md, Core library.

Rules and invariants:

- No dependencies on other engine folders. Everything else may depend on `core`.
- Phase 1 containers wrap std underneath, but only our API is exposed; no std-only methods leak out.
- Every container takes an allocator and every allocation is tagged, even while phase 1 ignores it.
- Containers never allocate silently: no implicit copies of heavy types (`= delete` the copy constructor).
- Debug and dev builds check bounds and invariants with asserts; ship builds don't.
