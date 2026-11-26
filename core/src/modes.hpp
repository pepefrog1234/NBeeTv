// 模式表與幀結構算術。
// OFDM 參數、前導符號與導頻沿用規格草案；模式本身都是學習式的（畫面由類神經網路變成通道上的實數，見 learned_frame.hpp）。
// 草案裡的數位模式（編號 0–5）與相干類比模式（8–10）已經移除，那些編號不再使用。
#pragma once

#include <array>
#include <numeric>

#include "ofdm_params.hpp"
#include "types.hpp"

namespace nbtv {

inline constexpr int kHeaderCells = 176;  // 標頭 11 位元組，迴旋碼 r=1/2 後重複兩次，QPSK（見 frame_header.hpp）

struct Mode {
    int id;                  // 空中的模式編號，由 S1 參考符號採用的序列表示
    int payload_symbols;     // 每幀的酬載符號數
    int cell_multiplier;     // 格點交錯的乘數 a：第 i 個格點放到位置 a·i mod N（N 是資料格數）
    int learned_values;      // 一張畫面變成幾個實數；模型的輸出要一樣多
    int learned_width;       // 模型第一層的通道數：實數個數相同、大小不同的模型各用一個模式
    int image_width;         // 畫面的寬與高（像素）；模型是照這個大小訓練的（權重檔裡記著，見 learned_codec.hpp）
    int image_height;
    // 酬載裡有沒有散佈導頻。沒有的是給 FM 語音通道用的幀：FM 的音訊通道在一幀之內不變，導頻（17% 的格點、29% 的功率）
    // 可以省。酬載的每個載波都放資料，酬載之後多一個已知的符號（結尾的參考符號，見 frame_map.hpp）；接收端靠頭尾兩個參考符號
    // 估通道與取樣時脈的漂移，每個符號的雜訊改聽頻帶上方沒用到的頻率（receiver.cpp 的 process_frame_free）。
    // 這種幀不能有頻偏（FM 解調出來的音訊沒有），也禁不起一幀之內就變的通道，所以不適合短波。
    bool scattered_pilots = true;

    constexpr int tail_symbols() const { return scattered_pilots ? 0 : 1; }  // 結尾的參考符號
    constexpr int frame_symbols() const { return kPreambleSymbols + payload_symbols + tail_symbols(); }
    constexpr int frame_samples() const { return frame_symbols() * kSymbolSamples; }
    constexpr double frame_seconds() const {
        return static_cast<double>(frame_samples()) / kSampleRate;
    }
    constexpr int data_cells() const { return scattered_pilots ? nbtv::data_cells(payload_symbols) : payload_symbols * kCarriers; }

    // 標頭之外的格數；每格放兩個實數
    constexpr int value_cells() const { return data_cells() - kHeaderCells; }
    constexpr int value_capacity() const { return 2 * value_cells(); }

    // 一張畫面的位元組數（8 位元 RGB）
    constexpr int image_rgb_bytes() const { return image_width * image_height * 3; }
    // 模型的潛在通道數：潛在張量的格子是畫面縮小 8 倍，實數個數 = 通道數 × 格子的列數 × 行數
    constexpr int latent_channels() const { return learned_values / ((image_height / 8) * (image_width / 8)); }

    // 交錯前的第 i 個格點（標頭在前、畫面的值在後）在酬載資料格裡的位置。
    constexpr int interleaved_position(int i) const {
        return static_cast<int>(static_cast<long long>(cell_multiplier) * i % data_cells());
    }
};

// 每個模式各有自己的模型，實數的個數是「潛在通道數 × 畫面縮小 8 倍的格子數」。11–14 的畫面是 96×72（格子 9×12）：
// 72、24 與 14 個通道。
// 13 的幀和 11 完全相同，只是模型的每一層加寬一倍（運算量約四倍）。
// 14 是最快的：23 個酬載符號、每張 0.667 秒；14 個通道，用的也是加寬的模型。
// 15 是給 VHF／UHF 的 FM 語音通道用的大畫面：幀也和 11 完全相同（同樣 7776 個實數、每張 2.72 秒），
// 畫面是 192×144（格子 18×24、18 個通道）。它的模型只照 10 dB 以上的訊雜比訓練（FM 的音訊訊雜比高、不像短波那樣衰落），
// 訊號弱時比 11 差，不適合短波。來歷與量到的結果見 tools/learned/README.md。
// 5、6、7 也是給 FM 用的大畫面（192×144），但酬載裡沒有導頻（見 Mode::scattered_pilots），同樣的時間多 2 成的資料格：
//   5 用的模型和 15 相同（18 個通道、7776 個實數）：83 個酬載符號，資料格數剛好和 15 一樣（4067），每張 2.29 秒；
//   6 是 23 個通道（9936 個實數）、每張 2.88 秒；7 是 36 個通道（15552 個實數）、每張 4.43 秒。
// 3、4 是同一種幀（給 FM 用、沒有導頻）裡快的兩個，用的都是加寬的模型：
//   3 的畫面是 96×72：22 個酬載符號、每張 0.667 秒，和 14 一樣快；省下導頻之後放得下 16 個通道（1728 個實數，14 是 14 個通道）；
//   4 的畫面是 192×144：35 個酬載符號、每張 1.013 秒；7 個通道（3024 個實數）。加寬的模型配上大畫面，運算量約是其他大畫面模式的四倍
//   （M4 Max：分給八條執行緒時解一張約 0.13 秒，只用一條是 0.67 秒）。核心少、比較慢的電腦可能跟不上，
//   所以外殼把這個模式發射端的預覽另外解（見 nbtv_model_decode），不接在編碼後面。
// 編號只能是 0..15（S1 只有 16 條序列），11–15 用掉了，所以用前面空著的；這幾個編號草案裡原來的模式早就移除了。
// 乘數取約 0.31·N、與 N 互質的數（模式 14 的 N = 935 = 5·11·17；模式 6 的 N = 5145 = 3·5·7³，模式 7 的 N = 7987 = 7²·163；
// 模式 3 的 N = 1078 = 2·7²·11，模式 4 的 N = 1715 = 5·7³）。
inline constexpr std::array<Mode, 10> kModes{{
    // id  酬載符號  乘數  實數個數  模型寬度  畫面寬  畫面高  散佈導頻
    {11, 100, 1255, 7776, 32, 96, 72},
    {12, 37, 464, 2592, 32, 96, 72},
    {13, 100, 1255, 7776, 64, 96, 72},
    {14, 23, 291, 1512, 64, 96, 72},
    {15, 100, 1255, 7776, 32, 192, 144},
    {5, 83, 1255, 7776, 32, 192, 144, false},
    {6, 105, 1594, 9936, 32, 192, 144, false},
    {7, 163, 2476, 15552, 32, 192, 144, false},
    {3, 22, 335, 1728, 64, 96, 72, false},
    {4, 35, 531, 3024, 64, 192, 144, false},
}};

constexpr const Mode* find_mode(int id) {
    for (const Mode& m : kModes) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

// 編譯期檢查：扣掉標頭後還有空間、畫面的實數放得下、交錯乘數與格數互質、畫面大小合理。
constexpr bool mode_table_is_consistent() {
    for (const Mode& m : kModes) {
        if (std::gcd(m.cell_multiplier, m.data_cells()) != 1) return false;
        if (m.value_cells() <= 0) return false;
        if (m.learned_values <= 0 || m.learned_values % 2 != 0 || m.learned_values > m.value_capacity()) return false;
        if (m.learned_width <= 0) return false;
        // 畫面的寬與高是 8 的倍數、不超過核心處理的最大畫面，而且實數個數剛好是整數個潛在通道
        if (m.image_width <= 0 || m.image_height <= 0 || m.image_width % 8 != 0 || m.image_height % 8 != 0) return false;
        if (m.image_width > kMaxImageWidth || m.image_height > kMaxImageHeight) return false;
        if (m.learned_values % ((m.image_height / 8) * (m.image_width / 8)) != 0) return false;
    }
    return true;
}
static_assert(mode_table_is_consistent());

}  // namespace nbtv
