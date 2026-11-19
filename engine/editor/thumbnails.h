#pragma once

#include "asset/asset_id.h"
#include "asset/format.h"
#include "core/allocator.h"
#include "core/array.h"
#include "core/fixed_string.h"
#include "core/hash_map.h"
#include "core/result.h"
#include "platform/file.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace de::platform {
class Vfs;
}

namespace de::editor {

// What a drawn picture (an effect's, a prefab's) was drawn from: an asset, and the file its contents came from
// (AssetRegistry::file_info). The picture holds while each of them is still that file.
struct PictureSource {
    asset::AssetRef asset;
    uint64_t size = 0;
    uint64_t modified_time = 0;
};
static_assert(sizeof(PictureSource) == 32);

// The pictures in Resources. Of models, textures and materials, the cooker draws each as it cooks it
// (asset::thumbnail_asset_id, a small BC7 texture: tools/cooker/thumbnail_cooker.h); this only reads them. Of effects
// and prefabs, the renderer draws them (preview_pictures.h): what a flame looks like is what its particles do. Those
// are kept as files beside the user's settings (user:/editor/pictures), each with what it was drawn from, and drawn
// again only when one of those has changed; this asks for the ones it has no good file of (take_render_request).
//
// They're shown from a few shared pages (ImGui textures of per_side x per_side pictures), at most max_pages, so GPU
// memory and texture slots stay the same however many there are: once they're full, the picture shown longest ago
// gives its place to the next one asked for. Asked for by source path as cards are drawn, read a few a frame.
class Thumbnails {
public:
    static constexpr uint32_t kPerSide = 8;  // pictures along a page's side (1024 texels)
    static constexpr uint32_t kMaxPages = 4; // 256 pictures shown at once at most, 16 MB
    // How drawn pictures look: a change to it (the light, the backdrop, the framing) draws them all again.
    static constexpr uint16_t kPictureLook = 1;

    // A picture for the renderer to draw.
    struct RenderRequest {
        uint32_t entry = 0;
        uint32_t generation = 0;
        asset::AssetRef asset; // the effect or the prefab
    };

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

    // The next picture the renderer has to draw, oldest asked first; false if none waits. Whoever takes it answers
    // with rendered(), once the picture's file is written (write_picture) or it turned out it can't be drawn.
    bool take_render_request(RenderRequest& out);
    void rendered(const RenderRequest& request, bool made);
    // Whether a request is still wanted: not after forget_all(), which asks again for what's still shown.
    bool wants(const RenderRequest& request) const { return request.generation == generation_; }
    uint32_t renders_waiting() const { return static_cast<uint32_t>(render_queue_.size()); }

    // Writes a drawn picture's file: kSize x kSize texels (RGBA, sRGB, rows from the top), and what it was drawn
    // from, the asset itself first. settled: everything it shows was loaded and in place; one that isn't is shown
    // for now and drawn again next time.
    static core::Result<void> write_picture(const platform::Vfs& vfs, asset::AssetId id,
                                            std::span<const std::byte> rgba, std::span<const PictureSource> drawn_from,
                                            bool settled, core::Allocator& allocator);

    uint32_t pages() const { return static_cast<uint32_t>(pages_.size()); }

private:
    static constexpr uint32_t kNoSlot = UINT32_MAX;
    enum class State : uint8_t { Wanted, Shown, Missing, Rendering };
    struct Entry {
        core::FixedString<255> path;
        uint32_t slot = kNoSlot;
        State state = State::Wanted;
        bool queued = false;
        bool rendered = false; // drawn since it was last forgotten: what its file holds is the best there is
    };
    struct Slot {
        uint32_t entry = UINT32_MAX; // whose picture it holds
        uint64_t used_frame = 0;     // last drawn
    };

    bool read(const platform::Vfs& vfs, Entry& entry, uint32_t index);
    bool read_drawn(const platform::Vfs& vfs, Entry& entry, uint32_t index, asset::AssetRef asset);
    bool drawn_from_holds(const platform::Vfs& vfs, std::span<const PictureSource> drawn_from);
    // A slot's place on its page: where its first texel is, and its page.
    uint8_t* slot_texels(uint32_t slot, uint32_t& page);
    uint32_t take_slot();
    void free_page(ImTextureData* page);

    core::Allocator* allocator_;
    core::Array<Entry> entries_;
    core::HashMap<uint64_t, uint32_t> by_path_; // path hash -> entries_ index
    core::Array<uint32_t> queue_;               // entries to read
    core::Array<Slot> slots_;                   // kPerSide^2 per page
    core::Array<ImTextureData*> pages_;
    core::Array<uint8_t> dirty_; // per page: written since it was last sent (1)
    core::Array<RenderRequest> render_queue_;
    // The files asked about since the last forget_all(), by asset: many pictures are drawn from the same ones.
    core::HashMap<uint64_t, platform::FileInfo> file_infos_;
    uint32_t generation_ = 1;
    uint32_t max_pages_;
    uint32_t per_side_;
    uint64_t frame_ = 1;
};

} // namespace de::editor
