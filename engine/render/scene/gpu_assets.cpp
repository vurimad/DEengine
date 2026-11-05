#include "render/scene/gpu_assets.h"

#include "asset/material_format.h"
#include "asset/mesh_format.h"
#include "asset/texture_format.h"
#include "core/allocator.h"
#include "core/array.h"
#include "core/log.h"
#include "core/profile.h"
#include "core/range_allocator.h"
#include "render/rhi/device.h"
#include "render/rhi/upload.h"
#include "render/scene/fog_noise.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace de::render {

namespace {

core::Allocator& render_heap() {
    return core::heap_allocator(core::MemoryTag::Render);
}

uint64_t key_of(asset::AssetHandle handle) {
    return (static_cast<uint64_t>(handle.index) << 32U) | handle.generation;
}

rhi::Format format_of(asset::TextureFormat format) {
    switch (format) {
    case asset::TextureFormat::RGBA8Unorm:
        return rhi::Format::RGBA8Unorm;
    case asset::TextureFormat::RGBA8Srgb:
        return rhi::Format::RGBA8Srgb;
    case asset::TextureFormat::BC1Unorm:
        return rhi::Format::BC1Unorm;
    case asset::TextureFormat::BC1Srgb:
        return rhi::Format::BC1Srgb;
    case asset::TextureFormat::BC4Unorm:
        return rhi::Format::BC4Unorm;
    case asset::TextureFormat::BC5Unorm:
        return rhi::Format::BC5Unorm;
    case asset::TextureFormat::BC7Unorm:
        return rhi::Format::BC7Unorm;
    case asset::TextureFormat::BC7Srgb:
        return rhi::Format::BC7Srgb;
    case asset::TextureFormat::Unknown:
        break;
    }
    return rhi::Format::Unknown;
}

struct TextureEntry {
    rhi::TextureHandle texture;
    uint64_t bytes = 0;
};

struct MaterialEntry {
    asset::AssetHandle handle;
    uint32_t slot = 0;
    bool queued = false;  // in GpuAssetsState::dirty_materials
    bool missing = false; // a texture it names failed: written again when a texture loads
};

// Where a mesh is in the geometry pool, in vertices and indices.
struct PoolPlace {
    uint64_t first_vertex = 0;
    uint64_t vertex_count = 0;
    uint64_t first_index = 0;
    uint64_t index_count = 0;
    uint64_t first_part = 0; // in the part table
    uint64_t part_count = 0;
    uint32_t table_slot = 0; // in the mesh table
};

struct MeshEntry {
    asset::AssetHandle handle;
    PoolPlace place;
    GpuMesh gpu;
    core::Array<GpuMeshPart> parts{render_heap()};
    core::Array<GpuMeshLod> lods{render_heap()};
    uint64_t bytes = 0;
    uint64_t upload_ticket = 0;        // the uploader's, when both its uploads are recorded
    uint64_t needs_table = UINT64_MAX; // the table upload its parts' materials are in (set in update)
    bool ready = false;
    bool missing = false; // a material it names failed: its parts are resolved again when one loads
};

enum DefaultTexture : uint32_t { White, FlatNormal, DefaultCount };

} // namespace

struct GpuAssetsState {
    rhi::Device* device = nullptr;
    rhi::Uploader* uploader = nullptr;
    asset::AssetRegistry* assets = nullptr;
    core::HashMap<uint64_t, TextureEntry> textures{render_heap()};
    core::HashMap<uint64_t, MaterialEntry> materials{render_heap()};
    core::HashMap<uint64_t, MeshEntry> meshes{render_heap()};
    core::Array<uint64_t> dirty_materials{render_heap()}; // keys of materials to (re)write
    core::Array<uint64_t> pending_meshes{render_heap()};  // keys of meshes waiting for their uploads
    core::Array<uint32_t> free_slots{render_heap()};      // material table slots, reused last first
    rhi::BufferHandle material_table;
    uint64_t material_table_address = 0;
    rhi::BufferHandle pool_vertices;
    rhi::BufferHandle pool_indices;
    uint64_t pool_vertex_address = 0;
    core::RangeAllocator vertex_ranges;
    core::RangeAllocator index_ranges;
    // The tables the GPU cull reads: a mesh's slot, and its parts.
    rhi::BufferHandle mesh_table;
    rhi::BufferHandle part_table;
    uint64_t mesh_table_address = 0;
    uint64_t part_table_address = 0;
    core::RangeAllocator part_ranges;
    core::Array<uint32_t> free_mesh_slots{render_heap()};
    bool draw_kinds_changed = false; // a material's flags did: meshes count their parts' kinds again
    // A mesh's old place, freed once the last frame that may have drawn from it is done.
    struct RetiredPlace {
        PoolPlace place;
        uint64_t last_frame = 0;
    };
    core::Array<RetiredPlace> retired_places{render_heap()};
    // Meshes that didn't fit: tried again as places come back. A full pool may be full for only a few frames.
    core::Array<asset::AssetHandle> dropped_meshes{render_heap()};

    GpuAssetsState(uint32_t vertices, uint32_t indices, uint32_t parts)
        : vertex_ranges(render_heap(), vertices), index_ranges(render_heap(), indices),
          part_ranges(render_heap(), parts) {}
    uint32_t max_materials = 0;
    rhi::TextureHandle defaults[DefaultCount];
    rhi::TextureHandle fog_noise;
    uint32_t default_index[DefaultCount] = {};
    uint64_t bytes = 0;
    // The table as last written, and the slots changed since it was last uploaded: one upload per
    // update(), of the span that changed.
    core::Array<GpuMaterial> table{render_heap()};
    uint32_t dirty_first = UINT32_MAX;
    uint32_t dirty_end = 0;
    uint64_t table_uploads = 0;                         // queued so far
    uint64_t table_recorded = 0;                        // of those, recorded: the GPU sees the table as of that upload
    core::Array<uint64_t> table_tickets{render_heap()}; // the uploader's, for those not yet recorded, oldest first
    // Textures replaced by a reload, kept until the table no longer points at them: once a table
    // upload queued after the replacement is recorded.
    struct Retired {
        rhi::TextureHandle texture;
        uint64_t after_upload = 0; // destroyed once table_uploads passed this and the table is recorded
    };
    core::Array<Retired> retired{render_heap()};
    core::Array<std::byte> texels{render_heap()}; // a texture's mips back to back, for one upload

    ~GpuAssetsState() {
        assets->remove_listener(&GpuAssetsState::on_notice, this);
        for (const auto& [key, texture] : textures) {
            device->destroy(texture.texture);
        }
        device->destroy(pool_vertices);
        device->destroy(pool_indices);
        device->destroy(mesh_table);
        device->destroy(part_table);
        device->destroy(fog_noise);
        for (const rhi::TextureHandle texture : defaults) {
            device->destroy(texture);
        }
        for (const Retired& old : retired) {
            device->destroy(old.texture);
        }
        device->destroy(material_table);
    }

    static void on_notice(void* context, const asset::AssetNotice& notice) {
        static_cast<GpuAssetsState*>(context)->handle(notice);
    }

    void handle(const asset::AssetNotice& notice) {
        const bool loaded = notice.event == asset::AssetEvent::Ready || notice.event == asset::AssetEvent::Reloaded;
        const bool unloaded = notice.event == asset::AssetEvent::Unloaded;
        switch (notice.asset.type) {
        case asset::AssetType::Texture:
            if (loaded) {
                load_texture(notice.handle);
            } else if (unloaded) {
                unload_texture(notice.handle);
            }
            break;
        case asset::AssetType::Material:
            if (loaded) {
                queue_material(notice.handle);
            } else if (unloaded) {
                unload_material(notice.handle);
            }
            break;
        case asset::AssetType::Mesh:
            if (loaded) {
                load_mesh(notice.handle);
            } else if (unloaded) {
                unload_mesh(notice.handle);
            }
            break;
        default:
            break;
        }
    }

    // --- Textures ----------------------------------------------------------------------------

    void load_texture(asset::AssetHandle handle) {
        const auto* asset = assets->get<asset::TextureAsset>(handle);
        const rhi::Format format = asset != nullptr ? format_of(asset->format) : rhi::Format::Unknown;
        if (format == rhi::Format::Unknown) {
            return;
        }
        const core::Result<rhi::TextureHandle> created = device->create_texture({
            .width = asset->width,
            .height = asset->height,
            .mip_count = asset->mips.count,
            .format = format,
            .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst,
            .debug_name = "asset texture",
        });
        if (!created) {
            DE_LOG_ERROR("render", "texture %s: %s", asset::format_asset_id(asset->header.id).c_str(),
                         created.error().message);
            return;
        }
        // Every mip in one upload (the uploader takes one upload per texture per frame): the file
        // pads mips to 16 bytes, the upload wants them back to back.
        texels.clear();
        for (uint32_t mip = 0; mip < asset->mips.count; ++mip) {
            const std::span<const std::byte> data = asset->mip_data(asset, mip);
            const size_t at = texels.size();
            texels.resize(at + data.size());
            std::memcpy(texels.data() + at, data.data(), data.size());
        }
        const uint64_t texel_bytes = texels.size();
        if (const core::Result<void> queued = uploader->upload_texture(*created, texels.span()); !queued) {
            // Not registered: its materials use the default instead of waiting for it forever.
            DE_LOG_ERROR("render", "texture %s: %s", asset::format_asset_id(asset->header.id).c_str(),
                         queued.error().message);
            device->destroy(*created);
            return;
        }
        TextureEntry& entry = textures.find_or_insert(key_of(handle));
        if (entry.texture.is_valid()) {
            // Reloaded: the old texture stays until the table stops pointing at it.
            retired.push({.texture = entry.texture, .after_upload = table_uploads});
            bytes -= entry.bytes;
            queue_all_materials();
        }
        entry = {.texture = *created, .bytes = texel_bytes};
        bytes += texel_bytes;
        // A texture that failed before may be this one, back: materials that fell back try again.
        for (auto [key, material] : materials) {
            if (material.missing && !material.queued) {
                material.missing = false;
                material.queued = true;
                dirty_materials.push(key);
            }
        }
    }

    void unload_texture(asset::AssetHandle handle) {
        if (const TextureEntry* entry = textures.find(key_of(handle)); entry != nullptr) {
            device->destroy(entry->texture);
            bytes -= entry->bytes;
            textures.remove(key_of(handle));
        }
    }

    // The bindless index of a material's texture: the default while it's missing or uploading
    // (still_waiting tells which, so the material is written again once it's in).
    uint32_t texture_index(asset::AssetId id, DefaultTexture fallback, bool& still_waiting, bool& missing) const {
        if (!id.is_valid()) {
            return default_index[fallback];
        }
        const asset::AssetHandle handle = assets->find({.id = id, .type = asset::AssetType::Texture});
        const TextureEntry* entry = handle.is_valid() ? textures.find(key_of(handle)) : nullptr;
        if (entry == nullptr) {
            // Loading (it will be Ready before the material is), or failed: the default.
            const bool loading = assets->state(handle) == asset::AssetState::Loading;
            still_waiting = still_waiting || loading;
            missing = missing || !loading;
            return default_index[fallback];
        }
        if (!uploader->ready(entry->texture)) {
            still_waiting = true;
            return default_index[fallback];
        }
        return device->texture_index(entry->texture);
    }

    // --- Materials ---------------------------------------------------------------------------

    void queue_material(asset::AssetHandle handle) {
        const uint64_t key = key_of(handle);
        MaterialEntry* entry = materials.find(key);
        if (entry == nullptr) {
            if (free_slots.empty()) {
                DE_LOG_ERROR("render", "the material table is full (%u): drawn with the default", max_materials);
                return;
            }
            entry = &materials.find_or_insert(key);
            *entry = {.handle = handle, .slot = free_slots.back()};
            free_slots.pop();
            // A material that failed before may be this one, back: meshes that fell back resolve
            // their parts again (and wait for the table again).
            for (auto [mesh_key, mesh] : meshes) {
                if (mesh.missing && resolve_parts(mesh)) {
                    // The GPU may be reading the old ones until this is recorded; it can't draw a part it lost.
                    if (const core::Result<void> queued = upload_parts(mesh, rhi::BufferRange::MayBeRead); queued) {
                        mesh.upload_ticket = uploader->buffer_upload_ticket();
                    } else {
                        DE_LOG_ERROR("render", "mesh parts upload: %s", queued.error().message);
                    }
                    mesh.ready = false;
                    mesh.needs_table = UINT64_MAX;
                    pending_meshes.push(mesh_key);
                }
            }
        }
        if (!entry->queued) {
            entry->queued = true;
            dirty_materials.push(key);
        }
    }

    void queue_all_materials() {
        for (auto [key, entry] : materials) {
            if (!entry.queued) {
                entry.queued = true;
                dirty_materials.push(key);
            }
        }
    }

    void unload_material(asset::AssetHandle handle) {
        if (const MaterialEntry* entry = materials.find(key_of(handle)); entry != nullptr) {
            free_slots.push(entry->slot);
            materials.remove(key_of(handle)); // a queued key that's gone is skipped
        }
    }

    // Writes the material's table entry; false if a texture is still uploading (try again).
    bool write_material(MaterialEntry& entry) {
        const auto* asset = assets->get<asset::MaterialAsset>(entry.handle);
        if (asset == nullptr) {
            return true; // not loaded any more: its unload is on the way
        }
        namespace m = asset::material;
        GpuMaterial gpu;
        const auto param = [&](core::StringId name, float* out, uint32_t count) {
            if (const asset::MaterialParam* found = asset->find_param(asset, name); found != nullptr) {
                for (uint32_t i = 0; i < count; ++i) {
                    out[i] = found->value[i];
                }
            }
        };
        param(m::kBaseColor, gpu.base_color, 4);
        param(m::kEmissive, gpu.emissive, 3);
        param(m::kMetallic, &gpu.metallic, 1);
        param(m::kRoughness, &gpu.roughness, 1);
        param(m::kNormalScale, &gpu.normal_scale, 1);
        param(m::kOcclusionStrength, &gpu.occlusion_strength, 1);
        param(m::kAlphaCutoff, &gpu.alpha_cutoff, 1);
        bool waiting = false;
        bool missing = false;
        const auto texture = [&](core::StringId slot, DefaultTexture fallback) {
            return texture_index(asset->find_texture(asset, slot), fallback, waiting, missing);
        };
        gpu.base_color_texture = texture(m::kBaseColorTexture, White);
        gpu.normal_texture = texture(m::kNormalTexture, FlatNormal);
        gpu.metallic_roughness_texture = texture(m::kMetallicRoughnessTexture, White);
        gpu.occlusion_texture = texture(m::kOcclusionTexture, White);
        gpu.emissive_texture = texture(m::kEmissiveTexture, White);
        entry.missing = missing;
        gpu.flags =
            (asset->alpha_mode == asset::AlphaMode::Masked ? GpuMaterialFlags::Masked : GpuMaterialFlags::None) |
            (core::has_any(asset->flags, asset::MaterialFlags::DoubleSided) ? GpuMaterialFlags::DoubleSided
                                                                            : GpuMaterialFlags::None);
        draw_kinds_changed = draw_kinds_changed || table[entry.slot].flags != gpu.flags;
        set_material(entry.slot, gpu);
        return !waiting;
    }

    // Changes a slot in the mirror; the next upload_table() sends it.
    void set_material(uint32_t slot, const GpuMaterial& gpu) {
        if (table[slot] == gpu) {
            return;
        }
        table[slot] = gpu;
        dirty_first = std::min(dirty_first, slot);
        dirty_end = std::max(dirty_end, slot + 1);
    }

    // The slots changed since the last upload, in one upload.
    void upload_table() {
        if (dirty_first >= dirty_end) {
            return;
        }
        const std::span<const GpuMaterial> changed = table.span().subspan(dirty_first, dirty_end - dirty_first);
        if (const core::Result<void> queued =
                uploader->upload_buffer(material_table, uint64_t{dirty_first} * sizeof(GpuMaterial),
                                        std::as_bytes(changed), rhi::ResourceState::ShaderRead);
            !queued) {
            DE_LOG_ERROR("render", "material table upload: %s", queued.error().message);
            return; // still dirty: tried again next update
        }
        ++table_uploads;
        table_tickets.push(uploader->buffer_upload_ticket());
        dirty_first = UINT32_MAX;
        dirty_end = 0;
    }

    uint32_t material_slot(asset::AssetId id) const {
        const asset::AssetHandle handle = assets->find({.id = id, .type = asset::AssetType::Material});
        const MaterialEntry* entry = handle.is_valid() ? materials.find(key_of(handle)) : nullptr;
        return entry != nullptr ? entry->slot : 0;
    }

    // --- Meshes ------------------------------------------------------------------------------

    // Whether a place in the pool and the tables is free for this mesh: first fit finds one if it's this big.
    bool fits(const asset::MeshAsset& asset) const {
        return asset.vertices.count <= vertex_ranges.largest_free() &&
               asset.indices.count <= index_ranges.largest_free() && part_total(asset) <= part_ranges.largest_free() &&
               !free_mesh_slots.empty();
    }

    static uint64_t part_total(const asset::MeshAsset& asset) {
        uint64_t total = 0;
        for (const asset::MeshLod& lod : asset.lods.get(&asset)) {
            total += lod.part_count;
        }
        return total;
    }

    void free_place(const PoolPlace& place) {
        vertex_ranges.free(place.first_vertex, place.vertex_count);
        index_ranges.free(place.first_index, place.index_count);
        if (place.part_count > 0) {
            part_ranges.free(place.first_part, place.part_count);
        }
        free_mesh_slots.push(place.table_slot);
    }

    // The part table's copy of a mesh's parts.
    core::Result<void> upload_parts(MeshEntry& entry, rhi::BufferRange range) {
        return uploader->upload_buffer(part_table, entry.place.first_part * sizeof(GpuMeshPart),
                                       std::as_bytes(entry.parts.span()), rhi::ResourceState::ShaderRead,
                                       rhi::ShaderStages::All, range);
    }

    void load_mesh(asset::AssetHandle handle) {
        const auto* asset = assets->get<asset::MeshAsset>(handle);
        if (asset == nullptr || asset->lods.count == 0 || asset->vertices.count == 0 || asset->indices.count == 0) {
            return;
        }
        const uint64_t key = key_of(handle);
        unload_mesh(handle); // reloaded: its old place is freed once the frames using it are done
        const auto vertices = asset->vertices.get(asset);
        const auto indices = asset->indices.get(asset);
        const uint64_t parts_total = part_total(*asset);
        if (!fits(*asset)) {
            DE_LOG_ERROR("render",
                         "mesh %s doesn't fit in the geometry pool (%zu vertices, %zu indices, %llu parts; %llu, %llu "
                         "and %llu free in one piece, %zu mesh slots free)",
                         asset::format_asset_id(asset->header.id).c_str(), vertices.size(), indices.size(),
                         static_cast<unsigned long long>(parts_total),
                         static_cast<unsigned long long>(vertex_ranges.largest_free()),
                         static_cast<unsigned long long>(index_ranges.largest_free()),
                         static_cast<unsigned long long>(part_ranges.largest_free()), free_mesh_slots.size());
            dropped_meshes.push(handle);
            return;
        }
        MeshEntry& entry = meshes.find_or_insert(key);
        entry.handle = handle;
        entry.place = {.first_vertex = vertex_ranges.allocate(vertices.size()),
                       .vertex_count = vertices.size(),
                       .first_index = index_ranges.allocate(indices.size()),
                       .index_count = indices.size(),
                       .first_part = part_ranges.allocate(parts_total),
                       .part_count = parts_total,
                       .table_slot = free_mesh_slots.back()};
        free_mesh_slots.pop();
        entry.gpu.vertex_offset = static_cast<uint32_t>(entry.place.first_vertex);
        entry.gpu.table_slot = entry.place.table_slot;
        std::memcpy(entry.gpu.bounds_min, asset->bounds.min, sizeof(entry.gpu.bounds_min));
        std::memcpy(entry.gpu.bounds_max, asset->bounds.max, sizeof(entry.gpu.bounds_max));
        std::memcpy(entry.gpu.bounds_center, asset->bounds.center, sizeof(entry.gpu.bounds_center));
        entry.gpu.bounds_radius_m = asset->bounds.radius;
        entry.bytes = vertices.size_bytes() + indices.size_bytes();
        bytes += entry.bytes;
        resolve_parts(entry);
        entry.needs_table = UINT64_MAX;
        pending_meshes.push(key);

        GpuMeshInfo info{.radius_m = asset->bounds.radius,
                         .vertex_offset = entry.gpu.vertex_offset,
                         .first_part = static_cast<uint32_t>(entry.place.first_part),
                         .part_count = entry.lods[0].part_count};
        std::memcpy(info.center, asset->bounds.center, sizeof(info.center));
        // A new place is read by nothing, so none of this waits for the frame in flight.
        core::Result<void> queued = uploader->upload_buffer(
            pool_vertices, entry.place.first_vertex * sizeof(asset::MeshVertex), std::as_bytes(vertices),
            rhi::ResourceState::ShaderRead, rhi::ShaderStages::All, rhi::BufferRange::Unread);
        if (queued) {
            queued = uploader->upload_buffer(pool_indices, entry.place.first_index * sizeof(uint32_t),
                                             std::as_bytes(indices), rhi::ResourceState::IndexBuffer,
                                             rhi::ShaderStages::All, rhi::BufferRange::Unread);
        }
        if (queued) {
            queued = upload_parts(entry, rhi::BufferRange::Unread);
        }
        if (queued) {
            queued = uploader->upload_buffer(mesh_table, uint64_t{entry.place.table_slot} * sizeof(GpuMeshInfo),
                                             std::as_bytes(std::span(&info, 1)), rhi::ResourceState::ShaderRead,
                                             rhi::ShaderStages::All, rhi::BufferRange::Unread);
        }
        if (!queued) {
            // Not drawn. What's already queued lands in a place nothing reads, before anything queued by the next
            // mesh to get it.
            DE_LOG_ERROR("render", "mesh upload: %s", queued.error().message);
            unload_mesh(handle);
            return;
        }
        entry.upload_ticket = uploader->buffer_upload_ticket();
    }

    // How many of LOD 0's parts draw each way: what sizes the draw lists.
    static void count_kinds(MeshEntry& entry, const core::Array<GpuMaterial>& materials_now) {
        for (uint32_t& count : entry.gpu.kind_parts) {
            count = 0;
        }
        if (entry.lods.empty()) {
            return;
        }
        for (const GpuMeshPart& part : entry.gpu.lod_parts(0)) {
            ++entry.gpu.kind_parts[draw_kind(materials_now[part.material].flags)];
        }
    }

    // The mesh's parts, of every LOD, and the table slots of their materials (the default for one that's
    // missing, remembered so they're resolved again when it loads). Returns whether a part changed.
    bool resolve_parts(MeshEntry& entry) {
        const size_t resolved_before = entry.parts.size();
        size_t count = 0;
        bool changed = false;
        entry.lods.clear();
        entry.missing = false;
        const auto* asset = assets->get<asset::MeshAsset>(entry.handle);
        if (asset != nullptr) {
            const std::span<const asset::MeshPart> parts = asset->parts.get(asset);
            const std::span<const asset::MeshMaterialSlot> slots = asset->materials.get(asset);
            for (const asset::MeshLod& lod : asset->lods.get(asset)) {
                entry.lods.push(
                    {.error_m = lod.error, .first_part = static_cast<uint32_t>(count), .part_count = lod.part_count});
                for (uint32_t p = lod.first_part; p < lod.first_part + lod.part_count; ++p) {
                    const asset::MeshPart& part = parts[p];
                    const asset::AssetId material = slots[part.material_slot].material;
                    const uint32_t slot = material_slot(material);
                    entry.missing = entry.missing || (slot == 0 && material.is_valid());
                    const GpuMeshPart resolved{.first_index =
                                                   static_cast<uint32_t>(entry.place.first_index) + part.first_index,
                                               .index_count = part.index_count,
                                               .material = slot};
                    if (count < resolved_before) {
                        changed = changed || entry.parts[count].material != slot;
                        entry.parts[count] = resolved;
                    } else {
                        changed = true;
                        entry.parts.push(resolved);
                    }
                    ++count;
                }
            }
        }
        entry.gpu.parts = entry.parts.span();
        entry.gpu.lods = entry.lods.span();
        count_kinds(entry, table);
        return changed;
    }

    void unload_mesh(asset::AssetHandle handle) {
        if (const MeshEntry* entry = meshes.find(key_of(handle)); entry != nullptr) {
            retired_places.push({.place = entry->place, .last_frame = device->frame_index()});
            bytes -= entry->bytes;
            meshes.remove(key_of(handle)); // a pending key that's gone is skipped
        }
        for (size_t i = 0; i < dropped_meshes.size(); ++i) {
            if (key_of(dropped_meshes[i]) == key_of(handle)) {
                dropped_meshes.remove_ordered(i);
                break;
            }
        }
    }

    // Frees the places of meshes that are gone once no frame in flight can draw from them, and gives meshes that
    // didn't fit another try.
    void free_retired_places() {
        if (retired_places.empty()) {
            return;
        }
        const uint64_t completed = device->completed_frame();
        size_t kept = 0;
        for (const RetiredPlace& gone : retired_places) {
            if (completed >= gone.last_frame) {
                free_place(gone.place);
            } else {
                retired_places[kept++] = gone;
            }
        }
        const bool freed = kept < retired_places.size();
        retired_places.resize(kept);
        if (!freed) {
            return;
        }
        // Each is either still too big, and stays, or leaves the list in load_mesh.
        for (size_t i = 0; i < dropped_meshes.size();) {
            const auto* asset = assets->get<asset::MeshAsset>(dropped_meshes[i]);
            if (asset == nullptr) {
                dropped_meshes.remove_ordered(i);
            } else if (fits(*asset)) {
                load_mesh(dropped_meshes[i]);
            } else {
                ++i;
            }
        }
    }
};

core::Result<GpuAssets> GpuAssets::create(const GpuAssetsDesc& desc) {
    DE_ASSERT_MSG(desc.pool_vertices <= uint32_t{INT32_MAX}, "a draw's vertex offset is signed");
    if (desc.device == nullptr || desc.uploader == nullptr || desc.assets == nullptr || desc.max_materials == 0) {
        return core::fail(core::ErrorCode::InvalidArgument, "GpuAssets needs a device, an uploader and a registry");
    }
    auto* state = ::new (render_heap().allocate(sizeof(GpuAssetsState), alignof(GpuAssetsState)))
        GpuAssetsState(desc.pool_vertices, desc.pool_indices, desc.max_parts);
    state->device = desc.device;
    state->uploader = desc.uploader;
    state->assets = desc.assets;
    state->max_materials = desc.max_materials;
    GpuAssets gpu_assets(state);

    // The defaults: white (base color, metal-roughness, occlusion, emissive) and a flat normal.
    constexpr uint8_t kTexels[DefaultCount][4] = {{255, 255, 255, 255}, {128, 128, 255, 255}};
    for (uint32_t i = 0; i < DefaultCount; ++i) {
        const core::Result<rhi::TextureHandle> texture =
            desc.device->create_texture({.format = rhi::Format::RGBA8Unorm,
                                         .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst,
                                         .debug_name = "default texture"});
        if (!texture) {
            return core::fail(texture.error());
        }
        state->defaults[i] = *texture;
        state->default_index[i] = desc.device->texture_index(*texture);
        if (const core::Result<void> queued =
                desc.uploader->upload_texture(*texture, std::as_bytes(std::span(kTexels[i])));
            !queued) {
            return core::fail(queued.error());
        }
    }

    const core::Result<rhi::TextureHandle> noise_texture =
        desc.device->create_texture({.width = kFogNoiseSize,
                                     .height = kFogNoiseSize,
                                     .depth = kFogNoiseSize,
                                     .mip_count = kFogNoiseMips,
                                     .format = rhi::Format::R8Unorm,
                                     .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst,
                                     .dimension = rhi::TextureDimension::Texture3D,
                                     .debug_name = "fog noise"});
    if (!noise_texture) {
        return core::fail(noise_texture.error());
    }
    state->fog_noise = *noise_texture;
    core::Array<uint8_t> noise_texels(render_heap());
    noise_texels.resize(kFogNoiseBytes);
    make_fog_noise(noise_texels.span());
    if (const core::Result<void> queued =
            desc.uploader->upload_texture(*noise_texture, std::as_bytes(noise_texels.span()));
        !queued) {
        return core::fail(queued.error());
    }

    const core::Result<rhi::BufferHandle> table =
        desc.device->create_buffer({.size = uint64_t{desc.max_materials} * sizeof(GpuMaterial),
                                    .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                                    .debug_name = "material table"});
    if (!table) {
        return core::fail(table.error());
    }
    state->material_table = *table;
    state->material_table_address = desc.device->buffer_address(*table);
    const core::Result<rhi::BufferHandle> pool_vertices =
        desc.device->create_buffer({.size = uint64_t{desc.pool_vertices} * sizeof(asset::MeshVertex),
                                    .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                                    .debug_name = "geometry pool vertices"});
    if (!pool_vertices) {
        return core::fail(pool_vertices.error());
    }
    state->pool_vertices = *pool_vertices;
    state->pool_vertex_address = desc.device->buffer_address(*pool_vertices);
    const core::Result<rhi::BufferHandle> pool_indices =
        desc.device->create_buffer({.size = uint64_t{desc.pool_indices} * sizeof(uint32_t),
                                    .usage = rhi::BufferUsage::Index | rhi::BufferUsage::CopyDst,
                                    .debug_name = "geometry pool indices"});
    if (!pool_indices) {
        return core::fail(pool_indices.error());
    }
    state->pool_indices = *pool_indices;
    const core::Result<rhi::BufferHandle> mesh_table =
        desc.device->create_buffer({.size = uint64_t{desc.max_meshes} * sizeof(GpuMeshInfo),
                                    .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                                    .debug_name = "mesh table"});
    if (!mesh_table) {
        return core::fail(mesh_table.error());
    }
    state->mesh_table = *mesh_table;
    state->mesh_table_address = desc.device->buffer_address(*mesh_table);
    const core::Result<rhi::BufferHandle> part_table =
        desc.device->create_buffer({.size = uint64_t{desc.max_parts} * sizeof(GpuMeshPart),
                                    .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                                    .debug_name = "part table"});
    if (!part_table) {
        return core::fail(part_table.error());
    }
    state->part_table = *part_table;
    state->part_table_address = desc.device->buffer_address(*part_table);
    for (uint32_t slot = desc.max_meshes; slot > 0; --slot) {
        state->free_mesh_slots.push(slot - 1); // handed out from the back: lowest first
    }
    // Slot 0 is the default material; the rest are handed out from the front.
    GpuMaterial fallback;
    fallback.base_color_texture = state->default_index[White];
    fallback.normal_texture = state->default_index[FlatNormal];
    fallback.metallic_roughness_texture = state->default_index[White];
    fallback.occlusion_texture = state->default_index[White];
    fallback.emissive_texture = state->default_index[White];
    state->table.resize(desc.max_materials);
    state->table[0] = fallback;
    state->dirty_first = 0;
    state->dirty_end = 1;
    state->upload_table();
    for (uint32_t slot = desc.max_materials - 1; slot > 0; --slot) {
        state->free_slots.push(slot);
    }

    if (const core::Result<void> listening = desc.assets->add_listener(&GpuAssetsState::on_notice, state); !listening) {
        return core::fail(listening.error());
    }
    return gpu_assets;
}

GpuAssets::~GpuAssets() {
    if (state_ == nullptr) {
        return;
    }
    state_->~GpuAssetsState();
    render_heap().deallocate(state_, sizeof(GpuAssetsState), alignof(GpuAssetsState));
}

GpuAssets::GpuAssets(GpuAssets&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}

GpuAssets& GpuAssets::operator=(GpuAssets&& other) noexcept {
    if (this != &other) {
        const GpuAssets discarded(std::exchange(state_, std::exchange(other.state_, nullptr)));
    }
    return *this;
}

uint32_t GpuAssets::fog_noise_index() const {
    return state_->uploader->ready(state_->fog_noise) ? state_->device->texture_index(state_->fog_noise) : 0;
}

void GpuAssets::update() {
    DE_PROFILE_ZONE("gpu assets");
    GpuAssetsState& state = *state_;
    // By ticket: a burst of other uploads, or a table upload every frame, doesn't hold this back.
    size_t recorded_tickets = 0;
    while (recorded_tickets < state.table_tickets.size() &&
           state.uploader->recorded(state.table_tickets[recorded_tickets])) {
        ++recorded_tickets;
    }
    if (recorded_tickets > 0) {
        for (size_t i = recorded_tickets; i < state.table_tickets.size(); ++i) {
            state.table_tickets[i - recorded_tickets] = state.table_tickets[i];
        }
        state.table_tickets.resize(state.table_tickets.size() - recorded_tickets);
        state.table_recorded += recorded_tickets;
    }
    // Materials whose textures were uploading: written again; kept while any still is.
    size_t kept = 0;
    for (size_t i = 0; i < state.dirty_materials.size(); ++i) {
        const uint64_t key = state.dirty_materials[i];
        MaterialEntry* entry = state.materials.find(key);
        if (entry == nullptr) {
            continue;
        }
        if (state.write_material(*entry)) {
            entry->queued = false;
        } else {
            state.dirty_materials[kept++] = key;
        }
    }
    state.dirty_materials.resize(kept);
    if (state.draw_kinds_changed) {
        for (auto [key, mesh] : state.meshes) {
            GpuAssetsState::count_kinds(mesh, state.table);
        }
        state.draw_kinds_changed = false;
    }
    state.upload_table();
    // Retired textures go once the table that no longer points at them is recorded.
    kept = 0;
    for (size_t i = 0; i < state.retired.size(); ++i) {
        if (state.table_recorded > state.retired[i].after_upload) {
            state.device->destroy(state.retired[i].texture);
        } else {
            state.retired[kept++] = state.retired[i];
        }
    }
    state.retired.resize(kept);
    // Meshes become drawable once their uploads and the table upload with their materials are in.
    kept = 0;
    for (size_t i = 0; i < state.pending_meshes.size(); ++i) {
        const uint64_t key = state.pending_meshes[i];
        MeshEntry* entry = state.meshes.find(key);
        if (entry == nullptr || entry->ready) {
            continue;
        }
        if (entry->needs_table == UINT64_MAX) {
            entry->needs_table = state.table_uploads; // every material it names is written by now
        }
        if (state.table_recorded >= entry->needs_table && state.uploader->recorded(entry->upload_ticket)) {
            entry->ready = true;
        } else {
            state.pending_meshes[kept++] = key;
        }
    }
    state.pending_meshes.resize(kept);
    state.free_retired_places();
}

uint64_t GpuAssets::mesh_table_address() const {
    return state_->mesh_table_address;
}

uint64_t GpuAssets::part_table_address() const {
    return state_->part_table_address;
}

uint64_t GpuAssets::pool_vertex_address() const {
    return state_->pool_vertex_address;
}

rhi::BufferHandle GpuAssets::pool_index_buffer() const {
    return state_->pool_indices;
}

const GpuMesh* GpuAssets::mesh(asset::AssetHandle mesh) const {
    const MeshEntry* entry = state_->meshes.find(key_of(mesh));
    return entry != nullptr && entry->ready ? &entry->gpu : nullptr;
}

GpuMaterialFlags GpuAssets::material_flags(uint32_t material) const {
    const GpuAssetsState& state = *state_;
    return material < state.table.size() ? state.table[material].flags : GpuMaterialFlags::None;
}

uint64_t GpuAssets::material_table_address() const {
    return state_->material_table_address;
}

GpuAssetsStats GpuAssets::stats() const {
    const GpuAssetsState& state = *state_;
    uint32_t meshes = 0;
    for (const auto& [key, mesh] : state.meshes) {
        meshes += mesh.ready ? 1 : 0;
    }
    return {.meshes = meshes,
            .textures = static_cast<uint32_t>(state.textures.size()),
            .materials = static_cast<uint32_t>(state.materials.size()),
            .pending = static_cast<uint32_t>(state.dirty_materials.size() + state.pending_meshes.size()),
            .bytes = state.bytes,
            .meshes_dropped = static_cast<uint32_t>(state.dropped_meshes.size()),
            .pool_vertices_free = state.vertex_ranges.free_total(),
            .pool_indices_free = state.index_ranges.free_total(),
            .table_parts_free = state.part_ranges.free_total()};
}

} // namespace de::render
