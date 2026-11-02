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
- Required: Vulkan 1.3 with dynamic rendering, cube map arrays, synchronization2, maintenance4, descriptor indexing (update-after-bind, partially bound, variable count, non-uniform indexing), buffer device address, draw indirect count, timeline semaphores, scalar block layout, BC textures, 64-bit shader integers. Optional, reported in `Capabilities`: ray query (with acceleration structures), memory budget, a separate async compute queue, a dedicated transfer queue.
- Every buffer a shader can read gets a device address (`Device::buffer_address`); shaders reach buffers through pointers in push constants. Bindless textures arrive in M0a task 9.
- One pipeline layout for every pipeline: 128 bytes of push constants visible to all stages, no per-pipeline descriptor layouts. Pipelines take compiled shader code (from `render/shader`), attachment formats and fixed state; viewport and scissor are dynamic. No vertex input layouts: vertices are pulled from buffers by address.
- Validation is on by default in debug and dev builds when the layer is installed (Vulkan SDK). Its messages go to the log under `vulkan` and are counted in `DeviceStats`; the tests require zero, with synchronization validation on. `deengine --frames N` fails when validation reported errors.
- Object names and command-list labels reach RenderDoc and Nsight whenever `VK_EXT_debug_utils` is available, in every build.

## Not yet

- Async compute and transfer submission with cross-queue sync (render graph), multi-threaded command recording, per-mip and per-layer views, stencil copies.
- Driver and VMA CPU-side allocations don't go through our tagged heaps yet (`VkAllocationCallbacks`).
- GPU timestamps and Tracy GPU zones.
