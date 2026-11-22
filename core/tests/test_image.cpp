// 數位模式的影像酬載：AVIF 外殼的拆裝，以及（有 libavif 時）編解碼與位元率控制。
#include <chrono>
#include <cstdio>
#include <vector>

#include "av1_payload.hpp"
#include "check.hpp"
#include "image_codec.hpp"
#include "modes.hpp"
#include "test_images.hpp"

using namespace nbtv;

namespace {

void test_payload_parsing() {
    // 不是 OBU 資料
    const std::vector<std::uint8_t> zeros(64, 0), junk = {0xFF, 0x12, 0x34};
    CHECK_EQ(static_cast<int>(av1_payload_length(zeros)), 0);
    CHECK_EQ(static_cast<int>(av1_payload_length(junk)), 0);
    CHECK_EQ(static_cast<int>(av1_payload_length({})), 0);
    CHECK(avif_from_payload(zeros).empty());
    CHECK(payload_from_avif(junk).empty());
    // 手工的序列標頭 OBU（libaom 對 96×72、4:4:4 的輸出）加一個假的畫面 OBU
    const std::vector<std::uint8_t> seq = {0x0A, 0x09, 0x38, 0x19, 0xAF, 0xC7, 0x6D, 0x01, 0x0D, 0x06, 0x90};
    Av1SequenceInfo info;
    CHECK(av1_parse_sequence_header(seq, info));
    CHECK_EQ(info.profile, 1);
    CHECK_EQ(info.level, 0);
    CHECK_EQ(info.width, 96);
    CHECK_EQ(info.height, 72);
    CHECK(!info.high_bitdepth);
    std::vector<std::uint8_t> payload = seq;
    payload.insert(payload.end(), {0x32, 0x03, 0xAA, 0xBB, 0xCC});  // OBU_FRAME，長度 3
    CHECK_EQ(static_cast<int>(av1_payload_length(payload)), 16);
    // 尾端補零不算在內
    std::vector<std::uint8_t> padded = payload;
    padded.resize(100, 0);
    CHECK_EQ(static_cast<int>(av1_payload_length(padded)), 16);
    // 長度欄位超出範圍：只算到前一個完整的 OBU
    std::vector<std::uint8_t> truncated = payload;
    truncated[12] = 0x50;
    CHECK_EQ(static_cast<int>(av1_payload_length(truncated)), 11);
    // 套上外殼：275 位元組的外殼 + 2 位元組的時序分隔 + 酬載；再拆開要還原
    const auto file = avif_from_payload(padded);
    CHECK_EQ(static_cast<int>(file.size()), static_cast<int>(kAvifShellBytes + kAvifTemporalDelimiterBytes + 16));
    CHECK(payload_from_avif(file) == payload);
}

void test_codec() {
    if (!image_codec_available()) {
        std::printf("  這個建置沒有 libavif，略過影像編解碼的測試\n");
        return;
    }
    // 規格草案的四種酬載大小
    const int sizes[] = {393, 888, 1338, 1473};
    double mean_psnr[4] = {0, 0, 0, 0};
    double total_ms = 0;
    int encodes = 0;
    for (const char* name : test_images::kNames) {
        const auto rgb = test_images::load(name);
        for (int q = 0; q < 4; ++q) {
            ImageEncodeResult r;
            const auto t0 = std::chrono::steady_clock::now();
            CHECK(image_encode(rgb, sizes[q], kDefaultEncoderSpeed, r));
            total_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ++encodes;
            CHECK(static_cast<int>(r.payload.size()) <= sizes[q]);
            CHECK(static_cast<int>(r.payload.size()) > sizes[q] * 3 / 4);  // 位元率控制有把空間用上
            CHECK(r.encodes <= 7);
            CHECK_EQ(static_cast<int>(av1_payload_length(r.payload)), static_cast<int>(r.payload.size()));
            // 補零到模式的酬載大小後仍解得出來
            std::vector<std::uint8_t> padded = r.payload;
            padded.resize(static_cast<std::size_t>(sizes[q]), 0);
            std::vector<std::uint8_t> out(kImageRgbBytes);
            CHECK(image_decode(padded, out));
            const double psnr = test_images::psnr(out, rgb);
            mean_psnr[q] += psnr / 3.0;
            CHECK(psnr > (q == 0 ? 21.0 : 27.0));
        }
        // 酬載加回外殼後，與 libavif 直接輸出的檔案逐位元相同（各種速度檔都要成立）
        for (int speed : {2, 3, 6, 9}) {
            std::vector<std::uint8_t> file;
            CHECK(image_encode_avif_file(rgb, 30, speed, file));
            const auto payload = payload_from_avif(file);
            CHECK_EQ(static_cast<int>(payload.size()), static_cast<int>(file.size() - kAvifShellBytes - kAvifTemporalDelimiterBytes));
            CHECK(avif_from_payload(payload) == file);
        }
    }
    std::printf("  AV1 4:4:4，速度檔 %d，三張測試圖平均 PSNR：393 位元組 %.1f dB、888 %.1f、1338 %.1f、1473 %.1f（每張平均 %.0f ms）\n",
                kDefaultEncoderSpeed, mean_psnr[0], mean_psnr[1], mean_psnr[2], mean_psnr[3], total_ms / encodes);
    // 每個數位模式的影像上限都放得下一張畫面
    const auto rgb = test_images::load("cat");
    for (const Mode& m : kModes) {
        if (!m.is_digital()) continue;
        ImageEncodeResult r;
        CHECK(image_encode(rgb, m.image_bytes(), kDefaultEncoderSpeed, r));
        CHECK(static_cast<int>(r.payload.size()) <= m.image_bytes());
    }
    // 壞掉的酬載：解碼失敗但不會出事
    ImageEncodeResult r;
    CHECK(image_encode(rgb, 888, kDefaultEncoderSpeed, r));
    std::vector<std::uint8_t> out(kImageRgbBytes);
    std::vector<std::uint8_t> bad = r.payload;
    bad.resize(40);
    CHECK(!image_decode(bad, out));
    CHECK(!image_decode(std::vector<std::uint8_t>(100, 0), out));
    // 放不下：連最粗的量化都超過上限
    CHECK(!image_encode(rgb, 30, kDefaultEncoderSpeed, r));
}

}  // namespace

int main() {
    test_payload_parsing();
    test_codec();
    return check::finish("image");
}
