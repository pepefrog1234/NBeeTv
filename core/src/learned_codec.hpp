// 學習式影像編碼：一個類神經網路把畫面直接變成通道上的實數值（兩個一組當格點的 I、Q），
// 另一個網路把收到的值連同訊雜比還原成畫面。沒有量化也沒有熵編碼，訊號變差時畫質跟著慢慢變差，
// 不像靠糾錯碼的數位傳輸那樣，訊號一低過門檻就整幀解不出來。結構、訓練與量到的結果見 tools/learned/。
//
// 權重放在獨立的二進位檔（models/），格式寫在 tools/learned/export.py 開頭。收發兩端必須用同一份，
// 以 model_id() 辨認。網路在兩端各自以浮點數計算，不同機器的結果會有末位的差異；那遠小於通道的雜訊，
// 不影響互通。
//
// 畫面的大小是模型的一部分：模型是照某個大小訓練的，權重檔裡記著（沒有記的是 96×72，模式 11–14 的模型都是）。
// 網路本身只有卷積，別的大小也算得出來，但畫質沒有保證，所以這裡只照權重檔記的大小編解碼。
//
// 這裡只有推論本身；數據機用到它的地方是模式 11 到 15（各用一份權重），見 learned_frame.hpp。
// encode、decode 會把計算分給幾條執行緒（parallel.hpp），做完才返回；結果和只用一條時逐位元相同。
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace nbtv {

// 解碼器認得的最高訊雜比（dB）；更高的值會被夾到這裡。最低的值依模型而定，見 min_snr_db()。
inline constexpr float kLearnedMaxSnrDb = 30.0f;

class LearnedCodec {
public:
    // 解析權重檔的內容。不是權重檔、長度不對、檢查碼不符、或張量和模型的結構對不上時回傳空指標，
    // error（可省略）填入原因。
    static std::unique_ptr<LearnedCodec> load(std::span<const std::uint8_t> file, std::string* error = nullptr);

    ~LearnedCodec();
    LearnedCodec(const LearnedCodec&) = delete;
    LearnedCodec& operator=(const LearnedCodec&) = delete;

    // 權重檔的識別碼（檔案內容的 CRC-32）。
    std::uint32_t model_id() const;

    // 這個模型的畫面大小（像素）：寬與高都是 8 的倍數，不超過 kMaxImageWidth × kMaxImageHeight。
    int image_width() const;
    int image_height() const;
    // 一張畫面的位元組數：寬 × 高 × 3（8 位元 RGB，由左上角開始逐列排列）。
    int rgb_bytes() const { return image_width() * image_height() * 3; }

    // 一張畫面變成幾個實數：潛在通道數 × 畫面縮小 8 倍的格子數。96×72 的格子是 9×12
    // （模式 11、13 的模型是 7776，模式 12 的是 2592，模式 14 的是 1512，模式 3 的是 1728）；192×144 的是 18×24
    // （模式 5 與 15 的也是 7776，模式 6 的是 9936，模式 7 的是 15552，模式 4 的是 3024）。
    int value_count() const;
    // 潛在通道數（權重檔的標頭裡記的）。
    int latent_channels() const;

    // 各層的寬度：解析度每減半一次的通道數，共四個。權重檔沒有另外記，是由張量的形狀看出來的。
    // 模式 11、12 的模型是 32、64、128、192；模式 13、14 的每一層加寬一倍（運算量約四倍）。
    std::array<int, 4> widths() const;
    // 第一個寬度。模式用它和實數個數來認自己的模型（learned_model_fits）。
    int base_width() const { return widths()[0]; }

    // 權重檔的結構編號（tools/learned/model.py）：1 的解碼器只吃訊雜比，2 的另外吃各個值的可靠程度。
    int architecture() const;

    // 解碼器認得的最低訊雜比（dB），訓練時涵蓋到這裡：結構 1 是 −2，結構 2 是 −6。更低的值會被夾到這裡。
    float min_snr_db() const;

    // rgb 是一張畫面（rgb_bytes() 個位元組）；values 收 value_count() 個實數，均方值是 1/2
    // （兩個一組當 I、Q 時，格點的平均功率是 1）。長度不對時回傳 false。
    bool encode(std::span<const std::uint8_t> rgb, std::span<float> values) const;

    // values 是等化後收到的值（順序和 encode 的輸出相同），snr_db 是格點訊雜比；rgb 收一張畫面（rgb_bytes() 個位元組）。
    // reliability 是各個值的可靠程度，0 到 1：1 是照常，愈小表示那個值愈不可信（接收端已經把它按比例壓小），
    // 0 等於沒收到。長度和 values 相同；給空的表示全部是 1。結構 1 的模型不看它。
    // 長度不對時回傳 false。
    bool decode(std::span<const float> values, std::span<const float> reliability, float snr_db,
                std::span<std::uint8_t> rgb) const;
    // 每個值都一樣可靠時的簡便寫法。
    bool decode(std::span<const float> values, float snr_db, std::span<std::uint8_t> rgb) const {
        return decode(values, {}, snr_db, rgb);
    }

    // encode、decode 不改變物件的狀態，可以同時從不同的執行緒呼叫同一個物件。

    struct Impl;

private:
    explicit LearnedCodec(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace nbtv
