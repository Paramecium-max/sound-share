#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace miplay::utf8 {

// Length of the next Unicode scalar, or zero for malformed UTF-8.
inline size_t scalar_length(std::string_view bytes) {
    if (bytes.empty()) return 0;
    const auto lead = static_cast<uint8_t>(bytes[0]);
    if (lead < 0x80) return 1;
    size_t size;
    if (lead >= 0xC2 && lead <= 0xDF) size = 2;
    else if (lead >= 0xE0 && lead <= 0xEF) size = 3;
    else if (lead >= 0xF0 && lead <= 0xF4) size = 4;
    else return 0;
    if (bytes.size() < size) return 0;
    for (size_t i = 1; i < size; ++i) {
        if ((static_cast<uint8_t>(bytes[i]) & 0xC0) != 0x80) return 0;
    }
    const auto second = static_cast<uint8_t>(bytes[1]);
    if ((lead == 0xE0 && second < 0xA0) || (lead == 0xED && second >= 0xA0) ||
        (lead == 0xF0 && second < 0x90) || (lead == 0xF4 && second >= 0x90)) return 0;
    return size;
}

inline bool valid(std::string_view bytes) {
    while (!bytes.empty()) {
        const auto size = scalar_length(bytes);
        if (!size) return false;
        bytes.remove_prefix(size);
    }
    return true;
}

inline void append(std::string& text, uint32_t scalar) {
    if (scalar < 0x80) {
        text.push_back(static_cast<char>(scalar));
        return;
    }
    const unsigned count = scalar < 0x800 ? 2 : scalar < 0x10000 ? 3 : 4;
    const unsigned shift = 6 * (count - 1);
    const unsigned prefix = count == 2 ? 0xC0 : count == 3 ? 0xE0 : 0xF0;
    text.push_back(static_cast<char>(prefix | (scalar >> shift)));
    for (unsigned bits = shift; bits > 0;) {
        bits -= 6;
        text.push_back(static_cast<char>(0x80 | ((scalar >> bits) & 0x3F)));
    }
}

} // namespace miplay::utf8
