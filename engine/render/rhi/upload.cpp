#include "render/rhi/upload.h"

#include "core/allocator.h"
#include "core/array.h"
#include "core/hash_map.h"
#include "core/log.h"
#include "core/profile.h"
#include "render/rhi/device.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace de::rhi {

namespace {

core::Allocator& render_heap() {
    return core::heap_allocator(core::MemoryTag::Render);
}

// Every mip and layer starts at a multiple of this in staging: whole texel blocks, and the 4 bytes
// depth copies need.
constexpr uint64_t kSubresourceAlignment = 16;
constexpr uint64_t kNotRecorded = UINT64_MAX;
constexpr uint32_t kMaxTrackedMips = 16;
// The ring's region list and the upload queue are compacted once this many entries are done and
// they are at least half of the list, so constant streaming doesn't grow them forever.
constexpr size_t kCompactAfter = 64;

uint32_t mip_extent(uint32_t size, uint32_t mip) {
    return std::max(size >> mip, 1U);
}

// Bytes of one layer of one mip, tightly packed in whole blocks.
uint64_t mip_layer_bytes(const TextureDesc& desc, uint32_t mip) {
    const FormatInfo& info = format_info(desc.format);
    const uint64_t blocks_x = (mip_extent(desc.width, mip) + info.block_width - 1) / info.block_width;
    const uint64_t blocks_y = (mip_extent(desc.height, mip) + info.block_height - 1) / info.block_height;
    const uint64_t depth = desc.dimension == TextureDimension::Texture3D ? mip_extent(desc.depth, mip) : 1;
    return blocks_x * blocks_y * depth * info.block_bytes;
}

uint32_t resolved_mip_count(const TextureDesc& desc, const TextureUpload& upload) {
    return upload.mip_count == kAllMips ? desc.mip_count - std::min(upload.first_mip, desc.mip_count)
                                        : upload.mip_count;
}

// One upload, waiting for record(): its data already sits in staging.
struct PendingUpload {
    bool texture = true;
    TextureHandle texture_handle{};
    BufferHandle buffer_handle{};
    TextureUpload range{};      // textures
    uint64_t buffer_offset = 0; // buffers
    ResourceState after = ResourceState::Undefined;
    ShaderStages after_stages = ShaderStages::All;
    BufferHandle staging{}; // the ring, or a dedicated buffer
    uint64_t staging_offset = 0;
    uint64_t bytes = 0;
    uint64_t region = kNotRecorded; // its ring region's sequence number; kNotRecorded for dedicated staging
    bool dropped = false;           // its resource was destroyed before the copy was recorded
};

// A stretch of the ring, from allocation until the frame that copied from it is done.
struct RingRegion {
    uint64_t end = 0;              // where the ring's tail moves when it is freed
    uint64_t frame = kNotRecorded; // the frame that copied from it
};

struct MipStates {
    ResourceState state[kMaxTrackedMips] = {};
    ShaderStages stages[kMaxTrackedMips] = {};
    uint64_t batch = 0; // the last record batch with an upload to it
};

struct BufferState {
    ResourceState state = ResourceState::Undefined;
    ShaderStages stages = ShaderStages::All;
    uint64_t batch = 0;
};

// Moves the live entries [head, size) to the front once enough are done.
template <typename T>
bool compact(core::Array<T>& items, size_t& head) {
    if (head < kCompactAfter || head * 2 < items.size()) {
        return false;
    }
    for (size_t i = head; i < items.size(); ++i) {
        items[i - head] = items[i];
    }
    items.resize(items.size() - head);
    head = 0;
    return true;
}

} // namespace

struct UploaderState {
    Device* device = nullptr;
    UploaderDesc desc;
    BufferHandle ring;
    std::byte* ring_memory = nullptr;
    uint64_t head = 0;                              // next free byte
    uint64_t tail = 0;                              // oldest byte the GPU may still read
    core::Array<RingRegion> regions{render_heap()}; // a queue: [region_head, size)
    size_t region_head = 0;
    uint64_t region_base = 0; // the sequence number of regions[0]

    core::Array<PendingUpload> queue{render_heap()}; // a queue: [queue_head, size)
    size_t queue_head = 0;

    core::HashMap<TextureHandle, MipStates> texture_states{render_heap()};
    core::HashMap<BufferHandle, BufferState> buffer_states{render_heap()};
    size_t purge_at = 256;

    uint64_t batch = 0; // record batches so far

    uint64_t queued_bytes = 0;
    uint64_t uploaded_bytes = 0;
    uint32_t dedicated = 0;
    uint32_t dropped = 0;
};

namespace {

// Frees ring regions whose frame is done.
void reclaim(UploaderState& state) {
    const uint64_t completed = state.device->completed_frame();
    while (state.region_head < state.regions.size() && state.regions[state.region_head].frame <= completed) {
        state.tail = state.regions[state.region_head].end;
        ++state.region_head;
    }
    if (state.region_head == state.regions.size()) {
        // Empty: start over at the beginning, keeping the array's memory.
        state.region_base += state.regions.size();
        state.regions.clear();
        state.region_head = 0;
        state.head = 0;
        state.tail = 0;
        return;
    }
    const size_t done = state.region_head;
    if (compact(state.regions, state.region_head)) {
        state.region_base += done;
    }
}

// A stretch of size bytes in the ring, or false if it doesn't fit right now. head never catches up
// with tail from behind (hence the strict compares), so head == tail always means empty.
bool ring_allocate(UploaderState& state, uint64_t size, uint64_t& offset) {
    const uint64_t capacity = state.desc.staging_bytes;
    const bool empty = state.region_head == state.regions.size();
    const uint64_t aligned = core::align_up(state.head, kSubresourceAlignment);
    if (empty || state.head >= state.tail) {
        // Free space: [head, capacity) and, wrapping, [0, tail).
        if (aligned + size <= capacity) {
            offset = aligned; // (when empty, head is 0: anything up to capacity fits here)
        } else if (size < state.tail) {
            offset = 0;
        } else {
            return false;
        }
    } else if (aligned + size < state.tail) {
        offset = aligned; // free space: [head, tail)
    } else {
        return false;
    }
    state.head = offset + size;
    state.regions.push(RingRegion{.end = state.head});
    return true;
}

// Where an upload's bytes go: a ring stretch, or a buffer of their own if the ring can't hold them
// now. The returned span is mapped staging memory; empty if there's no memory for it.
std::span<std::byte> stage(UploaderState& state, uint64_t bytes, PendingUpload& upload) {
    reclaim(state);
    uint64_t offset = 0;
    if (bytes <= state.desc.staging_bytes && ring_allocate(state, bytes, offset)) {
        upload.staging = state.ring;
        upload.staging_offset = offset;
        upload.region = state.region_base + state.regions.size() - 1;
        return {state.ring_memory + offset, static_cast<size_t>(bytes)};
    }
    const auto dedicated = state.device->create_buffer({.size = bytes,
                                                        .usage = BufferUsage::CopySrc,
                                                        .memory = MemoryLocation::Upload,
                                                        .debug_name = "upload staging"});
    if (!dedicated) {
        DE_LOG_ERROR("render", "no memory for %llu bytes of upload staging", static_cast<unsigned long long>(bytes));
        return {};
    }
    ++state.dedicated;
    upload.staging = *dedicated;
    upload.staging_offset = 0;
    upload.region = kNotRecorded;
    return state.device->mapped(*dedicated).first(static_cast<size_t>(bytes));
}

// Drops tracked states of destroyed resources, now and then.
void purge_states(UploaderState& state) {
    if (state.texture_states.size() + state.buffer_states.size() < state.purge_at) {
        return;
    }
    core::Array<TextureHandle> dead_textures(render_heap());
    for (const auto& entry : state.texture_states) {
        if (state.device->texture_desc(entry.key) == nullptr) {
            dead_textures.push(entry.key);
        }
    }
    for (const TextureHandle texture : dead_textures) {
        state.texture_states.remove(texture);
    }
    core::Array<BufferHandle> dead_buffers(render_heap());
    for (const auto& entry : state.buffer_states) {
        if (state.device->buffer_desc(entry.key) == nullptr) {
            dead_buffers.push(entry.key);
        }
    }
    for (const BufferHandle buffer : dead_buffers) {
        state.buffer_states.remove(buffer);
    }
    state.purge_at = std::max<size_t>(256, 2 * (state.texture_states.size() + state.buffer_states.size()));
}

} // namespace

uint64_t texture_upload_size(const TextureDesc& desc, const TextureUpload& upload) {
    if (upload.first_mip >= desc.mip_count) {
        return 0;
    }
    const uint32_t mips = resolved_mip_count(desc, upload);
    if (mips == 0 || upload.first_mip + mips > desc.mip_count) {
        return 0;
    }
    uint64_t bytes = 0;
    for (uint32_t mip = upload.first_mip; mip < upload.first_mip + mips; ++mip) {
        bytes += mip_layer_bytes(desc, mip) * desc.layer_count;
    }
    return bytes;
}

core::Result<Uploader> Uploader::create(Device& device, const UploaderDesc& desc) {
    if (desc.staging_bytes < 1024 || desc.frame_budget_bytes == 0) {
        return core::fail(core::ErrorCode::InvalidArgument, "the uploader needs a staging ring and a frame budget");
    }
    const auto ring = device.create_buffer({.size = desc.staging_bytes,
                                            .usage = BufferUsage::CopySrc,
                                            .memory = MemoryLocation::Upload,
                                            .debug_name = "upload staging ring"});
    if (!ring) {
        return core::fail(ring.error());
    }
    auto* state = ::new (render_heap().allocate(sizeof(UploaderState), alignof(UploaderState))) UploaderState();
    state->device = &device;
    state->desc = desc;
    state->ring = *ring;
    state->ring_memory = device.mapped(*ring).data();
    return Uploader(state);
}

Uploader::~Uploader() {
    if (state_ == nullptr) {
        return;
    }
    // Queued uploads that never reached the GPU go with their staging.
    for (size_t i = state_->queue_head; i < state_->queue.size(); ++i) {
        if (state_->queue[i].staging != state_->ring) {
            state_->device->destroy(state_->queue[i].staging);
        }
    }
    state_->device->destroy(state_->ring); // freed once the GPU is done with it
    state_->~UploaderState();
    render_heap().deallocate(state_, sizeof(UploaderState), alignof(UploaderState));
}

Uploader::Uploader(Uploader&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}

Uploader& Uploader::operator=(Uploader&& other) noexcept {
    if (this != &other) {
        const Uploader discarded(std::exchange(state_, std::exchange(other.state_, nullptr)));
    }
    return *this;
}

core::Result<void> Uploader::upload_texture(TextureHandle texture, std::span<const std::byte> data,
                                            const TextureUpload& upload) {
    UploaderState& state = *state_;
    const TextureDesc* desc = state.device->texture_desc(texture);
    if (desc == nullptr) {
        return core::fail(core::ErrorCode::InvalidArgument, "upload to a stale texture handle");
    }
    if (!core::has_any(desc->usage, TextureUsage::CopyDst)) {
        return core::fail(core::ErrorCode::InvalidArgument, "the texture needs CopyDst usage to be uploaded to");
    }
    if (format_info(desc->format).stencil) {
        return core::fail(core::ErrorCode::Unsupported, "uploads to depth-stencil formats aren't supported");
    }
    if (desc->mip_count > kMaxTrackedMips) {
        return core::fail(core::ErrorCode::Unsupported, "uploads go to textures of at most 16 mips");
    }
    if (upload.after == ResourceState::Undefined || !is_valid_state(upload.after, true)) {
        return core::fail(core::ErrorCode::InvalidArgument, "an upload must leave the texture in a valid state");
    }
    const uint64_t expected = texture_upload_size(*desc, upload);
    if (expected == 0) {
        return core::fail(core::ErrorCode::OutOfRange, "the upload's mips are outside the texture");
    }
    if (data.size() != expected) {
        DE_LOG_ERROR("render", "texture upload: %zu bytes given, %llu expected for those mips (%ux%u %s)", data.size(),
                     static_cast<unsigned long long>(expected), desc->width, desc->height,
                     format_info(desc->format).name);
        return core::fail(core::ErrorCode::InvalidArgument, "the upload's data size doesn't match the texture");
    }

    // Staging holds each mip of each layer at an aligned offset.
    const uint32_t mips = resolved_mip_count(*desc, upload);
    uint64_t staged_bytes = 0;
    for (uint32_t mip = upload.first_mip; mip < upload.first_mip + mips; ++mip) {
        for (uint32_t layer = 0; layer < desc->layer_count; ++layer) {
            staged_bytes = core::align_up(staged_bytes, kSubresourceAlignment) + mip_layer_bytes(*desc, mip);
        }
    }
    PendingUpload pending{.texture = true,
                          .texture_handle = texture,
                          .range = {.first_mip = upload.first_mip, .mip_count = mips},
                          .after = upload.after,
                          .after_stages = upload.after_stages,
                          .bytes = staged_bytes};
    const std::span<std::byte> staging = stage(state, staged_bytes, pending);
    if (staging.empty()) {
        return core::fail(core::ErrorCode::OutOfMemory, "no memory for the upload's staging");
    }
    uint64_t source = 0;
    uint64_t target = 0;
    for (uint32_t mip = upload.first_mip; mip < upload.first_mip + mips; ++mip) {
        const uint64_t layer_bytes = mip_layer_bytes(*desc, mip);
        for (uint32_t layer = 0; layer < desc->layer_count; ++layer) {
            target = core::align_up(target, kSubresourceAlignment);
            std::memcpy(staging.data() + target, data.data() + source, static_cast<size_t>(layer_bytes));
            source += layer_bytes;
            target += layer_bytes;
        }
    }
    state.device->flush_mapped(pending.staging, pending.staging_offset, staged_bytes);
    state.queued_bytes += staged_bytes;
    state.queue.push(pending);
    return {};
}

core::Result<void> Uploader::upload_buffer(BufferHandle buffer, uint64_t offset, std::span<const std::byte> data,
                                           ResourceState after, ShaderStages after_stages) {
    UploaderState& state = *state_;
    const BufferDesc* desc = state.device->buffer_desc(buffer);
    if (desc == nullptr) {
        return core::fail(core::ErrorCode::InvalidArgument, "upload to a stale buffer handle");
    }
    if (!core::has_any(desc->usage, BufferUsage::CopyDst)) {
        return core::fail(core::ErrorCode::InvalidArgument, "the buffer needs CopyDst usage to be uploaded to");
    }
    if (data.empty() || offset > desc->size || data.size() > desc->size - offset) {
        return core::fail(core::ErrorCode::OutOfRange, "the upload doesn't fit in the buffer");
    }
    if (after == ResourceState::Undefined || !is_valid_state(after, false)) {
        return core::fail(core::ErrorCode::InvalidArgument, "an upload must leave the buffer in a valid state");
    }
    PendingUpload pending{.texture = false,
                          .buffer_handle = buffer,
                          .buffer_offset = offset,
                          .after = after,
                          .after_stages = after_stages,
                          .bytes = data.size()};
    const std::span<std::byte> staging = stage(state, data.size(), pending);
    if (staging.empty()) {
        return core::fail(core::ErrorCode::OutOfMemory, "no memory for the upload's staging");
    }
    std::memcpy(staging.data(), data.data(), data.size());
    state.device->flush_mapped(pending.staging, pending.staging_offset, data.size());
    state.queued_bytes += data.size();
    state.queue.push(pending);
    return {};
}

void Uploader::record(CommandList& list) {
    UploaderState& state = *state_;
    if (state.queue_head == state.queue.size()) {
        return;
    }
    DE_PROFILE_ZONE("uploader record");
    reclaim(state);
    purge_states(state);

    // This frame's batch: FIFO, within the budget, and each resource at most once (barriers in one
    // command are unordered, so a second upload to the same resource waits for the next batch).
    // Uploads to resources destroyed since they were queued are dropped.
    const uint64_t batch = ++state.batch;
    const size_t first = state.queue_head;
    size_t last = first;
    uint64_t batch_bytes = 0;
    while (last < state.queue.size()) {
        PendingUpload& upload = state.queue[last];
        upload.dropped = upload.texture ? state.device->texture_desc(upload.texture_handle) == nullptr
                                        : state.device->buffer_desc(upload.buffer_handle) == nullptr;
        if (!upload.dropped) {
            if (last > first && batch_bytes + upload.bytes > state.desc.frame_budget_bytes) {
                break;
            }
            uint64_t& stamp = upload.texture ? state.texture_states.find_or_insert(upload.texture_handle).batch
                                             : state.buffer_states.find_or_insert(upload.buffer_handle).batch;
            if (stamp == batch) {
                break; // a second upload to this resource
            }
            stamp = batch;
            batch_bytes += upload.bytes;
        }
        ++last;
    }

    const ScopedDebugLabel label(list, "uploads");
    BarrierBatch to_copy;
    BarrierBatch to_use;
    for (size_t i = first; i < last; ++i) {
        const PendingUpload& upload = state.queue[i];
        if (upload.dropped) {
            continue;
        }
        if (upload.texture) {
            const TextureDesc& desc = *state.device->texture_desc(upload.texture_handle);
            const MipStates& mips = state.texture_states.find_or_insert(upload.texture_handle);
            const bool whole = upload.range.first_mip == 0 && upload.range.mip_count == desc.mip_count;
            for (uint32_t mip = upload.range.first_mip; mip < upload.range.first_mip + upload.range.mip_count; ++mip) {
                // One mip at a time: each may come from a different state.
                to_copy.add(TextureBarrier{.texture = upload.texture_handle,
                                           .before = mips.state[mip],
                                           .after = ResourceState::CopyDst,
                                           .before_stages = mips.stages[mip],
                                           .range = {.first_mip = mip, .mip_count = 1},
                                           .discard = whole});
                to_use.add(TextureBarrier{.texture = upload.texture_handle,
                                          .before = ResourceState::CopyDst,
                                          .after = upload.after,
                                          .after_stages = upload.after_stages,
                                          .range = {.first_mip = mip, .mip_count = 1}});
            }
        } else {
            const BufferState& buffer = state.buffer_states.find_or_insert(upload.buffer_handle);
            to_copy.add(BufferBarrier{.buffer = upload.buffer_handle,
                                      .before = buffer.state,
                                      .after = ResourceState::CopyDst,
                                      .before_stages = buffer.stages,
                                      .offset = upload.buffer_offset,
                                      .size = upload.bytes});
            to_use.add(BufferBarrier{.buffer = upload.buffer_handle,
                                     .before = ResourceState::CopyDst,
                                     .after = upload.after,
                                     .after_stages = upload.after_stages,
                                     .offset = upload.buffer_offset,
                                     .size = upload.bytes});
        }
    }
    to_copy.flush(list);

    const uint64_t frame = state.device->frame_index();
    for (size_t i = first; i < last; ++i) {
        const PendingUpload& upload = state.queue[i];
        if (upload.dropped) {
            ++state.dropped;
        } else if (upload.texture) {
            const TextureDesc& desc = *state.device->texture_desc(upload.texture_handle);
            uint64_t offset = upload.staging_offset;
            for (uint32_t mip = upload.range.first_mip; mip < upload.range.first_mip + upload.range.mip_count; ++mip) {
                for (uint32_t layer = 0; layer < desc.layer_count; ++layer) {
                    offset =
                        upload.staging_offset + core::align_up(offset - upload.staging_offset, kSubresourceAlignment);
                    list.copy_buffer_to_texture(upload.staging, upload.texture_handle,
                                                {.buffer_offset = offset, .mip = mip, .layer = layer});
                    offset += mip_layer_bytes(desc, mip);
                }
            }
            MipStates& mips = *state.texture_states.find(upload.texture_handle);
            for (uint32_t mip = upload.range.first_mip; mip < upload.range.first_mip + upload.range.mip_count; ++mip) {
                mips.state[mip] = upload.after;
                mips.stages[mip] = upload.after_stages;
            }
        } else {
            list.copy_buffer(upload.staging, upload.staging_offset, upload.buffer_handle, upload.buffer_offset,
                             upload.bytes);
            // Tracked per buffer, not per range: read states of several ranges add up, so the next
            // upload waits for every reader; a write state replaces them.
            BufferState& tracked = *state.buffer_states.find(upload.buffer_handle);
            const ResourceState combined = tracked.state | upload.after;
            if (tracked.state != ResourceState::Undefined && is_valid_state(combined, false)) {
                tracked = {.state = combined, .stages = tracked.stages | upload.after_stages};
            } else {
                tracked = {.state = upload.after, .stages = upload.after_stages};
            }
        }
        // The staging can go once this frame is done on the GPU.
        if (upload.region != kNotRecorded) {
            state.regions[upload.region - state.region_base].frame = frame;
        } else {
            state.device->destroy(upload.staging);
        }
        state.queued_bytes -= upload.bytes;
        state.uploaded_bytes += upload.dropped ? 0 : upload.bytes;
    }
    to_use.flush(list);

    state.queue_head = last;
    if (state.queue_head == state.queue.size()) {
        state.queue.clear(); // drained: start over, keeping the memory
        state.queue_head = 0;
    } else {
        compact(state.queue, state.queue_head);
    }
}

void Uploader::flush_and_wait() {
    DE_PROFILE_ZONE("uploader flush");
    Device& device = *state_->device;
    DE_ASSERT_MSG(!device.in_frame(), "flush_and_wait runs its own frames: call it outside begin_frame/end_frame");
    while (!idle()) {
        device.begin_frame();
        CommandList list = device.begin_command_list();
        record(list);
        device.end_frame();
    }
    device.wait_idle();
}

bool Uploader::idle() const {
    return state_->queue_head == state_->queue.size();
}

UploaderStats Uploader::stats() const {
    const UploaderState& state = *state_;
    uint64_t in_use = 0;
    if (state.region_head < state.regions.size()) {
        in_use =
            state.head >= state.tail ? state.head - state.tail : state.desc.staging_bytes - state.tail + state.head;
    }
    return {
        .queued_uploads = static_cast<uint32_t>(state.queue.size() - state.queue_head),
        .queued_bytes = state.queued_bytes,
        .staging_bytes_in_use = in_use,
        .staging_bytes = state.desc.staging_bytes,
        .uploaded_bytes = state.uploaded_bytes,
        .dedicated_staging = state.dedicated,
        .dropped_uploads = state.dropped,
    };
}

} // namespace de::rhi
