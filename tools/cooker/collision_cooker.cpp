#include "cooker/collision_cooker.h"

#include "asset/mesh_format.h"
#include "cooker/blob_writer.h"
#include "core/log.h"
#include "core/profile.h"
#include "physics/jolt_shapes.h"

#include <meshoptimizer.h>

#include <cstring>

namespace de::cooker {

namespace {

uint64_t triangles_of(const asset::MeshLod& lod, std::span<const asset::MeshPart> parts) {
    uint64_t indices = 0;
    for (const asset::MeshPart& part : parts.subspan(lod.first_part, lod.part_count)) {
        indices += part.index_count;
    }
    return indices / 3;
}

} // namespace

core::Result<core::Array<std::byte>> cook_collision(std::span<const std::byte> mesh_file, const MeshSettings& settings,
                                                    asset::AssetId id, std::string_view path,
                                                    Diagnostics& diagnostics) {
    DE_PROFILE_ZONE("cook collision");
    // Cooked files are read in aligned memory, and this one came from an array of bytes.
    struct alignas(asset::kAssetAlignment) Block {
        std::byte bytes[asset::kAssetAlignment];
    };
    core::Array<Block> aligned(tools_heap());
    aligned.resize_uninit((mesh_file.size() + sizeof(Block) - 1) / sizeof(Block));
    std::memcpy(aligned.data(), mesh_file.data(), mesh_file.size());
    const std::span<const std::byte> file(reinterpret_cast<const std::byte*>(aligned.data()), mesh_file.size());
    const core::Result<const asset::MeshAsset*> loaded = asset::load_in_place<asset::MeshAsset>(file);
    if (!loaded) {
        diagnostics.error(path, "its cooked mesh can't be read for its collision: %s", loaded.error().message);
        return core::fail(loaded.error());
    }
    const asset::MeshAsset& mesh = **loaded;
    const std::span<const asset::MeshLod> lods = mesh.lods.get(file.data());
    const std::span<const asset::MeshPart> parts = mesh.parts.get(file.data());
    const std::span<const asset::MeshVertex> vertices = mesh.vertices.get(file.data());
    const std::span<const uint32_t> indices = mesh.indices.get(file.data());

    size_t chosen = lods.size() - 1;
    for (size_t lod = 0; lod < lods.size(); ++lod) {
        if (triangles_of(lods[lod], parts) <= settings.collision_triangles) {
            chosen = lod;
            break;
        }
    }
    const uint64_t chosen_triangles = triangles_of(lods[chosen], parts);
    if (chosen_triangles > settings.collision_triangles) {
        diagnostics.warning(path,
                            "its coarsest LOD has %llu triangles, over its collision_triangles of %u: its collision "
                            "has them all",
                            static_cast<unsigned long long>(chosen_triangles), settings.collision_triangles);
    }

    // A LOD picked by its size can have lost what mattered: nothing would say so, and things would walk through it.
    if (lods[chosen].part_count < lods[0].part_count) {
        diagnostics.warning(path, "its collision comes from a LOD with %u of its %u parts: the rest have no collision",
                            lods[chosen].part_count, lods[0].part_count);
    }
    constexpr float kFarFromDrawnShare = 0.05f; // of its bounding sphere's radius
    if (lods[chosen].error > mesh.bounds.radius * kFarFromDrawnShare) {
        diagnostics.warning(path, "its collision lies up to %.2f m from what's drawn: raise collision_triangles",
                            static_cast<double>(lods[chosen].error));
    }

    // Each position once, in the order they're first used.
    core::Array<uint32_t> position_of(tools_heap());
    position_of.resize_uninit(vertices.size());
    meshopt_generatePositionRemap(position_of.data(), &vertices[0].position[0], vertices.size(),
                                  sizeof(asset::MeshVertex));
    constexpr uint32_t kUnused = UINT32_MAX;
    core::Array<uint32_t> welded_at(tools_heap());
    welded_at.resize(vertices.size(), kUnused);
    core::Array<float> positions(tools_heap());
    core::Array<uint32_t> triangles(tools_heap());
    for (const asset::MeshPart& part : parts.subspan(lods[chosen].first_part, lods[chosen].part_count)) {
        for (const uint32_t index : indices.subspan(part.first_index, part.index_count)) {
            const uint32_t position = position_of[index];
            if (welded_at[position] == kUnused) {
                welded_at[position] = static_cast<uint32_t>(positions.size() / 3);
                for (const float coordinate : vertices[position].position) {
                    positions.push(coordinate);
                }
            }
            triangles.push(welded_at[position]);
        }
    }

    core::Array<std::byte> shape(tools_heap());
    const core::Result<uint32_t> kept = physics::build_mesh_shape(positions.span(), triangles.span(), shape);
    if (!kept) {
        diagnostics.warning(path, "has no collision: %s", kept.error().message);
        return core::fail(kept.error());
    }
    asset::CollisionAsset collision;
    collision.shape_format = physics::mesh_shape_format();
    collision.triangles = *kept;
    collision.vertices = static_cast<uint32_t>(positions.size() / 3);
    collision.error_m = lods[chosen].error;
    collision.source_lod = static_cast<uint32_t>(chosen);
    BlobWriter writer(sizeof(asset::CollisionAsset));
    collision.shape = writer.append(std::span<const std::byte>(shape.span()));
    DE_LOG_INFO("cooker", "%.*s: collision from LOD %zu of %zu: %u triangles, %u vertices, %zu bytes",
                static_cast<int>(path.size()), path.data(), chosen, lods.size(), collision.triangles,
                collision.vertices, shape.size());
    return writer.finish(collision, id, {});
}

} // namespace de::cooker
