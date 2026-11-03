# render/rhi

The render hardware interface: the only door to the graphics API (docs/design.md, Platforms & RHI). Everything above it (the render graph, passes, materials) uses these types only. Vulkan 1.3 is the one backend, in `vulkan/`.

## Contents

| File | What it gives |
| --- | --- |
| `rhi.h` | Formats and `format_info`, texture, buffer and pipeline descriptions, `ResourceState`, barriers, attachments, `Capabilities`, the generational `TextureHandle` / `BufferHandle` / `PipelineHandle` |
| `device.h` | `Device`: creation (windowed or headless), resources, deferred destruction, frames, swapchain, stats |
| `command_list.h` | `CommandList`: barriers, dynamic rendering, pipelines, push constants, draws, dispatches, copies, debug labels; `BarrierBatch`, `ScopedDebugLabel` |
| `vulkan/` | The backend: volk, VMA, vk-bootstrap. Nothing outside this folder includes it. The window surface comes from `platform::create_vulkan_surface`, so no SDL here |

## How a frame runs

```cpp
device.begin_frame();                                    // waits for the frame kFramesInFlight ago, frees what it destroyed
TextureHandle backbuffer = device.acquire_swapchain_texture(); // invalid while minimized
CommandList list = device.begin_command_list();
list.barrier({.texture = backbuffer, .before = ResourceState::Present, .after = ResourceState::RenderTarget, .discard = true});
list.begin_rendering({.colors = std::span(&color, 1)});
list.end_rendering();
list.barrier(backbuffer, ResourceState::RenderTarget, ResourceState::Present);
device.end_frame();                                      // one submit for all lists, then present
```

## Rules and invariants

- No Vulkan type, enum or header above `vulkan/`. The public headers name no graphics API; a D3D12 or console backend would implement the same headers in its own folder, picked by CMake (no virtual calls).
- One `Device` per process, used from one thread (the render thread). Command lists are recorded on that thread too, for now.
- Clip space is +Y up with depth 0..1 on every backend; the Vulkan backend flips the viewport. Depth is reverse Z: clear to 0 (far), test greater.
- Barriers name states (`RenderTarget`, `ShaderRead`, `CopyDst`...), not pipeline stages or layouts. The render graph will emit them from what passes declare.
  - States are flags: read states combine (`IndirectArgument | ShaderRead`), a write state stands alone; textures combine only reads that share a layout (`DepthRead | ShaderRead`). `is_valid_state()` checks, and barriers assert it.
  - Shader states name the stages that use them (`ShaderStages::Compute`, `Fragment`...), so unrelated work overlaps the barrier. The default is every stage: correct, wider than needed.
  - `Undefined` means nothing on the GPU still uses the resource (just created, or its frame was waited for); it waits for nothing. A resource that may still be in flight names its real previous state, with `discard` for a texture whose contents will be overwritten: the barrier waits for the previous use but skips preserving the contents. The swapchain goes `Present` -> `RenderTarget` with discard every frame.
  - Buffer barriers can cover a range; a `GlobalBarrier` orders every buffer access at once, cheaper than many buffer barriers in GPU-driven passes.
  - One barrier command per `barriers()` call. `BarrierBatch` collects them on the stack and flushes them as one; the render graph will flush one batch per pass boundary.
  - Synchronization validation runs in the tests: a barrier that is too weak shows up as a `SYNC-HAZARD` error.
- Destroying a resource invalidates its handle at once; the GPU memory is freed when the frames in flight that may use it are done (`kFramesInFlight` = 2). Using a stale handle is a `DE_VERIFY` failure in every build.
- Frame pacing uses one timeline semaphore: frame N signals N. Swapchain present semaphores are per image, acquire semaphores per frame slot.
- The swapchain is recreated on resize, vsync change, or when the driver reports it out of date; suboptimal triggers one rebuild, not one per frame. A lost surface is recreated. Recreation waits for the GPU to go idle (rare, keeps it simple). Present semaphores live as long as the device, since nothing tells when a retired swapchain's present stopped using them.
- Swapchain formats are sRGB, so shaders write linear color; a window without one gets a warning in the log.
- Frames present from the graphics queue; a GPU whose graphics queue can't present to the window is rejected.
- Required: Vulkan 1.3 with dynamic rendering, cube map arrays, storage images without a declared format, synchronization2, maintenance4, shader demote to helper invocation (Slang's discard), descriptor indexing (update-after-bind, partially bound, variable count, non-uniform indexing), buffer device address, draw indirect count, timeline semaphores, scalar block layout, BC textures, 64-bit shader integers. Optional, reported in `Capabilities`: ray query (with acceleration structures), memory budget, a separate async compute queue, a dedicated transfer queue.
- Every buffer a shader can read gets a device address (`Device::buffer_address`); shaders reach buffers through pointers in push constants.
- Bindless textures and samplers: one descriptor set, bound once per command list, with a table per texture shape (`BindlessTable`: 2D, 2D array, cube, cube array, 3D sampled; 2D, 2D array, 3D float storage; 2D integer storage) plus samplers and comparison samplers (`shaders/common/bindless.slang`; a test checks both sides agree).
  - A texture gets its indices when it's created: `texture_index`, and `storage_texture_index(mip)` per mip. An index carries its table in its top 8 bits; shaders reach textures through accessors (`texture_2d(index)`...) that send an index of another table, or past the end, to entry 0. One compare, in every build.
  - Every entry of every table is always valid: entry 0 is the table's fallback (magenta for sampled tables, a scratch texel for storage), and every entry not in use points at it, freed ones included. A mistagged or garbage index reads the fallback, never another texture or freed memory, and a texture read as another shape lands on that shape's fallback (entries are unique across shapes).
  - Indices are freed with the texture's memory, once the GPU is done with it. Never-used entries are handed out first and freed ones reused oldest first, so a stale index reads the fallback until its kind of table has been used all the way round (16384 or 4096 textures later). A warning is logged when a kind of table is 80% full.
  - Depth-stencil textures are sampled through a depth-only view. Integer storage is R32Uint 2D only; integer textures can't be sampled.
  - The default samplers (`DefaultSampler`) sit at fixed indices: linear, point and anisotropic in wrap and clamp, and a reverse-Z shadow comparison sampler.
  - Fixed capacities (16384 sampled, 4096 storage entries per table, 64 samplers), so shaders know them; the spec guarantees far more on every GPU we accept.
  - What validation can't see: a texture sampled in the wrong state. Neither the CPU-side layer nor GPU-assisted validation checks image layouts inside partially bound tables. That correctness comes from structure: uploads leave textures in `ShaderRead`, and the render graph emits the barriers for every texture a pass declares.
- `DeviceDesc::gpu_validation` turns on GPU-assisted validation: shaders check their pointer accesses (out of bounds), descriptor indexing and more as they run. Very slow; a test proves it catches an out-of-bounds pointer write.
- One pipeline layout for every pipeline: 128 bytes of push constants visible to all stages, no per-pipeline descriptor layouts. Pipelines take compiled shader code (from `render/shader`), attachment formats and fixed state; viewport and scissor are dynamic. No vertex input layouts: vertices are pulled from buffers by address.
- Validation is on by default in debug and dev builds when the layer is installed (Vulkan SDK). Its messages go to the log under `vulkan` and are counted in `DeviceStats`; the tests require zero, with synchronization validation on. `deengine --frames N` fails when validation reported errors.
- Object names and command-list labels reach RenderDoc and Nsight whenever `VK_EXT_debug_utils` is available, in every build.

## Not yet

- Async compute and transfer submission with cross-queue sync (render graph), multi-threaded command recording, per-mip and per-layer views, stencil copies.
- Driver and VMA CPU-side allocations don't go through our tagged heaps yet (`VkAllocationCallbacks`).
- GPU timestamps and Tracy GPU zones.
