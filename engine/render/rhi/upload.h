#pragma once

#include "core/result.h"
#include "render/rhi/command_list.h"
#include "render/rhi/rhi.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace de::rhi {

class Device;
struct UploaderState;

struct UploaderDesc {
    // The staging ring: CPU-written memory the GPU copies from. Data is copied in when an upload
    // is queued; an upload bigger than the whole ring gets a staging buffer of its own.
    uint64_t staging_bytes = uint64_t{64} << 20U;
    // Bytes copied to the GPU per record() call at most, so a burst of uploads spreads over frames
    // instead of stalling one. A single upload bigger than this still goes, alone.
    // record() keeps each barrier command within kInlineBarriers, splitting big batches.
    uint64_t frame_budget_bytes = uint64_t{16} << 20U;
    // Copy on the transfer queue when the device has one (Capabilities::transfer_queue), so
    // uploads run alongside rendering; false keeps them on the graphics queue.
    bool use_transfer_queue = true;
};

// Which mips of a texture an upload fills, and the state it leaves them in.
struct TextureUpload {
    uint32_t first_mip = 0;
    uint32_t mip_count = kAllMips; // kAllMips: from first_mip to the last
    ResourceState after = ResourceState::ShaderRead;
    ShaderStages after_stages = ShaderStages::All;
    // Generate the mips after the uploaded ones on the GPU, from the last uploaded (rhi/mips.h):
    // upload from mip 0, typically just it. Needs can_generate_mips(); on the transfer queue, the
    // generation runs on the frame's graphics list.
    bool generate_mips = false;
};

// The bytes an upload of these mips takes, tightly packed: mip by mip from first_mip; within a mip
// layer by layer (cube faces in +X -X +Y -Y +Z -Z order), then depth slices, rows top to bottom.
// Compressed formats count whole blocks (a 2x2 BC mip is one 4x4 block). 0 if the range is invalid.
uint64_t texture_upload_size(const TextureDesc& desc, const TextureUpload& upload);

struct UploaderStats {
    uint32_t queued_uploads = 0; // validated and in staging, not yet copied on the GPU
    uint64_t queued_bytes = 0;
    uint64_t staging_bytes_in_use = 0; // ring bytes the GPU may still read
    uint64_t staging_bytes = 0;
    uint64_t uploaded_bytes = 0;    // copied to the GPU so far, in total
    uint32_t dedicated_staging = 0; // uploads that didn't fit the ring and got their own buffer
    uint32_t dropped_uploads = 0;   // uploads whose texture or buffer was destroyed before they were recorded
    bool transfer_queue = false;    // copies run on the transfer queue
};

// Moves data from the CPU into GPU textures and buffers (docs/design.md, File system & IO: GPU
// uploads go through a staging ring, batched).
//
//     uploader.upload_texture(albedo, texels);   // validated, copied into staging, queued
//     ...
//     uploader.record(list);                    // once per frame: copies and barriers, within budget
//
// It owns the state of what it uploads. The first upload to a texture takes every mip out of
// Undefined into the requested state (ShaderRead by default), so the texture's views are valid at
// once: mips not uploaded yet hold undefined texels until they are (sample them with a mip clamp).
// Later uploads (more mips streamed in, or new contents) start from the state the last one left
// and move only their mips. Don't transition those textures yourself until their uploads are
// recorded.
//
// Uploads land at the start of the frame that records them, before any of its other graphics
// work, wherever record() is called in it: record() puts them on a FrameStart graphics list.
// On the transfer queue, the copies go on transfer lists that run alongside the GPU's previous
// frame and hand what they wrote to graphics (the frame-start list acquires it); uploads over
// something already uploaded wait for the previous frame first, as it may still read the old
// contents, and the frame's graphics work waits for them. Overwrites thus cost the graphics queue
// the copy time; streaming new resources costs it nothing.
//
// The first upload to a texture or buffer assumes nothing else is using it, and one uploader
// handles all uploads to a resource.
//
// Uploads are copied into staging at once, so the caller's memory is free on return; they reach the
// GPU in the order they were queued, one batch a frame, where a batch holds as many as the budget allows
// but only one to a texture, or to any one place in a buffer. Destroying a texture or buffer with uploads still queued
// is fine: they are dropped. Single-threaded, like the device. Move-only; the device must outlive it.
class Uploader {
public:
    static core::Result<Uploader> create(Device& device, const UploaderDesc& desc = {});

    ~Uploader();
    Uploader(Uploader&& other) noexcept;
    Uploader& operator=(Uploader&& other) noexcept;
    Uploader(const Uploader&) = delete;
    Uploader& operator=(const Uploader&) = delete;

    // Fails, uploading nothing, if the texture is stale or lacks CopyDst usage, the mips are out of
    // range, the format is a depth format, or data isn't exactly texture_upload_size() bytes.
    // Every layer of the mips is uploaded at once.
    core::Result<void> upload_texture(TextureHandle texture, std::span<const std::byte> data,
                                      const TextureUpload& upload = {});

    // Fails if the buffer is stale, lacks CopyDst usage or the range doesn't fit. The bytes after
    // the upload are in after state, for after_stages. A buffer's state is tracked as a whole: read
    // states left by uploads to different ranges add up, and the next upload waits for all of them.
    core::Result<void> upload_buffer(BufferHandle buffer, uint64_t offset, std::span<const std::byte> data,
                                     ResourceState after, ShaderStages after_stages = ShaderStages::All);

    // Once per frame, between begin_frame and end_frame: records the queued copies, within the
    // frame budget, and the barriers around them, on lists of its own. Records nothing when idle().
    void record();

    // For load time and tests, outside a frame: runs frames until everything queued is copied, and
    // waits for the GPU.
    void flush_and_wait();

    [[nodiscard]] bool idle() const;
    // True once the texture's first upload has been recorded (in this frame's record() or before):
    // its mips are out of Undefined, so it may be sampled. Until then, don't.
    [[nodiscard]] bool ready(TextureHandle texture) const;
    // True once no upload to the buffer is waiting to be recorded (false for a stale handle): the
    // GPU work recorded after this frame's record() sees every upload queued to it so far.
    [[nodiscard]] bool ready(BufferHandle buffer) const;
    // For a buffer that's always being uploaded to, where ready() may never come. The ticket of the buffer upload
    // queued last, and whether that's been recorded: as for ready(), with every upload queued up to it. Uploads
    // are recorded oldest first, so a later ticket means the earlier ones too.
    [[nodiscard]] uint64_t buffer_upload_ticket() const;
    [[nodiscard]] bool recorded(uint64_t buffer_upload_ticket) const;
    UploaderStats stats() const;

private:
    explicit Uploader(UploaderState* state) : state_(state) {}

    UploaderState* state_ = nullptr;
};

} // namespace de::rhi
