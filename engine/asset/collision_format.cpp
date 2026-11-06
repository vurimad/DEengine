#include "asset/collision_format.h"

namespace de::asset {

core::Result<void> CollisionAsset::validate(std::span<const std::byte> file) const {
    if (!span_in_file(shape, file.size()) || shape.count == 0) {
        return core::fail(core::ErrorCode::CorruptData, "collision shape lies outside the file, or is empty");
    }
    return {};
}

} // namespace de::asset
