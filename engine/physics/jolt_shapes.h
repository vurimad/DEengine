#pragma once

#include "core/array.h"
#include "core/result.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace de::physics {

// What engine/physics shares with the cooker, with no Jolt types: a library of its own, so the cooker gets Jolt's build
// options from the same place the engine does, and doesn't need the world.

// Jolt's global setup (memory, logs, asserts, its factory), held while anything uses it. One thread at a time: the one
// Physics objects and cooks are made on.
void acquire_jolt();
void release_jolt();

// Which Jolt can read a mesh shape's bytes: its version, and the build options that change its types. Not
// JPH_VERSION_ID, which also changes with asserts and profiling, so a debug cooker's files would be refused by a dev
// build's engine.
uint64_t mesh_shape_format();

// A Jolt MeshShape over these triangles (counter-clockwise from the side that collides), its binary state appended to
// out. positions: x, y, z for each vertex. A triangle with no area, or a repeat of another, is dropped. Returns how
// many are left, and fails if Jolt won't have them. Triangles that share an edge must share its two indices, or Jolt
// takes it for an open edge, and what slides across it catches on it.
core::Result<uint32_t> build_mesh_shape(std::span<const float> positions, std::span<const uint32_t> indices,
                                        core::Array<std::byte>& out);

} // namespace de::physics
