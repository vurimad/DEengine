#include "physics/terrain_collision.h"

#include "asset/format.h"
#include "core/job_system.h"
#include "core/log.h"
#include "core/profile.h"
#include "platform/time.h"
#include "platform/vfs.h"

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <new>
#include <span>

namespace de::physics {

namespace {

constexpr uint32_t kSamples = asset::kTerrainNodeQuads + 1; // a node's own, its border left out
constexpr float kMaxErrorM = 0.01f;                         // what Jolt's compression of a sample may move it
constexpr float kFriction = 0.8f;

enum class TileState : uint8_t { Reading, Ready, Failed, Live };

} // namespace

struct TerrainTile {
    uint32_t node = 0;
    uint32_t block = asset::kTerrainNoBlock;
    double corner_x = 0.0; // its north-west sample, world meters
    double corner_z = 0.0;
    double base_m = 0.0; // what its samples' heights add to
    float spacing_m = 0.0f;
    float step_m = 0.0f;
    uint64_t wanted_tick = 0;
    std::atomic<TileState> state{TileState::Reading};
    core::JobCounter counter;
    JPH::RefConst<JPH::Shape> shape;
    JPH::BodyID body;
    uint16_t samples[asset::kTerrainNodeSamples * asset::kTerrainNodeSamples] = {};
};

namespace {

// The tile's height field, from its block: on a job (or now, without one).
void build(TerrainTile& tile) {
    DE_PROFILE_ZONE("terrain collision build");
    JPH::Array<float> heights(static_cast<size_t>(kSamples) * kSamples);
    for (uint32_t row = 0; row < kSamples; ++row) {
        for (uint32_t column = 0; column < kSamples; ++column) {
            const uint16_t sample = tile.samples[((row + asset::kTerrainNodeBorder) * asset::kTerrainNodeSamples) +
                                                 column + asset::kTerrainNodeBorder];
            heights[(static_cast<size_t>(row) * kSamples) + column] = static_cast<float>(sample) * tile.step_m;
        }
    }
    // Jolt's height field: x along X (east), its rows along Z (south), as the block's are.
    JPH::HeightFieldShapeSettings settings(heights.data(), JPH::Vec3::sZero(),
                                           JPH::Vec3(tile.spacing_m, 1.0f, tile.spacing_m), kSamples);
    settings.mBitsPerSample = settings.CalculateBitsPerSampleForError(kMaxErrorM);
    const JPH::ShapeSettings::ShapeResult made = settings.Create();
    if (made.HasError()) {
        DE_LOG_WARN("physics", "the ground's collision for terrain node %u wasn't made: %s", tile.node,
                    made.GetError().c_str());
        tile.state.store(TileState::Failed, std::memory_order_release);
        return;
    }
    tile.shape = made.Get();
    tile.state.store(TileState::Ready, std::memory_order_release);
}

void on_read(void* context, const core::Result<size_t>& read) {
    auto& tile = *static_cast<TerrainTile*>(context);
    if (!read || *read != sizeof(tile.samples)) {
        tile.state.store(TileState::Failed, std::memory_order_release);
        return;
    }
    build(tile);
}

// Blocking: the bytes at offset in path, for what has no job system.
bool read_now(platform::Vfs& vfs, const char* path, uint64_t offset, std::span<std::byte> dest) {
    const core::Result<platform::ReadHandle> handle =
        vfs.read({.path = path, .dest = dest, .offset = offset, .size = dest.size()});
    if (!handle) {
        return false;
    }
    for (platform::ReadStatus status = vfs.status(*handle);
         status == platform::ReadStatus::Pending || status == platform::ReadStatus::InFlight;
         status = vfs.status(*handle)) {
        platform::sleep_ns(20'000);
    }
    const core::Result<size_t> read = vfs.finish(*handle);
    return read && *read == dest.size();
}

} // namespace

TerrainColliders::TerrainColliders(core::JobSystem* jobs, core::Allocator& allocator)
    : jobs_(jobs), allocator_(&allocator), tiles_(allocator), needed_(allocator), adding_(allocator) {}

TerrainColliders::~TerrainColliders() {
    for (auto [node, tile] : tiles_) {
        destroy(tile);
    }
}

void TerrainColliders::set_terrain(const asset::TerrainAsset* terrain, platform::Vfs* vfs, JPH::PhysicsSystem& system) {
    if (terrain == terrain_ && vfs == vfs_ && (terrain == nullptr || terrain->heights_hash == heights_hash_)) {
        return;
    }
    remove_all(system);
    terrain_ = terrain != nullptr && vfs != nullptr && !terrain->levels.get(terrain).empty() ? terrain : nullptr;
    vfs_ = vfs;
    heights_hash_ = terrain_ != nullptr ? terrain_->heights_hash : 0;
    heights_ok_ = false;
    if (terrain_ == nullptr) {
        return;
    }
    // Its heights file, checked once against it.
    const asset::CookedPath path = asset::cooked_path(terrain_->header.id, asset::AssetType::TerrainHeights);
    asset::AssetHeader header;
    const core::Result<platform::FileInfo> info = vfs_->file_info(path.c_str());
    if (!info || !read_now(*vfs_, path.c_str(), 0, std::as_writable_bytes(std::span(&header, 1)))) {
        DE_LOG_WARN("physics", "no collision for the ground: %s isn't there (run the cooker)", path.c_str());
        return;
    }
    if (const core::Result<void> fits = terrain_->check_heights_header(header, info->size); !fits) {
        DE_LOG_WARN("physics", "no collision for the ground: %s: %s (run the cooker)", path.c_str(),
                    fits.error().message);
        return;
    }
    heights_ok_ = true;
}

void TerrainColliders::want(const DVec3& anchor, bool need) {
    if (terrain_ == nullptr || !heights_ok_) {
        return;
    }
    const asset::TerrainLevel& level = terrain_->levels.get(terrain_).back();
    const double node_m = static_cast<double>(level.spacing_m) * asset::kTerrainNodeQuads;
    const double east = anchor.x - terrain_->origin_m[0];
    const double south = anchor.z - terrain_->origin_m[1];
    const auto first = [node_m](double at, uint32_t count) {
        return static_cast<uint32_t>(std::clamp(std::floor((at - kKeepM) / node_m), 0.0, count - 1.0));
    };
    const auto last = [node_m](double at, uint32_t count) {
        return static_cast<uint32_t>(std::clamp(std::floor((at + kKeepM) / node_m), 0.0, count - 1.0));
    };
    if (east + kKeepM < 0.0 || south + kKeepM < 0.0 || east - kKeepM > level.nodes_x * node_m ||
        south - kKeepM > level.nodes_z * node_m) {
        return; // nowhere near the map
    }
    for (uint32_t z = first(south, level.nodes_z); z <= last(south, level.nodes_z); ++z) {
        for (uint32_t x = first(east, level.nodes_x); x <= last(east, level.nodes_x); ++x) {
            // How far the anchor is from the node's square.
            const double dx = std::max({(x * node_m) - east, 0.0, east - ((x + 1) * node_m)});
            const double dz = std::max({(z * node_m) - south, 0.0, south - ((z + 1) * node_m)});
            const double distance = std::sqrt((dx * dx) + (dz * dz));
            if (distance > kKeepM) {
                continue;
            }
            const uint32_t node = (z * level.nodes_x) + x;
            TerrainTile* tile = nullptr;
            if (TerrainTile** found = tiles_.find(node); found != nullptr) {
                tile = *found;
            } else {
                tile = start(node);
                if (tile == nullptr) {
                    continue;
                }
            }
            tile->wanted_tick = tick_;
            if (need && distance <= kNeedM && tile->state.load(std::memory_order_acquire) == TileState::Reading) {
                needed_.push(tile);
            }
        }
    }
}

TerrainTile* TerrainColliders::start(uint32_t node) {
    const asset::TerrainLevel& level = terrain_->levels.get(terrain_).back();
    const asset::TerrainNode& data = terrain_->nodes.get(terrain_)[level.first_node + node];
    if (data.block == asset::kTerrainNoBlock) {
        return nullptr;
    }
    auto* tile = ::new (allocator_->allocate(sizeof(TerrainTile), alignof(TerrainTile))) TerrainTile();
    const uint32_t x = node % level.nodes_x;
    const uint32_t z = node / level.nodes_x;
    const double node_m = static_cast<double>(level.spacing_m) * asset::kTerrainNodeQuads;
    tile->node = node;
    tile->block = data.block;
    tile->spacing_m = level.spacing_m;
    tile->step_m = level.spacing_m / asset::kTerrainStepsPerSpacing;
    tile->corner_x = terrain_->origin_m[0] + (x * node_m);
    tile->corner_z = terrain_->origin_m[1] + (z * node_m);
    tile->base_m = static_cast<double>(data.base_steps) * tile->step_m;
    tile->wanted_tick = tick_;
    tiles_.set(node, tile);
    const asset::CookedPath path = asset::cooked_path(terrain_->header.id, asset::AssetType::TerrainHeights);
    const std::span<std::byte> dest = std::as_writable_bytes(std::span(tile->samples));
    if (jobs_ != nullptr && vfs_->jobs() != nullptr) {
        // Read and built on the job system; finished there.
        const core::Result<platform::ReadHandle> reading =
            vfs_->read({.path = path.c_str(),
                        .dest = dest,
                        .offset = asset::terrain_block_offset(data.block),
                        .size = dest.size(),
                        .priority = platform::IoPriority::Near,
                        .on_complete = &on_read,
                        .context = tile,
                        .counter = &tile->counter});
        if (!reading) {
            tile->state.store(TileState::Failed, std::memory_order_release);
        }
        return tile;
    }
    if (read_now(*vfs_, path.c_str(), asset::terrain_block_offset(data.block), dest)) {
        build(*tile);
    } else {
        tile->state.store(TileState::Failed, std::memory_order_release);
    }
    return tile;
}

void TerrainColliders::finish_now(TerrainTile& tile) {
    if (jobs_ != nullptr && tile.state.load(std::memory_order_acquire) == TileState::Reading) {
        jobs_->wait(tile.counter);
    }
}

void TerrainColliders::update(JPH::PhysicsSystem& system, JPH::ObjectLayer layer) {
    DE_PROFILE_ZONE("terrain collision");
    // What something stands or falls on: built before the step.
    for (TerrainTile* tile : needed_) {
        finish_now(*tile);
    }
    needed_.clear();
    JPH::BodyInterface& bodies = system.GetBodyInterfaceNoLock();
    adding_.clear();
    core::Array<uint32_t> gone(*allocator_);
    for (auto [node, tile] : tiles_) {
        const TileState state = tile->state.load(std::memory_order_acquire);
        const bool unwanted = tick_ > tile->wanted_tick + kForgetTicks;
        if (state != TileState::Reading && unwanted) {
            gone.push(node); // built after nothing wanted it any more, too: never added
        } else if (state == TileState::Ready) {
            JPH::BodyCreationSettings settings(tile->shape.GetPtr(),
                                               JPH::RVec3(tile->corner_x, tile->base_m, tile->corner_z),
                                               JPH::Quat::sIdentity(), JPH::EMotionType::Static, layer);
            settings.mFriction = kFriction;
            settings.mUserData = 0; // the ground: no entity
            const JPH::Body* body = bodies.CreateBody(settings);
            if (body != nullptr) {
                tile->body = body->GetID();
                adding_.push(tile->body);
                tile->state.store(TileState::Live, std::memory_order_release);
                ++live_;
            }
        }
    }
    for (const JPH::BodyID id : adding_) {
        bodies.AddBody(id, JPH::EActivation::DontActivate);
    }
    for (const uint32_t node : gone) {
        TerrainTile* const* found = tiles_.find(node);
        TerrainTile* tile = *found;
        if (!tile->body.IsInvalid()) {
            bodies.RemoveBody(tile->body);
            bodies.DestroyBody(tile->body);
            --live_;
        }
        destroy(tile);
        tiles_.remove(node);
    }
    ++tick_;
}

void TerrainColliders::remove_all(JPH::PhysicsSystem& system) {
    JPH::BodyInterface& bodies = system.GetBodyInterfaceNoLock();
    for (auto [node, tile] : tiles_) {
        finish_now(*tile);
        if (!tile->body.IsInvalid()) {
            bodies.RemoveBody(tile->body);
            bodies.DestroyBody(tile->body);
        }
        destroy(tile);
    }
    tiles_.clear();
    needed_.clear();
    live_ = 0;
}

void TerrainColliders::destroy(TerrainTile* tile) {
    finish_now(*tile);
    tile->~TerrainTile();
    allocator_->deallocate(tile, sizeof(TerrainTile), alignof(TerrainTile));
}

} // namespace de::physics
