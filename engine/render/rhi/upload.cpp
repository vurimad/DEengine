#include "render/rhi/upload.h"

#include "core/allocator.h"
#include "core/array.h"
#include "core/hash_map.h"
#include "core/log.h"
#include "core/profile.h"
#include "render/rhi/device.h"
#include "render/rhi/mips.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace de::rhi {

namespace {

core::Allocator& render_heap() {
    return core::heap_allocator(core::MemoryTag::Render);
}

// Every upload and every mip starts at a multiple of this in staging: whole texel blocks, and the
// 4 bytes depth copies need. The layers of a mip follow each other tightly, as one copy reads them.
constexpr uint64_t kStagingAlignment = 16;
constexpr uint64_t kNotRecorded = UINT64_MAX;
constexpr uint32_t kMaxTrackedMips = 16;
// The ring's region list and the upload queue are compacted once this many entries are done and
// they are at least half of the list, so constant streaming doesn't grow them forever.
constexpr size_t kCompactAfter = 64;

// Bytes of one layer of one mip, tightly packed in whole blocks.
uint64_t mip_layer_bytes(const TextureDesc& desc, uint32_t mip) {
    const FormatInfo& info = format_info(desc.format);
    const MipExtent extent = mip_extent(desc, mip);
    const uint64_t blocks_x = (extent.width + info.block_width - 1) / info.block_width;
    const uint64_t blocks_y = (extent.height + info.block_height - 1) / info.block_height;
    return blocks_x * blocks_y * extent.depth * info.block_bytes;
}

uint32_t resolved_mip_count(const TextureDesc& desc, const TextureUpload& upload) {
    return upload.mip_count == kAllMips ? desc.mip_count - std::min(upload.first_mip, desc.mip_count)
                                        : upload.mip_count;
}

// A shader state needs the stages that use it.
bool stages_named(ResourceState state, ShaderStages stages) {
    return !core::has_any(state, kShaderStates) || stages != ShaderStages::None;
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
    // Set while record() runs: the texture's description, or null once the resource is destroyed
    // (the upload is then dropped).
    const TextureDesc* desc = nullptr;
    bool dropped = false;
    bool overwrite = false; // over something uploaded before, which the GPU may still read
    bool generate = false;  // the mips after the uploaded ones are generated on the GPU
};

// A stretch of the ring, from allocation until the frame that copied from it is done.
struct RingRegion {
    uint64_t end = 0;              // where the ring's tail moves when it is freed
    uint64_t frame = kNotRecorded; // the frame that copied from it
};

struct MipStates {
    ResourceState state[kMaxTrackedMips] = {};
    ShaderStages stages[kMaxTrackedMips] = {};
    bool initialized = false; // every mip out of Undefined: the first upload moves them all
    uint64_t batch = 0;       // the last record batch with an upload to it
};

// A buffer's state as a whole, not per range: every read state any upload left it in (another
// range may still be read that way), and the last write state.
struct BufferState {
    ResourceState reads = ResourceState::Undefined;
    ShaderStages read_stages = ShaderStages::None;
    ResourceState write = ResourceState::Undefined;
    ShaderStages write_stages = ShaderStages::None;
    uint64_t batch = 0;
    uint64_t batch_first = 0; // the span of what the batch uploads to it
    uint64_t batch_end = 0;
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
    core::HashMap<BufferHandle, uint32_t> buffer_queued{render_heap()}; // uploads waiting to be recorded
    uint64_t buffer_uploads_queued = 0;                                 // ever: the tickets
    uint64_t buffer_uploads_recorded = 0;
    size_t purge_at = 256;

    uint64_t batch = 0; // record batches so far
    uint64_t recorded_frame = 0;
    bool transfer = false; // copies on the transfer queue, handed over to graphics

    uint64_t queued_bytes = 0;
    uint64_t uploaded_bytes = 0;
    uint32_t dedicated = 0;
    uint32_t dropped = 0;
};

namespace {

// Frees ring regions whose frame is done. Asks the GPU (a driver call), so it runs once per
// record() and when an allocation doesn't fit, not per upload.
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
    const uint64_t aligned = core::align_up(state.head, kStagingAlignment);
    if (empty || state.head >= state.tail) {
        // Free space: [head, capacity) and, wrapping, [0, tail).
        if (aligned + size <= capacity) {
            offset = aligned;
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
    uint64_t offset = 0;
    bool in_ring = false;
    if (bytes <= state.desc.staging_bytes) {
        in_ring = ring_allocate(state, bytes, offset);
        if (!in_ring) {
            reclaim(state); // frames may have finished since the last look
            in_ring = ring_allocate(state, bytes, offset);
        }
    }
    if (in_ring) {
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

// Barriers to CopyDst, one per run of mips in the same state (contents discarded).
void add_mip_runs(const MipStates& mips, TextureHandle texture, uint32_t first, uint32_t end, BarrierBatch& batch) {
    uint32_t run = first;
    for (uint32_t mip = run + 1; mip <= end; ++mip) {
        if (mip == end || mips.state[mip] != mips.state[run] || mips.stages[mip] != mips.stages[run]) {
            batch.add(TextureBarrier{.texture = texture,
                                     .before = mips.state[run],
                                     .after = ResourceState::CopyDst,
                                     .before_stages = mips.stages[run],
                                     .range = {.first_mip = run, .mip_count = mip - run},
                                     .discard = true});
            run = mip;
        }
    }
}

// The barriers that take an upload's target to CopyDst. Every byte of the target is overwritten
// (all layers of the mips, or the buffer range), so old contents are discarded. A texture's first
// upload takes every mip out of Undefined; one that generates mips covers them too (on the
// graphics queue: the transfer queue only touches the uploaded mips, see add_use_barriers).
//
// On the transfer queue they start from Undefined: earlier graphics use is waited for by the
// semaphore between the queues (ListStart::AfterPreviousFrame), and discarded contents need no
// queue handover.
void add_copy_barriers(const UploaderState& state, const PendingUpload& upload, BarrierBatch& batch) {
    const uint32_t uploaded_end = upload.range.first_mip + upload.range.mip_count;
    if (!upload.texture) {
        if (state.transfer) {
            batch.add(BufferBarrier{.buffer = upload.buffer_handle,
                                    .after = ResourceState::CopyDst,
                                    .offset = upload.buffer_offset,
                                    .size = upload.bytes});
            return;
        }
        const BufferState& buffer = *state.buffer_states.find(upload.buffer_handle);
        const bool has_write = buffer.write != ResourceState::Undefined;
        batch.add(BufferBarrier{
            .buffer = upload.buffer_handle,
            .before = has_write ? buffer.write : buffer.reads,
            .after = ResourceState::CopyDst,
            .before_stages = has_write
                                 ? buffer.write_stages
                                 : (buffer.read_stages != ShaderStages::None ? buffer.read_stages : ShaderStages::All),
            .offset = upload.buffer_offset,
            .size = upload.bytes,
        });
        if (has_write && buffer.reads != ResourceState::Undefined) {
            // Readers of other ranges may still run; a write state and read states can't share one
            // buffer barrier, so the reads are waited for by a global one (rare: a buffer some
            // uploads leave written and others read).
            batch.add(GlobalBarrier{
                .before = buffer.reads, .after = ResourceState::CopyDst, .before_stages = buffer.read_stages});
        }
        return;
    }
    const MipStates& mips = *state.texture_states.find(upload.texture_handle);
    if (state.transfer) {
        const bool whole = !mips.initialized && !upload.generate;
        batch.add(TextureBarrier{
            .texture = upload.texture_handle,
            .after = ResourceState::CopyDst,
            .range = whole ? TextureRange{}
                           : TextureRange{.first_mip = upload.range.first_mip, .mip_count = upload.range.mip_count},
            .discard = true});
        return;
    }
    if (!mips.initialized) {
        batch.add(TextureBarrier{.texture = upload.texture_handle,
                                 .before = ResourceState::Undefined,
                                 .after = ResourceState::CopyDst,
                                 .discard = true});
        return;
    }
    add_mip_runs(mips, upload.texture_handle, upload.range.first_mip,
                 upload.generate ? upload.desc->mip_count : uploaded_end, batch);
}

// The barriers that take an upload's target from CopyDst to the state it asked for, and the
// tracked state that follows. On the transfer queue the uploaded part is handed over: released
// on the transfer list (release), acquired on the graphics list (acquire). What follows on the
// graphics list goes in finish, after any mip generation.
//
// Mip generation (graphics only): the uploaded mips stay in CopyDst but the last, the chain's
// source, which goes to CopySrc (on the transfer queue, during the handover); acquire also
// prepares the generated mips (CopyDst, contents discarded); then record_mip_chain runs, and
// finish moves every mip to the requested state.
void add_use_barriers(UploaderState& state, const PendingUpload& upload, BarrierBatch& release, BarrierBatch& acquire,
                      BarrierBatch& finish) {
    const auto hand_over = [&](auto barrier) {
        barrier.ownership = QueueOwnership::Release;
        barrier.other_queue = QueueType::Graphics;
        release.add(barrier);
        barrier.ownership = QueueOwnership::Acquire;
        barrier.other_queue = QueueType::Transfer;
        acquire.add(barrier);
    };
    if (!upload.texture) {
        const BufferBarrier barrier{.buffer = upload.buffer_handle,
                                    .before = ResourceState::CopyDst,
                                    .after = upload.after,
                                    .after_stages = upload.after_stages,
                                    .offset = upload.buffer_offset,
                                    .size = upload.bytes};
        if (state.transfer) {
            hand_over(barrier);
        } else {
            finish.add(barrier);
        }
        BufferState& buffer = *state.buffer_states.find(upload.buffer_handle);
        if (core::has_any(upload.after, kWriteStates)) {
            buffer.write = upload.after;
            buffer.write_stages = upload.after_stages;
        } else {
            buffer.reads |= upload.after;
            buffer.read_stages |= upload.after_stages;
        }
        return;
    }

    MipStates& mips = *state.texture_states.find(upload.texture_handle);
    const uint32_t mip_count = upload.desc->mip_count;
    const uint32_t uploaded_end = upload.range.first_mip + upload.range.mip_count;
    if (upload.generate) {
        // The uploaded mips start at 0 (upload_texture checks it); the chain's source is the last,
        // and goes to CopySrc for it (record_mip_chain).
        const uint32_t source = uploaded_end - 1;
        const TextureBarrier to_source{.texture = upload.texture_handle,
                                       .before = ResourceState::CopyDst,
                                       .after = ResourceState::CopySrc,
                                       .range = {.first_mip = source, .mip_count = 1}};
        if (!state.transfer) {
            acquire.add(to_source);
        } else {
            if (source > 0) {
                hand_over(TextureBarrier{.texture = upload.texture_handle,
                                         .before = ResourceState::CopyDst,
                                         .after = ResourceState::CopyDst,
                                         .range = {.first_mip = 0, .mip_count = source}});
            }
            hand_over(to_source);
            if (mips.initialized) {
                add_mip_runs(mips, upload.texture_handle, uploaded_end, mip_count, acquire);
            } else {
                acquire.add(TextureBarrier{.texture = upload.texture_handle,
                                           .after = ResourceState::CopyDst,
                                           .range = {.first_mip = uploaded_end, .mip_count = mip_count - uploaded_end},
                                           .discard = true});
            }
        }
        // After the chain: [0, source) in CopyDst, [source, last) in CopySrc, last in CopyDst.
        const auto to_after = [&](ResourceState before, uint32_t first, uint32_t count) {
            if (count > 0) {
                finish.add(TextureBarrier{.texture = upload.texture_handle,
                                          .before = before,
                                          .after = upload.after,
                                          .after_stages = upload.after_stages,
                                          .range = {.first_mip = first, .mip_count = count}});
            }
        };
        to_after(ResourceState::CopyDst, 0, source);
        to_after(ResourceState::CopySrc, source, mip_count - 1 - source);
        to_after(ResourceState::CopyDst, mip_count - 1, 1);
        for (uint32_t mip = 0; mip < mip_count; ++mip) {
            mips.state[mip] = upload.after;
            mips.stages[mip] = upload.after_stages;
        }
        mips.initialized = true;
        return;
    }

    // On the first upload every mip is in CopyDst, and all of them move on: the ones not uploaded
    // yet hold undefined texels until they are.
    const TextureRange range =
        mips.initialized ? TextureRange{.first_mip = upload.range.first_mip, .mip_count = upload.range.mip_count}
                         : TextureRange{};
    const TextureBarrier barrier{.texture = upload.texture_handle,
                                 .before = ResourceState::CopyDst,
                                 .after = upload.after,
                                 .after_stages = upload.after_stages,
                                 .range = range};
    if (state.transfer) {
        hand_over(barrier);
    } else {
        finish.add(barrier);
    }
    const uint32_t first = mips.initialized ? upload.range.first_mip : 0;
    const uint32_t end = mips.initialized ? uploaded_end : mip_count;
    for (uint32_t mip = first; mip < end; ++mip) {
        mips.state[mip] = upload.after;
        mips.stages[mip] = upload.after_stages;
    }
    mips.initialized = true;
}

void record_copies(const PendingUpload& upload, CommandList& list) {
    if (!upload.texture) {
        list.copy_buffer(upload.staging, upload.staging_offset, upload.buffer_handle, upload.buffer_offset,
                         upload.bytes);
        return;
    }
    // One copy per mip, all layers at once.
    const TextureDesc& desc = *upload.desc;
    uint64_t offset = upload.staging_offset;
    for (uint32_t mip = upload.range.first_mip; mip < upload.range.first_mip + upload.range.mip_count; ++mip) {
        offset = upload.staging_offset + core::align_up(offset - upload.staging_offset, kStagingAlignment);
        list.copy_buffer_to_texture(upload.staging, upload.texture_handle,
                                    {.buffer_offset = offset, .mip = mip, .layer_count = desc.layer_count});
        offset += mip_layer_bytes(desc, mip) * desc.layer_count;
    }
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
    state->transfer = desc.use_transfer_queue && device.capabilities().transfer_queue;
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
    if (format_info(desc->format).depth) {
        // Depth comes from rendering, and a transfer queue can't copy depth at all.
        return core::fail(core::ErrorCode::Unsupported, "uploads to depth formats aren't supported");
    }
    if (desc->mip_count > kMaxTrackedMips) {
        return core::fail(core::ErrorCode::Unsupported, "uploads go to textures of at most 16 mips");
    }
    if (upload.after == ResourceState::Undefined || !is_valid_state(upload.after, true) ||
        !stages_named(upload.after, upload.after_stages)) {
        return core::fail(core::ErrorCode::InvalidArgument,
                          "an upload must leave the texture in a valid state (with its shader stages)");
    }
    const uint64_t expected = texture_upload_size(*desc, upload);
    if (expected == 0) {
        return core::fail(core::ErrorCode::OutOfRange, "the upload's mips are outside the texture");
    }
    if (upload.generate_mips) {
        if (upload.first_mip != 0 || resolved_mip_count(*desc, upload) >= desc->mip_count) {
            return core::fail(core::ErrorCode::InvalidArgument,
                              "generating mips: upload the top mips (from mip 0) and leave some to generate");
        }
        if (!can_generate_mips(*state.device, *desc)) {
            return core::fail(core::ErrorCode::Unsupported,
                              "the texture can't generate mips (its format can't be blitted, or it lacks "
                              "CopySrc | CopyDst usage)");
        }
    }
    if (data.size() != expected) {
        DE_LOG_ERROR("render", "texture upload: %zu bytes given, %llu expected for those mips (%ux%u %s)", data.size(),
                     static_cast<unsigned long long>(expected), desc->width, desc->height,
                     format_info(desc->format).name);
        return core::fail(core::ErrorCode::InvalidArgument, "the upload's data size doesn't match the texture");
    }

    // Staging holds each mip (all its layers) at an aligned offset.
    const uint32_t mips = resolved_mip_count(*desc, upload);
    uint64_t staged_bytes = 0;
    for (uint32_t mip = upload.first_mip; mip < upload.first_mip + mips; ++mip) {
        staged_bytes =
            core::align_up(staged_bytes, kStagingAlignment) + (mip_layer_bytes(*desc, mip) * desc->layer_count);
    }
    PendingUpload pending{.texture = true,
                          .texture_handle = texture,
                          .range = {.first_mip = upload.first_mip, .mip_count = mips},
                          .after = upload.after,
                          .after_stages = upload.after_stages,
                          .bytes = staged_bytes,
                          .generate = upload.generate_mips};
    const std::span<std::byte> staging = stage(state, staged_bytes, pending);
    if (staging.empty()) {
        return core::fail(core::ErrorCode::OutOfMemory, "no memory for the upload's staging");
    }
    uint64_t source = 0;
    uint64_t target = 0;
    for (uint32_t mip = upload.first_mip; mip < upload.first_mip + mips; ++mip) {
        const uint64_t mip_bytes = mip_layer_bytes(*desc, mip) * desc->layer_count;
        target = core::align_up(target, kStagingAlignment);
        std::memcpy(staging.data() + target, data.data() + source, static_cast<size_t>(mip_bytes));
        source += mip_bytes;
        target += mip_bytes;
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
    if (after == ResourceState::Undefined || !is_valid_state(after, false) || !stages_named(after, after_stages)) {
        return core::fail(core::ErrorCode::InvalidArgument,
                          "an upload must leave the buffer in a valid state (with its shader stages)");
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
    ++state.buffer_queued.find_or_insert(buffer);
    ++state.buffer_uploads_queued;
    return {};
}

void Uploader::record() {
    DE_PROFILE_ZONE("uploader record");
    UploaderState& state = *state_;
    DE_ASSERT_MSG(state.device->in_frame(), "record() goes inside begin_frame/end_frame");
    DE_ASSERT_MSG(state.recorded_frame != state.device->frame_index(), "record() runs once per frame");
    state.recorded_frame = state.device->frame_index();
    if (state.queue_head == state.queue.size()) {
        return;
    }
    reclaim(state);
    purge_states(state);

    // This frame's batch: FIFO, within the budget (an upload bigger than it goes alone), and never
    // two uploads to the same place (barriers in one command are unordered, so a second upload to a
    // texture, or inside the span uploaded to a buffer, waits for the next batch). Uploads to
    // resources destroyed since they were queued are dropped.
    const uint64_t batch = ++state.batch;
    const size_t first = state.queue_head;
    size_t last = first;
    uint64_t batch_bytes = 0;
    size_t fresh = 0;
    size_t overwrites = 0;
    while (last < state.queue.size()) {
        PendingUpload& upload = state.queue[last];
        upload.desc = upload.texture ? state.device->texture_desc(upload.texture_handle) : nullptr;
        upload.dropped =
            upload.texture ? upload.desc == nullptr : state.device->buffer_desc(upload.buffer_handle) == nullptr;
        if (!upload.dropped) {
            if (batch_bytes > 0 && batch_bytes + upload.bytes > state.desc.frame_budget_bytes) {
                break;
            }
            if (upload.texture) {
                MipStates& mips = state.texture_states.find_or_insert(upload.texture_handle);
                upload.overwrite = mips.initialized;
                if (mips.batch == batch) {
                    break;
                }
                mips.batch = batch;
            } else {
                BufferState& buffer = state.buffer_states.find_or_insert(upload.buffer_handle);
                upload.overwrite = buffer.reads != ResourceState::Undefined || buffer.write != ResourceState::Undefined;
                const uint64_t upload_end = upload.buffer_offset + upload.bytes;
                if (buffer.batch != batch) {
                    buffer.batch = batch;
                    buffer.batch_first = upload.buffer_offset;
                    buffer.batch_end = upload_end;
                } else if (upload_end <= buffer.batch_first || upload.buffer_offset >= buffer.batch_end) {
                    buffer.batch_first = std::min(buffer.batch_first, upload.buffer_offset);
                    buffer.batch_end = std::max(buffer.batch_end, upload_end);
                } else {
                    break;
                }
            }
            batch_bytes += upload.bytes;
            if (upload.overwrite) {
                ++overwrites;
            } else {
                ++fresh;
            }
        }
        ++last;
    }

    // The lists, begun only when needed. The graphics list starts the frame, so the uploads land
    // before any of its other graphics work wherever record() is called. On the transfer queue,
    // the copies go on transfer lists (fresh resources on one that runs alongside the previous
    // frame, overwrites on one that waits for it, as that frame may still read the old contents)
    // and the graphics list gets the acquires.
    if (fresh + overwrites > 0) {
        CommandList graphics = state.device->begin_command_list(QueueType::Graphics, ListStart::FrameStart);
        const ScopedDebugLabel label(graphics, "uploads");
        const auto record_group = [&](bool overwrite_group, CommandList& copies) {
            // In chunks that keep each barrier command within its inline capacity (no heap; an
            // upload adds at most kMaxTrackedMips + 1 barriers to a batch): barriers to CopyDst,
            // the copies, the handover, mip generation, barriers to the requested states.
            BarrierBatch to_copy;
            BarrierBatch release;
            BarrierBatch acquire;
            BarrierBatch finish;
            const auto in_group = [&](const PendingUpload& upload) {
                return !upload.dropped && (!state.transfer || upload.overwrite == overwrite_group);
            };
            const auto fullest = [&] {
                const auto count = [](const BarrierBatch& barriers) {
                    return barriers.texture_count() + barriers.buffer_count();
                };
                return std::max({count(to_copy), count(release), count(acquire), count(finish)});
            };
            // The most barriers one upload adds to any of the batches: a buffer one (and maybe
            // a global barrier, merged); a texture's first upload a few; a later one a run per
            // mip, plus the handover of a generating upload's source mip and the rest.
            const auto upload_bound = [&](const PendingUpload& upload) -> size_t {
                if (!upload.texture) {
                    return 1;
                }
                return state.texture_states.find(upload.texture_handle)->initialized ? upload.desc->mip_count + 2 : 3;
            };
            size_t chunk_first = first;
            while (chunk_first < last) {
                size_t chunk_last = chunk_first;
                for (; chunk_last < last; ++chunk_last) {
                    const PendingUpload& upload = state.queue[chunk_last];
                    if (!in_group(upload)) {
                        continue;
                    }
                    if (fullest() > 0 && fullest() + upload_bound(upload) > kInlineBarriers) {
                        break;
                    }
                    add_copy_barriers(state, upload, to_copy);
                    add_use_barriers(state, upload, release, acquire, finish);
                }
                to_copy.flush(copies);
                for (size_t i = chunk_first; i < chunk_last; ++i) {
                    if (in_group(state.queue[i])) {
                        record_copies(state.queue[i], copies);
                    }
                }
                release.flush(copies);
                acquire.flush(graphics);
                for (size_t i = chunk_first; i < chunk_last; ++i) {
                    const PendingUpload& upload = state.queue[i];
                    if (in_group(upload) && upload.generate) {
                        record_mip_chain(graphics, upload.texture_handle, *upload.desc,
                                         upload.range.first_mip + upload.range.mip_count - 1);
                    }
                }
                finish.flush(graphics);
                chunk_first = chunk_last;
            }
        };
        if (!state.transfer) {
            record_group(false, graphics);
        } else {
            if (fresh > 0) {
                CommandList copies = state.device->begin_command_list(QueueType::Transfer);
                const ScopedDebugLabel copies_label(copies, "upload copies");
                record_group(false, copies);
            }
            if (overwrites > 0) {
                CommandList copies =
                    state.device->begin_command_list(QueueType::Transfer, ListStart::AfterPreviousFrame);
                const ScopedDebugLabel copies_label(copies, "upload overwrites");
                record_group(true, copies);
            }
        }
    }

    // Staging and stats: everything in the batch is done with, dropped or not. The staging can go
    // once this frame is done on the GPU.
    const uint64_t frame = state.device->frame_index();
    for (size_t i = first; i < last; ++i) {
        const PendingUpload& upload = state.queue[i];
        if (upload.dropped) {
            ++state.dropped;
        } else {
            state.uploaded_bytes += upload.bytes;
        }
        if (upload.region != kNotRecorded) {
            state.regions[upload.region - state.region_base].frame = frame;
        } else {
            state.device->destroy(upload.staging);
        }
        state.queued_bytes -= upload.bytes;
    }

    for (size_t i = first; i < last; ++i) {
        if (!state.queue[i].texture) {
            ++state.buffer_uploads_recorded;
            uint32_t* queued = state.buffer_queued.find(state.queue[i].buffer_handle);
            if (queued != nullptr && --*queued == 0) {
                state.buffer_queued.remove(state.queue[i].buffer_handle);
            }
        }
    }
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
        record();
        device.end_frame();
    }
    device.wait_idle();
}

bool Uploader::ready(TextureHandle texture) const {
    const MipStates* mips = state_->texture_states.find(texture);
    return mips != nullptr && mips->initialized;
}

bool Uploader::ready(BufferHandle buffer) const {
    const UploaderState& state = *state_;
    if (state.device->buffer_desc(buffer) == nullptr) {
        return false;
    }
    return state.buffer_queued.find(buffer) == nullptr;
}

uint64_t Uploader::buffer_upload_ticket() const {
    return state_->buffer_uploads_queued;
}

bool Uploader::recorded(uint64_t buffer_upload_ticket) const {
    return state_->buffer_uploads_recorded >= buffer_upload_ticket;
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
        .transfer_queue = state.transfer,
    };
}

} // namespace de::rhi
