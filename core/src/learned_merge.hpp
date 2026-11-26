// 接收端把最近幾幀合起來解（學習式模式）。
//
// 每一幀仍然是各自獨立的：發射端、空中格式、畫面的解碼器都不變。差別只在接收端：解這一幀之前，
// 先看前面幾幀收到的值。畫面沒變的地方，幾幀送的是同樣的值，照各自的雜訊加權平均就能把雜訊壓低
// （合 4 幀約 6 dB）；畫面變了的地方只用這一幀。判斷「變了多少」的是一個小網路：
//   1. 由這一幀與前面某一幀收到的值差多少、各自有多吵，估計每個位置的值真正差了多少；
//   2. 由估計的差距和雜訊的大小，決定前面那一幀的每個值要合進來多少（0 到 1，下面叫「閘」）。
// 合併後的值與它的雜訊變異數交給原來的 learned_picture。訊號弱、畫面平靜時好處最大；
// 訊號很好或畫面一直在變時，結果和不合併幾乎一樣。做法、訓練與量到的結果見 tools/learned/README.md。
//
// 只配畫面是 96×72 的模型（模式 11–14）：小網路是照 9×12 的格子寫的。192×144 的大畫面（模式 5–7 與 15）沒有這種權重，照原來的方式解。
//
// 小網路的權重放在獨立的檔案（models/），每個畫面模型各一份：不同模型送出來的值互不相通。
// 檔案裡記著它配的是哪一個畫面模型（model_id）。接收機怎麼用它見 receiver.hpp 的 set_merge。
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace nbtv {

class LearnedMerge {
public:
    // 解析權重檔的內容。不是這種權重檔、長度不對、檢查碼不符、或張量對不上時回傳空指標，
    // error（可省略）填入原因。
    static std::unique_ptr<LearnedMerge> load(std::span<const std::uint8_t> file, std::string* error = nullptr);

    ~LearnedMerge();
    LearnedMerge(const LearnedMerge&) = delete;
    LearnedMerge& operator=(const LearnedMerge&) = delete;

    // 這個檔案的識別碼（內容的 CRC-32）。
    std::uint32_t merge_id() const;
    // 它配的畫面模型（LearnedCodec::model_id）。
    std::uint32_t model_id() const;
    // 一幀幾個實數（和那個畫面模型的 value_count 相同）：潛在通道數 × 9 × 12。
    int value_count() const;
    // 一共看幾幀：現在這一幀，加上前面 frames() − 1 幀。
    int frames() const;

    // 前面的一幀。values 是空的表示那一幀沒有收到。
    struct Earlier {
        std::span<const float> values;
        std::span<const float> noise_var;
    };

    // values、noise_var：現在這一幀收到的值與各個值所在格點的複數雜訊變異數（value_count() 個，
    // 順序和 LearnedCodec::decode 的相同）。earlier[j − 1] 是 j 幀之前的那一幀（j = 1 … frames() − 1）；
    // 給得比較少的話，後面的當作沒收到，多的不看。merged_values、merged_noise_var 收合併後的結果。
    // 長度不對時回傳 false。前面的幀全部沒收到時，結果就是現在這一幀原來的值。
    bool merge(std::span<const float> values, std::span<const float> noise_var, std::span<const Earlier> earlier,
               std::span<float> merged_values, std::span<float> merged_noise_var) const;

    // merge 不改變物件的狀態，可以同時從不同的執行緒呼叫同一個物件。

    struct Impl;

private:
    explicit LearnedMerge(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace nbtv
