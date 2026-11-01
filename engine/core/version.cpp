#include "core/version.h"

#define DE_STRINGIFY_IMPL(x) #x
#define DE_STRINGIFY(x) DE_STRINGIFY_IMPL(x)

namespace de::core {

Version engine_version() {
    return Version{.major = DE_VERSION_MAJOR, .minor = DE_VERSION_MINOR, .patch = DE_VERSION_PATCH};
}

const char* engine_version_string() {
    return DE_STRINGIFY(DE_VERSION_MAJOR) "." DE_STRINGIFY(DE_VERSION_MINOR) "." DE_STRINGIFY(DE_VERSION_PATCH);
}

} // namespace de::core
