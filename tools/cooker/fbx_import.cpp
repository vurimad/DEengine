#include "cooker/model.h"
#include "cooker/source_tree.h"
#include "core/hash_map.h"
#include "core/math.h"
#include "core/profile.h"
#include "platform/vfs.h"

#include <glm/geometric.hpp>
#include <ufbx.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace de::cooker {

namespace {

constexpr size_t kAlignment = 16; // malloc's

void* fbx_alloc(void* /*user*/, size_t size) {
    return tools_heap().allocate(std::max<size_t>(size, 1), kAlignment);
}

void fbx_free(void* /*user*/, void* pointer, size_t size) {
    if (pointer != nullptr) {
        tools_heap().deallocate(pointer, std::max<size_t>(size, 1), kAlignment);
    }
}

void* fbx_realloc(void* user, void* pointer, size_t old_size, size_t new_size) {
    void* grown = fbx_alloc(user, new_size);
    if (pointer != nullptr) {
        std::memcpy(grown, pointer, std::min(old_size, new_size));
        fbx_free(user, pointer, old_size);
    }
    return grown;
}

Vec3 vec3(ufbx_vec3 v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

void store(float (&out)[3], Vec3 value) {
    out[0] = value.x;
    out[1] = value.y;
    out[2] = value.z;
}

class Importer {
public:
    Importer(const SourceTree& tree, const Source& source, Diagnostics& diagnostics, const ufbx_scene& scene)
        : tree_(tree), source_(source), diagnostics_(diagnostics), scene_(scene) {}

    bool run(ModelData& model) {
        model_ = &model;
        for (size_t i = 0; i < scene_.materials.count; ++i) {
            read_material(*scene_.materials.data[i]);
        }
        for (size_t i = 0; i < scene_.nodes.count; ++i) {
            const ufbx_node& node = *scene_.nodes.data[i];
            if (node.mesh != nullptr) {
                read_mesh(node, *node.mesh);
            }
        }
        if (model.primitives.empty()) {
            diagnostics_.error(source_.path.view(), "has no meshes with faces");
            return false;
        }
        return true;
    }

private:
    uint32_t texture(const ufbx_texture* map, TextureUsage usage) {
        if (map == nullptr) {
            return kNoTexture;
        }
        ModelTexture reference{.usage = usage};
        if (map->content.size > 0) {
            // One image per embedded file, however many materials share it.
            if (const uint32_t* known = embedded_.find(map->content.data); known != nullptr) {
                reference.embedded = *known;
            } else {
                ModelImage& image = model_->images.emplace();
                image.encoded.assign(std::span(static_cast<const std::byte*>(map->content.data), map->content.size));
                reference.embedded = static_cast<uint32_t>(model_->images.size() - 1);
                embedded_.set(map->content.data, reference.embedded);
            }
            return add_texture(*model_, reference, source_.path.view(), diagnostics_);
        }
        // DCCs write paths with backslashes, and often absolute ones from the artist's machine:
        // the relative path first, then the file name beside the model.
        char relative[kMaxSourcePath + 1] = {};
        std::memcpy(relative, map->relative_filename.data, std::min(map->relative_filename.length, kMaxSourcePath));
        std::replace(relative, relative + std::strlen(relative), '\\', '/');
        const std::string_view folder = folder_of(source_.path.view());
        const Source* found = nullptr;
        if (join_relative(folder, relative, reference.file)) {
            found = tree_.find(reference.file.view());
        }
        if (found == nullptr && join_relative(folder, file_name_of(relative), reference.file)) {
            found = tree_.find(reference.file.view());
        }
        if (found == nullptr || found->kind != SourceKind::Texture) {
            diagnostics_.error(source_.path.view(),
                               "uses texture '%s', which isn't a texture source beside it or at its relative path "
                               "(missing, misnamed, or outside content/)",
                               map->relative_filename.data);
            return kNoTexture;
        }
        return add_texture(*model_, reference, source_.path.view(), diagnostics_);
    }

    void read_material(const ufbx_material& source) {
        ModelMaterial& material = model_->materials.emplace();
        material.name.assign(std::string_view(source.name.data, source.name.length));
        const ufbx_material_pbr_maps& pbr = source.pbr;
        const ufbx_vec4 base = pbr.base_color.value_vec4;
        const auto base_factor = static_cast<float>(pbr.base_factor.has_value ? pbr.base_factor.value_real : 1.0);
        material.base_color[0] = static_cast<float>(base.x) * base_factor;
        material.base_color[1] = static_cast<float>(base.y) * base_factor;
        material.base_color[2] = static_cast<float>(base.z) * base_factor;
        material.base_color[3] = 1.0f;
        material.metallic = pbr.metalness.has_value ? static_cast<float>(pbr.metalness.value_real) : 0.0f;
        material.roughness = pbr.roughness.has_value ? static_cast<float>(pbr.roughness.value_real) : 0.5f;
        const auto emission = static_cast<float>(pbr.emission_factor.has_value ? pbr.emission_factor.value_real : 1.0);
        for (int i = 0; i < 3; ++i) {
            material.emissive[i] = static_cast<float>(pbr.emission_color.value_vec4.v[i]) * emission;
        }
        material.base_color_texture = texture(pbr.base_color.texture, TextureUsage::Color);
        material.normal_texture = texture(pbr.normal_map.texture, TextureUsage::Normal);
        material.emissive_texture = texture(pbr.emission_color.texture, TextureUsage::Color);
        material.occlusion_texture = texture(pbr.ambient_occlusion.texture, TextureUsage::Data);
        if (pbr.roughness.texture != nullptr || pbr.metalness.texture != nullptr) {
            diagnostics_.warning(source_.path.view(),
                                 "material '%s' has separate roughness or metalness textures, which aren't packed "
                                 "into one yet: their values are used instead",
                                 material.name.c_str());
        }
    }

    void read_mesh(const ufbx_node& node, const ufbx_mesh& mesh) {
        const ufbx_matrix& transform = node.geometry_to_world;
        const ufbx_matrix normal_matrix = ufbx_matrix_for_normals(&transform);
        const Vec3 x = vec3(transform.cols[0]);
        const Vec3 y = vec3(transform.cols[1]);
        const Vec3 z = vec3(transform.cols[2]);
        const bool mirrored = glm::dot(glm::cross(x, y), z) < 0.0f;
        const bool has_tangents = mesh.vertex_tangent.exists && mesh.vertex_bitangent.exists;
        core::Array<uint32_t> triangles(tools_heap());
        triangles.resize(mesh.max_face_triangles * 3);
        for (size_t p = 0; p < mesh.material_parts.count; ++p) {
            const ufbx_mesh_part& part = mesh.material_parts.data[p];
            if (part.num_triangles == 0) {
                continue;
            }
            ModelPrimitive& primitive = model_->primitives.emplace();
            if (part.index < mesh.materials.count && mesh.materials.data[part.index] != nullptr) {
                primitive.material = mesh.materials.data[part.index]->typed_id;
            }
            primitive.vertices.reserve(part.num_triangles * 3);
            for (size_t f = 0; f < part.face_indices.count; ++f) {
                const ufbx_face face = mesh.faces.data[part.face_indices.data[f]];
                const uint32_t count = ufbx_triangulate_face(triangles.data(), triangles.size(), &mesh, face);
                for (uint32_t c = 0; c < count * 3; ++c) {
                    primitive.vertices.push(read_vertex(mesh, triangles[c], transform, normal_matrix, has_tangents));
                }
            }
            primitive.indices.resize(primitive.vertices.size());
            for (size_t i = 0; i < primitive.indices.size(); ++i) {
                primitive.indices[i] = static_cast<uint32_t>(i);
            }
            if (mirrored) {
                for (size_t i = 0; i + 2 < primitive.indices.size(); i += 3) {
                    std::swap(primitive.indices[i + 1], primitive.indices[i + 2]);
                }
            }
            if (!has_tangents) {
                generate_tangents(primitive);
            }
        }
    }

    static ModelVertex read_vertex(const ufbx_mesh& mesh, uint32_t index, const ufbx_matrix& transform,
                                   const ufbx_matrix& normal_matrix, bool has_tangents) {
        ModelVertex vertex;
        store(vertex.position,
              vec3(ufbx_transform_position(&transform, ufbx_get_vertex_vec3(&mesh.vertex_position, index))));
        Vec3 normal(0.0f, 1.0f, 0.0f);
        if (mesh.vertex_normal.exists) {
            normal = glm::normalize(
                vec3(ufbx_transform_direction(&normal_matrix, ufbx_get_vertex_vec3(&mesh.vertex_normal, index))));
        }
        store(vertex.normal, normal);
        if (mesh.vertex_uv.exists) {
            const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh.vertex_uv, index);
            vertex.uv[0] = static_cast<float>(uv.x);
            vertex.uv[1] = 1.0f - static_cast<float>(uv.y); // FBX's UV origin is bottom left
        }
        if (has_tangents) {
            const Vec3 tangent = glm::normalize(
                vec3(ufbx_transform_direction(&transform, ufbx_get_vertex_vec3(&mesh.vertex_tangent, index))));
            // The bitangent follows V, which the flip above reversed.
            const Vec3 bitangent =
                -vec3(ufbx_transform_direction(&transform, ufbx_get_vertex_vec3(&mesh.vertex_bitangent, index)));
            vertex.tangent[0] = tangent.x;
            vertex.tangent[1] = tangent.y;
            vertex.tangent[2] = tangent.z;
            const bool negative = glm::dot(glm::cross(normal, tangent), bitangent) < 0.0f;
            // From world-space vectors, so a mirroring transform is already in the sign.
            vertex.tangent[3] = negative ? -1.0f : 1.0f;
        }
        if (mesh.vertex_color.exists) {
            const ufbx_vec4 color = ufbx_get_vertex_vec4(&mesh.vertex_color, index);
            for (int i = 0; i < 4; ++i) {
                vertex.color[i] = static_cast<float>(color.v[i]);
            }
        }
        return vertex;
    }

    const SourceTree& tree_;
    const Source& source_;
    Diagnostics& diagnostics_;
    const ufbx_scene& scene_;
    ModelData* model_ = nullptr;
    core::HashMap<const void*, uint32_t> embedded_{tools_heap()}; // embedded file data -> ModelData::images
};

} // namespace

core::Result<ModelData> import_fbx(const platform::Vfs& vfs, const SourceTree& tree, const Source& source,
                                   Diagnostics& diagnostics) {
    DE_PROFILE_ZONE("import fbx");
    const core::Result<core::Array<std::byte>> bytes = read_whole_file(vfs, src_path(source.path.view()).c_str());
    if (!bytes) {
        diagnostics.error(source.path.view(), "could not be read: %s", bytes.error().message);
        return core::fail(bytes.error());
    }
    ufbx_load_opts options{};
    const ufbx_allocator allocator{.alloc_fn = &fbx_alloc,
                                   .realloc_fn = &fbx_realloc,
                                   .free_fn = &fbx_free,
                                   .free_allocator_fn = nullptr,
                                   .user = nullptr};
    options.temp_allocator.allocator = allocator;
    options.result_allocator.allocator = allocator;
    // The engine's space: meters, right-handed, Y up; conversions baked into the vertices.
    options.target_axes = ufbx_axes_right_handed_y_up;
    options.target_unit_meters = 1.0f;
    options.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    options.generate_missing_normals = true;
    ufbx_error error{};
    ufbx_scene* scene = ufbx_load_memory(bytes->data(), bytes->size(), &options, &error);
    if (scene == nullptr) {
        diagnostics.error(source.path.view(), "can't be read as FBX: %.*s", static_cast<int>(error.description.length),
                          error.description.data);
        return core::fail(core::ErrorCode::CorruptData, "the FBX could not be read");
    }
    struct Free {
        ufbx_scene* scene;
        ~Free() { ufbx_free_scene(scene); }
    } const free_scene{scene};
    ModelData model;
    Importer importer(tree, source, diagnostics, *scene);
    if (!importer.run(model)) {
        return core::fail(core::ErrorCode::CorruptData, "the FBX could not be imported");
    }
    return model;
}

} // namespace de::cooker
