// 模式表與幀結構算術。
// 對應規格草案的「數位模式：模式表」與「類比模式：相干類比」兩節。
#pragma once

#include <array>
#include <numeric>

#include "ofdm_params.hpp"

namespace nbtv {

enum class ModeKind { Digital, Analog };

inline constexpr int kLdpcCodewordBits = 16200;  // DVB-S2/T2 短碼，每幀一個碼字
inline constexpr int kCrcBits = 32;              // 以 CRC-32 取代 BCH
inline constexpr int kDigitalHeaderBytes = 8;    // 版本與格式 1、幀序號 1、呼號 6
inline constexpr int kAnalogHeaderCells = 176;   // 標頭 11 位元組，摺積碼 r=1/2 後重複兩次，QPSK

struct Mode {
    int id;                  // 空中的模式編號，由 S1 參考符號採用的序列表示
    ModeKind kind;
    int payload_symbols;     // 每幀的酬載符號數
    int cell_multiplier;     // 格點交錯的乘數 a：第 i 個格點放到位置 a·i mod N

    // 以下只有數位模式使用，類比為 0
    int bits_per_cell;       // QPSK 2、16QAM 4、64QAM 6
    int rate_num, rate_den;  // LDPC 的名目碼率
    int ldpc_info_bits;      // LDPC 資訊位元數 K

    constexpr bool is_digital() const { return kind == ModeKind::Digital; }

    constexpr int frame_symbols() const { return payload_symbols + kPreambleSymbols; }
    constexpr int frame_samples() const { return frame_symbols() * kSymbolSamples; }
    constexpr double frame_seconds() const {
        return static_cast<double>(frame_samples()) / kSampleRate;
    }
    constexpr int data_cells() const { return nbtv::data_cells(payload_symbols); }

    // 數位：一個碼字佔用的格數、補滿幀用的格數、CRC 保護的位元組數、一張影像可用的位元組數
    constexpr int coded_cells() const { return is_digital() ? kLdpcCodewordBits / bits_per_cell : 0; }
    constexpr int padding_cells() const { return is_digital() ? data_cells() - coded_cells() : 0; }
    constexpr int payload_bytes() const { return is_digital() ? (ldpc_info_bits - kCrcBits) / 8 : 0; }
    constexpr int image_bytes() const { return is_digital() ? payload_bytes() - kDigitalHeaderBytes : 0; }

    // 類比：標頭之外的格數；每格放兩個實數
    constexpr int header_cells() const { return is_digital() ? 0 : kAnalogHeaderCells; }
    constexpr int analog_cells() const { return is_digital() ? 0 : data_cells() - kAnalogHeaderCells; }
    constexpr int analog_values() const { return 2 * analog_cells(); }

    // 格點交錯涵蓋的格數 N：數位是碼字佔用的格數（補足格排在最後，不交錯），類比是全部資料格。
    constexpr int interleaver_cells() const { return is_digital() ? coded_cells() : data_cells(); }
    // 交錯前的第 i 個格點在酬載資料格裡的位置。
    constexpr int interleaved_position(int i) const {
        return static_cast<int>(static_cast<long long>(cell_multiplier) * i % interleaver_cells());
    }
};

inline constexpr std::array<Mode, 9> kModes{{
    // id  種類                酬載符號  乘數  位元/格  碼率    K
    {0, ModeKind::Digital, 200, 2503, 2, 1, 4, 3240},
    {1, ModeKind::Digital, 200, 2503, 2, 1, 2, 7200},
    {2, ModeKind::Digital, 200, 2503, 2, 3, 4, 11880},
    {3, ModeKind::Digital, 100, 1249, 4, 1, 2, 7200},
    {4, ModeKind::Digital, 100, 1249, 4, 3, 4, 11880},
    {5, ModeKind::Digital, 67, 833, 6, 2, 3, 10800},
    // 類比的乘數是規格草案的待定值；這裡比照數位（約 0.31·N）取與 N 互質的數。
    {8, ModeKind::Analog, 37, 464, 0, 0, 0, 0},
    {9, ModeKind::Analog, 100, 1255, 0, 0, 0, 0},
    {10, ModeKind::Analog, 200, 2509, 0, 0, 0, 0},
}};

constexpr const Mode* find_mode(int id) {
    for (const Mode& m : kModes) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

// 編譯期檢查：碼字放得進酬載、位元組數是整數、類比模式扣掉標頭後還有空間、交錯乘數與格數互質。
constexpr bool mode_table_is_consistent() {
    for (const Mode& m : kModes) {
        if (std::gcd(m.cell_multiplier, m.interleaver_cells()) != 1) return false;
        if (m.is_digital()) {
            if (kLdpcCodewordBits % m.bits_per_cell != 0) return false;
            if (m.padding_cells() < 0) return false;
            if ((m.ldpc_info_bits - kCrcBits) % 8 != 0) return false;
            if (m.image_bytes() <= 0) return false;
        } else if (m.analog_cells() <= 0) {
            return false;
        }
    }
    return true;
}
static_assert(mode_table_is_consistent());

}  // namespace nbtv
