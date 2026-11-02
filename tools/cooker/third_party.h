#pragma once

namespace de::cooker {

// Routes the libraries with process-wide allocator hooks (meshoptimizer) to the Tools heap. Once;
// cook() calls it. stb, cgltf, ufbx and yyjson take allocators per call (third_party.cpp, importers).
void init_third_party();

} // namespace de::cooker
