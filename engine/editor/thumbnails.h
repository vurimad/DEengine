#pragma once

#include "asset/asset_id.h"
#include "core/allocator.h"
#include "core/array.h"
#include "core/fixed_string.h"
#include "core/hash_map.h"

#include <imgui.h>

#include <cstdint>
#include <string_view>

namespace de::platform {
class Vfs;
}

namespace de::editor {

// The pictures of models, textures and materials in Resources. The cooker draws each as it cooks it
// (asset::thumbnail_asset_id, a small BC7 texture: tools/cooker/thumbnail_cooker.h); this only reads them. They're kept
// in a few shared pages (ImGui textures of per_side x per_side pictures), at most max_pages, so GPU memory and texture
// slots stay the same however many models there are: once they're full, the picture shown longest ago gives its place
// to the next one asked for. Asked for by source path as cards are drawn, read a few a frame.
class Thumbnails {
public:
    static constexpr uint32_t kPerSide = 8;  // pictures along a page's side (1024 texels)
    static constexpr uint32_t kMaxPages = 4; // 256 pictures shown at once at most, 16 MB

    // Where a picture is: its page, and its corners there.
    struct Picture {
        ImTextureRef texture;
        ImVec2 uv_min;
        ImVec2 uv_max;
    };

    explicit Thumbnails(core::Allocator& allocator, uint32_t max_pages = kMaxPages, uint32_t per_side = kPerSide);
    ~Thumbnails(); // its pages go back to ImGui (and the renderer frees them)
    Thumbnails(const Thumbnails&) = delete;
    Thumbnails& operator=(const Thumbnails&) = delete;
    Thumbnails(Thumbnails&&) = delete;
    Thumbnails& operator=(Thumbnails&&) = delete;

    // The picture of a source ("src:/content/props/rock.gltf", a texture, a material), if it's read; else it's asked
    // for. False for now, and for good if it has none (not cooked, or cooked before models had pictures).
    bool find(std::string_view path, Picture& out);

    // Each frame: reads what was asked for, into the pages.
    void update(const platform::Vfs& vfs);
    // Read anew when next asked for (Refresh, a cook: models cooked since).
    void forget_all();

    uint32_t pages() const { return static_cast<uint32_t>(pages_.size()); }

private:
    static constexpr uint32_t kNoSlot = UINT32_MAX;
    enum class State : uint8_t { Wanted, Shown, Missing };
    struct Entry {
        core::FixedString<255> path;
        uint32_t slot = kNoSlot;
        State state = State::Wanted;
        bool queued = false;
    };
    struct Slot {
        uint32_t entry = UINT32_MAX; // whose picture it holds
        uint64_t used_frame = 0;     // last drawn
    };

    bool read(const platform::Vfs& vfs, Entry& entry, uint32_t index);
    uint32_t take_slot();
    void free_page(ImTextureData* page);

    core::Allocator* allocator_;
    core::Array<Entry> entries_;
    core::HashMap<uint64_t, uint32_t> by_path_; // path hash -> entries_ index
    core::Array<uint32_t> queue_;               // entries to read
    core::Array<Slot> slots_;                   // kPerSide^2 per page
    core::Array<ImTextureData*> pages_;
    core::Array<uint8_t> dirty_; // per page: written since it was last sent (1)
    uint32_t max_pages_;
    uint32_t per_side_;
    uint64_t frame_ = 1;
};

} // namespace de::editor
