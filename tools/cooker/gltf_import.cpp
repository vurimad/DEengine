#include "cooker/model.h"
#include "cooker/source_tree.h"
#include "core/math.h"
#include "core/profile.h"
#include "platform/vfs.h"

#include <cgltf.h>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <utility>

namespace de::cooker {

namespace {

constexpr std::string_view kSrcPrefix = "src:/";

struct ReadContext {
    const platform::Vfs* vfs = nullptr;
    ModelData* model = nullptr;
    std::string_view path; // the glTF's, for messages
    Diagnostics* diagnostics = nullptr;
};

void* gltf_alloc(void* /*user*/, cgltf_size size) {
    return core::tagged_malloc(core::MemoryTag::Tools, size);
}

void gltf_free(void* /*user*/, void* pointer) {
    core::tagged_free(core::MemoryTag::Tools, pointer);
}

// External buffers, through the VFS. cgltf joins the URI to the glTF's path ("src:/content/a/b.gltf"),
// so ".." segments are resolved here, and the file must stay inside content/.
cgltf_result read_external(const cgltf_memory_options* /*memory*/, const cgltf_file_options* file, const char* path,
                           cgltf_size* size, void** data) {
    ReadContext& context = *static_cast<ReadContext*>(file->user_data);
    const std::string_view text(path);
    SourcePath relative;
    if (!text.starts_with(kSrcPrefix) || !join_relative("", text.substr(kSrcPrefix.size()), relative) ||
        !relative.view().starts_with("content/")) {
        context.diagnostics->error(context.path, "refers to '%s', outside content/", path);
        return cgltf_result_file_not_found;
    }
    // Read straight into the memory cgltf keeps (and frees through release_external).
    const auto buffer_path = src_path(relative.view());
    const core::Result<uint64_t> length = context.vfs->file_size(buffer_path.c_str());
    // *size comes in as the glTF's byteLength, and cgltf keeps that size whatever is read: a shorter
    // file would be read past its end.
    if (length && *length < *size) {
        context.diagnostics->error(context.path,
                                   "its buffer %s is %llu bytes, shorter than the %llu the glTF says (a Git LFS "
                                   "pointer? run git lfs pull)",
                                   relative.c_str(), static_cast<unsigned long long>(*length),
                                   static_cast<unsigned long long>(*size));
        return cgltf_result_data_too_short;
    }
    void* memory = length ? core::tagged_malloc(core::MemoryTag::Tools, static_cast<size_t>(*length)) : nullptr;
    const core::Result<size_t> read =
        memory != nullptr ? context.vfs->read_file(buffer_path.c_str(), std::span(static_cast<std::byte*>(memory),
                                                                                  static_cast<size_t>(*length)))
                          : core::Result<size_t>(core::fail(core::ErrorCode::NotFound, "missing"));
    if (!read || *read != *length) {
        core::tagged_free(core::MemoryTag::Tools, memory);
        context.diagnostics->error(context.path, "its buffer %s could not be read", relative.c_str());
        return cgltf_result_file_not_found;
    }
    *size = static_cast<cgltf_size>(*length);
    *data = memory;
    context.model->dependencies.push(relative);
    return cgltf_result_success;
}

void release_external(const cgltf_memory_options* /*memory*/, const cgltf_file_options* /*file*/, void* data) {
    core::tagged_free(core::MemoryTag::Tools, data);
}

const char* result_text(cgltf_result result) {
    switch (result) {
    case cgltf_result_data_too_short:
        return "the file is cut short";
    case cgltf_result_unknown_format:
        return "not glTF";
    case cgltf_result_invalid_json:
        return "invalid JSON";
    case cgltf_result_invalid_gltf:
        return "invalid glTF";
    case cgltf_result_file_not_found:
        return "a file it refers to is missing";
    case cgltf_result_io_error:
        return "a file it refers to could not be read";
    case cgltf_result_out_of_memory:
        return "out of memory";
    case cgltf_result_legacy_gltf:
        return "glTF 1.0, which isn't supported (convert it to 2.0)";
    default:
        break;
    }
    return "unknown error";
}

class Importer {
public:
    Importer(const SourceTree& tree, const Source& source, Diagnostics& diagnostics, const cgltf_options& options,
             cgltf_data& data)
        : tree_(tree), source_(source), diagnostics_(diagnostics), options_(options), data_(data) {}

    bool run(ModelData& model) {
        model_ = &model;
        // Embedded images first, so materials can point at them.
        image_slots_.resize(data_.images_count, kNoTexture);
        for (size_t i = 0; i < data_.materials_count; ++i) {
            read_material(data_.materials[i]);
        }
        const cgltf_scene* scene = data_.scene != nullptr   ? data_.scene
                                   : data_.scenes_count > 0 ? &data_.scenes[0]
                                                            : nullptr;
        if (scene != nullptr) {
            for (size_t i = 0; i < scene->nodes_count; ++i) {
                if (!visit(*scene->nodes[i])) {
                    return false;
                }
            }
        } else {
            for (size_t i = 0; i < data_.nodes_count; ++i) {
                if (data_.nodes[i].parent == nullptr && !visit(data_.nodes[i])) {
                    return false;
                }
            }
        }
        if (model.primitives.empty()) {
            diagnostics_.error(source_.path.view(), "has no triangle meshes in its scene");
            return false;
        }
        return true;
    }

private:
    uint32_t texture(const cgltf_texture_view& view, TextureUsage usage) {
        if (view.texture == nullptr) {
            return kNoTexture;
        }
        if (view.has_transform) {
            diagnostics_.warning(source_.path.view(), "uses KHR_texture_transform, which isn't supported yet: ignored");
        }
        const cgltf_image* image = view.texture->image;
        if (image == nullptr) {
            diagnostics_.warning(source_.path.view(),
                                 "has a texture without a PNG or JPEG image (KTX2, WebP?): ignored");
            return kNoTexture;
        }
        ModelTexture reference{.usage = usage};
        const size_t index = cgltf_image_index(&data_, image);
        if (image->buffer_view != nullptr || (image->uri != nullptr && std::strncmp(image->uri, "data:", 5) == 0)) {
            if (image_slots_[index] == kNoTexture && !embed(*image, index)) {
                return kNoTexture;
            }
            reference.embedded = image_slots_[index];
        } else if (image->uri != nullptr) {
            char uri[kMaxSourcePath + 1] = {};
            const size_t length = std::strlen(image->uri);
            std::memcpy(uri, image->uri, std::min(length, kMaxSourcePath));
            cgltf_decode_uri(uri); // "%20" and friends, in place
            if (length > kMaxSourcePath || !join_relative(folder_of(source_.path.view()), uri, reference.file) ||
                tree_.find(reference.file.view()) == nullptr ||
                tree_.find(reference.file.view())->kind != SourceKind::Texture) {
                diagnostics_.error(source_.path.view(),
                                   "uses image '%s', which isn't a texture source under content/ (missing, misnamed, "
                                   "or outside the tree)",
                                   image->uri);
                return kNoTexture;
            }
        } else {
            return kNoTexture;
        }
        return add_texture(*model_, reference, source_.path.view(), diagnostics_);
    }

    // An image inside the model (a .glb's buffer, or a data: URI): cooked with the model.
    bool embed(const cgltf_image& image, size_t index) {
        ModelImage& embedded = model_->images.emplace();
        if (image.buffer_view != nullptr) {
            const cgltf_buffer_view& view = *image.buffer_view;
            if (view.buffer == nullptr || view.buffer->data == nullptr) {
                diagnostics_.error(source_.path.view(), "has an embedded image without data");
                return false;
            }
            const auto* bytes = static_cast<const std::byte*>(view.buffer->data) + view.offset;
            embedded.encoded.assign(std::span(bytes, view.size));
        } else {
            const char* comma = std::strchr(image.uri, ',');
            if (comma == nullptr || std::strstr(image.uri, ";base64,") == nullptr) {
                diagnostics_.error(source_.path.view(), "has a data: image URI that isn't base64");
                return false;
            }
            const std::string_view base64(comma + 1);
            size_t padding = 0;
            for (size_t i = base64.size(); i > 0 && base64[i - 1] == '='; --i) {
                ++padding;
            }
            // Whole groups of four, padding at most two: anything else isn't base64 (and would make
            // the size wrap below).
            if (base64.empty() || base64.size() % 4 != 0 || padding > 2) {
                diagnostics_.error(source_.path.view(), "has a data: image URI that isn't valid base64");
                return false;
            }
            const size_t size = base64.size() / 4 * 3 - padding;
            void* decoded = nullptr;
            if (cgltf_load_buffer_base64(&options_, size, comma + 1, &decoded) != cgltf_result_success) {
                diagnostics_.error(source_.path.view(), "has a data: image URI that doesn't decode");
                return false;
            }
            embedded.encoded.assign(std::span(static_cast<const std::byte*>(decoded), size));
            options_.memory.free_func(options_.memory.user_data, decoded);
        }
        embedded.key = static_cast<uint32_t>(index);
        image_slots_[index] = static_cast<uint32_t>(model_->images.size() - 1);
        return true;
    }

    void read_material(const cgltf_material& source) {
        ModelMaterial& material = model_->materials.emplace();
        if (source.name != nullptr) {
            material.name.assign(source.name);
        }
        if (source.has_pbr_metallic_roughness) {
            const cgltf_pbr_metallic_roughness& pbr = source.pbr_metallic_roughness;
            std::memcpy(material.base_color, pbr.base_color_factor, sizeof(material.base_color));
            material.metallic = pbr.metallic_factor;
            material.roughness = pbr.roughness_factor;
            material.base_color_texture = texture(pbr.base_color_texture, TextureUsage::Color);
            material.metallic_roughness_texture = texture(pbr.metallic_roughness_texture, TextureUsage::Data);
        }
        material.normal_texture = texture(source.normal_texture, TextureUsage::Normal);
        material.normal_scale = source.normal_texture.texture != nullptr ? source.normal_texture.scale : 1.0f;
        material.occlusion_texture = texture(source.occlusion_texture, TextureUsage::Data);
        material.occlusion_strength =
            source.occlusion_texture.texture != nullptr ? source.occlusion_texture.scale : 1.0f;
        material.emissive_texture = texture(source.emissive_texture, TextureUsage::Color);
        const float strength = source.has_emissive_strength ? source.emissive_strength.emissive_strength : 1.0f;
        for (int i = 0; i < 3; ++i) {
            material.emissive[i] = source.emissive_factor[i] * strength;
        }
        material.alpha_mode = source.alpha_mode == cgltf_alpha_mode_mask    ? asset::AlphaMode::Masked
                              : source.alpha_mode == cgltf_alpha_mode_blend ? asset::AlphaMode::Blend
                                                                            : asset::AlphaMode::Opaque;
        material.alpha_cutoff = source.alpha_cutoff;
        material.double_sided = source.double_sided != 0;
    }

    bool visit(const cgltf_node& node) {
        if (node.mesh != nullptr) {
            float world[16];
            cgltf_node_transform_world(&node, world);
            const Mat4 transform = glm::make_mat4(world);
            for (size_t i = 0; i < node.mesh->primitives_count; ++i) {
                if (!read_primitive(node.mesh->primitives[i], transform)) {
                    return false;
                }
            }
        }
        for (size_t i = 0; i < node.children_count; ++i) {
            if (!visit(*node.children[i])) {
                return false;
            }
        }
        return true;
    }

    bool read_primitive(const cgltf_primitive& primitive, const Mat4& transform) {
        if (primitive.has_draco_mesh_compression) {
            diagnostics_.error(source_.path.view(), "uses Draco compression, which the cooker doesn't read: export "
                                                    "without it");
            return false;
        }
        if (primitive.type != cgltf_primitive_type_triangles) {
            diagnostics_.warning(source_.path.view(),
                                 "has a primitive of points, lines or strips: skipped (triangles only)");
            return true;
        }
        const cgltf_accessor* positions = nullptr;
        const cgltf_accessor* normals = nullptr;
        const cgltf_accessor* tangents = nullptr;
        const cgltf_accessor* uvs = nullptr;
        const cgltf_accessor* colors = nullptr;
        for (size_t i = 0; i < primitive.attributes_count; ++i) {
            const cgltf_attribute& attribute = primitive.attributes[i];
            if (attribute.index != 0) {
                continue;
            }
            switch (attribute.type) {
            case cgltf_attribute_type_position:
                positions = attribute.data;
                break;
            case cgltf_attribute_type_normal:
                normals = attribute.data;
                break;
            case cgltf_attribute_type_tangent:
                tangents = attribute.data;
                break;
            case cgltf_attribute_type_texcoord:
                uvs = attribute.data;
                break;
            case cgltf_attribute_type_color:
                colors = attribute.data;
                break;
            default:
                break;
            }
        }
        if (positions == nullptr || positions->count == 0) {
            diagnostics_.warning(source_.path.view(), "has a primitive without positions: skipped");
            return true;
        }

        ModelPrimitive& out = model_->primitives.emplace();
        out.material = primitive.material != nullptr
                           ? static_cast<uint32_t>(cgltf_material_index(&data_, primitive.material))
                           : UINT32_MAX;
        const Mat3 linear(transform);
        const Mat3 normal_matrix = glm::inverseTranspose(linear);
        const bool mirrored = glm::determinant(linear) < 0.0f;
        out.vertices.resize(positions->count);
        bool readable = true;
        for (size_t v = 0; v < positions->count; ++v) {
            ModelVertex& vertex = out.vertices[v];
            readable = cgltf_accessor_read_float(positions, v, vertex.position, 3) && readable;
            const Vec4 position = transform * Vec4(vertex.position[0], vertex.position[1], vertex.position[2], 1.0f);
            vertex.position[0] = position.x;
            vertex.position[1] = position.y;
            vertex.position[2] = position.z;
            if (normals != nullptr) {
                readable = cgltf_accessor_read_float(normals, v, vertex.normal, 3) && readable;
                const Vec3 normal =
                    glm::normalize(normal_matrix * Vec3(vertex.normal[0], vertex.normal[1], vertex.normal[2]));
                vertex.normal[0] = normal.x;
                vertex.normal[1] = normal.y;
                vertex.normal[2] = normal.z;
            }
            if (tangents != nullptr) {
                readable = cgltf_accessor_read_float(tangents, v, vertex.tangent, 4) && readable;
                const Vec3 tangent =
                    glm::normalize(linear * Vec3(vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]));
                vertex.tangent[0] = tangent.x;
                vertex.tangent[1] = tangent.y;
                vertex.tangent[2] = tangent.z;
                vertex.tangent[3] = (vertex.tangent[3] < 0.0f) != mirrored ? -1.0f : 1.0f;
            }
            if (uvs != nullptr) {
                readable = cgltf_accessor_read_float(uvs, v, vertex.uv, 2) && readable;
            }
            if (colors != nullptr) {
                readable = cgltf_accessor_read_float(colors, v, vertex.color, std::size(vertex.color)) && readable;
            }
        }
        if (primitive.indices != nullptr) {
            out.indices.resize(primitive.indices->count);
            for (size_t i = 0; i < primitive.indices->count; ++i) {
                out.indices[i] = static_cast<uint32_t>(cgltf_accessor_read_index(primitive.indices, i));
                readable = readable && out.indices[i] < out.vertices.size();
            }
        } else {
            out.indices.resize(positions->count);
            for (size_t i = 0; i < positions->count; ++i) {
                out.indices[i] = static_cast<uint32_t>(i);
            }
        }
        if (!readable || out.indices.size() % 3 != 0) {
            diagnostics_.error(source_.path.view(), "has a primitive whose data can't be read (sparse or compressed "
                                                    "accessors, or indices out of range)");
            return false;
        }
        if (mirrored) {
            for (size_t i = 0; i + 2 < out.indices.size(); i += 3) {
                std::swap(out.indices[i + 1], out.indices[i + 2]);
            }
        }
        if (normals == nullptr) {
            generate_normals(out);
        }
        if (tangents == nullptr) {
            generate_tangents(out);
        }
        return true;
    }

    const SourceTree& tree_;
    const Source& source_;
    Diagnostics& diagnostics_;
    const cgltf_options& options_;
    cgltf_data& data_;
    ModelData* model_ = nullptr;
    core::Array<uint32_t> image_slots_{tools_heap()}; // glTF image -> ModelData::images
};

} // namespace

core::Result<ModelData> import_gltf(const platform::Vfs& vfs, const SourceTree& tree, const Source& source,
                                    std::span<const std::byte> bytes, Diagnostics& diagnostics) {
    DE_PROFILE_ZONE("import gltf");
    const auto path = src_path(source.path.view());
    ModelData model;
    ReadContext context{.vfs = &vfs, .model = &model, .path = source.path.view(), .diagnostics = &diagnostics};
    cgltf_options options{};
    options.memory = {.alloc_func = &gltf_alloc, .free_func = &gltf_free, .user_data = nullptr};
    options.file = {.read = &read_external, .release = &release_external, .user_data = &context};
    cgltf_data* data = nullptr;
    cgltf_result result = cgltf_parse(&options, bytes.data(), bytes.size(), &data);
    struct Free {
        cgltf_data* data;
        ~Free() { cgltf_free(data); }
    } const free_data{data};
    if (result == cgltf_result_success) {
        result = cgltf_load_buffers(&options, data, path.c_str());
    }
    if (result == cgltf_result_success) {
        result = cgltf_validate(data);
    }
    if (result != cgltf_result_success) {
        diagnostics.error(source.path.view(), "can't be read as glTF: %s", result_text(result));
        return core::fail(core::ErrorCode::CorruptData, "the glTF could not be read");
    }
    Importer importer(tree, source, diagnostics, options, *data);
    if (!importer.run(model)) {
        return core::fail(core::ErrorCode::CorruptData, "the glTF could not be imported");
    }
    return model;
}

} // namespace de::cooker
