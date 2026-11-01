#include "core/assert.h"

#include <cstdio>
#include <cstdlib>

namespace de::core {

void assert_failed(const char* condition, const char* message, const char* file, int line) {
    // file(line): format so IDEs and terminals make the location clickable.
    std::fprintf(stderr, "%s(%d): assertion failed: %s%s%s\n", file, line, condition, message ? " -- " : "",
                 message ? message : "");
    std::fflush(stderr);
    std::abort();
}

} // namespace de::core
