#include "bits.hpp"

#include <cassert>

namespace nbtv {

void bytes_to_bits(std::span<const std::uint8_t> bytes, std::span<std::uint8_t> out) {
    assert(out.size() == bytes.size() * 8);
    std::size_t o = 0;
    for (std::uint8_t b : bytes) {
        for (int i = 7; i >= 0; --i) out[o++] = static_cast<std::uint8_t>((b >> i) & 1);
    }
}

void bits_to_bytes(std::span<const std::uint8_t> bits, std::span<std::uint8_t> out) {
    assert(bits.size() % 8 == 0 && out.size() == bits.size() / 8);
    for (std::size_t i = 0; i < out.size(); ++i) {
        std::uint8_t b = 0;
        for (int j = 0; j < 8; ++j) b = static_cast<std::uint8_t>((b << 1) | (bits[i * 8 + static_cast<std::size_t>(j)] & 1));
        out[i] = b;
    }
}

namespace {

// CRC-32 的查表（多項式 0xEDB88320，反射式）。第 0 張是一般的位元組表：一個位元組的 8 次移位先算好。
// 第 s 張是「那個位元組之後又移過 s 個位元組」的結果；CRC 是線性的，各個位元組的結果互斥或起來就是整體的，
// 所以 8 個位元組可以各查一張表、一次併進去（slicing-by-8）。
// 權重檔每次載入都要算一遍檢查碼。一個位元一個位元算的話，一份 44 MB 的檔案要 0.3 秒（開發用的 Mac 上），
// 載入模型的時間有九成花在這裡（九份權重檔合計一秒半）；查表之後檢查碼快十幾倍，算出來的值相同。
using Crc32Tables = std::array<std::array<std::uint32_t, 256>, 8>;

Crc32Tables make_crc32_tables() {
    Crc32Tables t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        t[0][i] = c;
    }
    for (std::size_t s = 1; s < t.size(); ++s) {
        for (std::size_t i = 0; i < 256; ++i) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFFu];
    }
    return t;
}

// 四個位元組照「低位元組在前」組成一個數。不靠處理器的位元組順序，也不要求位址對齊；
// 低位元組在前的處理器上，編譯器會把它變成一次讀取。
std::uint32_t load_le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    static const Crc32Tables tables = make_crc32_tables();  // 第一次用到時算一次（8 KB）
    const Crc32Tables& t = tables;
    const std::size_t n = bytes.size();
    std::uint32_t crc = 0xFFFFFFFFu;
    std::size_t i = 0;
    // 一次 8 個位元組：前 4 個先和目前的值互斥或，8 個位元組各查「後面還有幾個位元組」的那一張表
    for (; n - i >= 8; i += 8) {
        const std::uint32_t lo = load_le32(bytes.data() + i) ^ crc, hi = load_le32(bytes.data() + i + 4);
        crc = t[7][lo & 0xFFu] ^ t[6][(lo >> 8) & 0xFFu] ^ t[5][(lo >> 16) & 0xFFu] ^ t[4][lo >> 24] ^ t[3][hi & 0xFFu] ^ t[2][(hi >> 8) & 0xFFu] ^
              t[1][(hi >> 16) & 0xFFu] ^ t[0][hi >> 24];
    }
    // 剩下不到 8 個的，一次一個位元組
    for (; i < n; ++i) crc = (crc >> 8) ^ t[0][(crc ^ bytes[i]) & 0xFFu];
    return crc ^ 0xFFFFFFFFu;
}

std::uint16_t crc16(std::span<const std::uint8_t> bytes) {
    std::uint16_t crc = 0xFFFFu;
    for (std::uint8_t b : bytes) {
        crc = static_cast<std::uint16_t>(crc ^ (static_cast<std::uint16_t>(b) << 8));
        for (int i = 0; i < 8; ++i) {
            crc = (crc & 0x8000u) ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021u) : static_cast<std::uint16_t>(crc << 1);
        }
    }
    return crc;
}

std::array<std::int8_t, 128> pilot_prbs() {
    std::array<std::uint8_t, 128> w{};
    for (std::size_t k = 0; k < w.size(); ++k) {
        w[k] = k < 11 ? std::uint8_t{1} : static_cast<std::uint8_t>(w[k - 9] ^ w[k - 11]);
    }
    std::array<std::int8_t, 128> out{};
    for (std::size_t k = 0; k < w.size(); ++k) out[k] = w[k] ? std::int8_t{-1} : std::int8_t{1};
    return out;
}

}  // namespace nbtv
