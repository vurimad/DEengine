#include "platform/random.h"

// clang-format off: bcrypt.h needs the types windows.h declares first
#include <windows.h>
#include <bcrypt.h>
// clang-format on

#include <algorithm>

namespace de::platform {

core::Result<void> random_bytes(std::span<std::byte> dest) {
    // BCryptGenRandom takes a ULONG size: big requests go in chunks.
    constexpr size_t kMaxChunk = 1U << 30;
    for (size_t done = 0; done < dest.size();) {
        const size_t chunk = std::min(dest.size() - done, kMaxChunk);
        const NTSTATUS status = BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(dest.data() + done),
                                                static_cast<ULONG>(chunk), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (!BCRYPT_SUCCESS(status)) {
            return core::fail(core::ErrorCode::Unknown, "the OS random number generator failed");
        }
        done += chunk;
    }
    return {};
}

} // namespace de::platform
