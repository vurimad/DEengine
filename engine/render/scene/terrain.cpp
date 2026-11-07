#include "render/scene/terrain.h"

#include "asset/format.h"
#include "core/log.h"
#include "core/profile.h"
#include "platform/vfs.h"
#include "render/rhi/command_list.h"
#include "render/rhi/device.h"
#include "render/rhi/upload.h"
#include "render/scene/scene_renderer.h"
#include "world/render_extract.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

namespace de::render {

namespace {

core::Allocator& render_heap() {
    return core::heap_allocator(core::MemoryTag::Render);
}

constexpr uint32_t kNoSlot = UINT32_MAX;
constexpr uint32_t kLetGoOne = UINT32_MAX - 1; // no slot yet, but one has been let go, and comes back in a few frames
constexpr uint32_t kViewCount = 1 + kShadowCascadeCount;
// Patches a view can draw. A view asks for about 1,000 in a wide open vista.
constexpr uint32_t kListCapacity = 16384;
// Blocks read at once: at about 35 KB each, 16 a frame would keep up with a jet.
constexpr uint32_t kMaxReads = 16;
constexpr uint32_t kPatchQuads = asset::kTerrainPatchQuads;
constexpr uint32_t kGridSide = kPatchQuads + 1;
constexpr uint32_t kGridVertices = kGridSide * kGridSide;
constexpr uint32_t kGridIndices = kPatchQuads * kPatchQuads * 6;
constexpr uint32_t kSkirtIndices = kPatchQuads * 4 * 6;
constexpr uint32_t kArgsWords = 5;
constexpr uint32_t kArgsInstanceCount = 1;
constexpr uint64_t kArgsBytes = uint64_t{kViewCount} * kArgsWords * sizeof(uint32_t);

// Matches TerrainLevelInfo in shaders/scene/terrain_common.slang.
struct GpuTerrainLevel {
    uint32_t nodes_x = 0;
    uint32_t nodes_z = 0;
    uint32_t first_node = 0;
    uint32_t first_patch = 0;
    uint32_t spacing_steps = 0;
    float spacing_m = 0.0f;
    float step_m = 0.0f;
    float range_m = 0.0f;
    float morph_start_m = 0.0f;
    float morph_scale = 0.0f;
    uint32_t padding[2] = {};
};
static_assert(sizeof(GpuTerrainLevel) == 48);

// Matches TerrainDraw in shaders/scene/terrain_common.slang.
struct GpuTerrainDraw {
    uint32_t level_x_z = 0;
    uint32_t slot = 0;
    int32_t base_steps = 0;
    float skirt_m = 0.0f;
};
static_assert(sizeof(GpuTerrainDraw) == 16);

// Matches TerrainSelectPush in shaders/scene/terrain_select.slang.
struct TerrainSelectPush {
    uint64_t frame = 0;
    uint64_t views = 0;
    uint64_t levels = 0;
    uint64_t patches = 0;
    uint64_t nodes = 0;
    uint64_t slots = 0;
    uint64_t draws = 0;
    uint64_t args = 0;
    uint32_t origin_steps[2] = {};
    float size_m[2] = {};
    uint32_t level = 0;
    uint32_t level_count = 0;
    uint32_t list_capacity = 0;
};
static_assert(sizeof(TerrainSelectPush) <= 128);

// Matches TerrainPush in shaders/scene/terrain.slang and terrain_shadow.slang.
struct TerrainDrawPush {
    uint64_t frame = 0;
    uint64_t draws = 0;
    uint64_t levels = 0;
    uint64_t heights = 0;
    uint32_t origin_steps[2] = {};
    uint32_t size_steps[2] = {};
    uint32_t cascade = 0;
    uint32_t show_levels = 0;
};
static_assert(sizeof(TerrainDrawPush) <= 128);

enum class NodeState : uint8_t { Unloaded, Reading, Uploading, Resident, Failed };

struct Node {
    uint32_t slot = kNoSlot;
    uint32_t last_wanted = 0;   // the update that last wanted it
    uint32_t parent = kNoSlot;  // the node above it; none for a root
    uint32_t children_held = 0; // with a slot, shown or on their way: it can't go before they do
    NodeState state = NodeState::Unloaded;
};

struct Read {
    platform::ReadHandle handle;
    uint32_t node = 0;
    bool active = false;
};

struct Uploading {
    uint64_t ticket = 0;
    uint32_t node = 0;
};

struct Retired {
    uint32_t slot = 0;
    uint32_t free_after = 0; // the update after which nothing in flight can read it
};

} // namespace

void terrain_wanted_nodes(const asset::TerrainAsset& terrain, std::span<const std::byte> file,
                          std::span<const float> node_reach, const DVec3& camera, core::Array<TerrainWant>& out) {
    out.clear();
    const std::span<const asset::TerrainLevel> levels = terrain.levels.get(file.data());
    const std::span<const asset::TerrainNode> nodes = terrain.nodes.get(file.data());
    const double east = camera.x - terrain.origin_m[0];
    const double south = camera.z - terrain.origin_m[1];
    for (uint32_t l = 0; l < levels.size(); ++l) {
        const asset::TerrainLevel& level = levels[l];
        const double node_size = static_cast<double>(level.spacing_m) * asset::kTerrainNodeQuads;
        const double reach = l == 0
                                 ? std::numeric_limits<double>::infinity()
                                 : static_cast<double>(terrain_level_range_m(level.spacing_m)) * kTerrainStreamMargin;
        const auto first = [&](double at, double limit) {
            return static_cast<uint32_t>(std::clamp(std::floor((at - reach) / node_size), 0.0, limit));
        };
        const auto last = [&](double at, double limit) {
            return static_cast<uint32_t>(std::clamp(std::floor((at + reach) / node_size), 0.0, limit));
        };
        const double max_x = static_cast<double>(level.nodes_x) - 1.0;
        const double max_z = static_cast<double>(level.nodes_z) - 1.0;
        const size_t level_start = out.size();
        for (uint32_t z = l == 0 ? 0 : first(south, max_z); z <= (l == 0 ? level.nodes_z - 1 : last(south, max_z));
             ++z) {
            for (uint32_t x = l == 0 ? 0 : first(east, max_x); x <= (l == 0 ? level.nodes_x - 1 : last(east, max_x));
                 ++x) {
                const uint32_t index = level.first_node + (z * level.nodes_x) + x;
                if (nodes[index].block == asset::kTerrainNoBlock) {
                    continue;
                }
                const double low_x = static_cast<double>(x) * node_size;
                const double low_z = static_cast<double>(z) * node_size;
                const double dx = std::max({low_x - east, east - (low_x + node_size), 0.0});
                const double dz = std::max({low_z - south, south - (low_z + node_size), 0.0});
                const double dy = std::max({static_cast<double>(node_reach[(index * 2) + 0]) - camera.y,
                                            camera.y - static_cast<double>(node_reach[(index * 2) + 1]), 0.0});
                const double distance = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
                if (distance < reach) {
                    out.push(TerrainWant{.node = index, .level = l, .distance_m = static_cast<float>(distance)});
                }
            }
        }
        std::sort(out.begin() + level_start, out.end(), [](const TerrainWant& a, const TerrainWant& b) {
            return a.distance_m < b.distance_m || (a.distance_m == b.distance_m && a.node < b.node);
        });
    }
}

struct TerrainState {
    rhi::Device* device = nullptr;
    rhi::Uploader* uploader = nullptr;
    platform::Vfs* vfs = nullptr;
    uint32_t max_blocks = 0;

    rhi::BufferHandle heights; // the pool: max_blocks blocks
    rhi::BufferHandle indices; // a patch's triangles, the same for all
    rhi::BufferHandle draws;   // per view, kListCapacity
    rhi::BufferHandle args;    // per view, a DrawIndexedIndirect
    rhi::BufferHandle readback[rhi::kFramesInFlight];
    bool readback_used[rhi::kFramesInFlight] = {};
    rhi::BufferHandle slots[rhi::kFramesInFlight]; // per node, where its heights are, by frame in flight
    uint64_t slots_version[rhi::kFramesInFlight] = {};
    uint64_t heights_address = 0;
    uint64_t draws_address = 0;
    uint64_t args_address = 0;
    uint64_t index_ticket = 0;
    bool buffers_read = false;

    // The terrain being shown, and what's sized by it.
    const asset::TerrainAsset* terrain = nullptr;
    std::span<const std::byte> file;
    rhi::BufferHandle levels;
    rhi::BufferHandle nodes_buffer;
    rhi::BufferHandle patches;
    uint64_t tables_ticket = 0;
    uint32_t level_count = 0;
    core::Array<Node> nodes{render_heap()};
    core::Array<uint32_t> slot_table{render_heap()}; // what the GPU's table should hold
    uint64_t slot_table_version = 1;
    core::Array<float> node_reach{render_heap()};
    uint32_t origin_steps[2] = {};
    uint32_t size_steps[2] = {};
    float size_m[2] = {};
    asset::CookedPath heights_path;
    asset::AssetHeader heights_header{};
    platform::ReadHandle header_read;
    bool header_reading = false;
    bool header_ok = false;

    // Streaming, kept between frames.
    core::Array<std::byte> read_memory{render_heap()}; // kMaxReads blocks
    Read reads[kMaxReads] = {};
    core::Array<Uploading> uploading{render_heap()};
    core::Array<uint32_t> free_slots{render_heap()};
    core::Array<uint32_t> slot_owner{render_heap()}; // per slot, its node, or kNoSlot
    core::Array<Retired> retired{render_heap()};
    core::Array<TerrainWant> wanted{render_heap()};
    uint32_t update_number = 0;
    uint32_t next_to_let_go = 0;
    uint32_t resident = 0; // shown in the table
    bool warned_over_capacity = false;

    TerrainStats counters;
    TerrainStats drawn;

    [[nodiscard]] bool tables_ready() const {
        return terrain != nullptr && header_ok && uploader->recorded(tables_ticket) && uploader->recorded(index_ticket);
    }

    void wait_for_reads() {
        for (Read& read : reads) {
            if (read.active) {
                while (vfs->status(read.handle) == platform::ReadStatus::Pending ||
                       vfs->status(read.handle) == platform::ReadStatus::InFlight) {
                    if (vfs->cancel(read.handle) != platform::CancelResult::InFlight) {
                        read.active = false;
                        break;
                    }
                }
                if (read.active) {
                    static_cast<void>(vfs->finish(read.handle));
                    read.active = false;
                }
            }
        }
        if (header_reading) {
            while (vfs->status(header_read) == platform::ReadStatus::Pending ||
                   vfs->status(header_read) == platform::ReadStatus::InFlight) {
                if (vfs->cancel(header_read) != platform::CancelResult::InFlight) {
                    header_reading = false;
                    break;
                }
            }
            if (header_reading) {
                static_cast<void>(vfs->finish(header_read));
                header_reading = false;
            }
        }
    }

    void destroy_tables() {
        for (rhi::BufferHandle* buffer : {&levels, &nodes_buffer, &patches}) {
            if (buffer->is_valid()) {
                device->destroy(*buffer);
                *buffer = {};
            }
        }
        for (rhi::BufferHandle& buffer : slots) {
            if (buffer.is_valid()) {
                device->destroy(buffer);
                buffer = {};
            }
        }
    }

    void destroy_all() {
        destroy_tables();
        for (rhi::BufferHandle* buffer : {&heights, &indices, &draws, &args}) {
            if (buffer->is_valid()) {
                device->destroy(*buffer);
                *buffer = {};
            }
        }
        for (rhi::BufferHandle& buffer : readback) {
            if (buffer.is_valid()) {
                device->destroy(buffer);
                buffer = {};
            }
        }
    }

    void read_drawn() {
        const auto slot = static_cast<uint32_t>(device->frame_index() % rhi::kFramesInFlight);
        if (!readback_used[slot]) {
            return; // nothing copied there yet
        }
        device->invalidate_mapped(readback[slot]);
        const std::span<const std::byte> bytes = device->mapped(readback[slot]);
        uint32_t words[kViewCount * kArgsWords] = {};
        std::memcpy(words, bytes.data(), sizeof(words));
        drawn.patches = words[kArgsInstanceCount];
        drawn.triangles = drawn.patches * ((kGridIndices + kSkirtIndices) / 3);
        drawn.shadow_patches = 0;
        for (uint32_t view = 1; view < kViewCount; ++view) {
            drawn.shadow_patches += words[(view * kArgsWords) + kArgsInstanceCount];
        }
        drawn.shadow_triangles = drawn.shadow_patches * (kGridIndices / 3);
    }

    // Gives a node a slot, from what's free or else from what's gone longest unwanted. kNoSlot: all are wanted.
    uint32_t take_slot() {
        if (!free_slots.empty()) {
            const uint32_t slot = free_slots.back();
            free_slots.resize(free_slots.size() - 1);
            return slot;
        }
        // The next that isn't wanted, going round, so this doesn't walk the pool each time. Not one with children held:
        // the table has to hold whole chains from a root. Only with everything wanted does it go all the way round.
        for (uint32_t tried = 0; tried < max_blocks; ++tried) {
            const uint32_t slot = next_to_let_go;
            next_to_let_go = (next_to_let_go + 1) % max_blocks;
            const uint32_t owner = slot_owner[slot];
            if (owner == kNoSlot || nodes[owner].state != NodeState::Resident || nodes[owner].children_held != 0 ||
                nodes[owner].last_wanted >= update_number) {
                continue;
            }
            // What's in flight may still read this slot, so it's free again only after those frames.
            Node& node = nodes[owner];
            if (node.parent != kNoSlot) {
                --nodes[node.parent].children_held;
            }
            node.state = NodeState::Unloaded;
            node.slot = kNoSlot;
            slot_table[owner] = kNoSlot;
            ++slot_table_version;
            slot_owner[slot] = kNoSlot;
            retired.push(Retired{.slot = slot, .free_after = update_number + rhi::kFramesInFlight + 1});
            --resident;
            return kLetGoOne;
        }
        return kNoSlot;
    }

    void release_slot(Node& node) {
        if (node.parent != kNoSlot) {
            --nodes[node.parent].children_held;
        }
        slot_owner[node.slot] = kNoSlot;
        free_slots.push(node.slot);
        node.slot = kNoSlot;
    }

    void finish_header() {
        if (!header_reading) {
            return;
        }
        const platform::ReadStatus status = vfs->status(header_read);
        if (status != platform::ReadStatus::Done && status != platform::ReadStatus::Failed) {
            return;
        }
        const core::Result<size_t> read = vfs->finish(header_read);
        header_reading = false;
        if (!read || *read != sizeof(asset::AssetHeader)) {
            DE_LOG_ERROR("terrain", "no heights: can't read %s", heights_path.c_str());
            return;
        }
        const core::Result<platform::FileInfo> info = vfs->file_info(heights_path.c_str());
        if (!info) {
            DE_LOG_ERROR("terrain", "no heights: %s: %s", heights_path.c_str(), info.error().message);
            return;
        }
        if (const core::Result<void> fits = terrain->check_heights_header(heights_header, info->size); !fits) {
            DE_LOG_ERROR("terrain", "no heights: %s: %s (run the cooker)", heights_path.c_str(), fits.error().message);
            return;
        }
        header_ok = true;
    }

    void collect_reads() {
        for (uint32_t r = 0; r < kMaxReads; ++r) {
            Read& read = reads[r];
            if (!read.active) {
                continue;
            }
            const platform::ReadStatus status = vfs->status(read.handle);
            if (status != platform::ReadStatus::Done && status != platform::ReadStatus::Failed) {
                continue;
            }
            const core::Result<size_t> got = vfs->finish(read.handle);
            read.active = false;
            Node& node = nodes[read.node];
            if (!got || *got != asset::kTerrainNodeBytes) {
                DE_LOG_ERROR("terrain", "node %u: can't read its heights; what's coarser stands in", read.node);
                node.state = NodeState::Failed;
                release_slot(node);
                ++counters.failed;
                continue;
            }
            const std::span<const std::byte> block(read_memory.data() + (r * asset::kTerrainNodeBytes),
                                                   asset::kTerrainNodeBytes);
            // Nothing reads this slot: the table doesn't name it until this has been recorded.
            const core::Result<void> queued = uploader->upload_buffer(heights, node.slot * asset::kTerrainNodeBytes,
                                                                      block, rhi::ResourceState::ShaderRead,
                                                                      rhi::ShaderStages::All, rhi::BufferRange::Unread);
            if (!queued) {
                DE_LOG_ERROR("terrain", "node %u: %s", read.node, queued.error().message);
                node.state = NodeState::Failed;
                release_slot(node);
                ++counters.failed;
                continue;
            }
            node.state = NodeState::Uploading;
            uploading.push(Uploading{.ticket = uploader->buffer_upload_ticket(), .node = read.node});
        }
    }

    void publish_uploads() {
        size_t kept = 0;
        for (const Uploading entry : uploading) { // by value: kept only ever writes behind this
            if (!uploader->recorded(entry.ticket)) {
                uploading[kept++] = entry;
                continue;
            }
            Node& node = nodes[entry.node];
            // Shown only after its parent, so the table holds whole chains from a root. If the parent can't come, nor
            // can this: what's above them stands in.
            if (node.parent != kNoSlot && nodes[node.parent].state != NodeState::Resident) {
                if (nodes[node.parent].state == NodeState::Failed) {
                    node.state = NodeState::Failed;
                    release_slot(node);
                    ++counters.failed;
                } else {
                    uploading[kept++] = entry;
                }
                continue;
            }
            node.state = NodeState::Resident;
            ++resident;
            slot_table[entry.node] = node.slot;
            ++slot_table_version;
            ++counters.blocks_loaded;
        }
        uploading.resize(kept);
    }

    void free_retired() {
        size_t kept = 0;
        for (const Retired entry : retired) { // by value: kept only ever writes behind this
            if (entry.free_after >= update_number) {
                retired[kept++] = entry;
            } else {
                free_slots.push(entry.slot);
            }
        }
        retired.resize(kept);
    }

    void start_reads() {
        counters.over_capacity = 0;
        uint32_t next_read = 0;
        uint32_t let_go = 0;
        for (const TerrainWant& want : wanted) {
            Node& node = nodes[want.node];
            if (node.state != NodeState::Unloaded) {
                continue;
            }
            // Its parent first: a block is shown only after it, so it'd only sit in its slot, waiting.
            if (node.parent != kNoSlot && nodes[node.parent].state == NodeState::Unloaded) {
                continue;
            }
            while (next_read < kMaxReads && reads[next_read].active) {
                ++next_read;
            }
            if (next_read == kMaxReads) {
                return;
            }
            const uint32_t slot = take_slot();
            if (slot == kLetGoOne) {
                // It comes back in a few frames: as many at a time as could be read at once.
                if (++let_go == kMaxReads) {
                    return;
                }
                continue;
            }
            if (slot == kNoSlot) {
                // Everything held is wanted, or waits for what's held below it.
                if (retired.empty()) {
                    ++counters.over_capacity;
                    if (!warned_over_capacity) {
                        warned_over_capacity = true;
                        DE_LOG_WARN("terrain", "more nodes wanted than the %u blocks there's room for", max_blocks);
                    }
                }
                return;
            }
            const uint32_t block = terrain->nodes.get(file.data())[want.node].block;
            const core::Result<platform::ReadHandle> handle =
                vfs->read({.path = heights_path.c_str(),
                           .dest = std::span(read_memory.data() + (next_read * asset::kTerrainNodeBytes),
                                             asset::kTerrainNodeBytes),
                           .offset = asset::terrain_block_offset(block),
                           .size = asset::kTerrainNodeBytes,
                           // The roots first: they're what stands in for everything, and there are few.
                           .priority = want.level == 0 ? platform::IoPriority::Critical : platform::IoPriority::Near});
            if (!handle) {
                free_slots.push(slot);
                return; // the queue is full: next frame
            }
            reads[next_read] = Read{.handle = *handle, .node = want.node, .active = true};
            node.state = NodeState::Reading;
            node.slot = slot;
            slot_owner[slot] = want.node;
            if (node.parent != kNoSlot) {
                ++nodes[node.parent].children_held;
            }
        }
    }
};

core::Result<Terrain> Terrain::create(const TerrainDesc& desc) {
    DE_ASSERT(desc.device != nullptr && desc.uploader != nullptr && desc.vfs != nullptr);
    auto* state = new (render_heap().allocate(sizeof(TerrainState), alignof(TerrainState))) TerrainState;
    Terrain terrain(state);
    state->device = desc.device;
    state->uploader = desc.uploader;
    state->vfs = desc.vfs;
    state->max_blocks = desc.max_blocks;

    const auto make = [&](rhi::BufferHandle& out, const rhi::BufferDesc& buffer) -> core::Result<void> {
        core::Result<rhi::BufferHandle> made = desc.device->create_buffer(buffer);
        if (!made) {
            return core::fail(made.error());
        }
        out = *made;
        return {};
    };
    using rhi::BufferUsage;
    if (auto made = make(state->heights, {.size = desc.max_blocks * asset::kTerrainNodeBytes,
                                          .usage = BufferUsage::Storage | BufferUsage::CopyDst,
                                          .debug_name = "terrain heights"});
        !made) {
        return core::fail(made.error());
    }
    if (auto made = make(state->indices, {.size = (kGridIndices + kSkirtIndices) * sizeof(uint16_t),
                                          .usage = BufferUsage::Index | BufferUsage::CopyDst,
                                          .debug_name = "terrain patch"});
        !made) {
        return core::fail(made.error());
    }
    if (auto made = make(state->draws, {.size = uint64_t{kViewCount} * kListCapacity * sizeof(GpuTerrainDraw),
                                        .usage = BufferUsage::Storage,
                                        .debug_name = "terrain draws"});
        !made) {
        return core::fail(made.error());
    }
    if (auto made = make(state->args, {.size = kArgsBytes,
                                       .usage = BufferUsage::Storage | BufferUsage::Indirect | BufferUsage::CopyDst |
                                                BufferUsage::CopySrc,
                                       .debug_name = "terrain args"});
        !made) {
        return core::fail(made.error());
    }
    for (rhi::BufferHandle& buffer : state->readback) {
        if (auto made = make(buffer, {.size = kArgsBytes,
                                      .usage = BufferUsage::CopyDst,
                                      .memory = rhi::MemoryLocation::Readback,
                                      .debug_name = "terrain args readback"});
            !made) {
            return core::fail(made.error());
        }
    }
    state->heights_address = desc.device->buffer_address(state->heights);
    state->draws_address = desc.device->buffer_address(state->draws);
    state->args_address = desc.device->buffer_address(state->args);

    // A patch: the grid's squares along one diagonal, then a side hanging from each edge, facing out.
    core::Array<uint16_t> indices(render_heap());
    indices.reserve(kGridIndices + kSkirtIndices);
    const auto grid = [](uint32_t x, uint32_t z) { return static_cast<uint16_t>((z * kGridSide) + x); };
    for (uint32_t z = 0; z < kPatchQuads; ++z) {
        for (uint32_t x = 0; x < kPatchQuads; ++x) {
            for (const uint16_t index :
                 {grid(x, z), grid(x, z + 1), grid(x + 1, z + 1), grid(x, z), grid(x + 1, z + 1), grid(x + 1, z)}) {
                indices.push(index);
            }
        }
    }
    const auto hanging = [](uint32_t side, uint32_t along) {
        return static_cast<uint16_t>(kGridVertices + (side * kGridSide) + along);
    };
    for (uint32_t k = 0; k < kPatchQuads; ++k) {
        const uint16_t north[2] = {grid(k, 0), grid(k + 1, 0)};
        const uint16_t south[2] = {grid(k + 1, kPatchQuads), grid(k, kPatchQuads)};
        const uint16_t west[2] = {grid(0, k + 1), grid(0, k)};
        const uint16_t east[2] = {grid(kPatchQuads, k), grid(kPatchQuads, k + 1)};
        const uint16_t hang_north[2] = {hanging(0, k), hanging(0, k + 1)};
        const uint16_t hang_south[2] = {hanging(1, k + 1), hanging(1, k)};
        const uint16_t hang_west[2] = {hanging(2, k + 1), hanging(2, k)};
        const uint16_t hang_east[2] = {hanging(3, k), hanging(3, k + 1)};
        const auto side = [&](const uint16_t (&top)[2], const uint16_t (&bottom)[2]) {
            for (const uint16_t index : {top[0], top[1], bottom[0], top[1], bottom[1], bottom[0]}) {
                indices.push(index);
            }
        };
        side(north, hang_north);
        side(south, hang_south);
        side(west, hang_west);
        side(east, hang_east);
    }
    if (const core::Result<void> queued = desc.uploader->upload_buffer(
            state->indices, 0, std::as_bytes(indices.span()), rhi::ResourceState::IndexBuffer, rhi::ShaderStages::All,
            rhi::BufferRange::Unread);
        !queued) {
        return core::fail(queued.error());
    }
    state->index_ticket = desc.uploader->buffer_upload_ticket();

    state->read_memory.resize(kMaxReads * asset::kTerrainNodeBytes);
    state->slot_owner.resize(desc.max_blocks);
    state->free_slots.reserve(desc.max_blocks);
    state->uploading.reserve(size_t{kMaxReads} * 4);
    state->retired.reserve(desc.max_blocks);
    return terrain;
}

Terrain::~Terrain() {
    if (state_ == nullptr) {
        return;
    }
    state_->wait_for_reads();
    state_->destroy_all();
    state_->~TerrainState();
    render_heap().deallocate(state_, sizeof(TerrainState), alignof(TerrainState));
}

Terrain::Terrain(Terrain&& other) noexcept : state_(other.state_) {
    other.state_ = nullptr;
}

Terrain& Terrain::operator=(Terrain&& other) noexcept {
    std::swap(state_, other.state_);
    return *this;
}

void Terrain::set(const asset::TerrainAsset* terrain, std::span<const std::byte> file) {
    TerrainState& state = *state_;
    state.wait_for_reads();
    state.device->wait_idle(); // what's in flight reads the slots about to be reused
    state.destroy_tables();
    state.terrain = nullptr;
    state.file = {};
    state.header_ok = false;
    state.uploading.clear();
    state.retired.clear();
    state.resident = 0;
    state.next_to_let_go = 0;
    state.counters = {};
    state.drawn = {};
    if (terrain == nullptr) {
        return;
    }
    const std::span<const asset::TerrainLevel> levels = terrain->levels.get(file.data());
    const std::span<const asset::TerrainNode> nodes = terrain->nodes.get(file.data());
    const std::span<const asset::TerrainPatch> patches = terrain->patches.get(file.data());

    state.nodes.clear();
    state.nodes.resize(nodes.size());
    state.slot_table.clear();
    state.slot_table.resize(nodes.size());
    std::ranges::fill(state.slot_table, kNoSlot);
    ++state.slot_table_version;
    std::ranges::fill(state.slot_owner, kNoSlot);
    state.free_slots.clear();
    for (uint32_t slot = state.max_blocks; slot-- > 0;) {
        state.free_slots.push(slot);
    }

    // Who's above whom.
    for (uint32_t l = 1; l < levels.size(); ++l) {
        const asset::TerrainLevel& level = levels[l];
        const asset::TerrainLevel& above = levels[l - 1];
        for (uint32_t z = 0; z < level.nodes_z; ++z) {
            for (uint32_t x = 0; x < level.nodes_x; ++x) {
                state.nodes[level.first_node + (z * level.nodes_x) + x].parent =
                    above.first_node + ((z / 2) * above.nodes_x) + (x / 2);
            }
        }
    }

    // What each node reaches, from its patches, to choose what to load without reading any.
    state.node_reach.clear();
    state.node_reach.resize(nodes.size() * 2);
    for (const asset::TerrainLevel& level : levels) {
        const uint32_t patches_x = level.nodes_x * asset::kTerrainPatchesPerNode;
        for (uint32_t z = 0; z < level.nodes_z; ++z) {
            for (uint32_t x = 0; x < level.nodes_x; ++x) {
                float low = std::numeric_limits<float>::max();
                float high = std::numeric_limits<float>::lowest();
                for (uint32_t pz = 0; pz < asset::kTerrainPatchesPerNode; ++pz) {
                    for (uint32_t px = 0; px < asset::kTerrainPatchesPerNode; ++px) {
                        const uint32_t patch = level.first_patch +
                                               (((z * asset::kTerrainPatchesPerNode) + pz) * patches_x) +
                                               (x * asset::kTerrainPatchesPerNode) + px;
                        low = std::min(low, patches[patch].min_m);
                        high = std::max(high, patches[patch].max_m);
                    }
                }
                const uint32_t node = level.first_node + (z * level.nodes_x) + x;
                state.node_reach[(node * 2) + 0] = low;
                state.node_reach[(node * 2) + 1] = high;
            }
        }
    }

    core::Array<GpuTerrainLevel> gpu_levels(render_heap());
    gpu_levels.reserve(levels.size());
    for (uint32_t l = 0; l < levels.size(); ++l) {
        const asset::TerrainLevel& level = levels[l];
        const float range = l == 0 ? std::numeric_limits<float>::infinity() : terrain_level_range_m(level.spacing_m);
        gpu_levels.push(GpuTerrainLevel{
            .nodes_x = level.nodes_x,
            .nodes_z = level.nodes_z,
            .first_node = level.first_node,
            .first_patch = level.first_patch,
            .spacing_steps =
                static_cast<uint32_t>(std::lround(static_cast<double>(level.spacing_m) * world::kRenderStepsPerM)),
            .spacing_m = level.spacing_m,
            .step_m = level.spacing_m / asset::kTerrainStepsPerSpacing,
            .range_m = range,
            // The root has nothing coarser: it never morphs, with no scale and a start that's nowhere near.
            .morph_start_m = l == 0 ? 0.0f : range * kTerrainMorphStart,
            .morph_scale = l == 0 ? 0.0f : 1.0f / (range * (1.0f - kTerrainMorphStart))});
    }

    const auto make_table = [&](rhi::BufferHandle& out, std::span<const std::byte> bytes, const char* name) {
        core::Result<rhi::BufferHandle> made = state.device->create_buffer(
            {.size = bytes.size(), .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst, .debug_name = name});
        if (!made) {
            DE_LOG_ERROR("terrain", "%s: %s", name, made.error().message);
            return false;
        }
        out = *made;
        const core::Result<void> queued = state.uploader->upload_buffer(
            out, 0, bytes, rhi::ResourceState::ShaderRead, rhi::ShaderStages::All, rhi::BufferRange::Unread);
        if (!queued) {
            DE_LOG_ERROR("terrain", "%s: %s", name, queued.error().message);
            return false;
        }
        return true;
    };
    if (!make_table(state.levels, std::as_bytes(gpu_levels.span()), "terrain levels") ||
        !make_table(state.nodes_buffer, std::as_bytes(nodes), "terrain nodes") ||
        !make_table(state.patches, std::as_bytes(patches), "terrain patches")) {
        state.destroy_tables();
        return;
    }
    state.tables_ticket = state.uploader->buffer_upload_ticket();
    for (uint32_t i = 0; i < rhi::kFramesInFlight; ++i) {
        core::Result<rhi::BufferHandle> made = state.device->create_buffer({.size = nodes.size() * sizeof(uint32_t),
                                                                            .usage = rhi::BufferUsage::Storage,
                                                                            .memory = rhi::MemoryLocation::Upload,
                                                                            .debug_name = "terrain slots"});
        if (!made) {
            DE_LOG_ERROR("terrain", "slots: %s", made.error().message);
            state.destroy_tables();
            return;
        }
        state.slots[i] = *made;
        state.slots_version[i] = 0;
    }

    const auto to_steps = [](double meters) { return static_cast<uint32_t>(world::render_position_steps(meters)); };
    state.origin_steps[0] = to_steps(terrain->origin_m[0]);
    state.origin_steps[1] = to_steps(terrain->origin_m[1]);
    state.size_steps[0] = to_steps(terrain->width_m);
    state.size_steps[1] = to_steps(terrain->depth_m);
    state.size_m[0] = terrain->width_m;
    state.size_m[1] = terrain->depth_m;
    state.level_count = static_cast<uint32_t>(levels.size());
    state.counters.blocks = terrain->block_count;

    state.terrain = terrain;
    state.file = file;
    state.heights_path = asset::cooked_path(terrain->header.id, asset::AssetType::TerrainHeights);
    const core::Result<platform::ReadHandle> header =
        state.vfs->read({.path = state.heights_path.c_str(),
                         .dest = std::as_writable_bytes(std::span(&state.heights_header, 1)),
                         .offset = 0,
                         .size = sizeof(asset::AssetHeader),
                         .priority = platform::IoPriority::Critical});
    if (!header) {
        DE_LOG_ERROR("terrain", "no heights: %s: %s", state.heights_path.c_str(), header.error().message);
        return;
    }
    state.header_read = *header;
    state.header_reading = true;
}

void Terrain::update(const DVec3& camera) {
    DE_PROFILE_ZONE("Terrain::update");
    TerrainState& state = *state_;
    if (state.terrain == nullptr) {
        return;
    }
    ++state.update_number;
    state.read_drawn();
    state.finish_header();
    if (!state.header_ok) {
        return;
    }
    state.collect_reads();
    state.publish_uploads();
    state.free_retired();
    terrain_wanted_nodes(*state.terrain, state.file, state.node_reach, camera, state.wanted);
    for (const TerrainWant& want : state.wanted) {
        state.nodes[want.node].last_wanted = state.update_number;
    }
    state.start_reads();
    state.counters.wanted = static_cast<uint32_t>(state.wanted.size());
    state.counters.resident = state.resident;
    state.counters.reading = 0;
    for (const Read& read : state.reads) {
        state.counters.reading += read.active ? 1 : 0;
    }
    state.counters.uploading = static_cast<uint32_t>(state.uploading.size());
}

TerrainFrame Terrain::add_select_passes(RenderGraph& graph, rhi::PipelineHandle select, uint64_t frame,
                                        uint64_t views) {
    TerrainState& state = *state_;
    if (!select.is_valid() || !state.tables_ready()) {
        return {};
    }
    // The table for this frame's copy, if it's behind: those a frame in flight still reads stay as they are.
    const auto slot = static_cast<uint32_t>(state.device->frame_index() % rhi::kFramesInFlight);
    if (state.slots_version[slot] != state.slot_table_version) {
        const std::span<std::byte> mapped = state.device->mapped(state.slots[slot]);
        std::memcpy(mapped.data(), state.slot_table.data(), state.slot_table.size() * sizeof(uint32_t));
        state.device->flush_mapped(state.slots[slot]);
        state.slots_version[slot] = state.slot_table_version;
    }

    const rhi::ResourceState was_args =
        state.buffers_read ? rhi::ResourceState::IndirectArgument : rhi::ResourceState::Undefined;
    const rhi::ResourceState was_draws =
        state.buffers_read ? rhi::ResourceState::ShaderRead : rhi::ResourceState::Undefined;
    state.buffers_read = true;
    TerrainFrame out;
    out.args = graph.import_buffer({.buffer = state.args,
                                    .name = "terrain args",
                                    .state = was_args,
                                    .final_state = rhi::ResourceState::IndirectArgument});
    out.draws = graph.import_buffer({.buffer = state.draws,
                                     .name = "terrain draws",
                                     .state = was_draws,
                                     .final_state = rhi::ResourceState::ShaderRead});
    out.valid = true;

    graph.add_pass(
        "terrain clear", [&](PassBuilder& pass) { pass.use(out.args, rhi::ResourceState::CopyDst); },
        [args = out.args](PassContext& context) {
            const rhi::BufferHandle buffer = context.buffer(args);
            // Each word once: fills over the same bytes have no order between them. The camera draws the sides that
            // hide gaps; a shadow map has no use for them.
            for (uint32_t view = 0; view < kViewCount; ++view) {
                const uint64_t first = uint64_t{view} * kArgsWords * sizeof(uint32_t);
                context.list.fill_buffer(buffer, first, sizeof(uint32_t),
                                         view == 0 ? kGridIndices + kSkirtIndices : kGridIndices);
                context.list.fill_buffer(buffer, first + sizeof(uint32_t), (kArgsWords - 1) * sizeof(uint32_t), 0);
            }
        });
    graph.add_pass(
        "terrain select",
        [&](PassBuilder& pass) {
            pass.use(out.args, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            pass.use(out.draws, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
        },
        [&state, select, frame, views, slot](PassContext& context) {
            DE_PROFILE_ZONE("terrain select pass");
            const std::span<const asset::TerrainLevel> levels = state.terrain->levels.get(state.file.data());
            TerrainSelectPush push{.frame = frame,
                                   .views = views,
                                   .levels = state.device->buffer_address(state.levels),
                                   .patches = state.device->buffer_address(state.patches),
                                   .nodes = state.device->buffer_address(state.nodes_buffer),
                                   .slots = state.device->buffer_address(state.slots[slot]),
                                   .draws = state.draws_address,
                                   .args = state.args_address,
                                   .origin_steps = {state.origin_steps[0], state.origin_steps[1]},
                                   .size_m = {state.size_m[0], state.size_m[1]},
                                   .level = 0,
                                   .level_count = state.level_count,
                                   .list_capacity = kListCapacity};
            context.list.bind_pipeline(select);
            for (uint32_t l = 0; l < levels.size(); ++l) {
                push.level = l;
                context.list.push_constants(push);
                const uint32_t patches_x = levels[l].nodes_x * asset::kTerrainPatchesPerNode;
                const uint32_t patches_z = levels[l].nodes_z * asset::kTerrainPatchesPerNode;
                context.list.dispatch((patches_x + 7) / 8, (patches_z + 7) / 8);
            }
        });
    return out;
}

void Terrain::use_draws(PassBuilder& pass, const TerrainFrame& frame) const {
    if (frame.valid) {
        pass.use(frame.args, rhi::ResourceState::IndirectArgument);
        pass.use(frame.draws, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Vertex);
    }
}

void Terrain::draw_gbuffer(PassContext& context, const TerrainFrame& frame, rhi::PipelineHandle pipeline,
                           uint64_t frame_address, bool show_levels) const {
    const TerrainState& state = *state_;
    if (!frame.valid || !pipeline.is_valid()) {
        return;
    }
    const TerrainDrawPush push{.frame = frame_address,
                               .draws = state.draws_address,
                               .levels = state.device->buffer_address(state.levels),
                               .heights = state.heights_address,
                               .origin_steps = {state.origin_steps[0], state.origin_steps[1]},
                               .size_steps = {state.size_steps[0], state.size_steps[1]},
                               .cascade = 0,
                               .show_levels = show_levels ? 1U : 0U};
    context.list.bind_pipeline(pipeline);
    context.list.push_constants(push);
    context.list.bind_index_buffer(state.indices, 0, rhi::IndexType::Uint16);
    context.list.draw_indexed_indirect(context.buffer(frame.args), 0, 1);
}

void Terrain::draw_shadow(PassContext& context, const TerrainFrame& frame, rhi::PipelineHandle pipeline,
                          uint64_t frame_address, uint32_t cascade) const {
    const TerrainState& state = *state_;
    if (!frame.valid || !pipeline.is_valid()) {
        return;
    }
    const uint64_t list = uint64_t{cascade + 1} * kListCapacity * sizeof(GpuTerrainDraw);
    const TerrainDrawPush push{.frame = frame_address,
                               .draws = state.draws_address + list,
                               .levels = state.device->buffer_address(state.levels),
                               .heights = state.heights_address,
                               .origin_steps = {state.origin_steps[0], state.origin_steps[1]},
                               .size_steps = {state.size_steps[0], state.size_steps[1]},
                               .cascade = cascade,
                               .show_levels = 0};
    context.list.bind_pipeline(pipeline);
    context.list.push_constants(push);
    context.list.bind_index_buffer(state.indices, 0, rhi::IndexType::Uint16);
    context.list.draw_indexed_indirect(context.buffer(frame.args),
                                       uint64_t{cascade + 1} * kArgsWords * sizeof(uint32_t), 1);
}

void Terrain::add_stats_pass(RenderGraph& graph, const TerrainFrame& frame) {
    TerrainState& state = *state_;
    if (!frame.valid) {
        return;
    }
    const auto slot = static_cast<uint32_t>(state.device->frame_index() % rhi::kFramesInFlight);
    const GraphBuffer readback = graph.import_buffer(
        {.buffer = state.readback[slot],
         .name = "terrain args readback",
         .state = state.readback_used[slot] ? rhi::ResourceState::HostRead : rhi::ResourceState::Undefined,
         .final_state = rhi::ResourceState::HostRead});
    state.readback_used[slot] = true;
    graph.add_pass(
        "terrain stats",
        [&](PassBuilder& pass) {
            pass.use(frame.args, rhi::ResourceState::CopySrc);
            pass.use(readback, rhi::ResourceState::CopyDst);
        },
        [args = frame.args, readback](PassContext& context) {
            context.list.copy_buffer(context.buffer(args), 0, context.buffer(readback), 0, kArgsBytes);
        });
}

TerrainStats Terrain::stats() const {
    TerrainStats out = state_->counters;
    out.patches = state_->drawn.patches;
    out.shadow_patches = state_->drawn.shadow_patches;
    out.triangles = state_->drawn.triangles;
    out.shadow_triangles = state_->drawn.shadow_triangles;
    return out;
}

} // namespace de::render
