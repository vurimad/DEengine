#include "cooker/mesh_cooker.h"

#include "cooker/blob_writer.h"
#include "core/profile.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace de::cooker {

namespace {

static_assert(sizeof(ModelVertex) == 16 * sizeof(float), "vertices are compared byte by byte: no padding");

int32_t snorm(float value, float scale) {
    return static_cast<int32_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * scale));
}

// One material's triangles in LOD 0, before and after optimization.
struct PartBuild {
    uint32_t slot = 0;
    uint32_t first_index = 0; // into the combined LOD 0 indices
    uint32_t index_count = 0;
};

// Marks every vertex whose position two or more parts use: what no part may move when it simplifies.
// What a LOD may change besides its shape, as ModelVertex has them from normal on, relative to the mesh's size. A
// flat floor with painted colors has no error in its shape, and a triangle across two uv islands hardly any, so
// without these they'd look fine to a renderer picking by error.
constexpr std::array<float, 13> kAttributeWeights = {0.1f,  0.1f,  0.1f,        // normal
                                                     0.0f,  0.0f,  0.0f, 0.0f,  // tangent, follows the normal
                                                     0.25f, 0.25f,              // uv
                                                     0.5f,  0.5f,  0.5f, 0.5f}; // color
static_assert(offsetof(ModelVertex, uv) - offsetof(ModelVertex, normal) == 7 * sizeof(float));
static_assert(offsetof(ModelVertex, color) - offsetof(ModelVertex, normal) == 9 * sizeof(float));

void lock_seams(std::span<const ModelVertex> vertices, std::span<const uint32_t> indices,
                std::span<const PartBuild> parts, core::Array<uint8_t>& locks) {
    locks.clear();
    locks.resize(vertices.size(), 0);
    if (parts.size() < 2) {
        return;
    }
    // The first vertex with each position stands for all of them.
    core::Array<uint32_t> position_of(tools_heap());
    position_of.resize_uninit(vertices.size());
    meshopt_generatePositionRemap(position_of.data(), &vertices[0].position[0], vertices.size(), sizeof(ModelVertex));
    constexpr uint32_t kUnused = UINT32_MAX;
    constexpr uint32_t kShared = UINT32_MAX - 1;
    core::Array<uint32_t> owner(tools_heap()); // per position: the one part using it, or kShared
    owner.resize(vertices.size(), kUnused);
    for (uint32_t p = 0; p < parts.size(); ++p) {
        for (const uint32_t index : indices.subspan(parts[p].first_index, parts[p].index_count)) {
            uint32_t& part = owner[position_of[index]];
            part = part == kUnused || part == p ? p : kShared;
        }
    }
    for (size_t v = 0; v < vertices.size(); ++v) {
        locks[v] = owner[position_of[v]] == kShared ? meshopt_SimplifyVertex_Lock : 0;
    }
}

// Which piece of the surface each vertex is in: the vertices LOD 0's triangles join, one way or another, are one
// piece. Where the source split its vertices all round a patch (a uv island, a face with normals of its own), the
// patch is a piece. Each vertex gets the vertex that stands for its piece.
void find_pieces(std::span<const uint32_t> indices, size_t vertex_count, core::Array<uint32_t>& piece) {
    piece.resize_uninit(vertex_count);
    for (size_t v = 0; v < vertex_count; ++v) {
        piece[v] = static_cast<uint32_t>(v);
    }
    const auto root = [&piece](uint32_t v) {
        while (piece[v] != v) {
            piece[v] = piece[piece[v]]; // half the way there for whoever asks next
            v = piece[v];
        }
        return v;
    };
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t first = root(indices[i]);
        for (size_t corner = 1; corner < 3; ++corner) {
            const uint32_t other = root(indices[i + corner]);
            if (other != first) {
                piece[other] = first;
            }
        }
    }
    for (size_t v = 0; v < vertex_count; ++v) {
        piece[v] = root(static_cast<uint32_t>(v));
    }
}

// Whether a triangle of these joins vertices of two pieces: a seam between them has moved.
bool joins_pieces(std::span<const uint32_t> indices, std::span<const uint32_t> piece) {
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t first = piece[indices[i]];
        if (piece[indices[i + 1]] != first || piece[indices[i + 2]] != first) {
            return true;
        }
    }
    return false;
}

} // namespace

uint32_t encode_octahedral(const float (&normal)[3]) {
    float x = normal[0];
    float y = normal[1];
    const float z = normal[2];
    const float sum = std::abs(x) + std::abs(y) + std::abs(z);
    if (sum > 0.0f) {
        x /= sum;
        y /= sum;
    }
    if (z < 0.0f) {
        const float folded_x = (1.0f - std::abs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        const float folded_y = (1.0f - std::abs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = folded_x;
        y = folded_y;
    }
    const auto low = static_cast<uint16_t>(static_cast<int16_t>(snorm(x, 32767.0f)));
    const auto high = static_cast<uint16_t>(static_cast<int16_t>(snorm(y, 32767.0f)));
    return static_cast<uint32_t>(low) | (static_cast<uint32_t>(high) << 16U);
}

void decode_octahedral(uint32_t packed, float (&normal)[3]) {
    const float x = std::max(static_cast<float>(static_cast<int16_t>(packed & 0xFFFFU)) / 32767.0f, -1.0f);
    const float y = std::max(static_cast<float>(static_cast<int16_t>(packed >> 16U)) / 32767.0f, -1.0f);
    float n[3] = {x, y, 1.0f - std::abs(x) - std::abs(y)};
    if (n[2] < 0.0f) {
        n[0] = (1.0f - std::abs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        n[1] = (1.0f - std::abs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
    }
    const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (int i = 0; i < 3; ++i) {
        normal[i] = n[i] / length;
    }
}

uint32_t encode_snorm8x4(const float (&value)[4]) {
    uint32_t packed = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        packed |= static_cast<uint32_t>(static_cast<uint8_t>(static_cast<int8_t>(snorm(value[i], 127.0f)))) << (i * 8U);
    }
    return packed;
}

uint32_t encode_unorm8x4(const float (&value)[4]) {
    uint32_t packed = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        const auto byte = static_cast<uint32_t>(std::lround(std::clamp(value[i], 0.0f, 1.0f) * 255.0f));
        packed |= byte << (i * 8U);
    }
    return packed;
}

namespace {

// Wind for plants, into the vertex color (MeshWind; shaders/scene/wind.slang's plant_sway). Red is how far a vertex
// flutters, green its piece's phase, blue how far that piece is from being the plant's main stem; alpha is the
// source's. A piece is what's connected: a leaf, a twig, a branch, joined across vertex splits by position. It moves
// about its lowest point, more toward its far end. One that's most of the plant's height (a trunk, a stem) hardly
// flutters and leaves that to the whole plant's sway. Baked once, here on the full mesh, so the LOD chain carries the
// colors along and nothing jumps at a switch; leaves welded into one blob come out as one piece, and want Height
// instead.
void bake_wind(std::span<ModelVertex> vertices, std::span<const uint32_t> indices, MeshWind wind) {
    if (wind == MeshWind::Keep || vertices.empty()) {
        return;
    }
    float low = std::numeric_limits<float>::max();
    float high = std::numeric_limits<float>::lowest();
    for (const ModelVertex& vertex : vertices) {
        low = std::min(low, vertex.position[1]);
        high = std::max(high, vertex.position[1]);
    }
    const float height = std::max(high - low, 1e-4f);
    const auto phase = [](uint32_t key) {
        uint32_t h = key * 0x9e3779b9U;
        h ^= h >> 16U;
        h *= 0x85ebca6bU;
        h ^= h >> 13U;
        return static_cast<float>(h >> 8U) / static_cast<float>(1U << 24U);
    };
    if (wind == MeshWind::Height) {
        for (ModelVertex& vertex : vertices) {
            const float up = (vertex.position[1] - low) / height;
            vertex.color[0] = up * up;
            vertex.color[1] =
                phase(static_cast<uint32_t>(std::lround(vertex.position[0] * 7.0f + vertex.position[2] * 13.0f)));
            vertex.color[2] = 1.0f;
        }
        return;
    }

    // Pieces: joined by triangles, and across splits where two vertices share a position.
    const size_t count = vertices.size();
    core::Array<uint32_t> parent(tools_heap());
    parent.resize_uninit(count);
    for (size_t i = 0; i < count; ++i) {
        parent[i] = static_cast<uint32_t>(i);
    }
    const auto find = [&](uint32_t i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };
    const auto unite = [&](uint32_t a, uint32_t b) {
        a = find(a);
        b = find(b);
        if (a != b) {
            parent[std::max(a, b)] = std::min(a, b);
        }
    };
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        unite(indices[t], indices[t + 1]);
        unite(indices[t + 1], indices[t + 2]);
    }
    core::Array<uint32_t> by_position(tools_heap());
    by_position.resize_uninit(count);
    for (size_t i = 0; i < count; ++i) {
        by_position[i] = static_cast<uint32_t>(i);
    }
    const auto same_place = [&](uint32_t a, uint32_t b) {
        return std::ranges::equal(vertices[a].position, vertices[b].position);
    };
    std::ranges::sort(by_position, [&](uint32_t a, uint32_t b) {
        return std::ranges::lexicographical_compare(vertices[a].position, vertices[b].position);
    });
    for (size_t i = 1; i < count; ++i) {
        if (same_place(by_position[i - 1], by_position[i])) {
            unite(by_position[i - 1], by_position[i]);
        }
    }

    // Each piece's lowest vertex, its top, and how far it reaches from that lowest vertex.
    core::Array<uint32_t> lowest(tools_heap());
    core::Array<float> top(tools_heap());
    core::Array<float> reach(tools_heap());
    lowest.resize(count, UINT32_MAX);
    top.resize(count, std::numeric_limits<float>::lowest());
    reach.resize(count, 0.0f);
    const auto distance = [&](uint32_t a, uint32_t b) {
        const float dx = vertices[a].position[0] - vertices[b].position[0];
        const float dy = vertices[a].position[1] - vertices[b].position[1];
        const float dz = vertices[a].position[2] - vertices[b].position[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t piece = find(i);
        if (lowest[piece] == UINT32_MAX || vertices[i].position[1] < vertices[lowest[piece]].position[1]) {
            lowest[piece] = i;
        }
        top[piece] = std::max(top[piece], vertices[i].position[1]);
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t piece = find(i);
        reach[piece] = std::max(reach[piece], distance(i, lowest[piece]));
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t piece = find(i);
        const float along = distance(i, lowest[piece]) / std::max(reach[piece], 1e-4f);
        const float tall = (top[piece] - vertices[lowest[piece]].position[1]) / height;
        const float t = std::clamp((tall - 0.35f) / 0.35f, 0.0f, 1.0f);
        const float stem = t * t * (3.0f - 2.0f * t);
        const float up = (vertices[i].position[1] - low) / height;
        ModelVertex& vertex = vertices[i];
        vertex.color[0] = std::clamp(along * (1.0f - 0.85f * stem) * (0.4f + 0.6f * up), 0.0f, 1.0f);
        vertex.color[1] = phase(piece);
        vertex.color[2] = 1.0f - stem;
    }
}

} // namespace

core::Result<core::Array<std::byte>> cook_mesh(const ModelData& model, std::span<const asset::AssetId> material_ids,
                                               const MeshSettings& settings, asset::AssetId id, std::string_view path,
                                               Diagnostics& diagnostics) {
    DE_PROFILE_ZONE("cook mesh");
    // Slots: the materials the primitives use, in order of first use; one more for "none".
    constexpr uint32_t kNoSlot = UINT32_MAX;
    core::Array<uint32_t> slot_of_material(tools_heap());
    slot_of_material.resize(model.materials.size() + 1, kNoSlot);
    core::Array<asset::MeshMaterialSlot> slots(tools_heap());
    core::Array<PartBuild> parts(tools_heap());
    for (const ModelPrimitive& primitive : model.primitives) {
        const size_t material =
            primitive.material < model.materials.size() ? primitive.material : model.materials.size();
        if (slot_of_material[material] == kNoSlot) {
            slot_of_material[material] = static_cast<uint32_t>(slots.size());
            asset::MeshMaterialSlot& slot = slots.emplace();
            if (material < model.materials.size()) {
                const auto& name = model.materials[material].name;
                slot.name = core::StringId(name.c_str(), name.size());
                slot.material = material < material_ids.size() ? material_ids[material] : asset::AssetId();
            }
            parts.push(PartBuild{.slot = slot_of_material[material]});
        }
    }

    // All triangles in one buffer, grouped by part; positions scaled.
    core::Array<ModelVertex> vertices(tools_heap());
    core::Array<uint32_t> indices(tools_heap());
    for (PartBuild& part : parts) {
        part.first_index = static_cast<uint32_t>(indices.size());
        for (const ModelPrimitive& primitive : model.primitives) {
            const size_t material =
                primitive.material < model.materials.size() ? primitive.material : model.materials.size();
            if (slot_of_material[material] != part.slot) {
                continue;
            }
            const auto base = static_cast<uint32_t>(vertices.size());
            for (ModelVertex vertex : primitive.vertices) {
                for (float& coordinate : vertex.position) {
                    coordinate *= settings.scale;
                }
                vertices.push(vertex);
            }
            for (const uint32_t index : primitive.indices) {
                indices.push(base + index);
            }
        }
        part.index_count = static_cast<uint32_t>(indices.size()) - part.first_index;
    }
    if (indices.empty() || vertices.size() >= UINT32_MAX) {
        diagnostics.error(path, "has no triangles, or more vertices than 32-bit indices reach");
        return core::fail(core::ErrorCode::InvalidArgument, "the mesh is empty or too large");
    }

    // Identical vertices merged (importers hand over one vertex per triangle corner).
    core::Array<uint32_t> remap(tools_heap());
    remap.resize_uninit(vertices.size());
    const size_t unique = meshopt_generateVertexRemap(remap.data(), indices.data(), indices.size(), vertices.data(),
                                                      vertices.size(), sizeof(ModelVertex));
    core::Array<ModelVertex> merged(tools_heap());
    merged.resize(unique);
    meshopt_remapVertexBuffer(merged.data(), vertices.data(), vertices.size(), sizeof(ModelVertex), remap.data());
    meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());
    vertices = std::move(merged);
    bake_wind(vertices.span(), indices.span(), settings.wind);

    const float* positions = &vertices[0].position[0];
    if (settings.optimize) {
        for (const PartBuild& part : parts) {
            uint32_t* range = indices.data() + part.first_index; // both work in place
            meshopt_optimizeVertexCache(range, range, part.index_count, vertices.size());
            meshopt_optimizeOverdraw(range, range, part.index_count, positions, vertices.size(), sizeof(ModelVertex),
                                     1.05f);
        }
        core::Array<ModelVertex> fetched(tools_heap());
        fetched.resize(vertices.size());
        const size_t used = meshopt_optimizeVertexFetch(fetched.data(), indices.data(), indices.size(), vertices.data(),
                                                        vertices.size(), sizeof(ModelVertex));
        fetched.resize(used);
        vertices = std::move(fetched);
        positions = &vertices[0].position[0];
    }

    // LOD chain: each level simplified from LOD 0 (not from the last level, which would add up
    // errors), until the mesh won't get meaningfully smaller.
    core::Array<asset::MeshLod> lods(tools_heap());
    core::Array<asset::MeshPart> mesh_parts(tools_heap());
    lods.push(asset::MeshLod{.error = 0.0f, .first_part = 0, .part_count = static_cast<uint32_t>(parts.size())});
    for (const PartBuild& part : parts) {
        mesh_parts.push(asset::MeshPart{
            .first_index = part.first_index, .index_count = part.index_count, .material_slot = part.slot});
    }
    const size_t lod0_indices = indices.size();
    const float target_error =
        settings.lod_error * meshopt_simplifyScale(positions, vertices.size(), sizeof(ModelVertex));
    size_t previous_indices = lod0_indices;
    // Parts simplify one by one; where two meet (the same position, whether or not the vertex is
    // split there), the vertices stay put, so no LOD opens a crack along a material seam.
    core::Array<uint8_t> locks(tools_heap());
    lock_seams(vertices.span(), indices.span(), parts.span(), locks);
    // What LOD 0 keeps apart, so a level that joins it across can say so (MeshLod::seams_moved).
    core::Array<uint32_t> piece(tools_heap());
    find_pieces(indices.span(), vertices.size(), piece);
    core::Array<uint32_t> simplified(tools_heap());
    core::Array<uint32_t> permissive(tools_heap());
    core::Array<float> part_errors(tools_heap()); // at the last level
    part_errors.resize(parts.size(), 0.0f);
    core::Array<float> part_sizes(tools_heap()); // the diagonal of each part's box
    part_sizes.resize(parts.size(), 0.0f);
    for (size_t p = 0; p < parts.size(); ++p) {
        float low[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
        float high[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (uint32_t i = parts[p].first_index; i < parts[p].first_index + parts[p].index_count; ++i) {
            for (int axis = 0; axis < 3; ++axis) {
                low[axis] = std::min(low[axis], vertices[indices[i]].position[axis]);
                high[axis] = std::max(high[axis], vertices[indices[i]].position[axis]);
            }
        }
        part_sizes[p] = std::hypot(high[0] - low[0], high[1] - low[1], high[2] - low[2]);
    }
    for (uint32_t level = 1; level < settings.lod_count; ++level) {
        const float keep = std::pow(settings.lod_ratio, static_cast<float>(level));
        const auto first_part = static_cast<uint32_t>(mesh_parts.size());
        const size_t first_index = indices.size();
        float error = 0.0f;
        bool seams_moved = false;
        for (size_t p = 0; p < parts.size(); ++p) {
            const PartBuild& part = parts[p];
            const auto target = static_cast<size_t>(static_cast<float>(part.index_count) * keep) / 3 * 3;
            const size_t wanted = std::max<size_t>(target, 3);
            const auto simplify = [&](core::Array<uint32_t>& out, unsigned int options, float& out_error) {
                out.resize_uninit(part.index_count);
                return meshopt_simplifyWithAttributes(
                    out.data(), indices.data() + part.first_index, part.index_count, positions, vertices.size(),
                    sizeof(ModelVertex), &vertices[0].normal[0], sizeof(ModelVertex), kAttributeWeights.data(),
                    kAttributeWeights.size(), locks.data(), wanted, target_error,
                    meshopt_SimplifyErrorAbsolute | options, &out_error);
            };
            float part_error = 0.0f;
            size_t count = simplify(simplified, 0, part_error);
            // A mesh whose vertices are split all over (normals or uvs that don't carry across triangles) has few edges
            // it may collapse without moving those seams, so it stops short of the target, or gets there at a high
            // price. Then the seams may move, if that's no worse. Seams between parts stay: they're locked.
            if (count > wanted + (wanted / 4) || part_error > 4.0f * part_errors[p]) {
                float permissive_error = 0.0f;
                const size_t permissive_count = simplify(permissive, meshopt_SimplifyPermissive, permissive_error);
                if (permissive_count > 0 && permissive_count <= count && permissive_error <= 4.0f * part_error) {
                    std::swap(simplified, permissive);
                    count = permissive_count;
                    part_error = permissive_error;
                    // Free to move its seams, it may not have: only a triangle across two pieces says it did.
                    seams_moved = seams_moved || joins_pieces({simplified.data(), count}, piece.span());
                }
            }
            part_errors[p] = part_error;
            if (count == 0) {
                error = std::max(error, part_sizes[p]); // it's gone: from where this shows, that has to not show
                continue;
            }
            if (settings.optimize) {
                meshopt_optimizeVertexCache(simplified.data(), simplified.data(), count, vertices.size());
            }
            mesh_parts.push(asset::MeshPart{.first_index = static_cast<uint32_t>(indices.size()),
                                            .index_count = static_cast<uint32_t>(count),
                                            .material_slot = part.slot});
            for (size_t i = 0; i < count; ++i) {
                indices.push(simplified[i]);
            }
            error = std::max(error, part_error);
        }
        const size_t level_indices = indices.size() - first_index;
        // Less than 10% smaller than the level before, or nothing left: the chain ends here.
        if (level_indices == 0 || static_cast<double>(level_indices) > 0.9 * static_cast<double>(previous_indices)) {
            indices.resize(first_index);
            mesh_parts.resize(first_part);
            break;
        }
        // Never less than the LOD before: a renderer stops at the first one that's too coarse.
        lods.push(asset::MeshLod{.error = std::max(error, lods.back().error),
                                 .first_part = first_part,
                                 .part_count = static_cast<uint32_t>(mesh_parts.size()) - first_part,
                                 .seams_moved = seams_moved ? 1U : 0U});
        previous_indices = level_indices;
    }

    // Packed vertices and bounds.
    core::Array<asset::MeshVertex> packed(tools_heap());
    packed.resize(vertices.size());
    asset::MeshBounds bounds;
    for (int axis = 0; axis < 3; ++axis) {
        bounds.min[axis] = vertices[0].position[axis];
        bounds.max[axis] = vertices[0].position[axis];
    }
    for (size_t i = 0; i < vertices.size(); ++i) {
        const ModelVertex& vertex = vertices[i];
        asset::MeshVertex& out = packed[i];
        for (int axis = 0; axis < 3; ++axis) {
            out.position[axis] = vertex.position[axis];
            bounds.min[axis] = std::min(bounds.min[axis], vertex.position[axis]);
            bounds.max[axis] = std::max(bounds.max[axis], vertex.position[axis]);
        }
        out.normal = encode_octahedral(vertex.normal);
        out.tangent = encode_snorm8x4(vertex.tangent);
        out.uv[0] = vertex.uv[0];
        out.uv[1] = vertex.uv[1];
        out.color = encode_unorm8x4(vertex.color);
    }
    float radius_squared = 0.0f;
    for (int axis = 0; axis < 3; ++axis) {
        bounds.center[axis] = (bounds.min[axis] + bounds.max[axis]) * 0.5f;
    }
    for (const ModelVertex& vertex : vertices) {
        float distance = 0.0f;
        for (int axis = 0; axis < 3; ++axis) {
            const float d = vertex.position[axis] - bounds.center[axis];
            distance += d * d;
        }
        radius_squared = std::max(radius_squared, distance);
    }
    bounds.radius = std::sqrt(radius_squared);
    const float extent =
        std::max({bounds.max[0] - bounds.min[0], bounds.max[1] - bounds.min[1], bounds.max[2] - bounds.min[2]});
    if (extent > 2000.0f || extent < 0.001f) {
        diagnostics.warning(path, "is %g m across: check its units (the engine's are meters) or the mesh scale",
                            extent);
    }

    core::Array<asset::AssetRef> dependencies(tools_heap());
    for (const asset::MeshMaterialSlot& slot : slots) {
        if (slot.material.is_valid()) {
            const asset::AssetRef ref{.id = slot.material, .type = asset::AssetType::Material};
            if (std::ranges::find(dependencies, ref) == dependencies.end()) { // parts may share one
                dependencies.push(ref);
            }
        }
    }
    asset::MeshAsset mesh;
    mesh.bounds = bounds;
    BlobWriter writer(sizeof(asset::MeshAsset));
    mesh.vertices = writer.append(packed.span());
    mesh.indices = writer.append(indices.span());
    mesh.lods = writer.append(lods.span());
    mesh.parts = writer.append(mesh_parts.span());
    mesh.materials = writer.append(slots.span());
    return writer.finish(mesh, id, dependencies.span());
}

} // namespace de::cooker
