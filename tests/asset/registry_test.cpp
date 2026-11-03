#include "asset/material_format.h"
#include "asset/mesh_format.h"
#include "asset/registry.h"
#include "asset/texture_format.h"
#include "cooker/material_cooker.h"
#include "cooker/mesh_cooker.h"
#include "cooker/texture_cooker.h"
#include "core/allocator.h"
#include "core/job_system.h"
#include "core/memory_stats.h"
#include "platform/vfs.h"
#include "support/sandbox.h"

#include <doctest/doctest.h>

#include <vector>

using namespace de::asset;
using de::platform::Vfs;
using de::test::Sandbox;

namespace {

constexpr AssetId kMesh = AssetId::from_value(0x1000);
constexpr AssetId kMaterial = AssetId::from_value(0x2000);
constexpr AssetId kAlbedo = AssetId::from_value(0x3000);
constexpr AssetId kMissing = AssetId::from_value(0x4000);
constexpr AssetId kDetail = AssetId::from_value(0x5000);

void put(const Vfs& vfs, AssetId id, AssetType type, const de::core::Array<std::byte>& bytes) {
    REQUIRE(vfs.write_file(cooked_path(id, type).c_str(), bytes.span()).ok());
}

void put_texture(const Vfs& vfs, AssetId id, uint32_t size) {
    de::cooker::Image image;
    image.width = size;
    image.height = size;
    image.rgba.resize(size_t{size} * size * 4, 200);
    de::cooker::Diagnostics diagnostics;
    const auto cooked = de::cooker::cook_texture(image, {}, id, nullptr, "test", diagnostics);
    REQUIRE(cooked.ok());
    put(vfs, id, AssetType::Texture, *cooked);
}

void put_material(const Vfs& vfs, AssetId id, AssetId base_color, AssetId normal) {
    de::cooker::MaterialBuild build;
    build.set_texture(material::kBaseColorTexture, base_color);
    build.set_texture(material::kNormalTexture, normal);
    put(vfs, id, AssetType::Material, de::cooker::cook_material(build, id));
}

void put_mesh(const Vfs& vfs, AssetId id, AssetId material_id) {
    de::cooker::ModelData model;
    model.materials.emplace().name.assign("paint");
    de::cooker::ModelPrimitive& primitive = model.primitives.emplace();
    primitive.material = 0;
    for (int i = 0; i < 3; ++i) {
        de::cooker::ModelVertex& vertex = primitive.vertices.emplace();
        vertex.position[0] = static_cast<float>(i);
        vertex.position[1] = static_cast<float>(i % 2);
        primitive.indices.push(static_cast<uint32_t>(i));
    }
    const AssetId materials[] = {material_id};
    de::cooker::Diagnostics diagnostics;
    const auto cooked = de::cooker::cook_mesh(model, materials, {}, id, "test", diagnostics);
    REQUIRE(cooked.ok());
    put(vfs, id, AssetType::Mesh, *cooked);
}

// A mesh, its material, and the material's two textures, one of them never cooked.
void put_scene(const Vfs& vfs) {
    put_texture(vfs, kAlbedo, 16);
    put_material(vfs, kMaterial, kAlbedo, kMissing);
    put_mesh(vfs, kMesh, kMaterial);
}

// Every event, in order.
struct Recorder {
    std::vector<AssetNotice> notices;

    static void record(void* context, const AssetNotice& notice) {
        static_cast<Recorder*>(context)->notices.push_back(notice);
    }
    size_t count(AssetEvent event, AssetId id) const {
        size_t n = 0;
        for (const AssetNotice& notice : notices) {
            n += notice.event == event && notice.asset.id == id ? 1 : 0;
        }
        return n;
    }
    // Where the first event of a kind for an asset is in the order; SIZE_MAX if never.
    size_t first(AssetEvent event, AssetId id) const {
        for (size_t i = 0; i < notices.size(); ++i) {
            if (notices[i].event == event && notices[i].asset.id == id) {
                return i;
            }
        }
        return SIZE_MAX;
    }
};

struct Fixture {
    Sandbox sandbox;
    de::core::JobSystem jobs{{.worker_count = 2}};
    Vfs vfs = sandbox.make_vfs({.jobs = &jobs, .game_writable = true});
    Recorder recorder;

    AssetRegistry make(AssetRegistryDesc desc = {}) {
        desc.vfs = &vfs;
        desc.jobs = &jobs;
        auto registry = AssetRegistry::create(desc);
        REQUIRE(registry.ok());
        REQUIRE(registry->add_listener(&Recorder::record, &recorder).ok());
        return std::move(registry).value();
    }
};

AssetHandle dependency_of(const AssetRegistry& registry, AssetHandle handle, AssetId id) {
    for (const AssetHandle dependency : registry.dependencies(handle)) {
        if (registry.asset(dependency).id == id) {
            return dependency;
        }
    }
    return {};
}

} // namespace

TEST_CASE("an asset loads by ID with everything it depends on; a missing one fails alone") {
    Fixture fixture;
    put_scene(fixture.vfs);
    AssetRegistry registry = fixture.make({.hot_reload = false});
    const AssetHandle mesh = registry.load<MeshAsset>(kMesh);
    CHECK(registry.state(mesh) == AssetState::Loading);
    CHECK(registry.get<MeshAsset>(mesh) == nullptr);
    registry.wait_all();

    REQUIRE(registry.state(mesh) == AssetState::Ready);
    const auto* asset = registry.get<MeshAsset>(mesh);
    REQUIRE(asset != nullptr);
    CHECK(asset->header.id == kMesh);
    CHECK(asset->indices.count == 3);
    CHECK(registry.get<TextureAsset>(mesh) == nullptr); // another type
    CHECK(registry.file(mesh).size() == asset->header.file_size);

    // mesh -> material -> two textures, one never cooked.
    const AssetHandle material = dependency_of(registry, mesh, kMaterial);
    REQUIRE(material.is_valid());
    CHECK(registry.state(material) == AssetState::Ready);
    const AssetHandle albedo = dependency_of(registry, material, kAlbedo);
    const AssetHandle missing = dependency_of(registry, material, kMissing);
    CHECK(registry.state(albedo) == AssetState::Ready);
    CHECK(registry.state(missing) == AssetState::Failed);
    CHECK(registry.get<TextureAsset>(missing) == nullptr);
    CHECK(registry.get<TextureAsset>(albedo)->width == 16);
    CHECK(registry.find({.id = kAlbedo, .type = AssetType::Texture}) == albedo);

    // Ready only once what it depends on is settled: textures, then the material, then the mesh.
    const Recorder& events = fixture.recorder;
    CHECK(events.count(AssetEvent::Failed, kMissing) == 1);
    CHECK(events.first(AssetEvent::Ready, kAlbedo) < events.first(AssetEvent::Ready, kMaterial));
    CHECK(events.first(AssetEvent::Failed, kMissing) < events.first(AssetEvent::Ready, kMaterial));
    CHECK(events.first(AssetEvent::Ready, kMaterial) < events.first(AssetEvent::Ready, kMesh));

    const AssetRegistryStats stats = registry.stats();
    CHECK(stats.assets == 4);
    CHECK(stats.failed == 1);
    CHECK(stats.loading == 0);
    CHECK(stats.bytes > 0);
}

TEST_CASE("assets stay while anything holds them, and go with their last holder") {
    Fixture fixture;
    put_scene(fixture.vfs);
    AssetRegistry registry = fixture.make({.hot_reload = false});
    // The material twice directly, and once more through the mesh.
    const AssetHandle material = registry.load<MaterialAsset>(kMaterial);
    CHECK(registry.load<MaterialAsset>(kMaterial) == material);
    const AssetHandle mesh = registry.load<MeshAsset>(kMesh);
    registry.wait_all();
    REQUIRE(registry.state(mesh) == AssetState::Ready);
    CHECK(dependency_of(registry, mesh, kMaterial) == material);

    registry.release(material);
    registry.release(material);
    registry.update();
    CHECK(registry.state(material) == AssetState::Ready); // the mesh still holds it
    CHECK(fixture.recorder.count(AssetEvent::Unloaded, kMaterial) == 0);

    registry.release(mesh);
    registry.update();
    CHECK(registry.stats().assets == 0);
    CHECK(registry.stats().bytes == 0);
    CHECK(registry.stats().failed == 0);
    CHECK(registry.state(mesh) == AssetState::Failed); // stale
    CHECK(registry.get<MeshAsset>(mesh) == nullptr);
    for (const AssetId id : {kMesh, kMaterial, kAlbedo, kMissing}) {
        CHECK(fixture.recorder.count(AssetEvent::Unloaded, id) == 1);
    }

    // Held again after it was let go, before an update: it stays.
    const AssetHandle again = registry.load<MeshAsset>(kMesh);
    registry.wait_all();
    registry.release(again);
    CHECK(registry.load<MeshAsset>(kMesh) == again);
    registry.update();
    CHECK(registry.state(again) == AssetState::Ready);
    registry.release(again);
    registry.update();
    CHECK(registry.stats().assets == 0);
}

TEST_CASE("an asset let go while it loads is unloaded once its read is in") {
    Fixture fixture;
    put_scene(fixture.vfs);
    AssetRegistry registry = fixture.make({.hot_reload = false});
    const AssetHandle mesh = registry.load<MeshAsset>(kMesh);
    registry.release(mesh);
    registry.update(); // the read may still be in flight
    registry.wait_all();
    registry.update();
    CHECK(registry.stats().assets == 0);
    CHECK(registry.stats().loading == 0);
}

TEST_CASE("wrong types, bad IDs and corrupt files fail with a reason, and nothing else breaks") {
    Fixture fixture;
    put_scene(fixture.vfs);
    const de::core::Array<std::byte> garbage = [] {
        de::core::Array<std::byte> bytes;
        bytes.resize(256, std::byte{0x5a});
        return bytes;
    }();
    put(fixture.vfs, kDetail, AssetType::Texture, garbage);
    AssetRegistry registry = fixture.make({.hot_reload = false});
    const AssetHandle as_texture = registry.load<TextureAsset>(kMesh); // it's a mesh
    const AssetHandle invalid = registry.load({.id = AssetId(), .type = AssetType::Texture});
    const AssetHandle no_type = registry.load({.id = kAlbedo, .type = AssetType::Unknown});
    const AssetHandle corrupt = registry.load<TextureAsset>(kDetail);
    registry.wait_all();
    CHECK(registry.state(as_texture) == AssetState::Failed);
    CHECK(registry.state(invalid) == AssetState::Failed);
    CHECK(registry.state(no_type) == AssetState::Failed);
    CHECK(registry.state(corrupt) == AssetState::Failed);
    CHECK(registry.stats().failed == 4);
    for (const AssetHandle handle : {as_texture, invalid, no_type, corrupt}) {
        registry.release(handle);
    }
    registry.update();
    CHECK(registry.stats().assets == 0);
    CHECK(registry.stats().failed == 0);
}

TEST_CASE("a re-cooked file is loaded again in place, and a missing one is picked up when it appears") {
    Fixture fixture;
    put_scene(fixture.vfs);
    AssetRegistry registry = fixture.make({.hot_reload = true, .reload_checks_per_update = 64});
    const AssetHandle mesh = registry.load<MeshAsset>(kMesh);
    registry.wait_all();
    const AssetHandle material = dependency_of(registry, mesh, kMaterial);
    const AssetHandle albedo = dependency_of(registry, material, kAlbedo);
    REQUIRE(registry.get<TextureAsset>(albedo)->width == 16);

    const auto settle = [&] {
        for (int i = 0; i < 4; ++i) {
            registry.update(); // checks files, starts reads
            registry.wait_all();
        }
    };

    // The texture re-cooked smaller: same handle, new contents.
    put_texture(fixture.vfs, kAlbedo, 8);
    settle();
    CHECK(fixture.recorder.count(AssetEvent::Reloaded, kAlbedo) == 1);
    REQUIRE(registry.get<TextureAsset>(albedo) != nullptr);
    CHECK(registry.get<TextureAsset>(albedo)->width == 8);
    CHECK(registry.stats().reloads == 1);

    // The missing texture appears: it loads (the material using it was Ready all along).
    put_texture(fixture.vfs, kMissing, 4);
    settle();
    const AssetHandle missing = dependency_of(registry, material, kMissing);
    CHECK(registry.state(missing) == AssetState::Ready);
    CHECK(fixture.recorder.count(AssetEvent::Ready, kMissing) == 1);
    CHECK(registry.stats().failed == 0);

    // The material re-cooked with other textures: it holds the new one, lets the old one go.
    put_texture(fixture.vfs, kDetail, 4);
    put_material(fixture.vfs, kMaterial, kAlbedo, kDetail);
    settle();
    CHECK(fixture.recorder.count(AssetEvent::Reloaded, kMaterial) == 1);
    CHECK(dependency_of(registry, material, kDetail).is_valid());
    CHECK_FALSE(dependency_of(registry, material, kMissing).is_valid());
    CHECK(fixture.recorder.count(AssetEvent::Unloaded, kMissing) == 1);
    CHECK(registry.state(dependency_of(registry, material, kDetail)) == AssetState::Ready);

    // A broken re-cook keeps the version that works.
    const de::core::Array<std::byte> broken = [] {
        de::core::Array<std::byte> bytes;
        bytes.resize(300, std::byte{1});
        return bytes;
    }();
    put(fixture.vfs, kAlbedo, AssetType::Texture, broken);
    settle();
    CHECK(fixture.recorder.count(AssetEvent::Reloaded, kAlbedo) == 1);
    REQUIRE(registry.get<TextureAsset>(albedo) != nullptr);
    CHECK(registry.get<TextureAsset>(albedo)->width == 8);

    registry.release(mesh);
    registry.update();
    CHECK(registry.stats().assets == 0);
}

TEST_CASE("an idle update allocates nothing, hot reload checks included") {
    Fixture fixture;
    put_scene(fixture.vfs);
    for (const bool hot_reload : {false, true}) {
        CAPTURE(hot_reload);
        AssetRegistry registry = fixture.make({.hot_reload = hot_reload});
        const AssetHandle mesh = registry.load<MeshAsset>(kMesh);
        registry.wait_all();
        for (int i = 0; i < 4; ++i) {
            registry.update(); // the first checks of each file
        }
        const uint64_t before = de::core::heap_allocation_count();
        for (int i = 0; i < 200; ++i) {
            registry.update();
        }
        CHECK(de::core::heap_allocation_count() - before == 0);
        registry.release(mesh);
    }
}

TEST_CASE("a registry can go while reads are in flight") {
    Fixture fixture;
    put_scene(fixture.vfs);
    const uint64_t live = de::core::memory_stats(de::core::MemoryTag::Streaming).live_allocations;
    for (int round = 0; round < 20; ++round) {
        AssetRegistry registry = fixture.make({.hot_reload = false});
        static_cast<void>(registry.load<MeshAsset>(kMesh));
        static_cast<void>(registry.load<TextureAsset>(kAlbedo));
    } // destroyed with loads in flight and holds outstanding
    CHECK(de::core::memory_stats(de::core::MemoryTag::Streaming).live_allocations == live); // all freed
}
