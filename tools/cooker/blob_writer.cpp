#include "cooker/blob_writer.h"

#include "core/allocator.h"

#include <utility>

namespace de::cooker {

uint32_t BlobWriter::append_bytes(std::span<const std::byte> data) {
    const size_t offset = core::align_up(bytes_.size(), asset::kAssetAlignment);
    DE_VERIFY_MSG(offset + data.size() <= asset::kMaxAssetFileSize, "cooked files are at most 4 GB");
    bytes_.resize(offset + data.size()); // the padding is zeroed: files are reproducible
    if (!data.empty()) {
        std::memcpy(bytes_.data() + offset, data.data(), data.size());
    }
    return static_cast<uint32_t>(offset);
}

core::Array<std::byte> BlobWriter::seal() {
    // Whole blocks of 16 bytes, so a file read into aligned memory ends aligned too.
    bytes_.resize(core::align_up(bytes_.size(), asset::kAssetAlignment));
    asset::AssetHeader header;
    std::memcpy(&header, bytes_.data(), sizeof(header));
    header.file_size = bytes_.size();
    header.content_hash = asset::content_hash(bytes_.span());
    std::memcpy(bytes_.data(), &header, sizeof(header));
    return std::move(bytes_);
}

} // namespace de::cooker
