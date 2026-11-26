// 核對 OFDM 參數、導頻位置與模式表。OFDM 參數與導頻的期望值抄自規格草案 v0.1。
#include <cstring>

#include "check.hpp"
#include "frame_map.hpp"
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

// 每個模式的幀：符號數、每張的時間、標頭之外放得下的實數個數，模型的潛在通道數（實數個數 = 通道數 × 畫面縮小 8 倍的格子數）與寬度，
// 以及畫面的大小
struct ModeRow {
    int id, frame_symbols;
    double seconds;
    int data_cells, value_capacity, latent_channels;
    int width;  // 模型第一層的通道數
    int image_width, image_height;
};
const ModeRow kModeSpec[] = {
    {11, 102, 2.720, 4067, 7782, 72, 32, 96, 72},
    {12, 39, 1.040, 1505, 2658, 24, 32, 96, 72},
    {13, 102, 2.720, 4067, 7782, 72, 64, 96, 72},  // 幀和模式 11 完全相同，模型的每一層加寬一倍
    {14, 25, 0.667, 935, 1518, 14, 64, 96, 72},    // 最快的一個：23 個酬載符號
    {15, 102, 2.720, 4067, 7782, 18, 32, 192, 144},  // FM 用的大畫面：幀也和模式 11 完全相同，畫面長寬各兩倍
    // 沒有散佈導頻的幀（FM 用）：符號數含結尾的一個參考符號，酬載的每個載波都是資料格
    {5, 86, 2.293, 4067, 7782, 18, 32, 192, 144},    // 和模式 15 同一個模型；83 個酬載符號，資料格數剛好一樣
    {6, 108, 2.880, 5145, 9938, 23, 32, 192, 144},
    {7, 166, 4.427, 7987, 15622, 36, 32, 192, 144},
    // 同一種幀裡快的兩個，模型是加寬的：3 的畫面是 96×72（和模式 14 一樣是 25 個符號，省下導頻多放 2 個通道），4 是 192×144
    {3, 25, 0.667, 1078, 1804, 16, 64, 96, 72},
    {4, 38, 1.013, 1715, 3078, 7, 64, 192, 144},
};

void test_mode_table() {
    CHECK_EQ(static_cast<int>(kModes.size()), static_cast<int>(sizeof kModeSpec / sizeof kModeSpec[0]));
    for (const ModeRow& row : kModeSpec) {
        const Mode* m = find_mode(row.id);
        CHECK(m != nullptr);
        if (m == nullptr) continue;
        CHECK_EQ(m->frame_symbols(), row.frame_symbols);
        CHECK_EQ(m->payload_symbols, row.frame_symbols - kPreambleSymbols - m->tail_symbols());
        CHECK_EQ(m->tail_symbols(), m->scattered_pilots ? 0 : 1);
        CHECK_EQ(m->data_cells(), m->scattered_pilots ? data_cells(m->payload_symbols) : m->payload_symbols * kCarriers);
        CHECK_EQ(m->frame_samples(), row.frame_symbols * kSymbolSamples);
        CHECK_NEAR(m->frame_seconds(), row.seconds, 0.0005);
        CHECK_EQ(m->data_cells(), row.data_cells);
        CHECK_EQ(m->value_cells(), row.data_cells - 176);  // 標頭佔 176 格
        CHECK_EQ(m->value_capacity(), row.value_capacity);
        CHECK_EQ(m->image_width, row.image_width);
        CHECK_EQ(m->image_height, row.image_height);
        CHECK_EQ(m->image_rgb_bytes(), row.image_width * row.image_height * 3);
        CHECK(m->image_width % 8 == 0 && m->image_height % 8 == 0 && m->image_width * 3 == m->image_height * 4);  // 都是 4:3
        CHECK(m->image_width <= kMaxImageWidth && m->image_height <= kMaxImageHeight);
        const int grid = (m->image_height / 8) * (m->image_width / 8);  // 96×72 是 9×12 = 108，192×144 是 18×24 = 432
        CHECK_EQ(m->learned_values, row.latent_channels * grid);
        CHECK_EQ(m->latent_channels(), row.latent_channels);
        CHECK_EQ(m->learned_width, row.width);
        CHECK(m->learned_values <= m->value_capacity());          // 放得進去
        CHECK(m->learned_values + grid > m->value_capacity());    // 而且再多一個通道就放不下
        // 格點交錯的乘數約是資料格數的 0.31 倍：相鄰的兩個格點在幀裡隔了三成
        CHECK_NEAR(static_cast<double>(m->cell_multiplier) / m->data_cells(), 0.31, 0.005);
    }
    // 發射機、接收機靠「實數個數、模型寬度與畫面大小」決定哪個模型給哪個模式用。三者都相同的兩個模式用的是同一個模型，
    // 那只能是幀的種類不同（一個有散佈導頻、一個沒有）；現在只有模式 5 與 15 這一對。編號也兩兩不同。
    int shared = 0;
    for (const Mode& a : kModes) {
        for (const Mode& b : kModes) {
            if (&a == &b) continue;
            CHECK(a.id != b.id);
            const bool same_model = a.learned_values == b.learned_values && a.learned_width == b.learned_width &&
                                    a.image_width == b.image_width && a.image_height == b.image_height;
            if (!same_model) continue;
            CHECK(a.scattered_pilots != b.scattered_pilots);
            CHECK((a.id == 5 && b.id == 15) || (a.id == 15 && b.id == 5));
            ++shared;
        }
    }
    CHECK_EQ(shared, 2);
    // 模式 11 與 13 的幀完全相同，只有模型不一樣
    const Mode& m11 = *find_mode(11);
    const Mode& m13 = *find_mode(13);
    CHECK(m11.payload_symbols == m13.payload_symbols && m11.cell_multiplier == m13.cell_multiplier &&
          m11.learned_values == m13.learned_values && m11.learned_width != m13.learned_width);
    // 模式 15 的幀也和 11 完全相同，模型的寬度也相同，只有畫面比較大（潛在通道比較少、格子比較密）
    const Mode& m15 = *find_mode(15);
    CHECK(m11.payload_symbols == m15.payload_symbols && m11.cell_multiplier == m15.cell_multiplier &&
          m11.learned_values == m15.learned_values && m11.learned_width == m15.learned_width);
    CHECK(m15.image_width == 2 * m11.image_width && m15.image_height == 2 * m11.image_height);
    CHECK(m15.image_width == kMaxImageWidth && m15.image_height == kMaxImageHeight);
    for (int id : {11, 12, 13, 14}) CHECK(find_mode(id)->image_width == kImageWidth && find_mode(id)->image_height == kImageHeight);
    // 有散佈導頻的是 11–15；3–7 沒有。其中 4–7 是 192×144 的大畫面，3 的畫面是 96×72
    for (int id : {11, 12, 13, 14, 15}) CHECK(find_mode(id)->scattered_pilots);
    for (int id : {4, 5, 6, 7}) {
        const Mode& m = *find_mode(id);
        CHECK(!m.scattered_pilots && m.image_width == kMaxImageWidth && m.image_height == kMaxImageHeight);
    }
    // 模式 3 和模式 14 一樣長（都是 25 個符號），畫面也一樣大、模型一樣寬；省下導頻之後資料格多 15%，多放 2 個通道
    const Mode& m3 = *find_mode(3);
    const Mode& m14 = *find_mode(14);
    CHECK(!m3.scattered_pilots && m3.image_width == kImageWidth && m3.image_height == kImageHeight);
    CHECK(m3.frame_symbols() == m14.frame_symbols() && m3.learned_width == m14.learned_width);
    CHECK_EQ(m3.latent_channels(), m14.latent_channels() + 2);
    CHECK_NEAR(static_cast<double>(m3.data_cells()) / m14.data_cells(), 1.153, 0.001);
    // 模式 4 的模型和 3 一樣寬（都比 5–7 的寬一倍），每張 1 秒出頭
    const Mode& m4 = *find_mode(4);
    CHECK(m4.learned_width == m3.learned_width && m4.learned_width == 2 * find_mode(5)->learned_width);
    CHECK(m4.frame_seconds() > 1.0 && m4.frame_seconds() < 1.02);
    // 模式 5 用的模型和 15 相同、資料格一樣多，少了導頻所以符號比較少：每張快 16%
    const Mode& m5 = *find_mode(5);
    CHECK(m5.data_cells() == m15.data_cells() && m5.cell_multiplier == m15.cell_multiplier && m5.payload_symbols < m15.payload_symbols);
    CHECK_NEAR(m5.frame_seconds() / m15.frame_seconds(), 0.843, 0.001);
    // 同樣的符號數，沒有導頻的幀多 2 成的資料格
    CHECK_NEAR(100.0 * kCarriers / data_cells(100), 1.205, 0.001);
}

void test_undefined_ids() {
    // 0–2、8–10 是已經移除的數位與類比模式用過的編號，現在沒有定義（3–7 後來給了沒有散佈導頻的幀）
    for (int id : {-1, 0, 1, 2, 8, 9, 10, 16}) CHECK(find_mode(id) == nullptr);
    // 沒有散佈導頻的幀結尾的參考符號用的是 S1 的第 0 條序列：它不能是任何模式的編號
    CHECK(find_mode(kTailReferenceId) == nullptr);
}

void test_c_api() {
    CHECK(std::strcmp(nbtv_version_string(), "0.1.0") == 0);
    CHECK_EQ(nbtv_mode_count(), 10);
    CHECK_EQ(NBTV_IMAGE_MAX_RGB_BYTES, kMaxImageRgbBytes);
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
        CHECK_EQ(a.image_width, m->image_width);
        CHECK_EQ(a.image_height, m->image_height);
        CHECK_EQ(a.scattered_pilots, m->scattered_pilots ? 1 : 0);
        CHECK(a.image_width * a.image_height * 3 <= NBTV_IMAGE_MAX_RGB_BYTES);
        CHECK_EQ(a.data_cells, a.header_cells + a.value_capacity / 2);
        CHECK_EQ(b.frame_samples, a.frame_samples);
        CHECK_EQ(b.learned_values, a.learned_values);
    }
    nbtv_mode_info x{};
    CHECK_EQ(nbtv_mode_at(-1, &x), -1);
    CHECK_EQ(nbtv_mode_at(nbtv_mode_count(), &x), -1);
    CHECK_EQ(nbtv_mode_at(0, nullptr), -1);
    CHECK_EQ(nbtv_mode_by_id(9, &x), -1);
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
