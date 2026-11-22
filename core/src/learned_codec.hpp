// 學習式影像編碼：一個類神經網路把 96×72 的畫面直接變成通道上的實數值（兩個一組當格點的 I、Q），
// 另一個網路把收到的值連同訊雜比還原成畫面。沒有量化也沒有熵編碼，訊號變差時畫質跟著慢慢變差，
// 不會像數位模式一樣整幀解不出來。結構、訓練與量到的結果見 tools/learned/。
//
// 權重放在獨立的二進位檔（models/），格式寫在 tools/learned/export.py 開頭。收發兩端必須用同一份，
// 以 model_id() 辨認。網路在兩端各自以浮點數計算，不同機器的結果會有末位的差異；那遠小於通道的雜訊，
// 不影響互通。
//
// 目前只有推論本身，還沒有接進數據機的模式。和核心其他部分一樣不開執行緒。
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace nbtv {

// 解碼器認得的訊雜比範圍（dB）；超出的值會被夾到範圍內。訓練時涵蓋 −2 到 28 dB。
inline constexpr float kLearnedMinSnrDb = -2.0f;
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

    // 一張畫面變成幾個實數：潛在通道數 × 9 × 12（目前的模型是 7776）。
    int value_count() const;

    // rgb 是 96×72×3 的畫面；values 收 value_count() 個實數，均方值是 1/2
    // （兩個一組當 I、Q 時，格點的平均功率是 1）。長度不對時回傳 false。
    bool encode(std::span<const std::uint8_t> rgb, std::span<float> values) const;

    // values 是等化後收到的值（順序和 encode 的輸出相同），snr_db 是格點訊雜比；rgb 收 96×72×3 的畫面。
    // 長度不對時回傳 false。
    bool decode(std::span<const float> values, float snr_db, std::span<std::uint8_t> rgb) const;

    // encode、decode 不改變物件的狀態，可以同時從不同的執行緒呼叫同一個物件。

    struct Impl;

private:
    explicit LearnedCodec(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace nbtv
