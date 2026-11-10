#include "editor/thumbnails.h"

#include "asset/format.h"
#include "asset/texture_format.h"
#include "core/hash.h"
#include "core/profile.h"
#include "editor/source_files.h"
#include "platform/vfs.h"

#include <bc7enc_rdo/bc7decomp.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace de::editor {
namespace {

constexpr uint32_t kSize = asset::kThumbnailSize;
constexpr uint32_t kReadsPerFrame = 16; // a 16 KB file each

} // namespace

Thumbnails::Thumbnails(core::Allocator& allocator, uint32_t max_pages, uint32_t per_side)
    : allocator_(&allocator), entries_(allocator), by_path_(allocator), queue_(allocator), slots_(allocator),
      pages_(allocator), dirty_(allocator), max_pages_(std::max(max_pages, 1U)), per_side_(std::max(per_side, 1U)) {}

Thumbnails::~Thumbnails() {
    for (ImTextureData* page : pages_) {
        free_page(page);
    }
}

void Thumbnails::free_page(ImTextureData* page) {
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::UnregisterUserTexture(page);
    }
    page->~ImTextureData();
    allocator_->deallocate(page, sizeof(ImTextureData), alignof(ImTextureData));
}

bool Thumbnails::find(std::string_view path, Picture& out) {
    const uint64_t key = core::hash_bytes(path.data(), path.size());
    uint32_t index = 0;
    if (const uint32_t* found = by_path_.find(key); found != nullptr) {
        index = *found;
    } else {
        if (path.size() > core::FixedString<255>::capacity()) {
            return false;
        }
        index = static_cast<uint32_t>(entries_.size());
        by_path_.set(key, index);
        entries_.push({.path = core::FixedString<255>(path)});
    }
    Entry& entry = entries_[index];
    if (entry.state == State::Shown) {
        Slot& slot = slots_[entry.slot];
        slot.used_frame = frame_;
        const uint32_t page = entry.slot / (per_side_ * per_side_);
        const uint32_t at = entry.slot % (per_side_ * per_side_);
        const float cell = 1.0f / static_cast<float>(per_side_);
        const float inset =
            0.5f / static_cast<float>(per_side_ * kSize); // a texel's half: its neighbours don't bleed in
        const uint32_t column = at % per_side_;
        const uint32_t row = at / per_side_;
        const float u = static_cast<float>(column) * cell;
        const float v = static_cast<float>(row) * cell;
        out.texture = pages_[page]->GetTexRef();
        out.uv_min = ImVec2(u + inset, v + inset);
        out.uv_max = ImVec2(u + cell - inset, v + cell - inset);
        return true;
    }
    if (entry.state == State::Wanted && !entry.queued) {
        entry.queued = true;
        queue_.push(index);
    }
    return false;
}

void Thumbnails::update(const platform::Vfs& vfs) {
    DE_PROFILE_ZONE("Thumbnails::update");
    ++frame_;
    // Oldest asked first; one that finds no room waits for a picture to go out of sight.
    uint32_t reads = 0;
    size_t kept = 0;
    for (const uint32_t index : queue_) {
        Entry& entry = entries_[index];
        if (reads >= kReadsPerFrame || !read(vfs, entry, index)) {
            queue_[kept++] = index;
            continue;
        }
        ++reads;
        entry.queued = false;
    }
    queue_.resize(kept);
    // Pages written to go to the GPU again (whole: the renderer has no partial uploads); a new one goes when made.
    for (size_t page = 0; page < pages_.size(); ++page) {
        if (dirty_[page] != 0 && pages_[page]->Status == ImTextureStatus_OK) {
            pages_[page]->SetStatus(ImTextureStatus_WantUpdates);
        }
        dirty_[page] = 0;
    }
}

void Thumbnails::forget_all() {
    entries_.clear();
    by_path_.clear();
    queue_.clear();
    for (Slot& slot : slots_) {
        slot = {};
    }
}

uint32_t Thumbnails::take_slot() {
    // A free one; else a new page while there may be more; else the one drawn longest ago, if not this frame or the
    // last (it's still on screen).
    uint32_t oldest = kNoSlot;
    for (uint32_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].entry == UINT32_MAX) {
            return i;
        }
        if (oldest == kNoSlot || slots_[i].used_frame < slots_[oldest].used_frame) {
            oldest = i;
        }
    }
    if (pages_.size() < max_pages_) {
        auto* page = ::new (allocator_->allocate(sizeof(ImTextureData), alignof(ImTextureData))) ImTextureData();
        page->Create(ImTextureFormat_RGBA32, static_cast<int>(per_side_ * kSize), static_cast<int>(per_side_ * kSize));
        std::memset(page->GetPixels(), 0, static_cast<size_t>(page->GetSizeInBytes()));
        ImGui::RegisterUserTexture(page);
        pages_.push(page);
        dirty_.push(0);
        const auto first = static_cast<uint32_t>(slots_.size());
        slots_.resize(slots_.size() + (static_cast<size_t>(per_side_) * per_side_));
        return first;
    }
    if (oldest == kNoSlot || slots_[oldest].used_frame + 1 >= frame_) {
        return kNoSlot;
    }
    Entry& evicted = entries_[slots_[oldest].entry];
    evicted.slot = kNoSlot;
    evicted.state = State::Wanted; // asked for again when it's drawn again
    slots_[oldest] = {};
    return oldest;
}

bool Thumbnails::read(const platform::Vfs& vfs, Entry& entry, uint32_t index) {
    asset::Guid guid;
    asset::AssetId mesh;
    if (!source_asset_id(vfs, entry.path.view(), guid, mesh)) {
        entry.state = State::Missing; // no meta: never cooked
        return true;
    }
    const asset::CookedPath path = asset::cooked_path(asset::thumbnail_asset_id(mesh), asset::AssetType::Texture);
    if (!vfs.exists(path.c_str())) {
        entry.state = State::Missing;
        return true;
    }
    const auto file = vfs.read_whole_file(path.c_str(), *allocator_);
    const asset::TextureAsset* texture = nullptr;
    if (file) {
        if (const core::Result<const asset::TextureAsset*> loaded =
                asset::load_in_place<asset::TextureAsset>(file->span())) {
            texture = *loaded;
        }
    }
    const bool usable =
        texture != nullptr &&
        (texture->format == asset::TextureFormat::BC7Srgb || texture->format == asset::TextureFormat::BC7Unorm) &&
        texture->width == kSize && texture->height == kSize && !texture->mips.get(texture).empty();
    if (!usable) {
        entry.state = State::Missing;
        return true;
    }
    const uint32_t slot = take_slot();
    if (slot == kNoSlot) {
        return false; // every place is on screen: wait
    }
    // Its blocks into its place on the page.
    const uint32_t page = slot / (per_side_ * per_side_);
    const uint32_t at = slot % (per_side_ * per_side_);
    auto* pixels = static_cast<uint8_t*>(pages_[page]->GetPixels());
    const size_t left = static_cast<size_t>(at % per_side_) * kSize;
    const size_t top = static_cast<size_t>(at / per_side_) * kSize;
    const asset::TextureMip& level = texture->mips.get(texture)[0];
    const std::span<const std::byte> blocks = texture->mip_data(texture, 0);
    for (uint32_t by = 0; by < kSize / 4; ++by) {
        for (uint32_t bx = 0; bx < kSize / 4; ++bx) {
            bc7decomp::color_rgba decoded[16];
            static_cast<void>(bc7decomp::unpack_bc7(
                blocks.data() + (static_cast<size_t>(by) * level.row_bytes) + (static_cast<size_t>(bx) * 16), decoded));
            for (size_t row = 0; row < 4; ++row) {
                const size_t y = top + (static_cast<size_t>(by) * 4) + row;
                const size_t x = left + (static_cast<size_t>(bx) * 4);
                std::memcpy(pixels + (((y * (static_cast<size_t>(per_side_) * kSize)) + x) * 4), &decoded[row * 4], 16);
            }
        }
    }
    slots_[slot] = {.entry = index, .used_frame = frame_};
    entry.slot = slot;
    entry.state = State::Shown;
    dirty_[page] = 1;
    return true;
}

} // namespace de::editor
