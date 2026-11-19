#include "cooker/prefab_cooker.h"

#include "asset/collision_format.h"
#include "cooker/blob_writer.h"
#include "cooker/entity_transform.h"
#include "cooker/json.h"
#include "cooker/light_source.h"
#include "core/hash.h"

#include <algorithm>
#include <cmath>

namespace de::cooker {

namespace {

using asset::PrefabLimits;
using asset::PrefabPart;

using PartPath = core::FixedString<kMaxSourcePath>;

// Where a part is, from its prefab's origin.
struct Placed {
    Vec3 position{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f};
};

// A part placed in a prefab that's itself placed.
Placed compose(const Placed& outer, const Placed& inner) {
    return {.position = outer.position + (outer.rotation * (outer.scale * inner.position)),
            .rotation = glm::normalize(outer.rotation * inner.rotation),
            .scale = outer.scale * inner.scale};
}

bool valid_id(std::string_view id) {
    return !id.empty() && id.size() <= PrefabLimits::kMaxIdLength &&
           std::ranges::all_of(id, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

// Whether a path names this part or something it's inside: "table" covers "table" and "table/candle/flame".
bool covers(std::string_view named, std::string_view part) {
    return part == named || (part.size() > named.size() && part.starts_with(named) && part[named.size()] == '/');
}

std::string_view text_of(yyjson_val* value) {
    return yyjson_is_str(value) ? std::string_view(yyjson_get_str(value), yyjson_get_len(value)) : std::string_view();
}

class PrefabImport {
public:
    PrefabImport(const SourceTree& tree, std::string_view path, SourceReader read, Diagnostics& diagnostics)
        : tree_(tree), path_(path), read_(read), diagnostics_(diagnostics) {}

    core::Result<PrefabBuild> run(yyjson_val* root) {
        const size_t errors_before = diagnostics_.error_count();
        SourcePath self;
        static_cast<void>(self.assign(path_));
        nesting_.push(self);
        flatten(root, path_, {}, {}, {}, true);
        if (flat_.empty() && diagnostics_.error_count() == errors_before) {
            diagnostics_.error(path_, "parts should hold at least one part");
        }
        read_variants(yyjson_obj_get(root, "variants"));
        if (diagnostics_.error_count() != errors_before) {
            return core::fail(core::ErrorCode::InvalidArgument, "the prefab has errors");
        }
        for (const Flat& flat : flat_) {
            PrefabPart part = flat.part;
            const std::string_view name = flat.path.view();
            part.name_offset = static_cast<uint32_t>(build_.names.size());
            part.name_length = static_cast<uint16_t>(name.size());
            part.id = asset::prefab_part_id(name);
            for (const char c : name) {
                build_.names.push(c);
            }
            build_.reach_m =
                std::max(build_.reach_m, glm::length(Vec3(part.position[0], part.position[1], part.position[2])));
            build_.parts.push(part);
        }
        return std::move(build_);
    }

private:
    struct Flat {
        PrefabPart part;
        PartPath path; // its IDs, from this prefab down
    };

    uint32_t depend(const asset::AssetRef& ref, const SourcePath& source) {
        if (!std::ranges::any_of(build_.read,
                                 [&source](const SourcePath& had) { return had.view() == source.view(); })) {
            build_.read.push(source);
        }
        for (size_t i = 0; i < build_.dependencies.size(); ++i) {
            if (build_.dependencies[i] == ref) {
                return static_cast<uint32_t>(i);
            }
        }
        build_.dependencies.push(ref);
        return static_cast<uint32_t>(build_.dependencies.size() - 1);
    }

    // The source a part names, relative to the file that names it: of the kinds given, or null (reported).
    const Source* named(std::string_view file, std::string_view where, const char* key, std::string_view reference,
                        std::initializer_list<SourceKind> kinds, const char* what) {
        SourcePath joined;
        const Source* found = join_relative(folder_of(file), reference, joined) ? tree_.find(joined.view()) : nullptr;
        if (found == nullptr || std::ranges::find(kinds, found->kind) == kinds.end()) {
            diagnostics_.error(path_, "%.*s: %s '%.*s' should name %s, relative to %.*s",
                               static_cast<int>(where.size()), where.data(), key, static_cast<int>(reference.size()),
                               reference.data(), what, static_cast<int>(file.size()), file.data());
            return nullptr;
        }
        return found;
    }

    // The parts of one prefab source into flat_, each placed by `outer` and named under `prefix`; `variant` of that
    // source's variants leaves its parts out. top: the prefab being cooked, whose own variants are kept as bits.
    void flatten(yyjson_val* root, std::string_view file, const Placed& outer, std::string_view prefix,
                 std::string_view variant, bool top) {
        const JsonReader reader{.object = root, .path = path_, .diagnostics = &diagnostics_};
        if (top) {
            static constexpr const char* kKeys[] = {"parts", "variants"};
            reader.check_keys(kKeys, "prefab");
        }
        yyjson_val* parts = yyjson_obj_get(root, "parts");
        if (!yyjson_is_obj(parts)) {
            diagnostics_.error(path_, "%.*s: parts should be an object: an ID for each part",
                               static_cast<int>(file.size()), file.data());
            return;
        }
        // What the variant asked for leaves out, among this source's parts.
        yyjson_val* left_out = nullptr;
        if (!variant.empty()) {
            yyjson_val* variants = yyjson_obj_get(root, "variants");
            left_out = yyjson_is_obj(variants) ? yyjson_obj_getn(variants, variant.data(), variant.size()) : nullptr;
            if (!yyjson_is_obj(left_out)) {
                diagnostics_.error(path_, "%.*s has no variant '%.*s'", static_cast<int>(file.size()), file.data(),
                                   static_cast<int>(variant.size()), variant.data());
                left_out = nullptr;
            }
        }
        const size_t first = flat_.size();
        size_t i = 0;
        size_t n = 0;
        yyjson_val* key = nullptr;
        yyjson_val* value = nullptr;
        yyjson_obj_foreach(parts, i, n, key, value) {
            read_part(text_of(key), value, file, outer, prefix);
        }
        if (left_out != nullptr) {
            // Paths in a variant are from its own prefab: under `prefix` here.
            yyjson_obj_foreach(left_out, i, n, key, value) {
                PartPath gone;
                if (!join(prefix, text_of(key), gone)) {
                    continue;
                }
                size_t kept = first;
                for (size_t at = first; at < flat_.size(); ++at) {
                    if (!covers(gone.view(), flat_[at].path.view())) {
                        if (kept != at) {
                            flat_[kept] = flat_[at];
                        }
                        ++kept;
                    }
                }
                flat_.resize(kept);
            }
        }
    }

    static bool join(std::string_view prefix, std::string_view id, PartPath& out) {
        return out.assign(prefix) && (prefix.empty() || out.append('/')) && out.append(id);
    }

    void read_part(std::string_view id, yyjson_val* object, std::string_view file, const Placed& outer,
                   std::string_view prefix) {
        PartPath path;
        if (!valid_id(id) || !join(prefix, id, path)) {
            diagnostics_.error(path_,
                               "%.*s: part '%.*s' should have an ID of a-z, 0-9 and _, at most %u characters (and "
                               "its path through the prefabs it's in at most %zu)",
                               static_cast<int>(file.size()), file.data(), static_cast<int>(id.size()), id.data(),
                               PrefabLimits::kMaxIdLength, PartPath::capacity());
            return;
        }
        const std::string_view where = path.view();
        const auto bad = [&](const char* what) {
            diagnostics_.error(path_, "part '%.*s': %s", static_cast<int>(where.size()), where.data(), what);
        };
        if (!yyjson_is_obj(object)) {
            bad("should be an object: a mesh, an effect or a prefab, and where it is");
            return;
        }
        const JsonReader reader{.object = object, .path = path_, .diagnostics = &diagnostics_};
        static constexpr const char* kKeys[] = {"mesh",     "collision", "effect",       "prefab", "variant",
                                                "position", "rotation",  "rotation_deg", "scale",  "light"};
        reader.check_keys(kKeys, "prefab part");
        Placed local;
        float position[3] = {};
        float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        float scale[3] = {1.0f, 1.0f, 1.0f};
        read_entity_transform(object, reader, position, rotation, scale, bad);
        local.position = Vec3(position[0], position[1], position[2]);
        local.rotation = Quat(rotation[3], rotation[0], rotation[1], rotation[2]);
        local.scale = Vec3(scale[0], scale[1], scale[2]);
        const Placed placed = compose(outer, local);

        if (reader.has("prefab")) {
            if (reader.has("mesh") || reader.has("effect") || reader.has("collision") || reader.has("light")) {
                bad("is a prefab: a mesh, collision, an effect and a light belong to parts of their own");
            }
            nest(reader.string("prefab", ""), reader.string("variant", ""), file, placed, where, bad);
            return;
        }
        if (reader.has("variant")) {
            bad("names a variant, which only a part that's a prefab has");
        }
        if (!reader.has("mesh") && !reader.has("effect") && !reader.has("light")) {
            bad("has nothing in it: give it a mesh, an effect, a light or a prefab");
            return;
        }
        Flat flat;
        flat.path = path;
        for (int axis = 0; axis < 3; ++axis) {
            flat.part.position[axis] = placed.position[axis];
            flat.part.scale[axis] = placed.scale[axis];
        }
        flat.part.rotation[0] = placed.rotation.x;
        flat.part.rotation[1] = placed.rotation.y;
        flat.part.rotation[2] = placed.rotation.z;
        flat.part.rotation[3] = placed.rotation.w;
        if (!std::isfinite(glm::length(placed.position)) || glm::length(placed.position) > PrefabLimits::kMaxReachM) {
            bad("lies too far from the prefab's origin");
        }
        if (reader.has("collision")) {
            if (reader.string("collision", "mesh") != "mesh") {
                bad("collision should be mesh");
            } else if (!reader.has("mesh")) {
                bad("collision needs a mesh to take its shape from");
            }
        }
        if (reader.has("mesh")) {
            if (const Source* mesh = named(file, where, "mesh", reader.string("mesh", ""),
                                           {SourceKind::Gltf, SourceKind::Fbx}, "a model source (.gltf, .glb, .fbx)")) {
                flat.part.mesh = depend({.id = mesh->id, .type = asset::AssetType::Mesh}, mesh->path);
                if (reader.has("collision")) {
                    flat.part.collision = depend(
                        {.id = asset::collision_asset_id(mesh->id), .type = asset::AssetType::Collision}, mesh->path);
                }
            }
        }
        if (reader.has("effect")) {
            if (const Source* effect = named(file, where, "effect", reader.string("effect", ""), {SourceKind::Effect},
                                             "an effect source (.vfx)")) {
                flat.part.effect = depend({.id = effect->id, .type = asset::AssetType::Effect}, effect->path);
            }
        }
        if (yyjson_val* light = yyjson_obj_get(object, "light"); light != nullptr) {
            asset::LightDesc read;
            read_light(light, path_, diagnostics_, read, bad);
            flat.part.light = static_cast<uint32_t>(build_.lights.size());
            build_.lights.push(read);
        }
        if (flat_.size() >= PrefabLimits::kMaxParts) {
            if (flat_.size() == PrefabLimits::kMaxParts && !too_many_) {
                too_many_ = true;
                diagnostics_.error(path_, "has more than %u parts, with those of the prefabs in it",
                                   PrefabLimits::kMaxParts);
            }
            return;
        }
        flat_.push(flat);
    }

    // Another prefab's parts, placed where the part that names it is.
    template <typename Bad>
    void nest(std::string_view reference, std::string_view variant, std::string_view file, const Placed& placed,
              std::string_view prefix, Bad&& bad) {
        const Source* inner =
            named(file, prefix, "prefab", reference, {SourceKind::Prefab}, "a prefab source (.prefab)");
        if (inner == nullptr) {
            return;
        }
        if (std::ranges::any_of(nesting_,
                                [inner](const SourcePath& open) { return open.view() == inner->path.view(); })) {
            bad("is a prefab that holds this one: a prefab can't be inside itself");
            return;
        }
        if (nesting_.size() > PrefabLimits::kMaxDepth) {
            bad("is nested too deep: at most 8 prefabs inside one another");
            return;
        }
        if (!std::ranges::any_of(build_.read,
                                 [inner](const SourcePath& had) { return had.view() == inner->path.view(); })) {
            build_.read.push(inner->path);
        }
        const core::Result<core::Array<std::byte>> bytes = read_(inner->path.view());
        if (!bytes) {
            return; // reported by read
        }
        // Its mistakes are its own cook's to report: here, only that it can't be used.
        Diagnostics quiet(false);
        const core::Result<JsonDoc> doc = JsonDoc::parse(bytes->span(), inner->path.view(), quiet);
        if (!doc || !yyjson_is_obj(doc->root())) {
            bad("is a prefab that can't be read: see its own errors");
            return;
        }
        nesting_.push(inner->path);
        flatten(doc->root(), inner->path.view(), placed, prefix, variant, false);
        nesting_.pop();
    }

    // "variants": {"unlit": {"flame": null}}: a bit each, set in the parts it leaves out.
    void read_variants(yyjson_val* variants) {
        if (variants == nullptr) {
            return;
        }
        if (!yyjson_is_obj(variants) || yyjson_obj_size(variants) > PrefabLimits::kMaxVariants) {
            diagnostics_.error(path_, "variants should be an object of at most %u variants",
                               PrefabLimits::kMaxVariants);
            return;
        }
        size_t i = 0;
        size_t n = 0;
        yyjson_val* key = nullptr;
        yyjson_val* value = nullptr;
        yyjson_obj_foreach(variants, i, n, key, value) {
            const std::string_view name = text_of(key);
            const auto bit = static_cast<uint32_t>(build_.variants.size());
            if (!valid_id(name) || !yyjson_is_obj(value)) {
                diagnostics_.error(path_,
                                   "variant '%.*s' should have a name of a-z, 0-9 and _, at most %u characters, and "
                                   "be an object of the parts it leaves out",
                                   static_cast<int>(name.size()), name.data(), PrefabLimits::kMaxIdLength);
                continue;
            }
            const uint32_t hash = asset::prefab_variant_hash(name);
            if (std::ranges::any_of(build_.variants,
                                    [hash](const asset::PrefabVariant& had) { return had.name_hash == hash; })) {
                diagnostics_.error(path_, "variant '%.*s' is there twice", static_cast<int>(name.size()), name.data());
                continue;
            }
            build_.variants.push({.name_offset = static_cast<uint32_t>(build_.names.size()),
                                  .name_length = static_cast<uint16_t>(name.size()),
                                  .name_hash = hash});
            for (const char c : name) {
                build_.names.push(c);
            }
            size_t j = 0;
            size_t m = 0;
            yyjson_val* part = nullptr;
            yyjson_val* what = nullptr;
            yyjson_obj_foreach(value, j, m, part, what) {
                const std::string_view named_part = text_of(part);
                bool found = false;
                for (Flat& flat : flat_) {
                    if (covers(named_part, flat.path.view())) {
                        flat.part.absent_in |= 1U << bit;
                        found = true;
                    }
                }
                if (!yyjson_is_null(what)) {
                    diagnostics_.error(path_,
                                       "variant '%.*s': '%.*s' should be null (the part left out): a variant can't "
                                       "change a part yet",
                                       static_cast<int>(name.size()), name.data(), static_cast<int>(named_part.size()),
                                       named_part.data());
                } else if (!found) {
                    diagnostics_.error(path_, "variant '%.*s' leaves out '%.*s', which isn't a part of the prefab",
                                       static_cast<int>(name.size()), name.data(), static_cast<int>(named_part.size()),
                                       named_part.data());
                }
            }
        }
    }

    const SourceTree& tree_;
    std::string_view path_;
    SourceReader read_;
    Diagnostics& diagnostics_;
    PrefabBuild build_;
    core::Array<Flat> flat_{tools_heap()};
    core::Array<SourcePath> nesting_{tools_heap()}; // the prefabs being read, outermost first
    bool too_many_ = false;
};

// The variants object of a prefab source, or null.
yyjson_val* variants_of(const JsonDoc& doc) {
    yyjson_val* variants = yyjson_is_obj(doc.root()) ? yyjson_obj_get(doc.root(), "variants") : nullptr;
    return yyjson_is_obj(variants) ? variants : nullptr;
}

} // namespace

core::Result<PrefabBuild> import_prefab(const SourceTree& tree, const Source& source, std::span<const std::byte> bytes,
                                        SourceReader read, Diagnostics& diagnostics) {
    const std::string_view path = source.path.view();
    const core::Result<JsonDoc> doc = JsonDoc::parse(bytes, path, diagnostics);
    if (!doc) {
        return core::fail(doc.error());
    }
    if (!yyjson_is_obj(doc->root())) {
        diagnostics.error(path, "should be a JSON object");
        return core::fail(core::ErrorCode::InvalidArgument, "the prefab has errors");
    }
    PrefabImport import(tree, path, read, diagnostics);
    return import.run(doc->root());
}

core::Array<std::byte> cook_prefab(const PrefabBuild& build, asset::AssetId id) {
    asset::PrefabAsset prefab;
    prefab.reach_m = build.reach_m;
    BlobWriter writer(sizeof(asset::PrefabAsset));
    prefab.parts = writer.append(build.parts.span());
    prefab.variants = writer.append(build.variants.span());
    prefab.names = writer.append(build.names.span());
    prefab.lights = writer.append(build.lights.span());
    return writer.finish(prefab, id, build.dependencies.span());
}

uint64_t prefab_variants_hash(std::span<const std::byte> bytes) {
    Diagnostics quiet(false);
    const core::Result<JsonDoc> doc = JsonDoc::parse(bytes, "", quiet);
    if (!doc || !yyjson_is_obj(doc->root())) {
        return 0;
    }
    // Whatever order they're written in.
    uint64_t hash = 0x9e3779b97f4a7c15ULL;
    if (yyjson_val* variants = variants_of(*doc)) {
        size_t i = 0;
        size_t n = 0;
        yyjson_val* key = nullptr;
        yyjson_val* value = nullptr;
        yyjson_obj_foreach(variants, i, n, key, value) {
            const std::string_view name = text_of(key);
            hash += core::mix64(core::fnv1a_64(name.data(), name.size()));
        }
    }
    return hash;
}

bool prefab_has_variant(std::span<const std::byte> bytes, std::string_view variant) {
    Diagnostics quiet(false);
    const core::Result<JsonDoc> doc = JsonDoc::parse(bytes, "", quiet);
    if (!doc) {
        return false;
    }
    yyjson_val* variants = variants_of(*doc);
    return variants != nullptr && yyjson_is_obj(yyjson_obj_getn(variants, variant.data(), variant.size()));
}

} // namespace de::cooker
