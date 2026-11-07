# app

The framework that runs an application, and the applications (docs/design.md, High-level architecture). `engine/main.cpp` is only the process's entry point: it parses the arguments, declares the application and calls `run_framework`.

## Contents

| File | What it gives |
| --- | --- |
| `framework.h` | `FrameworkArgs` and `parse_args` (the engine's flags; `flag_value` and `has_flag` for an application's own), `Engine` (the running systems, and what the frame draws), `Application`, `run_framework` |
| `viewer.h` | `Viewer`: the application until the game's own lands; loads a world (`--world`, or `world` in `config/engine.settings`), every cell of it, lights it by its environment and flies a free camera through it from its start |
| `agent_eyes.h` | `EyesFlags` and `AgentEyes`, dev builds: what lets an agent place the camera and take a repeatable picture from the command line (below) |

## Rules and invariants

- **The framework owns the engine and the frame loop;** the application says what to load and does its own per-frame work. `startup()` runs once the engine is up: it adds input actions and loads the world. `update()` runs every frame after input is sampled, before assets update and the world advances; returning false ends the run. Quitting is the application's (the game asks first), not the framework's. The application sets `Engine::lighting` and `Engine::view`; the framework draws them.
- **The frame:** pump and sample input, shader hot reload, `Application::update`, assets, GPU copies, `World::advance` (fixed ticks, then RenderExtract), the debug UI, then the render graph (scene passes, the debug UI over them).
- **Settings are text:** `config/engine.settings`, one name and value a line (`setting(engine.settings, "world")`), read once at startup from the same place as the bindings.
- **The viewer follows its assets:** a re-cooked world loads the cells it now lists and unloads the ones it doesn't (the camera stays put); a re-cooked environment relights the frame.
- **Bindings are text**, read after `startup()` adds the actions: `config/input.bindings` from `src:/` in dev builds (edits apply on the next run, no cook), from `game:/config/` in ship; then the player's `user:/input.bindings`, whose lines replace the defaults of the actions they name. Each file is checked whole before anything binds. Lines for actions a build doesn't add (dev tools, in ship) are skipped; in dev builds, where every action is added, they're logged as typos.
- **Frame time goes in refresh intervals:** with vsync, once the loop runs about a frame a refresh, `FrameClock` hands the application and the world whole reported refresh intervals, carrying the remainder, so the noise in the loop's clock doesn't move the camera unevenly. Missed refreshes don't turn it off (a frame of two intervals is two); over 5% of them, or many long frames, do. Otherwise it's the measured time. Limits: a locked lower rate (120 fps on a 240 Hz display) stays on the measured time; a variable-rate display within about 5% of its maximum gets occasional double steps, as does a display a little slower than it reports, and one a little faster runs that little fast. The debug UI runs on measured time. The stats panel shows the measured times, and how many are late (F1): that's the loop's rhythm, which can be uneven while the screen is smooth. Mouse look moves by device deltas, so it still follows that rhythm.
- **The window's size is a flag:** `--window 2560x1440`, in points, or `--fullscreen` for the display's own. 1600 x 900 otherwise.
- **Dev tools are dev builds only** (`DE_BUILD_DEV`): the debug UI and stats panel (F1), the G-buffer views (F2), TAA on and off (F3; `taa` in `config/engine.settings` sets it in every build), the frozen cull (F4, or `--freeze-cull` to start with it; F1 and the `cull frozen` plot show it; see `render/scene/README.md`), height fog thinning with a low camera on and off (F5), the raw stick readout (`--show-sticks`), the validation layer and `src:/`.
- Construction order is dependency order, so teardown (reverse) never leaves a system using one that's gone: the registry before the world (cells release through it) and before GPU copies (they mirror it).

## The agent's eyes

What lets an agent see the real engine with no human and no re-cook: place the camera, take a repeatable picture, compare it with another (`tools/image_diff`). Dev builds only: `parse_args` doesn't read these flags in ship, and nothing in ship's `deengine` reaches this code, so none of it is linked. They stand in until the command API's `camera.goto` and `screenshot.capture` (docs/design.md, AI-friendly by design). The window still opens: there's no headless mode yet.

```
deengine --view camp_south --still src:/build/eyes/camp.png              # a settled, repeatable picture, and exit
deengine --camera 64,2,74,0,-7 --still src:/build/eyes/camp.png          # the same from the command line
deengine --camera 16000.25,40,-16000.5,135,-5                            # just place the free camera, and fly from there
deengine --camera-path src:/config/camp_ride.path --path-shots src:/build/eyes/ride/shot --path-every 30
image_diff build/eyes/before.png build/eyes/camp.png --out build/eyes/diff.png
```

| Flag | What it does |
| --- | --- |
| `--camera X,Y,Z,YAW_DEG,PITCH_DEG` | Places the free camera: meters and degrees, as the stats panel shows it (F1). Positions stay in doubles from the text to the entity |
| `--view NAME`, `--views-file PATH` | The same from a named view; the default file is `camera.views` in `config/` (one a line: a name and the five numbers) |
| `--still PATH` | A settled, repeatable picture and exit: `--screenshot` at the settle frame, the fixed step, no vsync |
| `--screenshot PATH`, `--screenshot-frame N` | The frame's final image as a PNG at frame N (the first is 1; by default the settle frame), then exit, unless `--frames` says go on |
| `--settle N` | How many frames settling takes, if not the 96 measured below |
| `--fixed-step S` | The frame clock advances by S seconds a frame, whatever time passes. A still or a path gets 1/60 unless this says otherwise |
| `--camera-path PATH`, `--path-speed M/S` | Flies a path file: holds at its start for the settle frames, flies its waypoints (straight lines, at 13 m/s, or its `speed` line, or this flag), exits at its end. No vsync |
| `--path-shots PREFIX`, `--path-every N` | A picture every N frames (default 30) from where the flight starts, as `PREFIX_0000.png`, `_0001` and so on |

Paths are VFS paths, so lowercase. `src:/build/eyes/...` lands in `build/`, which git ignores: pictures aren't committed, and baselines stay outside the repository, one set per GPU. The log says where each picture went. A flag that makes no sense, or a file or path that can't work, is refused before a window opens. The exit code is failure if a picture wasn't taken or written, if a frame wasn't drawn, or if validation reported errors.

A still, a screenshot or a path makes the run **scripted**:

- **It takes no input.** `Input::sample()` isn't called, so a stray key or a drifting stick can't move what's being pictured. A window can still be closed, which fails the run.
- **It shows no debug UI.** The stats panel's numbers change from run to run. What's read back is still the final image, what the screen shows: after the UI pass, with the swapchain's bytes kept as they are, without alpha.
- **Its first frame starts with the scene on the GPU** (`wait_for_scene`), on frames of its own that present nothing, so how fast the disk was doesn't decide what the first frames drew.
- **A frame not drawn fails it** (the window minimized): the history its pictures rest on would be gone.

With no flags none of this costs anything but a few branches a frame. The swapchain gets `CopySrc` only when pictures are asked for (`DeviceDesc::swapchain_copy_src`), and the readback buffer is made at the first picture. A picture waits for the GPU and encodes on the main thread, on that frame alone.

### The settle rule: 96 frames

TAA keeps 92% of its history, and the froxels' light 90%. Measured in the test world at 1600 x 900 on an RTX 4080 Laptop, world time held still with `--fixed-step 0.000000001` so only the history was changing, at whole periods of the 16-sample jitter, against the same view at frame 2048 (worst of `camp_south`, `statue_close`, `in_the_mist`):

| Frames | Pixels off by over 1 level | Largest difference | Mean difference |
| --- | --- | --- | --- |
| 16 | 140,580 | 27 | 0.559 |
| 32 | 5,852 | 8 | 0.200 |
| 48 | 9 | 3 | 0.064 |
| 64 | 1 | 3 | 0.018 |
| 80 | 0 | 1 | 0.007 |
| 96 | 0 | 1 | 0.004 |
| 128 and on | 0 | 1 | 0.004 |

96 is the first count where no view is over a level from frame 2048 or from the count after it, and the mean stops falling after that. It's 1.6 s of the fixed step. This is one small world: thin foliage may want more, so measure again when there is some, and a change of the number changes every baseline.

With time running, a picture never stops changing where something drifts: 16 frames later `camp_south` has 884 pixels off by over 8, up to 14, all in the mist. So compare stills taken at the same frame count.

### How repeatable a still is

On that GPU and driver (NVIDIA 617.14), every comparison of the same view at the settle frame and step was identical, not a pixel off, and so were a flown path's pictures. Far past the settle frame that isn't proven for every view: see the end of this section.

| Compared | Pixels differing |
| --- | --- |
| `camp_south` at 1600 x 900, 6 runs | 0 of 1,440,000 |
| `camp_south` at 2560 x 1440 with `--no-validation`, 5 runs | 0 of 3,686,400 |
| `statue_close`, `in_the_mist` and `far_corner`, 3 runs each | 0 of 1,440,000 |
| Against the Debug build, against validation off, against vsync on | 0 of 1,440,000 |
| `config/camp_ride.path`, 8 pictures, 2 runs | 0 of 1,440,000 |

And what a real change does to it, from `camp_south`, pixels off by over 0, 2 and 8 levels:

| Change | Over 0 | Over 2 | Over 8 | Largest | Mean |
| --- | --- | --- | --- | --- | --- |
| The camera 1 mm to the side | 597,201 | 145,280 | 7,891 | 112 | 0.857 |
| Yaw by 0.01 degrees | 662,464 | 144,509 | 5,736 | 112 | 0.892 |
| The camera 1 cm to the side | 775,856 | 578,932 | 335,670 | 147 | 6.230 |
| The camera 1 m forward | 898,513 | 817,287 | 664,803 | 196 | 17.24 |
| Another view | 1,420,086 | 1,166,994 | 907,307 | 198 | 37.18 |
| The real clock, against itself | 239,573 | 122,788 | 34,500 | 38 | 0.726 |

So on one GPU and driver the threshold to use is none: `image_diff`'s defaults. A picture from another GPU, or another driver, isn't comparable with these (llvmpipe differs slightly from an RTX): take new baselines there.

**Unexplained: long stills of `camp_south`.** Its stills at frames 384, 512 and 2048, with time held still, were taken again about 75 minutes later on a rebuilt engine, and 5,745, 5,647 and 5,661 of the 1,440,000 pixels came back one level off. At 512, the only one looked at, they all lie along rounding contours in the sky. `statue_close` and `in_the_mist` at 2048 came back identical across the same rebuild. `camp_south` wasn't repeated on one binary, so this is either that rebuild or a real difference between runs. Until it is, don't expect a long `camp_south` still to match exactly; `--threshold 1` passes all three. It doesn't touch the settle rule, which counts only pixels off by over 1 level.

## Not yet

- The game as an application (M1); streaming the world's cells around the player instead of loading all of them.
- Modes of the one executable: `--editor`, `--headless`.
