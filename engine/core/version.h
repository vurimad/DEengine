#pragma once

#include <cstdint>

namespace de::core {

struct Version {
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;
};

Version engine_version();

// Static storage, never allocates: "major.minor.patch".
const char* engine_version_string();

} // namespace de::core
