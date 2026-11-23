// 核對 OFDM 參數、導頻位置與模式表：期望值都抄自規格草案 v0.1。
#include <cstring>

#include "check.hpp"
#include "modes.hpp"
#include "nbtv/nbtv.h"

using namespace nbtv;

namespace {

// 規格草案「數位模式：模式表」。
struct DigitalRow {
    int id, bits_per_cell, rate_num, rate_den;
    int payload_symbols, frame_symbols;
    double seconds;
    int ldpc_info_bits, payload_bytes, image_bytes, padding_cells;
    double net_kbps;
};
const DigitalRow kDigitalSpec[] = {
    {0, 2, 1, 4, 200, 202, 5.387, 3240, 401, 393, 33, 0.60},    // QPSK 1/4
    {1, 2, 1, 2, 200, 202, 5.387, 7200, 896, 888, 33, 1.33},    // QPSK 1/2
    {2, 2, 3, 4, 200, 202, 5.387, 11880, 1481, 1473, 33, 2.20}, // QPSK 3/4
    {3, 4, 1, 2, 100, 102, 2.720, 7200, 896, 888, 17, 2.64},    // 16QAM 1/2
    {4, 4, 3, 4, 100, 102, 2.720, 11880, 1481, 1473, 17, 4.36}, // 16QAM 3/4
    {5, 6, 2, 3, 67, 69, 1.840, 10800, 1346, 1338, 25, 5.85},   // 64QAM 2/3
};

// 規格草案「類比模式：相干類比」。
struct AnalogRow {
    int id, payload_symbols, frame_symbols;
    double seconds;
    int coefficient_cells;  // 資料格數扣掉標頭的 176 格
};
const AnalogRow kAnalogSpec[] = {
    {8, 37, 39, 1.040, 1329},
    {9, 100, 102, 2.720, 3891},
    {10, 200, 202, 5.387, 7957},
};

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

void test_digital_modes() {
    for (const DigitalRow& row : kDigitalSpec) {
        const Mode* m = find_mode(row.id);
        CHECK(m != nullptr);
        if (m == nullptr) continue;
        CHECK(m->kind == ModeKind::Digital);
        CHECK_EQ(m->bits_per_cell, row.bits_per_cell);
        CHECK_EQ(m->rate_num, row.rate_num);
        CHECK_EQ(m->rate_den, row.rate_den);
        CHECK_EQ(m->payload_symbols, row.payload_symbols);
        CHECK_EQ(m->frame_symbols(), row.frame_symbols);
        CHECK_NEAR(m->frame_seconds(), row.seconds, 0.0005);
        CHECK_EQ(m->ldpc_info_bits, row.ldpc_info_bits);
        CHECK_EQ(m->payload_bytes(), row.payload_bytes);
        CHECK_EQ(m->image_bytes(), row.image_bytes);
        CHECK_EQ(m->padding_cells(), row.padding_cells);
        CHECK_EQ(m->coded_cells() * m->bits_per_cell, kLdpcCodewordBits);
        CHECK_EQ(m->coded_cells() + m->padding_cells(), m->data_cells());
        // 淨位元率 = CRC 保護的位元數 ÷ 每幀時間
        CHECK_NEAR(m->payload_bytes() * 8 / m->frame_seconds() / 1000.0, row.net_kbps, 0.005);
        CHECK_EQ(m->header_cells(), 0);
        CHECK_EQ(m->analog_values(), 0);
    }
}

void test_analog_modes() {
    for (const AnalogRow& row : kAnalogSpec) {
        const Mode* m = find_mode(row.id);
        CHECK(m != nullptr);
        if (m == nullptr) continue;
        CHECK(m->kind == ModeKind::Analog);
        CHECK_EQ(m->payload_symbols, row.payload_symbols);
        CHECK_EQ(m->frame_symbols(), row.frame_symbols);
        CHECK_NEAR(m->frame_seconds(), row.seconds, 0.0005);
        CHECK_EQ(m->header_cells(), 176);
        CHECK_EQ(m->analog_cells(), row.coefficient_cells);
        CHECK_EQ(m->analog_values(), 2 * row.coefficient_cells);
        CHECK_EQ(m->payload_bytes(), 0);
        CHECK_EQ(m->image_bytes(), 0);
        CHECK_EQ(m->padding_cells(), 0);
    }
}

// 學習式模式（實驗中）：幀的安排和某個類比模式一樣，只是放的是類神經網路產生的實數
struct LearnedRow {
    int id, analog_id, frame_symbols;
    double seconds;
    int analog_values, latent_channels;  // 標頭之外放得下的實數個數；模型的潛在通道數（實數個數 = 通道數 × 9 × 12）
    int width;                           // 模型第一層的通道數
};
const LearnedRow kLearnedSpec[] = {
    {11, 9, 102, 2.720, 7782, 72, 32},
    {12, 8, 39, 1.040, 2658, 24, 32},
    {13, 9, 102, 2.720, 7782, 72, 64},  // 幀和模式 11 完全相同，模型的每一層加寬一倍
    {14, 0, 25, 0.667, 1518, 14, 64},   // 最快的一個：沒有對應的類比模式（analog_id 寫 0），23 個酬載符號
};

void test_learned_modes() {
    int learned_count = 0;
    for (const LearnedRow& row : kLearnedSpec) {
        const Mode* m = find_mode(row.id);
        const Mode* analog = row.analog_id > 0 ? find_mode(row.analog_id) : nullptr;
        CHECK(m != nullptr);
        if (m == nullptr) continue;
        ++learned_count;
        CHECK(m->kind == ModeKind::Learned);
        CHECK(m->is_learned() && !m->is_digital() && !m->is_analog());
        if (analog != nullptr) {  // 幀比照某個類比模式：符號數、交錯乘數、資料格數都相同
            CHECK(analog->is_analog() && !analog->is_learned());
            CHECK_EQ(m->payload_symbols, analog->payload_symbols);
            CHECK_EQ(m->cell_multiplier, analog->cell_multiplier);
            CHECK_EQ(m->data_cells(), analog->data_cells());
        } else {  // 自己的幀長：沒有哪個類比模式一樣長
            for (const Mode& other : kModes) CHECK(!other.is_analog() || other.payload_symbols != m->payload_symbols);
        }
        CHECK_EQ(m->frame_symbols(), row.frame_symbols);
        CHECK_EQ(m->payload_symbols, row.frame_symbols - kPreambleSymbols);
        CHECK_NEAR(m->frame_seconds(), row.seconds, 0.0005);
        CHECK_EQ(m->header_cells(), 176);
        CHECK_EQ(m->analog_values(), row.analog_values);
        CHECK_EQ(m->learned_values, row.latent_channels * 9 * 12);
        CHECK_EQ(m->learned_width, row.width);
        CHECK(m->learned_values <= m->analog_values());             // 放得進去
        CHECK(m->learned_values + 9 * 12 > m->analog_values());     // 而且再多一個通道就放不下
        CHECK_EQ(m->interleaver_cells(), m->data_cells());
        CHECK_EQ(m->payload_bytes(), 0);
        CHECK_EQ(m->image_bytes(), 0);
        CHECK_EQ(m->padding_cells(), 0);
    }
    // 其他模式都沒有這兩個欄位；學習式模式的「實數個數與模型寬度」兩兩不同（發射機、接收機靠它決定哪個模型給哪個模式用）
    for (const Mode& other : kModes) {
        CHECK_EQ(other.learned_values > 0, other.is_learned());
        CHECK_EQ(other.learned_width > 0, other.is_learned());
        learned_count -= other.is_learned() ? 1 : 0;
        for (const Mode& another : kModes) {
            if (&other != &another && other.is_learned() && another.is_learned())
                CHECK(other.learned_values != another.learned_values || other.learned_width != another.learned_width);
        }
    }
    CHECK_EQ(learned_count, 0);  // 上面的表涵蓋了所有的學習式模式
}

void test_undefined_ids() {
    for (int id : {-1, 6, 7, 15, 16}) CHECK(find_mode(id) == nullptr);
}

void test_c_api() {
    CHECK(std::strcmp(nbtv_version_string(), "0.1.0") == 0);
    CHECK_EQ(nbtv_mode_count(), 13);
    for (int i = 0; i < nbtv_mode_count(); ++i) {
        nbtv_mode_info a{};
        nbtv_mode_info b{};
        CHECK_EQ(nbtv_mode_at(i, &a), 0);
        CHECK_EQ(nbtv_mode_by_id(a.id, &b), 0);
        const Mode* m = find_mode(a.id);
        CHECK(m != nullptr);
        if (m == nullptr) continue;
        CHECK_EQ(b.id, a.id);
        CHECK(a.kind == (m->is_digital() ? NBTV_MODE_DIGITAL : m->is_learned() ? NBTV_MODE_LEARNED : NBTV_MODE_ANALOG));
        CHECK_EQ(a.payload_symbols, m->payload_symbols);
        CHECK_EQ(a.frame_symbols, m->frame_symbols());
        CHECK_EQ(a.frame_samples, m->frame_samples());
        CHECK_NEAR(a.frame_seconds, m->frame_seconds(), 1e-12);
        CHECK_EQ(a.data_cells, m->data_cells());
        CHECK_EQ(a.bits_per_cell, m->bits_per_cell);
        CHECK_EQ(a.code_rate_num, m->rate_num);
        CHECK_EQ(a.code_rate_den, m->rate_den);
        CHECK_EQ(a.ldpc_info_bits, m->ldpc_info_bits);
        CHECK_EQ(a.payload_bytes, m->payload_bytes());
        CHECK_EQ(a.image_bytes, m->image_bytes());
        CHECK_EQ(a.padding_cells, m->padding_cells());
        CHECK_EQ(a.header_cells, m->header_cells());
        CHECK_EQ(a.analog_values, m->analog_values());
        CHECK((a.analog_coefficients > 0) == m->is_analog());
        CHECK_EQ(a.learned_values, m->learned_values);
        CHECK_EQ(a.learned_width, m->learned_width);
        CHECK(a.analog_coefficients <= a.analog_values);
        CHECK_EQ(b.frame_samples, a.frame_samples);
        CHECK_EQ(b.image_bytes, a.image_bytes);
    }
    nbtv_mode_info x{};
    CHECK_EQ(nbtv_mode_at(-1, &x), -1);
    CHECK_EQ(nbtv_mode_at(nbtv_mode_count(), &x), -1);
    CHECK_EQ(nbtv_mode_at(0, nullptr), -1);
    CHECK_EQ(nbtv_mode_by_id(6, &x), -1);
    CHECK_EQ(nbtv_mode_by_id(1, nullptr), -1);
}

}  // namespace

int main() {
    test_ofdm_parameters();
    test_pilots();
    test_digital_modes();
    test_analog_modes();
    test_learned_modes();
    test_undefined_ids();
    test_c_api();
    return check::finish("modes");
}
