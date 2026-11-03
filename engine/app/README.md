# app

The framework that runs an application, and the applications (docs/design.md, High-level architecture). `engine/main.cpp` is only the process's entry point: it parses the arguments, declares the application and calls `run_framework`.

## Contents

| File | What it gives |
| --- | --- |
| `framework.h` | `FrameworkArgs` and `parse_args` (the engine's flags; `flag_value` and `has_flag` for an application's own), `Engine` (the running systems, and what the frame draws), `Application`, `run_framework` |
| `viewer.h` | `Viewer`: the application until the game's own lands; loads a cell (`--cell`) and flies a free camera through it |

## Rules and invariants

- **The framework owns the engine and the frame loop;** the application says what to load and does its own per-frame work. `startup()` runs once the engine is up: it adds input actions and loads the world. `update()` runs every frame after input is sampled, before assets update and the world advances. The application sets `Engine::lighting` and `Engine::view`; the framework draws them.
- **The frame:** pump and sample input, shader hot reload, `Application::update`, assets, GPU copies, `World::advance` (fixed ticks, then RenderExtract), the debug UI, then the render graph (scene passes, the debug UI over them).
- **Bindings are text**, read after `startup()` adds the actions: `config/input.bindings` from `src:/` in dev builds (edits apply on the next run, no cook), from `game:/config/` in ship; then the player's `user:/input.bindings`, which adds to them. Lines for actions a build doesn't add (dev tools, in ship) are skipped.
- **Dev tools are dev builds only** (`DE_BUILD_DEV`): the debug UI and stats panel (F1), the G-buffer views (F2), the raw stick readout (`--show-sticks`), the validation layer and `src:/`.
- Construction order is dependency order, so teardown (reverse) never leaves a system using one that's gone: the registry before the world (cells release through it) and before GPU copies (they mirror it).

## Not yet

- The game as an application (M1), with its world asset (cells, start pose, environment) replacing the viewer's test cell and hardcoded lighting.
- Modes of the one executable: `--editor`, `--headless`.
