#include "cooker/model.h"

#include "core/math.h"

#include <glm/geometric.hpp>

#include <cmath>

namespace de::cooker {

namespace {

Vec3 vec3(const float (&v)[3]) {
    return {v[0], v[1], v[2]};
}

Vec2 vec2(const float (&v)[2]) {
    return {v[0], v[1]};
}

void store(float (&out)[3], Vec3 value) {
    out[0] = value.x;
    out[1] = value.y;
    out[2] = value.z;
}

// Any unit vector perpendicular to n.
Vec3 perpendicular(Vec3 n) {
    const Vec3 axis = std::abs(n.x) < 0.9f ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 1.0f, 0.0f);
    return glm::normalize(glm::cross(n, axis));
}

} // namespace

void generate_normals(ModelPrimitive& primitive) {
    // glTF asks for flat normals when a mesh has none: every triangle gets its own three vertices.
    core::Array<ModelVertex> flat(tools_heap());
    flat.reserve(primitive.indices.size());
    for (size_t i = 0; i + 2 < primitive.indices.size(); i += 3) {
        ModelVertex corners[3] = {primitive.vertices[primitive.indices[i]],
                                  primitive.vertices[primitive.indices[i + 1]],
                                  primitive.vertices[primitive.indices[i + 2]]};
        const Vec3 normal = glm::cross(vec3(corners[1].position) - vec3(corners[0].position),
                                       vec3(corners[2].position) - vec3(corners[0].position));
        const float length = glm::length(normal);
        for (ModelVertex& corner : corners) {
            store(corner.normal, length > 0.0f ? normal / length : Vec3(0.0f, 1.0f, 0.0f));
            flat.push(corner);
        }
    }
    for (size_t i = 0; i < primitive.indices.size(); ++i) {
        primitive.indices[i] = static_cast<uint32_t>(i);
    }
    primitive.vertices = std::move(flat);
}

void generate_tangents(ModelPrimitive& primitive) {
    // Lengyel's method: each triangle's UV directions, summed per vertex, then made orthogonal to
    // the normal. (MikkTSpace would match more bakers exactly; see the README.)
    const size_t count = primitive.vertices.size();
    core::Array<Vec3> tangents(tools_heap());
    core::Array<Vec3> bitangents(tools_heap());
    tangents.resize(count, Vec3(0.0f));
    bitangents.resize(count, Vec3(0.0f));
    for (size_t i = 0; i + 2 < primitive.indices.size(); i += 3) {
        const uint32_t index[3] = {primitive.indices[i], primitive.indices[i + 1], primitive.indices[i + 2]};
        const ModelVertex& a = primitive.vertices[index[0]];
        const ModelVertex& b = primitive.vertices[index[1]];
        const ModelVertex& c = primitive.vertices[index[2]];
        const Vec3 edge1 = vec3(b.position) - vec3(a.position);
        const Vec3 edge2 = vec3(c.position) - vec3(a.position);
        const Vec2 duv1 = vec2(b.uv) - vec2(a.uv);
        const Vec2 duv2 = vec2(c.uv) - vec2(a.uv);
        const float determinant = duv1.x * duv2.y - duv2.x * duv1.y;
        if (std::abs(determinant) < 1e-12f) {
            continue;
        }
        const float r = 1.0f / determinant;
        const Vec3 tangent = (edge1 * duv2.y - edge2 * duv1.y) * r;
        const Vec3 bitangent = (edge2 * duv1.x - edge1 * duv2.x) * r;
        for (const uint32_t v : index) {
            tangents[v] += tangent;
            bitangents[v] += bitangent;
        }
    }
    for (size_t v = 0; v < count; ++v) {
        ModelVertex& vertex = primitive.vertices[v];
        const Vec3 n = vec3(vertex.normal);
        Vec3 t = tangents[v] - n * glm::dot(n, tangents[v]);
        const float length = glm::length(t);
        t = length > 1e-12f ? t / length : perpendicular(n);
        const float sign = glm::dot(glm::cross(n, t), bitangents[v]) < 0.0f ? -1.0f : 1.0f;
        vertex.tangent[0] = t.x;
        vertex.tangent[1] = t.y;
        vertex.tangent[2] = t.z;
        vertex.tangent[3] = sign;
    }
}

uint32_t add_texture(ModelData& model, const ModelTexture& texture, std::string_view path, Diagnostics& diagnostics) {
    for (size_t i = 0; i < model.textures.size(); ++i) {
        ModelTexture& existing = model.textures[i];
        const bool same =
            texture.file.empty() ? existing.embedded == texture.embedded : existing.file == texture.file.view();
        if (!same) {
            continue;
        }
        if (existing.usage == TextureUsage::Unknown) {
            existing.usage = texture.usage;
        } else if (texture.usage != TextureUsage::Unknown && texture.usage != existing.usage) {
            diagnostics.warning(path, "uses %s as %s and as %s: cooked for %s use",
                                texture.file.empty() ? "an embedded image" : texture.file.c_str(),
                                texture_usage_name(existing.usage), texture_usage_name(texture.usage),
                                texture_usage_name(existing.usage));
        }
        return static_cast<uint32_t>(i);
    }
    model.textures.push(texture);
    return static_cast<uint32_t>(model.textures.size() - 1);
}

} // namespace de::cooker
