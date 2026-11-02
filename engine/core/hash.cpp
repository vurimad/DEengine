#include "core/hash.h"

// Inlined into this file: no xxHash library to load at runtime.
#define XXH_INLINE_ALL // NOLINT(readability-identifier-naming): xxHash's configuration macro
#include <xxhash.h>

namespace de::core {

uint64_t hash_bytes(const void* data, size_t size, uint64_t seed) {
    return XXH3_64bits_withSeed(data, size, seed);
}

} // namespace de::core
