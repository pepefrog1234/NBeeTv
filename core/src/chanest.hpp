// 通道估測：由 S0、S1 與散佈導頻估出每個格點的通道響應與雜訊變異數。
// 做法：已知格點的最小平方估計 → 沿時間做 Wiener 內插（每個偶數格每 3 個符號有一個導頻）
// → 沿頻率做 Wiener 內插到每一格（等同於在延遲域只留下一段視窗）。
#pragma once

#include <vector>

#include "modes.hpp"
#include "types.hpp"

namespace nbtv {

// 一幀解調後的頻域格點：第 0 列是 S0、第 1 列是 S1，其後是酬載符號；每列 49 個載波（第 8..56 格）。
// 解調時已經把 FFT 視窗提前量造成的相位斜率拿掉，所以第一條路徑落在延遲 0 附近。
struct SymbolGrid {
    int rows = 0;
    std::vector<cf> cells;

    explicit SymbolGrid(int row_count = 0) : rows(row_count), cells(static_cast<std::size_t>(row_count) * kCarriers) {}
    cf& at(int row, int carrier) { return cells[static_cast<std::size_t>(row) * kCarriers + static_cast<std::size_t>(carrier)]; }
    const cf& at(int row, int carrier) const {
        return cells[static_cast<std::size_t>(row) * kCarriers + static_cast<std::size_t>(carrier)];
    }
};

struct ChannelEstimate {
    std::vector<cf> h;             // 每個格點的通道響應，排列同 SymbolGrid
    std::vector<float> est_gain;   // 每個格點通道估計的雜訊增益（估計誤差的變異數 ÷ 雜訊變異數）
    float noise_var = 0.0f;        // 每個格點的複數雜訊變異數（FFT 輸出的刻度）；取各符號估計值的中位數，不受短暫的突波影響
    // 雜訊不一定是平的：靜電突波只打到幾個符號，窄頻干擾只落在幾個載波。
    // 所以另外估出每一列（符號）的雜訊變異數，以及每個載波相對於一般載波的倍數；某個格點的雜訊是兩者的乘積。
    std::vector<float> row_noise;      // 每一列的雜訊變異數
    std::vector<float> carrier_noise;  // 每個載波的倍數，一般是 1
    float noise_at(int row, int carrier) const {
        return row_noise[static_cast<std::size_t>(row)] * carrier_noise[static_cast<std::size_t>(carrier)];
    }
    float mean_power = 0.0f;       // 酬載格點 |H|² 的平均（已扣掉估計雜訊的偏差）
    float snr_db = 0.0f;           // 換算成 3 kHz 雜訊頻寬、訊號含導頻的訊雜比

    const cf& at(int row, int carrier) const {
        return h[static_cast<std::size_t>(row) * kCarriers + static_cast<std::size_t>(carrier)];
    }
    float gain_at(int row, int carrier) const {
        return est_gain[static_cast<std::size_t>(row) * kCarriers + static_cast<std::size_t>(carrier)];
    }
};

// 設計參數（草案沒有規定，接收端可以自行取捨）。
inline constexpr double kChanestDopplerHz = 1.5;   // 時間內插假設的最大都卜勒（含殘餘頻偏）
inline constexpr int kChanestDelayBefore = 16;     // 延遲視窗：第一條路徑之前（取樣數）
inline constexpr int kChanestDelayAfter = 48;      // 第一條路徑之後，4 ms

class ChannelEstimator {
public:
    ChannelEstimator();

    // 殘餘頻偏：同一格相鄰兩個導頻（相隔 3 個符號）的平均相位差，換算成每個符號的弧度。
    // 可分辨的範圍是 ±6.25 Hz。
    double residual_phase_per_symbol(const Mode& mode, const SymbolGrid& grid) const;

    // 第一條有份量的路徑相對於目前時間基準的延遲（取樣數，可為負）。
    // 由整幀導頻平均出的延遲功率分布求得，比單看 S1 可靠。
    double first_path_delay(const Mode& mode, const SymbolGrid& grid) const;

    ChannelEstimate estimate(const Mode& mode, const SymbolGrid& grid) const;

private:
    struct TimeWeights;
    // 已知格點的最小平方估計，只取偶數格：ls[row·25 + i]，沒有已知值的位置不使用。
    std::vector<cf> known_cell_estimates(const Mode& mode, const SymbolGrid& grid) const;
    TimeWeights time_weights(const Mode& mode) const;
    std::vector<cf> interpolate_time(const Mode& mode, const std::vector<cf>& ls, const TimeWeights& tw) const;

    std::vector<cd> freq_weights_;       // 49×25：由偶數格內插到每一格
    std::vector<double> freq_gain_;      // 49×3：各導頻相位類別的 Σ|W|²
};

}  // namespace nbtv
