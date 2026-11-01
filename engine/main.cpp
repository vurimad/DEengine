#include "core/version.h"

#include <cstdio>

// Placeholder entry point until the platform layer (M0a) brings up a window.
int main() {
    std::printf("DEengine %s\n", de::core::engine_version_string());
    return 0;
}
