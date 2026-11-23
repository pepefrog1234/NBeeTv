// 畫面 ↔ 一幀的資料格。
//
// 幀的安排：開頭 kHeaderCells 格是標頭（frame_header.hpp），之後每格放兩個實數，最後整幀做格點交錯
// （Mode::interleaved_position）。實數由 LearnedCodec 產生（mode.learned_values 個）；模式多出來的格點是 0。
//
// 實數放進格點之前先照一個固定的亂數排列打散（learned_value_order）。編碼器的輸出是「通道 × 列 × 行」，
// 照原來的順序放的話，衰落抹掉的一片格點會集中落在畫面的某幾行上：那一塊在每個通道都沒有資料，
// 解碼器就在那裡畫出黑塊與點陣。打散之後，同樣的衰落在每個位置只抹掉差不多比例的通道，畫面是整體變糊。
//
// 標頭：「格式」那 4 個位元是 kLearnedFrameFormat（排法改了就換一個數字），另有一個位元組放
// 模型識別碼的最低 8 位元。兩者有一個對不上，接收端就知道解不出來，不交出畫面。
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "frame_header.hpp"
#include "learned_codec.hpp"
#include "modes.hpp"
#include "types.hpp"

namespace nbtv {

// 標頭的「格式」欄：1 是「實數先照 learned_value_order 打散」的排法。（0 是打散之前的試作，已經不用。）
inline constexpr int kLearnedFrameFormat = 1;

// 實數在幀裡的順序：第 k 個傳送位置（兩個一格，接在標頭後面）放的是編碼器輸出的第 order[k] 個實數。
// 是 0..learned_values−1 的一個排列，由固定種子的亂數洗牌產生，收發兩端算出來的相同。
const std::vector<std::uint16_t>& learned_value_order(const Mode& mode);

// 這個模型能不能用在這個模式：實數的個數與寬度（LearnedCodec::base_width）都要和模式的相同。
bool learned_model_fits(const Mode& mode, const LearnedCodec& codec);

// 發射機、接收機手上的一組模型。每個模式用「實數個數與寬度都和它相同」的那一個，所以每種組合最多留一個。
class LearnedModels {
public:
    // 加進一個模型；已經有實數個數與寬度都相同的就換成這一個。空指標表示全部拿掉。
    void set(std::shared_ptr<const LearnedCodec> model);
    // 這個模式用的模型；沒有的話是空指標。
    const LearnedCodec* find(const Mode& mode) const;

private:
    std::vector<std::shared_ptr<const LearnedCodec>> models_;
};

// 畫面 → mode.data_cells() 個格點，依傳送順序。header 的 format 與 model_tag 由這裡填。
// values 不是空指標時，另外交出編碼器產生的實數（發射端做預覽用）。模型和模式不合時回傳空的。
std::vector<cf> learned_encode(const Mode& mode, const LearnedCodec& codec, const FrameHeader& header,
                               std::span<const std::uint8_t> rgb, std::vector<float>* values = nullptr);

// 還原交錯、解出標頭之後的一幀
struct LearnedUnpacked {
    bool header_ok = false;
    FrameHeader header;            // header_ok 時有效；model_tag 是對方模型識別碼的最低 8 位元
    std::vector<float> values;     // mode.learned_values 個實數
    std::vector<float> noise_var;  // 各實數所在格點的複數雜訊變異數
};

// cells 是等化後的資料格（mode.data_cells() 個，依傳送順序），noise_var 是各格點等化後的複數雜訊變異數。
LearnedUnpacked learned_unpack(const Mode& mode, std::span<const cf> cells, std::span<const float> noise_var);

// 標頭說的排法與模型是不是手上這一份。標頭沒解出來時無從比對，當成是。
bool learned_model_matches(const LearnedUnpacked& frame, const LearnedCodec& codec);

// 模型訓練時，一張畫面的每個格點雜訊一樣大；實際上各格點的雜訊不同（衰落、窄頻干擾）。
// 這裡是交給解碼器之前的處理方式，公開出來讓模擬工具可以比較。
struct LearnedRxOptions {
    // 告訴解碼器的訊雜比由各格點雜訊的中位數算；false 的話用平均值（少數很差的格點會把它拉得很低）。
    bool median_snr = true;
    // 雜訊比參考值大的格點，把收到的值按比例壓小（雜訊極大時趨近於 0）。
    // 不壓的話，落在衰落谷底的格點等化後是很大的亂數，解碼器沒見過這種輸入。
    bool attenuate = true;
    // 訊雜比比解碼器認得的下限（LearnedCodec::min_snr_db）還低時，參考值停在下限，超過的雜訊同樣用壓小的方式處理。
    // 不這樣做的話，解碼器拿到的值比它見過的都大，會畫出不存在的紋理與顏色。
    bool floor = true;
};

// 取出的實數 → 96×72×3 的畫面。回傳告訴解碼器的訊雜比（dB）。
float learned_picture(const LearnedCodec& codec, const LearnedUnpacked& frame, const LearnedRxOptions& options,
                      std::span<std::uint8_t> rgb);

}  // namespace nbtv
