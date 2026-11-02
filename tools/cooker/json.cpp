#include "cooker/json.h"

#include <cstring>
#include <utility>

namespace de::cooker {

namespace {

void* json_malloc(void* /*context*/, size_t size) {
    return core::tagged_malloc(core::MemoryTag::Tools, size);
}

void* json_realloc(void* /*context*/, void* pointer, size_t /*old_size*/, size_t size) {
    return core::tagged_realloc(core::MemoryTag::Tools, pointer, size);
}

void json_free(void* /*context*/, void* pointer) {
    core::tagged_free(core::MemoryTag::Tools, pointer);
}

constexpr yyjson_alc kAllocator{.malloc = &json_malloc, .realloc = &json_realloc, .free = &json_free, .ctx = nullptr};

} // namespace

const yyjson_alc* json_allocator() {
    return &kAllocator;
}

core::Result<JsonDoc> JsonDoc::parse(std::span<const std::byte> text, std::string_view path, Diagnostics& diagnostics) {
    yyjson_read_err error{};
    // yyjson doesn't write to the text without YYJSON_READ_INSITU.
    char* data = const_cast<char*>(reinterpret_cast<const char*>(text.data()));
    yyjson_doc* doc = yyjson_read_opts(
        data, text.size(), YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS, json_allocator(), &error);
    if (doc == nullptr) {
        size_t line = 0;
        size_t column = 0;
        size_t character = 0;
        yyjson_locate_pos(data, text.size(), error.pos, &line, &column, &character);
        diagnostics.error(path, "%zu:%zu: invalid JSON: %s", line, column, error.msg != nullptr ? error.msg : "");
        return core::fail(core::ErrorCode::CorruptData, "invalid JSON");
    }
    return JsonDoc(doc);
}

JsonDoc::~JsonDoc() {
    yyjson_doc_free(doc_);
}

JsonDoc::JsonDoc(JsonDoc&& other) noexcept : doc_(std::exchange(other.doc_, nullptr)) {}

JsonDoc& JsonDoc::operator=(JsonDoc&& other) noexcept {
    if (this != &other) {
        yyjson_doc_free(doc_);
        doc_ = std::exchange(other.doc_, nullptr);
    }
    return *this;
}

JsonBuilder::JsonBuilder() : doc_(yyjson_mut_doc_new(json_allocator())) {
    DE_VERIFY_MSG(doc_ != nullptr, "out of memory");
    set_root(object());
}

JsonBuilder::~JsonBuilder() {
    yyjson_mut_doc_free(doc_);
}

JsonBuilder::JsonBuilder(JsonBuilder&& other) noexcept : doc_(std::exchange(other.doc_, nullptr)) {}

JsonBuilder& JsonBuilder::operator=(JsonBuilder&& other) noexcept {
    if (this != &other) {
        yyjson_mut_doc_free(doc_);
        doc_ = std::exchange(other.doc_, nullptr);
    }
    return *this;
}

void JsonBuilder::merge(yyjson_val* patch) {
    if (patch == nullptr) {
        return;
    }
    yyjson_mut_val* merged = yyjson_mut_merge_patch(doc_, root(), yyjson_val_mut_copy(doc_, patch));
    DE_VERIFY_MSG(merged != nullptr, "out of memory");
    set_root(merged);
}

JsonDoc JsonBuilder::freeze() const {
    yyjson_doc* copy = yyjson_mut_doc_imut_copy(doc_, json_allocator());
    DE_VERIFY_MSG(copy != nullptr, "out of memory");
    return JsonDoc(copy);
}

core::Array<char> JsonBuilder::write(bool pretty) const {
    core::Array<char> text(tools_heap());
    const yyjson_write_flag flags = pretty ? (YYJSON_WRITE_PRETTY_TWO_SPACES | YYJSON_WRITE_NEWLINE_AT_END) : 0;
    size_t length = 0;
    char* written = yyjson_mut_write_opts(doc_, flags, json_allocator(), &length, nullptr);
    if (written == nullptr) {
        return text;
    }
    text.resize_uninit(length);
    std::memcpy(text.data(), written, length);
    json_free(nullptr, written);
    return text;
}

bool JsonReader::boolean(const char* key, bool fallback) const {
    yyjson_val* value = yyjson_obj_get(object, key);
    if (value == nullptr) {
        return fallback;
    }
    if (!yyjson_is_bool(value)) {
        diagnostics->error(path, "'%s' should be true or false", key);
        return fallback;
    }
    return yyjson_get_bool(value);
}

double JsonReader::number(const char* key, double fallback) const {
    yyjson_val* value = yyjson_obj_get(object, key);
    if (value == nullptr) {
        return fallback;
    }
    if (!yyjson_is_num(value)) {
        diagnostics->error(path, "'%s' should be a number", key);
        return fallback;
    }
    return yyjson_get_num(value);
}

std::string_view JsonReader::string(const char* key, std::string_view fallback) const {
    yyjson_val* value = yyjson_obj_get(object, key);
    if (value == nullptr) {
        return fallback;
    }
    if (!yyjson_is_str(value)) {
        diagnostics->error(path, "'%s' should be a string", key);
        return fallback;
    }
    return {yyjson_get_str(value), yyjson_get_len(value)};
}

void JsonReader::check_keys(std::span<const char* const> known, const char* what) const {
    if (!yyjson_is_obj(object)) {
        return;
    }
    size_t index = 0;
    size_t count = 0;
    yyjson_val* key = nullptr;
    yyjson_val* value = nullptr;
    yyjson_obj_foreach(object, index, count, key, value) {
        bool found = false;
        for (const char* name : known) {
            found = found || std::strcmp(name, yyjson_get_str(key)) == 0;
        }
        if (!found) {
            diagnostics->warning(path, "unknown %s setting '%s' (ignored)", what, yyjson_get_str(key));
        }
    }
}

} // namespace de::cooker
