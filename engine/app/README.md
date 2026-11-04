# app

The framework that runs an application, and the applications (docs/design.md, High-level architecture). `engine/main.cpp` is only the process's entry point: it parses the arguments, declares the application and calls `run_framework`.

## Contents

| File | What it gives |
| --- | --- |
| `framework.h` | `FrameworkArgs` and `parse_args` (the engine's flags; `flag_value` and `has_flag` for an application's own), `Engine` (the running systems, and what the frame draws), `Application`, `run_framework` |
| `viewer.h` | `Viewer`: the application until the game's own lands; loads a world (`--world`, or `world` in `config/engine.settings`), every cell of it, lights it by its environment and flies a free camera through it from its start |

## Rules and invariants

- **The framework owns the engine and the frame loop;** the application says what to load and does its own per-frame work. `startup()` runs once the engine is up: it adds input actions and loads the world. `update()` runs every frame after input is sampled, before assets update and the world advances; returning false ends the run. Quitting is the application's (the game asks first), not the framework's. The application sets `Engine::lighting` and `Engine::view`; the framework draws them.
- **The frame:** pump and sample input, shader hot reload, `Application::update`, assets, GPU copies, `World::advance` (fixed ticks, then RenderExtract), the debug UI, then the render graph (scene passes, the debug UI over them).
- **Settings are text:** `config/engine.settings`, one name and value a line (`setting(engine.settings, "world")`), read once at startup from the same place as the bindings.
- **The viewer follows its assets:** a re-cooked world loads the cells it now lists and unloads the ones it doesn't (the camera stays put); a re-cooked environment relights the frame.
- **Bindings are text**, read after `startup()` adds the actions: `config/input.bindings` from `src:/` in dev builds (edits apply on the next run, no cook), from `game:/config/` in ship; then the player's `user:/input.bindings`, whose lines replace the defaults of the actions they name. Each file is checked whole before anything binds. Lines for actions a build doesn't add (dev tools, in ship) are skipped; in dev builds, where every action is added, they're logged as typos.
- **Frame time goes in refresh intervals:** with vsync, while the frame rate holds the refresh rate, `FrameClock` hands the application and the world whole intervals, carrying the remainder, so the noise in the loop's clock doesn't move the camera unevenly. Otherwise, the measured time. The stats panel shows the measured times, and how many are late (F1): that's the loop's rhythm, which can be uneven while the screen is smooth.
- **Dev tools are dev builds only** (`DE_BUILD_DEV`): the debug UI and stats panel (F1), the G-buffer views (F2), TAA on and off (F3; `taa` in `config/engine.settings` sets it in every build), the raw stick readout (`--show-sticks`), the validation layer and `src:/`.
- Construction order is dependency order, so teardown (reverse) never leaves a system using one that's gone: the registry before the world (cells release through it) and before GPU copies (they mirror it).

## Not yet

- The game as an application (M1); streaming the world's cells around the player instead of loading all of them.
- Modes of the one executable: `--editor`, `--headless`.
