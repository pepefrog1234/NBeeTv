#include "frame_map.hpp"

#include <array>
#include <cmath>

#include "bits.hpp"

namespace nbtv {

namespace {

// S1 的設計（tools/design/design_preamble.cpp 的搜尋結果）：
// X_m[c] = kS1Mask[c] · (−1)^popcount(m & kS1Class[c])，c = 0..48 對應第 8..56 格。
// 49 個載波分成 16 類（第 0 類 4 個、其餘各 3 個），所以任兩條序列的內積恰為 ±1；
// 16 條序列的包絡峰均比最大 6.7 dB。
constexpr std::int8_t kS1Class[kCarriers] = {5,  0, 1,  0,  15, 4, 5,  14, 9, 7,  13, 13, 7,  2,  1, 14, 1,
                                             4,  11, 13, 10, 4,  5, 8,  7,  12, 0,  12, 8,  2,  10, 9, 10, 2,
                                             14, 11, 3,  9,  6,  0, 3,  12, 15, 8,  3,  6,  11, 15, 6};
constexpr std::int8_t kS1Mask[kCarriers] = {1,  1, 1,  -1, -1, 1,  1,  -1, 1,  -1, 1, -1, 1, -1, 1, -1, -1,
                                            1,  1, 1,  1,  -1, -1, 1,  -1, 1,  1,  1, 1,  -1, -1, -1, -1, 1,
                                            1,  1, -1, 1,  1,  1,  1,  1,  -1, 1,  1, 1,  -1, 1,  1};

constexpr bool in_band(int bin) { return bin >= kFirstBin && bin <= kLastBin; }

int parity4(int v) { return ((v >> 0) ^ (v >> 1) ^ (v >> 2) ^ (v >> 3)) & 1; }

}  // namespace

float pilot_value(int bin) {
    static const std::array<std::int8_t, 128> prbs = pilot_prbs();
    return kPilotAmplitude * static_cast<float>(prbs[static_cast<std::size_t>(bin)]);
}

cf s0_value(int bin) {
    if (!in_band(bin) || (bin - kFirstBin) % 2 != 0) return cf(0, 0);
    // 二次相位（Zadoff–Chu 型，根值 1、長度 25）：exp(−jπ·i·(i+1)/25)
    static const std::array<cf, kS0Carriers> table = [] {
        std::array<cf, kS0Carriers> t{};
        for (int i = 0; i < kS0Carriers; ++i) {
            const double phase = -kPi * i * (i + 1) / kS0Carriers;
            t[static_cast<std::size_t>(i)] =
                cf(static_cast<float>(kS0Amplitude * std::cos(phase)), static_cast<float>(kS0Amplitude * std::sin(phase)));
        }
        return t;
    }();
    return table[static_cast<std::size_t>((bin - kFirstBin) / 2)];
}

float s1_value(int mode_id, int bin) {
    if (!in_band(bin)) return 0.0f;
    const int c = bin - kFirstBin;
    const int sign = parity4(mode_id & kS1Class[c]) ? -kS1Mask[c] : kS1Mask[c];
    return kS1Amplitude * static_cast<float>(sign);
}

std::vector<CellPos> data_cell_positions(int payload_symbols) {
    std::vector<CellPos> cells;
    cells.reserve(static_cast<std::size_t>(data_cells(payload_symbols)));
    for (int j = 0; j < payload_symbols; ++j) {
        for (int k = kFirstBin; k <= kLastBin; ++k) {
            if (!is_pilot(j, k)) cells.push_back({static_cast<std::uint16_t>(j), static_cast<std::uint8_t>(k)});
        }
    }
    return cells;
}

std::vector<CellPos> data_cell_positions(const Mode& mode) {
    if (mode.scattered_pilots) return data_cell_positions(mode.payload_symbols);
    std::vector<CellPos> cells;
    cells.reserve(static_cast<std::size_t>(mode.data_cells()));
    for (int j = 0; j < mode.payload_symbols; ++j) {
        for (int k = kFirstBin; k <= kLastBin; ++k) cells.push_back({static_cast<std::uint16_t>(j), static_cast<std::uint8_t>(k)});
    }
    return cells;
}

}  // namespace nbtv
