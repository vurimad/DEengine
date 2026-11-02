#include "platform/random.h"

#include <sys/random.h>

#include <cerrno>

namespace de::platform {

core::Result<void> random_bytes(std::span<std::byte> dest) {
    for (size_t done = 0; done < dest.size();) {
        const ssize_t got = getrandom(dest.data() + done, dest.size() - done, 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return core::fail(core::ErrorCode::Unknown, "the OS random number generator failed");
        }
        done += static_cast<size_t>(got);
    }
    return {};
}

} // namespace de::platform
