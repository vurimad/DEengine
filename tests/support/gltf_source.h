#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace de::test {

inline std::string base64(const void* data, size_t size) {
    static constexpr char kDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* bytes = static_cast<const uint8_t*>(data);
    std::string out;
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t chunk = (uint32_t{bytes[i]} << 16U) | (i + 1 < size ? uint32_t{bytes[i + 1]} << 8U : 0U) |
                               (i + 2 < size ? uint32_t{bytes[i + 2]} : 0U);
        out += kDigits[(chunk >> 18U) & 63U];
        out += kDigits[(chunk >> 12U) & 63U];
        out += i + 1 < size ? kDigits[(chunk >> 6U) & 63U] : '=';
        out += i + 2 < size ? kDigits[chunk & 63U] : '=';
    }
    return out;
}

struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
};

// A glTF with one mesh in one node (translated by node_y), its buffer embedded as a data: URI.
// materials_json, images_json and textures_json are spliced in as they are. With second_indices,
// the mesh has a second primitive over the same vertices, with material 1.
inline std::string gltf(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, float node_y,
                        const std::string& materials_json, const std::string& images_json,
                        const std::string& textures_json, const std::vector<uint32_t>& second_indices = {}) {
    std::vector<uint8_t> buffer;
    const auto append = [&](const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        buffer.insert(buffer.end(), bytes, bytes + size);
    };
    float min[3] = {1e9f, 1e9f, 1e9f};
    float max[3] = {-1e9f, -1e9f, -1e9f};
    for (const Vertex& vertex : vertices) {
        append(vertex.position, 12);
        for (int axis = 0; axis < 3; ++axis) {
            min[axis] = std::min(min[axis], vertex.position[axis]);
            max[axis] = std::max(max[axis], vertex.position[axis]);
        }
    }
    for (const Vertex& vertex : vertices) {
        append(vertex.normal, 12);
    }
    for (const Vertex& vertex : vertices) {
        append(vertex.uv, 8);
    }
    append(indices.data(), indices.size() * 4);
    const size_t count = vertices.size();
    std::string second_primitive;
    std::string second_view;
    std::string second_accessor;
    if (!second_indices.empty()) {
        const size_t offset = buffer.size();
        append(second_indices.data(), second_indices.size() * 4);
        second_primitive =
            R"(, {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 4, "material": 1})";
        second_view = R"(, {"buffer": 0, "byteOffset": )" + std::to_string(offset) + R"(, "byteLength": )" +
                      std::to_string(second_indices.size() * 4) + "}";
        second_accessor = R"(, {"bufferView": 4, "componentType": 5125, "count": )" +
                          std::to_string(second_indices.size()) + R"(, "type": "SCALAR"})";
    }
    char text[4096];
    std::snprintf(text, sizeof(text),
                  R"({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
  "nodes": [{"mesh": 0, "translation": [0, %g, 0]}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}%s]}],
  "buffers": [{"byteLength": %zu, "uri": "data:application/octet-stream;base64,%%s"}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": %zu}, {"buffer": 0, "byteOffset": %zu, "byteLength": %zu},
                  {"buffer": 0, "byteOffset": %zu, "byteLength": %zu}, {"buffer": 0, "byteOffset": %zu, "byteLength": %zu}%s],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": %zu, "type": "VEC3", "min": [%g, %g, %g], "max": [%g, %g, %g]},
    {"bufferView": 1, "componentType": 5126, "count": %zu, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5126, "count": %zu, "type": "VEC2"},
    {"bufferView": 3, "componentType": 5125, "count": %zu, "type": "SCALAR"}%s],
  "materials": %s, "images": %s, "textures": %s}
)",
                  node_y, second_primitive.c_str(), buffer.size(), count * 12, count * 12, count * 12, count * 24,
                  count * 8, count * 32, indices.size() * 4, second_view.c_str(), count, min[0], min[1], min[2], max[0],
                  max[1], max[2], count, count, indices.size(), second_accessor.c_str(), materials_json.c_str(),
                  images_json.c_str(), textures_json.c_str());
    std::string json(text);
    const size_t at = json.find("%s");
    json.replace(at, 2, base64(buffer.data(), buffer.size()));
    return json;
}

// A flat square, two triangles, facing +Z.
inline void quad(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices) {
    vertices = {{.position = {-1, -1, 0}, .normal = {0, 0, 1}, .uv = {0, 1}},
                {.position = {1, -1, 0}, .normal = {0, 0, 1}, .uv = {1, 1}},
                {.position = {1, 1, 0}, .normal = {0, 0, 1}, .uv = {1, 0}},
                {.position = {-1, 1, 0}, .normal = {0, 0, 1}, .uv = {0, 0}}};
    indices = {0, 1, 2, 0, 2, 3};
}

// A wavy n x n grid: something to simplify.
inline void grid(uint32_t n, std::vector<Vertex>& vertices, std::vector<uint32_t>& indices) {
    vertices.clear();
    indices.clear();
    for (uint32_t y = 0; y <= n; ++y) {
        for (uint32_t x = 0; x <= n; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(n);
            const float v = static_cast<float>(y) / static_cast<float>(n);
            vertices.push_back({.position = {u * 4.0f, 0.05f * std::sin(u * 6.0f) * std::cos(v * 5.0f), v * 4.0f},
                                .normal = {0, 1, 0},
                                .uv = {u, v}});
        }
    }
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            const uint32_t i = y * (n + 1) + x;
            indices.insert(indices.end(), {i, i + n + 1, i + 1, i + 1, i + n + 1, i + n + 2});
        }
    }
}

} // namespace de::test
