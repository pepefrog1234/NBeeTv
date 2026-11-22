// 學習式影像編碼：權重檔的解析，以及 C++ 的推論和 PyTorch 算出來的是否一致。
// 參考答案（data/learned_7776_v1_ref.bin）由 tools/learned/export.py 產生。
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "bits.hpp"
#include "check.hpp"
#include "learned_codec.hpp"
#include "rng.hpp"
#include "test_images.hpp"

using namespace nbtv;

namespace {

constexpr std::uint32_t kModelId = 0xDB08E739u;  // models/learned_7776_v1.bin

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::vector<std::uint8_t> bytes;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return bytes;
    std::uint8_t chunk[65536];
    for (std::size_t n = 0; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) bytes.insert(bytes.end(), chunk, chunk + n);
    std::fclose(f);
    return bytes;
}

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

// 改過內容之後把結尾的檢查碼補對，用來測「檢查碼對、內容不對」的情形
void fix_crc(std::vector<std::uint8_t>& file) {
    const std::uint32_t crc = crc32(std::span<const std::uint8_t>(file).first(file.size() - 4));
    for (int i = 0; i < 4; ++i) file[file.size() - 4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(crc >> (8 * i));
}

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void test_loader(const std::vector<std::uint8_t>& file) {
    std::string error;
    const auto codec = LearnedCodec::load(file, &error);
    CHECK(codec != nullptr);
    CHECK(error.empty());
    if (codec == nullptr) return;
    CHECK_EQ(codec->value_count(), 7776);
    CHECK_EQ(codec->model_id(), kModelId);

    // 不是權重檔
    CHECK(LearnedCodec::load({}, &error) == nullptr);
    CHECK(!error.empty());
    CHECK(LearnedCodec::load({}) == nullptr);  // 不要原因也可以
    const std::vector<std::uint8_t> junk(100, 0x55);
    CHECK(LearnedCodec::load(junk) == nullptr);

    // 截斷在各種位置
    for (std::size_t keep : {std::size_t{8}, std::size_t{35}, std::size_t{36}, std::size_t{1000}, file.size() - 5, file.size() - 1}) {
        const std::vector<std::uint8_t> cut(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(keep));
        CHECK(LearnedCodec::load(cut) == nullptr);
    }
    // 截斷之後檢查碼剛好補對：長度和內容對不上，也要拒絕
    {
        std::vector<std::uint8_t> cut(file.begin(), file.begin() + 5000);
        fix_crc(cut);
        CHECK(LearnedCodec::load(cut, &error) == nullptr);
    }
    // 資料中間壞了一個位元
    {
        std::vector<std::uint8_t> bad = file;
        bad[bad.size() / 2] ^= 0x01;
        CHECK(LearnedCodec::load(bad, &error) == nullptr);
    }
    // 以下都把檢查碼補對，看結構上的檢查
    {
        std::vector<std::uint8_t> bad = file;  // 格式版本
        bad[8] = 2;
        fix_crc(bad);
        CHECK(LearnedCodec::load(bad) == nullptr);
    }
    {
        std::vector<std::uint8_t> bad = file;  // 第一個張量的名稱（位移 34 起）改掉一個字
        bad[34] = 'x';
        fix_crc(bad);
        CHECK(LearnedCodec::load(bad, &error) == nullptr);
        CHECK(error.find("enc.stem.weight") != std::string::npos);
    }
    {
        std::vector<std::uint8_t> bad = file;  // 第一個張量的第一個維度（名稱 15 個字、維度數 1 個位元組之後）32 → 31
        CHECK_EQ(static_cast<int>(bad[34 + 15 + 1]), 32);
        bad[34 + 15 + 1] = 31;
        fix_crc(bad);
        CHECK(LearnedCodec::load(bad) == nullptr);
    }
    {
        std::vector<std::uint8_t> bad = file;  // 結尾多出資料
        bad.insert(bad.end() - 4, {1, 2, 3, 4});
        fix_crc(bad);
        CHECK(LearnedCodec::load(bad) == nullptr);
    }
    {
        std::vector<std::uint8_t> bad = file;  // 權重裡出現無限大（半精度 0x7C00）；第一個張量的資料從位移 66 開始
        bad[66] = 0x00;
        bad[67] = 0x7C;
        fix_crc(bad);
        CHECK(LearnedCodec::load(bad, &error) == nullptr);
    }
}

void test_against_pytorch(const LearnedCodec& codec, const std::vector<std::uint8_t>& ref) {
    const std::size_t n = static_cast<std::size_t>(codec.value_count());
    const std::size_t per_image = n * 4 + 2 * static_cast<std::size_t>(kImageRgbBytes);
    CHECK(ref.size() == 20 + 3 * per_image && std::memcmp(ref.data(), "NBTVLRF1", 8) == 0);
    if (ref.size() != 20 + 3 * per_image) return;
    CHECK_EQ(le32(ref.data() + 8), codec.model_id());
    CHECK_EQ(le32(ref.data() + 12), 3);
    CHECK_EQ(le32(ref.data() + 16), codec.value_count());

    double worst_value = 0.0, encode_ms = 0.0, decode_ms = 0.0;
    int worst_level = 0, differing = 0;
    for (int k = 0; k < 3; ++k) {
        const auto rgb = test_images::load(test_images::kNames[k]);
        const std::uint8_t* p = ref.data() + 20 + static_cast<std::size_t>(k) * per_image;
        std::vector<float> expected(n);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t bits = le32(p + 4 * i);
            std::memcpy(&expected[i], &bits, sizeof(float));
        }

        // 編碼器
        std::vector<float> values(n);
        auto t0 = std::chrono::steady_clock::now();
        CHECK(codec.encode(rgb, values));
        encode_ms += ms_since(t0);
        double power = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            worst_value = std::max(worst_value, static_cast<double>(std::fabs(values[i] - expected[i])));
            power += static_cast<double>(values[i]) * values[i];
        }
        CHECK_NEAR(power / static_cast<double>(n), 0.5, 1e-5);

        // 解碼器：輸入用 PyTorch 的通道值，兩邊才是同一個起點
        for (int s = 0; s < 2; ++s) {
            std::vector<std::uint8_t> out(static_cast<std::size_t>(kImageRgbBytes));
            t0 = std::chrono::steady_clock::now();
            CHECK(codec.decode(expected, s == 0 ? 10.0f : 30.0f, out));
            decode_ms += ms_since(t0);
            const std::uint8_t* want = p + 4 * n + static_cast<std::size_t>(s) * static_cast<std::size_t>(kImageRgbBytes);
            for (std::size_t i = 0; i < out.size(); ++i) {
                const int d = std::abs(static_cast<int>(out[i]) - static_cast<int>(want[i]));
                worst_level = std::max(worst_level, d);
                differing += d != 0 ? 1 : 0;
            }
        }
    }
    // 兩邊都是單精度，只差在加總的順序與誤差函數的實作。通道值的均方根是 0.707。
    CHECK(worst_value < 1e-4);
    // 解出來的畫面：四捨五入的邊界上可能差一階，不會更多，而且只有極少數的值
    CHECK(worst_level <= 1);
    CHECK(differing <= 6 * kImageRgbBytes / 500);
    std::printf("  和 PyTorch 比：通道值最大差 %.2e；畫面 %d 個值裡有 %d 個差一階\n", worst_value, 6 * kImageRgbBytes, differing);
    std::printf("  編碼每張 %.0f ms、解碼每張 %.0f ms（單執行緒）\n", encode_ms / 3.0, decode_ms / 6.0);
}

void test_round_trip(const LearnedCodec& codec) {
    // 自己編、自己解。對照 export.py 印出來的 PyTorch 數字（三張測試圖）：
    //   不加雜訊、訊雜比輸入 30 dB：26.896、27.833、28.213 dB；加 10 dB 白雜訊平均 26.465 dB
    const double clean_expected[3] = {26.896, 27.833, 28.213};
    const std::size_t n = static_cast<std::size_t>(codec.value_count());
    std::vector<float> values(n), noisy(n);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(kImageRgbBytes));
    double noisy_total = 0.0;
    int noisy_count = 0;
    const double sigma = std::sqrt(std::pow(10.0, -10.0 / 10.0) / 2.0);  // 每個實數的雜訊標準差
    for (int k = 0; k < 3; ++k) {
        const auto rgb = test_images::load(test_images::kNames[k]);
        CHECK(codec.encode(rgb, values));
        CHECK(codec.decode(values, 30.0f, out));
        CHECK_NEAR(test_images::psnr(out, rgb), clean_expected[k], 0.02);
        Rng rng(static_cast<std::uint64_t>(100 + k));
        for (int trial = 0; trial < 2; ++trial) {
            for (std::size_t i = 0; i < n; ++i) noisy[i] = values[i] + static_cast<float>(sigma * rng.gauss());
            CHECK(codec.decode(noisy, 10.0f, out));
            noisy_total += test_images::psnr(out, rgb);
            ++noisy_count;
        }
    }
    CHECK_NEAR(noisy_total / noisy_count, 26.465, 0.3);

    // 訊雜比超出範圍或不是數值：夾到範圍內，結果和範圍的端點相同
    std::vector<std::uint8_t> a(out.size()), b(out.size());
    CHECK(codec.decode(values, kLearnedMaxSnrDb, a));
    CHECK(codec.decode(values, 1e9f, b));
    CHECK(a == b);
    CHECK(codec.decode(values, std::numeric_limits<float>::infinity(), b));
    CHECK(a == b);
    CHECK(codec.decode(values, std::numeric_limits<float>::quiet_NaN(), b));
    // 收到的值裡有非數值：當成 0，照樣解得出畫面
    noisy = values;
    noisy[0] = std::numeric_limits<float>::quiet_NaN();
    noisy[1] = std::numeric_limits<float>::infinity();
    CHECK(codec.decode(noisy, 20.0f, b));
    // 長度不對
    CHECK(!codec.encode(std::vector<std::uint8_t>(100), values));
    CHECK(!codec.encode(std::vector<std::uint8_t>(static_cast<std::size_t>(kImageRgbBytes)), std::span<float>(values).first(n - 1)));
    CHECK(!codec.decode(std::span<const float>(values).first(n - 1), 10.0f, out));
    CHECK(!codec.decode(values, 10.0f, std::span<std::uint8_t>(out).first(100)));
}

}  // namespace

int main() {
    const std::string model_path = std::string(NBTV_MODEL_DIR) + "/learned_7776_v1.bin";
    const std::vector<std::uint8_t> file = read_file(model_path);
    if (file.empty()) {
        std::printf("  找不到權重檔 %s，略過學習式編碼的測試\n", model_path.c_str());
        return check::finish("learned");
    }
    test_loader(file);
    const auto codec = LearnedCodec::load(file);
    if (codec != nullptr) {
        test_against_pytorch(*codec, read_file(std::string(NBTV_TEST_DATA_DIR) + "/learned_7776_v1_ref.bin"));
        test_round_trip(*codec);
    }
    return check::finish("learned");
}
