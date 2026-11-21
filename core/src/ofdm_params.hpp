// OFDM 參數：數位模式與相干類比模式共用。
// 對應規格草案的「OFDM 參數」與「幀結構與導頻」兩節。
#pragma once

namespace nbtv {

inline constexpr int kSampleRate = 12000;  // 數據機取樣率 (Hz)；音訊裝置端 48 kHz，以 4:1 轉換
inline constexpr int kFftSize = 256;       // 載波間隔 46.875 Hz，有效符號長 21.33 ms
inline constexpr int kCyclicPrefix = 64;   // 循環字首 5.33 ms
inline constexpr int kSymbolSamples = kFftSize + kCyclicPrefix;  // 每個符號 320 個取樣，26.67 ms

inline constexpr int kFirstBin = 8;   // 最低的有效載波，375 Hz
inline constexpr int kLastBin = 56;   // 最高的有效載波，2625 Hz
inline constexpr int kCarriers = kLastBin - kFirstBin + 1;  // 49

inline constexpr int kPreambleSymbols = 2;  // 每幀開頭的 S0 同步符號與 S1 參考符號

inline constexpr double kCarrierSpacingHz = static_cast<double>(kSampleRate) / kFftSize;

constexpr double bin_frequency_hz(int bin) { return bin * kCarrierSpacingHz; }

// 散佈導頻：酬載符號 j（從 0 起算）的相位 p = j mod 3，載波 k 滿足 k mod 6 = 2p 時是導頻。
constexpr bool is_pilot(int payload_symbol, int bin) {
    return bin % 6 == 2 * (payload_symbol % 3);
}

constexpr int pilots_in_symbol(int payload_symbol) {
    int n = 0;
    for (int k = kFirstBin; k <= kLastBin; ++k) n += is_pilot(payload_symbol, k) ? 1 : 0;
    return n;
}

constexpr int data_cells_in_symbol(int payload_symbol) {
    return kCarriers - pilots_in_symbol(payload_symbol);
}

// 前 payload_symbols 個酬載符號裡，扣掉導頻後的資料格數。導頻位置每三個符號重複一次。
constexpr int data_cells(int payload_symbols) {
    const int per_cycle = data_cells_in_symbol(0) + data_cells_in_symbol(1) + data_cells_in_symbol(2);
    int n = (payload_symbols / 3) * per_cycle;
    for (int j = 0; j < payload_symbols % 3; ++j) n += data_cells_in_symbol(j);
    return n;
}

}  // namespace nbtv
