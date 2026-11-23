// 核對 OFDM 參數、導頻位置與模式表。OFDM 參數與導頻的期望值抄自規格草案 v0.1。
#include <cstring>

#include "check.hpp"
#include "modes.hpp"
#include "nbtv/nbtv.h"

using namespace nbtv;

namespace {

void test_ofdm_parameters() {
    CHECK_EQ(kSymbolSamples, 320);
    CHECK_EQ(kCarriers, 49);
    CHECK_NEAR(kCarrierSpacingHz, 46.875, 1e-12);
    CHECK_NEAR(1000.0 * kFftSize / kSampleRate, 21.333, 0.001);        // 有效符號長 (ms)
    CHECK_NEAR(1000.0 * kCyclicPrefix / kSampleRate, 5.333, 0.001);    // 循環字首 (ms)
    CHECK_NEAR(1000.0 * kSymbolSamples / kSampleRate, 26.667, 0.001);  // 符號長 (ms)，每秒 37.5 個符號
    CHECK_NEAR(bin_frequency_hz(kFirstBin), 375.0, 1e-9);
    CHECK_NEAR(bin_frequency_hz(kLastBin), 2625.0, 1e-9);
    // 佔用頻寬 352–2648 Hz：兩端各多半個載波間隔
    CHECK_NEAR(bin_frequency_hz(kFirstBin) - kCarrierSpacingHz / 2, 352.0, 0.5);
    CHECK_NEAR(bin_frequency_hz(kLastBin) + kCarrierSpacingHz / 2, 2648.0, 0.5);
}

void test_pilots() {
    CHECK_EQ(pilots_in_symbol(0), 8);
    CHECK_EQ(pilots_in_symbol(1), 9);
    CHECK_EQ(pilots_in_symbol(2), 8);
    CHECK_EQ(data_cells_in_symbol(0), 41);
    CHECK_EQ(data_cells_in_symbol(1), 40);
    CHECK_EQ(data_cells_in_symbol(2), 41);
    CHECK_EQ(data_cells(3), 122);
    // data_cells() 的算式和逐個符號相加的結果一致
    int sum = 0;
    for (int n = 0; n <= 210; ++n) {
        CHECK_EQ(data_cells(n), sum);
        sum += data_cells_in_symbol(n);
    }
    for (int j = 3; j < 12; ++j) CHECK_EQ(pilots_in_symbol(j), pilots_in_symbol(j % 3));
    // 連續三個符號裡，偶數載波各當一次導頻，奇數載波從不當導頻
    for (int k = kFirstBin; k <= kLastBin; ++k) {
        const int times = (is_pilot(0, k) ? 1 : 0) + (is_pilot(1, k) ? 1 : 0) + (is_pilot(2, k) ? 1 : 0);
        CHECK_EQ(times, k % 2 == 0 ? 1 : 0);
    }
    // 導頻佔 17%
    CHECK_NEAR((8 + 9 + 8) / (3.0 * kCarriers), 0.17, 0.005);
}

// 每個模式的幀：符號數、每張的時間、標頭之外放得下的實數個數，以及模型的潛在通道數（實數個數 = 通道數 × 9 × 12）與寬度
struct ModeRow {
    int id, frame_symbols;
    double seconds;
    int data_cells, value_capacity, latent_channels;
    int width;  // 模型第一層的通道數
};
const ModeRow kModeSpec[] = {
    {11, 102, 2.720, 4067, 7782, 72, 32},
    {12, 39, 1.040, 1505, 2658, 24, 32},
    {13, 102, 2.720, 4067, 7782, 72, 64},  // 幀和模式 11 完全相同，模型的每一層加寬一倍
    {14, 25, 0.667, 935, 1518, 14, 64},    // 最快的一個：23 個酬載符號
};

void test_mode_table() {
    CHECK_EQ(static_cast<int>(kModes.size()), static_cast<int>(sizeof kModeSpec / sizeof kModeSpec[0]));
    for (const ModeRow& row : kModeSpec) {
        const Mode* m = find_mode(row.id);
        CHECK(m != nullptr);
        if (m == nullptr) continue;
        CHECK_EQ(m->frame_symbols(), row.frame_symbols);
        CHECK_EQ(m->payload_symbols, row.frame_symbols - kPreambleSymbols);
        CHECK_EQ(m->frame_samples(), row.frame_symbols * kSymbolSamples);
        CHECK_NEAR(m->frame_seconds(), row.seconds, 0.0005);
        CHECK_EQ(m->data_cells(), row.data_cells);
        CHECK_EQ(m->value_cells(), row.data_cells - 176);  // 標頭佔 176 格
        CHECK_EQ(m->value_capacity(), row.value_capacity);
        CHECK_EQ(m->learned_values, row.latent_channels * 9 * 12);
        CHECK_EQ(m->learned_width, row.width);
        CHECK(m->learned_values <= m->value_capacity());          // 放得進去
        CHECK(m->learned_values + 9 * 12 > m->value_capacity());  // 而且再多一個通道就放不下
        // 格點交錯的乘數約是資料格數的 0.31 倍：相鄰的兩個格點在幀裡隔了三成
        CHECK_NEAR(static_cast<double>(m->cell_multiplier) / m->data_cells(), 0.31, 0.005);
    }
    // 「實數個數與模型寬度」兩兩不同：發射機、接收機靠它決定哪個模型給哪個模式用
    for (const Mode& a : kModes) {
        for (const Mode& b : kModes) {
            if (&a != &b) CHECK(a.learned_values != b.learned_values || a.learned_width != b.learned_width);
        }
    }
    // 模式 11 與 13 的幀完全相同，只有模型不一樣
    const Mode& m11 = *find_mode(11);
    const Mode& m13 = *find_mode(13);
    CHECK(m11.payload_symbols == m13.payload_symbols && m11.cell_multiplier == m13.cell_multiplier &&
          m11.learned_values == m13.learned_values && m11.learned_width != m13.learned_width);
}

void test_undefined_ids() {
    // 0–5、8–10 是已經移除的數位與類比模式用過的編號
    for (int id : {-1, 0, 1, 5, 6, 7, 8, 9, 10, 15, 16}) CHECK(find_mode(id) == nullptr);
}

void test_c_api() {
    CHECK(std::strcmp(nbtv_version_string(), "0.1.0") == 0);
    CHECK_EQ(nbtv_mode_count(), 4);
    for (int i = 0; i < nbtv_mode_count(); ++i) {
        nbtv_mode_info a{};
        nbtv_mode_info b{};
        CHECK_EQ(nbtv_mode_at(i, &a), 0);
        CHECK_EQ(nbtv_mode_by_id(a.id, &b), 0);
        const Mode* m = find_mode(a.id);
        CHECK(m != nullptr);
        if (m == nullptr) continue;
        CHECK_EQ(b.id, a.id);
        CHECK_EQ(a.payload_symbols, m->payload_symbols);
        CHECK_EQ(a.frame_symbols, m->frame_symbols());
        CHECK_EQ(a.frame_samples, m->frame_samples());
        CHECK_NEAR(a.frame_seconds, m->frame_seconds(), 1e-12);
        CHECK_EQ(a.data_cells, m->data_cells());
        CHECK_EQ(a.header_cells, kHeaderCells);
        CHECK_EQ(a.value_capacity, m->value_capacity());
        CHECK_EQ(a.learned_values, m->learned_values);
        CHECK_EQ(a.learned_width, m->learned_width);
        CHECK_EQ(a.data_cells, a.header_cells + a.value_capacity / 2);
        CHECK_EQ(b.frame_samples, a.frame_samples);
        CHECK_EQ(b.learned_values, a.learned_values);
    }
    nbtv_mode_info x{};
    CHECK_EQ(nbtv_mode_at(-1, &x), -1);
    CHECK_EQ(nbtv_mode_at(nbtv_mode_count(), &x), -1);
    CHECK_EQ(nbtv_mode_at(0, nullptr), -1);
    CHECK_EQ(nbtv_mode_by_id(6, &x), -1);
    CHECK_EQ(nbtv_mode_by_id(1, &x), -1);
    CHECK_EQ(nbtv_mode_by_id(11, nullptr), -1);
}

}  // namespace

int main() {
    test_ofdm_parameters();
    test_pilots();
    test_mode_table();
    test_undefined_ids();
    test_c_api();
    return check::finish("modes");
}
