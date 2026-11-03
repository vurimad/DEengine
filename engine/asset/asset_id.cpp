#include "asset/asset_id.h"

namespace de::asset {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Writes value's low digits nibbles as hex, most significant first; returns the end.
char* write_hex(char* out, uint64_t value, int digits) {
    for (int i = digits - 1; i >= 0; --i) {
        *out++ = kHexDigits[(value >> (static_cast<unsigned>(i) * 4U)) & 0xFU];
    }
    return out;
}

bool parse_hex(std::string_view text, uint64_t& value) {
    value = 0;
    for (const char c : text) {
        const int digit = hex_value(c);
        if (digit < 0) {
            return false;
        }
        value = (value << 4U) | static_cast<uint64_t>(digit);
    }
    return true;
}

} // namespace

GuidText format_guid(Guid guid) {
    char text[kGuidTextLength];
    char* out = write_hex(text, guid.high >> 32U, 8);
    *out++ = '-';
    out = write_hex(out, guid.high >> 16U, 4);
    *out++ = '-';
    out = write_hex(out, guid.high, 4);
    *out++ = '-';
    out = write_hex(out, guid.low >> 48U, 4);
    *out++ = '-';
    write_hex(out, guid.low, 12);
    return GuidText(std::string_view(text, kGuidTextLength));
}

bool parse_guid(std::string_view text, Guid& guid) {
    if (text.size() != kGuidTextLength || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-') {
        return false;
    }
    uint64_t parts[5] = {};
    if (!parse_hex(text.substr(0, 8), parts[0]) || !parse_hex(text.substr(9, 4), parts[1]) ||
        !parse_hex(text.substr(14, 4), parts[2]) || !parse_hex(text.substr(19, 4), parts[3]) ||
        !parse_hex(text.substr(24, 12), parts[4])) {
        return false;
    }
    const Guid parsed{.high = (parts[0] << 32U) | (parts[1] << 16U) | parts[2], .low = (parts[3] << 48U) | parts[4]};
    if (!parsed.is_valid()) {
        return false;
    }
    guid = parsed;
    return true;
}

AssetIdText format_asset_id(AssetId id) {
    char text[kAssetIdTextLength];
    write_hex(text, id.value(), 16);
    return AssetIdText(std::string_view(text, kAssetIdTextLength));
}

bool parse_asset_id(std::string_view text, AssetId& id) {
    uint64_t value = 0;
    if (!parse_hex64(text, value) || value == 0) {
        return false;
    }
    id = AssetId::from_value(value);
    return true;
}

bool parse_hex64(std::string_view text, uint64_t& value) {
    uint64_t parsed = 0;
    if (text.size() != kAssetIdTextLength || !parse_hex(text, parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

} // namespace de::asset
