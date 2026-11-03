#pragma once

#include "cooker/common.h"

#include <yyjson.h>

#include <cstddef>
#include <span>
#include <string_view>

namespace de::cooker {

// JSON (yyjson) for the cooker's text files: metas, folder rules, text materials and its own state.
// Comments and trailing commas are accepted in what people write.

// yyjson's allocator, on the Tools heap.
const yyjson_alc* json_allocator();

// A parsed, read-only document. Move-only.
class JsonDoc {
public:
    JsonDoc() = default;
    // Reports a syntax error as "line:column: message" against path.
    static core::Result<JsonDoc> parse(std::span<const std::byte> text, std::string_view path,
                                       Diagnostics& diagnostics);

    ~JsonDoc();
    JsonDoc(JsonDoc&& other) noexcept;
    JsonDoc& operator=(JsonDoc&& other) noexcept;
    JsonDoc(const JsonDoc&) = delete;
    JsonDoc& operator=(const JsonDoc&) = delete;

    yyjson_val* root() const { return doc_ != nullptr ? yyjson_doc_get_root(doc_) : nullptr; }

private:
    friend class JsonBuilder;
    explicit JsonDoc(yyjson_doc* doc) : doc_(doc) {}
    yyjson_doc* doc_ = nullptr;
};

// A document being built (settings layered by merge patches, files the cooker writes). Move-only.
class JsonBuilder {
public:
    JsonBuilder();
    ~JsonBuilder();
    JsonBuilder(JsonBuilder&& other) noexcept;
    JsonBuilder& operator=(JsonBuilder&& other) noexcept;
    JsonBuilder(const JsonBuilder&) = delete;
    JsonBuilder& operator=(const JsonBuilder&) = delete;

    yyjson_mut_doc* doc() const { return doc_; }
    yyjson_mut_val* root() const { return yyjson_mut_doc_get_root(doc_); }
    void set_root(yyjson_mut_val* root) { yyjson_mut_doc_set_root(doc_, root); }

    // An object, owned by this document.
    yyjson_mut_val* object() { return yyjson_mut_obj(doc_); }
    // Applies patch (RFC 7386 merge patch: objects merge key by key, null deletes, the rest
    // replaces) to the root, which starts as an empty object.
    void merge(yyjson_val* patch);

    // A read-only copy, for JsonReader.
    JsonDoc freeze() const;

    // Pretty: two-space indents and a final newline, as people's files are. Compact otherwise
    // (hashing). Empty on failure, which only running out of memory causes.
    core::Array<char> write(bool pretty) const;

private:
    yyjson_mut_doc* doc_ = nullptr;
};

// Typed reads of an object's fields, with a fallback for missing ones. A field of the wrong type
// is reported against path and gets the fallback.
struct JsonReader {
    yyjson_val* object = nullptr;
    std::string_view path;
    Diagnostics* diagnostics = nullptr;

    bool has(const char* key) const { return yyjson_obj_get(object, key) != nullptr; }
    bool boolean(const char* key, bool fallback) const;
    double number(const char* key, double fallback) const;
    std::string_view string(const char* key, std::string_view fallback) const;
    // Reports every field not in known (a typo would otherwise be silently ignored).
    void check_keys(std::span<const char* const> known, const char* what) const;
};

// count numbers from an array of exactly that many ([x, y, z]); false otherwise.
template <typename T>
bool read_numbers(yyjson_val* value, T* out, size_t count) {
    if (!yyjson_is_arr(value) || yyjson_arr_size(value) != count) {
        return false;
    }
    size_t i = 0;
    size_t n = 0;
    yyjson_val* item = nullptr;
    yyjson_arr_foreach(value, i, n, item) {
        if (!yyjson_is_num(item)) {
            return false;
        }
        out[i] = static_cast<T>(yyjson_get_num(item));
    }
    return true;
}

} // namespace de::cooker
