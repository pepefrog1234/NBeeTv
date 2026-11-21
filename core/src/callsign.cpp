#include "callsign.hpp"

namespace nbtv {

namespace {

constexpr std::uint64_t kBase = 40;
constexpr std::uint64_t kLimit = 262144000000000ULL;  // 40^9

int symbol_index(char c) {
    const auto pos = kCallsignAlphabet.find(c);
    return pos == std::string_view::npos ? -1 : static_cast<int>(pos);
}

}  // namespace

std::string normalize_callsign(std::string_view text) {
    std::string out;
    for (char c : text) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (symbol_index(c) < 0) continue;
        if (out.empty() && c == ' ') continue;  // 開頭的空白
        if (static_cast<int>(out.size()) < kCallsignMaxChars) out.push_back(c);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::array<std::uint8_t, kCallsignBytes> encode_callsign(std::string_view text) {
    const std::string call = normalize_callsign(text);
    std::uint64_t value = 0;
    for (int i = 0; i < kCallsignMaxChars; ++i) {
        const int sym = i < static_cast<int>(call.size()) ? symbol_index(call[static_cast<std::size_t>(i)]) : 0;
        value = value * kBase + static_cast<std::uint64_t>(sym);
    }
    std::array<std::uint8_t, kCallsignBytes> out{};
    for (int i = kCallsignBytes - 1; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value & 0xFF);
        value >>= 8;
    }
    return out;
}

std::optional<std::string> decode_callsign(const std::array<std::uint8_t, kCallsignBytes>& bytes) {
    std::uint64_t value = 0;
    for (std::uint8_t b : bytes) value = (value << 8) | b;
    if (value >= kLimit) return std::nullopt;
    std::string out(static_cast<std::size_t>(kCallsignMaxChars), ' ');
    for (int i = kCallsignMaxChars - 1; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kCallsignAlphabet[static_cast<std::size_t>(value % kBase)];
        value /= kBase;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

}  // namespace nbtv
