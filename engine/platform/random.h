#pragma once

#include "core/result.h"

#include <cstddef>
#include <span>

namespace de::platform {

// Fills dest with random bytes from the OS (cryptographic quality): GUIDs for new assets, seeds.
// Not for gameplay randomness, which is seeded and deterministic (docs/design.md, Determinism).
core::Result<void> random_bytes(std::span<std::byte> dest);

} // namespace de::platform
