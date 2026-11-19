# render/graph

The frame graph (docs/design.md, Rendering: render graph v1): each frame's passes, the resources they use, and the barriers between them.

## Contents

| File | What it gives |
| --- | --- |
| `render_graph.h` | `RenderGraph` (begin, import, add_pass, execute, stats, gpu_times, dump; for debug views `pass_info`, `pass_use`, `texture_info`, `buffer_info` and `set_pass_hook`), `PassBuilder` (create and use resources), `PassContext` (the pass's command list and its resources), `GraphTexture`, `GraphBuffer` |

## Rules and invariants

- **One frame:** `begin()` after `device.begin_frame()`, passes added, `execute()` before `device.end_frame()`. Everything is recorded on the graph's own graphics command list.
- **Passes declare, the graph transitions:** a pass's setup creates and uses resources with a state (`RenderTarget`, `ShaderRead`...); the graph records the barriers between passes from those declarations, one batch per pass. Passes never record their own barriers on graph resources.
- **Culling:** a pass runs only if it writes something an imported resource or a later running pass uses, or `never_cull()` says so; every earlier writer of such a resource runs too (v1 can't tell a full overwrite from a load).
- **Transients** are pooled by description across frames and returned to the device after `keep_unused_frames` unused frames. A transient the device can't make skips the passes that use it (counted, logged) instead of crashing.
- **Imports:** importing the same texture or buffer twice in a frame gives one handle; imported resources come in from their stated state and leave in their final one.
- **Pass data and closures** live in the graph's frame arena and are never destroyed: they hold graph handles, pointers and values, never containers or references to the stack.
- **GPU time per pass** (`RenderGraphDesc::gpu_timing`, on in dev builds): each pass, its barriers included, between two GPU timestamps; `gpu_times()` gives the latest frame the GPU finished (the device's `kFramesInFlight` frames back) and stays as it was over frames the graph didn't time. Each pass is a Tracy plot (`gpu ms: <pass>`, and `gpu frame ms`), so `tools/profile.ps1` prints them.
- **Debug views** read the frame's graph between `execute()` and the next `begin()`: every pass in the order it was added (culled ones too), what it used and in which state, what its execute recorded (`rhi::ListWork`: draws, indirect calls, dispatches, copies), and the frame's textures and buffers. The names point into the frame's memory and go with it. `set_pass_hook()` is called at the end of every `add_pass`, so a debug view can add a pass of its own right after the one it watches, which reads a texture as that pass left it; the hook isn't called for the passes it adds. The frame debugger (`tools/frame_debugger.h`) is built on both.
- **Immediate mode** (dev builds, for bisecting graph bugs) runs each pass as it is added: no culling, transients made with every usage their format allows.
- Single-threaded, move-only; the device must outlive it.

## Not yet

Render graph v2 and v3 (docs/design.md, Roadmap): transient memory aliasing with a lifetime and heap view, a `discard` declaration on uses, parallel pass recording, async compute passes, barrier grouping and pass reordering.
