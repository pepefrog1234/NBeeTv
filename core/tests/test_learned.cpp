// 各個模式的模型：權重檔的解析、C++ 的推論和 PyTorch 算出來的是否一致，以及模式 11 到 14 的幀與完整的收發鏈。
// 每個模式各有一份權重（models/）與一份參考答案（data/…_ref.bin，由 tools/learned/export.py 產生）。
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "bits.hpp"
#include "channel.hpp"
#include "check.hpp"
#include "frame_map.hpp"
#include "learned_codec.hpp"
#include "learned_frame.hpp"
#include "link_util.hpp"
#include "modes.hpp"
#include "receiver.hpp"
#include "rng.hpp"
#include "test_images.hpp"
#include "transmitter.hpp"

using namespace nbtv;

namespace {

// 一個模式和它的權重。對照的數字除了註明的以外，都是 export.py 產生參考答案時印出來的（PyTorch，核心的三張測試圖）；
// 換了權重要一起更新。
struct ModelCase {
    int mode_id;
    const char* name;        // models/<name>.bin 與 data/<name>_ref.bin；名稱是「訓練完成的年月日-當天第幾個」
    std::uint32_t model_id;  // 權重檔的識別碼
    double clean[3];         // 不加雜訊、訊雜比輸入 30 dB 時三張圖的 PSNR
    double awgn10;           // 加 10 dB 白雜訊時三張圖的平均
    // 以下是 C++ 這邊量到的數字留一點餘裕之後的門檻（都是貓那一張，或黑白剪影）
    double link_awgn10;      // 完整的收發鏈，白雜訊 10 dB（3 kHz 雜訊頻寬）：平均值，容許 ±0.8 dB
    double link_poor15;      // 完整的收發鏈，差通道 15 dB：下限
    double faded;            // 一成的格點在衰落谷底、其餘 15 dB：下限
    double silhouette;       // 黑白剪影過差通道（0、2、5 dB）：平均的下限
    double flipped;          // 同上：純色的地方變成另一色的比例（最差的一張）的上限
};
const ModelCase kCases[] = {
    // 量到的：收發鏈 28.3、28.3 dB，衰落谷底 27.5 dB，剪影 30.3 dB、0.0%
    {11, "261123-1", 0xD6ED61D0u, {28.677, 29.665, 29.837}, 28.149, 28.3, 26.4, 25.8, 28.4, 0.01},
    // 量到的：收發鏈 23.6、23.8 dB，衰落谷底 23.1 dB，剪影 24.1 dB、1.3%
    {12, "261122-3", 0xBD805526u, {23.888, 25.168, 24.806}, 23.570, 23.6, 22.0, 21.5, 22.0, 0.025},
    // 模式 13：幀和模式 11 相同，模型的每一層加寬一倍。量到的：收發鏈 29.0、29.0 dB，衰落谷底 28.3 dB，剪影 30.6 dB、0.0%
    {13, "261122-4", 0x8D00009Au, {29.539, 30.796, 30.963}, 28.858, 29.0, 27.1, 26.6, 28.7, 0.01},
    // 模式 14：每張 0.667 秒、14 個通道，加寬的模型。量到的：收發鏈 22.0、22.4 dB，衰落谷底 22.2 dB，剪影 23.5 dB、0.6%
    {14, "261123-3", 0x89CF65FDu, {22.519, 23.795, 23.314}, 22.142, 22.0, 20.5, 20.5, 21.6, 0.02},
};

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

// 「另一份權重」：改掉一個權重（第一個張量的資料從位移 66 開始），識別碼就跟著變。
// 幀的標頭只帶識別碼的最低 8 位元；剛好相同的話換一個位置再試。
std::shared_ptr<const LearnedCodec> altered_model(const std::vector<std::uint8_t>& file, const LearnedCodec& model) {
    for (std::size_t at = 66; at < 200; at += 2) {
        std::vector<std::uint8_t> altered = file;
        altered[at] ^= 0x01;
        fix_crc(altered);
        std::shared_ptr<const LearnedCodec> candidate = LearnedCodec::load(altered);
        if (candidate != nullptr && (candidate->model_id() & 0xFFu) != (model.model_id() & 0xFFu)) return candidate;
    }
    return nullptr;
}

void test_loader(const ModelCase& model, const std::vector<std::uint8_t>& file) {
    std::string error;
    const auto codec = LearnedCodec::load(file, &error);
    CHECK(codec != nullptr);
    CHECK(error.empty());
    if (codec == nullptr) return;
    CHECK_EQ(codec->value_count(), find_mode(model.mode_id)->learned_values);
    CHECK_EQ(codec->model_id(), model.model_id);
    CHECK_EQ(codec->architecture(), 2);
    // 各層的寬度由張量的形狀看出來；第一個要和模式指定的相同，後面的依序是它的 2、4、6 倍（model.py 的兩組寬度都是這樣）
    const int width = find_mode(model.mode_id)->learned_width;
    CHECK(codec->widths() == (std::array<int, 4>{width, 2 * width, 4 * width, 6 * width}));
    CHECK_EQ(codec->base_width(), width);
    CHECK_NEAR(codec->min_snr_db(), -6.0, 1e-6);

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
        std::vector<std::uint8_t> bad = file;  // 這一版不認得的結構編號
        bad[12] = 3;
        fix_crc(bad);
        CHECK(LearnedCodec::load(bad) == nullptr);
    }
    {
        std::vector<std::uint8_t> bad = file;  // 結構編號和張量的形狀對不上（結構 1 的輸入通道數是一半）
        bad[12] = 1;
        fix_crc(bad);
        CHECK(LearnedCodec::load(bad, &error) == nullptr);
        CHECK(error.find("dec.inp.weight") != std::string::npos);
    }
    {
        std::vector<std::uint8_t> bad = file;  // 第一個張量的名稱（位移 34 起）改掉一個字
        bad[34] = 'x';
        fix_crc(bad);
        CHECK(LearnedCodec::load(bad, &error) == nullptr);
        CHECK(error.find("enc.stem.weight") != std::string::npos);
    }
    {
        std::vector<std::uint8_t> bad = file;  // 第一個張量的第一個維度（名稱 15 個字、維度數 1 個位元組之後）少 1：
        CHECK_EQ(static_cast<int>(bad[34 + 15 + 1]), width);  // 寬度是由它看出來的，其餘的張量就都對不上了
        bad[34 + 15 + 1] = static_cast<std::uint8_t>(width - 1);
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
    // 每張影像：通道值、測試用的一組可靠程度、兩張解碼結果（格式見 export.py）
    const std::size_t n = static_cast<std::size_t>(codec.value_count());
    const std::size_t per_image = 2 * n * 4 + 2 * static_cast<std::size_t>(kImageRgbBytes);
    CHECK(ref.size() == 20 + 3 * per_image && std::memcmp(ref.data(), "NBTVLRF2", 8) == 0);
    if (ref.size() != 20 + 3 * per_image) return;
    CHECK_EQ(le32(ref.data() + 8), codec.model_id());
    CHECK_EQ(le32(ref.data() + 12), 3);
    CHECK_EQ(le32(ref.data() + 16), codec.value_count());

    auto floats = [n](const std::uint8_t* p) {
        std::vector<float> v(n);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t bits = le32(p + 4 * i);
            std::memcpy(&v[i], &bits, sizeof(float));
        }
        return v;
    };
    double worst_value = 0.0, encode_ms = 0.0, decode_ms = 0.0;
    int worst_level = 0, differing = 0;
    for (int k = 0; k < 3; ++k) {
        const auto rgb = test_images::load(test_images::kNames[k]);
        const std::uint8_t* p = ref.data() + 20 + static_cast<std::size_t>(k) * per_image;
        const std::vector<float> expected = floats(p);
        const std::vector<float> reliability = floats(p + 4 * n);

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

        // 解碼器：輸入用 PyTorch 的通道值，兩邊才是同一個起點。
        // 第一種每個值都可靠（10 dB）；第二種把值乘上那組可靠程度，連同可靠程度一起交給解碼器（5 dB）。
        std::vector<float> weighted(n);
        for (std::size_t i = 0; i < n; ++i) weighted[i] = expected[i] * reliability[i];
        for (int s = 0; s < 2; ++s) {
            std::vector<std::uint8_t> out(static_cast<std::size_t>(kImageRgbBytes));
            t0 = std::chrono::steady_clock::now();
            if (s == 0) CHECK(codec.decode(expected, 10.0f, out));
            else CHECK(codec.decode(weighted, reliability, 5.0f, out));
            decode_ms += ms_since(t0);
            const std::uint8_t* want = p + 8 * n + static_cast<std::size_t>(s) * static_cast<std::size_t>(kImageRgbBytes);
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

void test_round_trip(const ModelCase& model, const LearnedCodec& codec) {
    // 自己編、自己解，對照 export.py 印出來的 PyTorch 數字
    const double* clean_expected = model.clean;
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
    CHECK_NEAR(noisy_total / noisy_count, model.awgn10, 0.3);

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
    // 可靠程度：長度不對就拒絕；超出 0–1 或不是數值的會被夾住，照樣解得出畫面；全部是 1 等於沒有給
    std::vector<float> reliability(n, 1.0f);
    CHECK(!codec.decode(values, std::span<const float>(reliability).first(n - 1), 10.0f, b));
    CHECK(codec.decode(values, reliability, kLearnedMaxSnrDb, b));
    CHECK(a == b);
    reliability[0] = std::numeric_limits<float>::quiet_NaN();
    reliability[1] = -3.0f;
    reliability[2] = 7.0f;
    CHECK(codec.decode(values, reliability, 20.0f, b));
    // 長度不對
    CHECK(!codec.encode(std::vector<std::uint8_t>(100), values));
    CHECK(!codec.encode(std::vector<std::uint8_t>(static_cast<std::size_t>(kImageRgbBytes)), std::span<float>(values).first(n - 1)));
    CHECK(!codec.decode(std::span<const float>(values).first(n - 1), 10.0f, out));
    CHECK(!codec.decode(values, 10.0f, std::span<std::uint8_t>(out).first(100)));
}

// ---------------------------------------------------------------- 一幀的資料格

void test_frame(const ModelCase& model, const LearnedCodec& codec) {
    const Mode& mode = *find_mode(model.mode_id);
    CHECK(learned_model_fits(mode, codec));
    for (const Mode& other : kModes) CHECK_EQ(learned_model_fits(other, codec), other.id == mode.id);  // 只合自己的模式
    const auto rgb = test_images::load("cat");
    FrameHeader header;
    header.sequence = 200;
    header.callsign = "N0CALL";
    std::vector<float> values;
    const std::vector<cf> cells = learned_encode(mode, codec, header, rgb, &values);
    CHECK_EQ(static_cast<int>(cells.size()), mode.data_cells());
    CHECK_EQ(static_cast<int>(values.size()), mode.learned_values);
    for (const Mode& other : kModes) {  // 別的模式的模型編不出這個模式的幀
        if (other.id != mode.id) CHECK(learned_encode(other, codec, header, rgb).empty());
    }
    // 用到的格點平均功率是 1；用不到的格點是 0（模式 11、13、14 各有 3 格、模式 12 有 33 格）
    double power = 0.0;
    int zeros = 0;
    for (const cf& c : cells) {
        power += std::norm(c);
        zeros += c == cf(0, 0) ? 1 : 0;
    }
    CHECK_EQ(zeros, (mode.value_capacity() - mode.learned_values) / 2);
    CHECK_NEAR(power / static_cast<double>(static_cast<int>(cells.size()) - zeros), 1.0, 0.01);

    // 幾乎沒有雜訊：標頭與實數都原樣取回
    const std::vector<float> quiet(cells.size(), 1e-4f);
    const LearnedUnpacked u = learned_unpack(mode, cells, quiet);
    CHECK(u.header_ok);
    CHECK_EQ(u.header.sequence, 200);
    CHECK(u.header.callsign == "N0CALL");
    CHECK_EQ(u.header.model_tag, static_cast<int>(codec.model_id() & 0xFFu));
    CHECK(u.values == values);
    // 標頭裡那個位元組對不上，就不是同一份權重；標頭壞了無從比對，當成是
    CHECK(learned_model_matches(u, codec));
    LearnedUnpacked other = u;
    other.header.model_tag ^= 0x01;
    CHECK(!learned_model_matches(other, codec));
    other.header_ok = false;
    CHECK(learned_model_matches(other, codec));
    other = u;
    CHECK_EQ(u.header.format, kLearnedFrameFormat);
    other.header.format = 0;  // 打散之前的舊排法
    CHECK(!learned_model_matches(other, codec));

    // 實數在幀裡的順序是一個排列
    const std::vector<std::uint16_t>& order = learned_value_order(mode);
    CHECK_EQ(static_cast<int>(order.size()), mode.learned_values);
    std::vector<int> seen(order.size(), 0);
    for (std::uint16_t t : order) ++seen[t];
    CHECK(std::all_of(seen.begin(), seen.end(), [](int c) { return c == 1; }));
    // 打散的目的：一片衰落（這裡是 12 個載波 × 四成的符號，佔一成）抹掉的值，要平均分到畫面的每個位置，
    // 不能集中在某幾個位置上。照原來的順序放的話（模式 11），最慘的位置會少掉 28% 的通道、有些位置完全不受影響。
    {
        const std::vector<CellPos> positions = data_cell_positions(mode.payload_symbols);
        const int first_symbol = mode.payload_symbols / 5, last_symbol = first_symbol + 2 * mode.payload_symbols / 5;
        const double channels = mode.learned_values / 108.0;  // 每個位置有幾個通道：72 或 24
        std::vector<int> lost(9 * 12, 0);
        int total = 0;
        for (int k = 0; k < mode.learned_values; ++k) {
            const CellPos& at = positions[static_cast<std::size_t>(mode.interleaved_position(kHeaderCells + k / 2))];
            const int carrier = at.bin - kFirstBin;
            if (carrier < 30 || carrier >= 42 || at.symbol < first_symbol || at.symbol >= last_symbol) continue;
            ++lost[order[static_cast<std::size_t>(k)] % 108];  // 編碼器的輸出是 [通道][列][行]
            ++total;
        }
        const double mean = static_cast<double>(total) / mode.learned_values;
        CHECK_NEAR(mean, 0.10, 0.02);
        // 每個位置的通道裡各抽一成：和平均的差距在三個標準差多一點之內（72 個通道是 ±0.11，24 個是 ±0.19）
        const double spread = 3.1 * std::sqrt(mean * (1.0 - mean) / channels);
        const double most = *std::max_element(lost.begin(), lost.end()) / channels, least = *std::min_element(lost.begin(), lost.end()) / channels;
        std::printf("  一片衰落抹掉 %.1f%% 的值：各個位置少掉的通道在 %.1f%% 到 %.1f%% 之間\n", 100.0 * mean, 100.0 * least, 100.0 * most);
        CHECK(most < mean + spread);
        CHECK(least > mean - spread);
    }

    // 各格點的雜訊一樣大時，接收端的處理不起作用，結果和直接解碼相同
    std::vector<std::uint8_t> a(static_cast<std::size_t>(kImageRgbBytes)), b(a.size()), c(a.size());
    LearnedRxOptions plain;
    plain.attenuate = false;
    // 雜訊變異數 1e-4 是 40 dB，超過解碼器認得的上限，告訴它的是 30 dB
    CHECK_NEAR(learned_picture(codec, u, LearnedRxOptions{}, a), 30.0, 0.01);
    learned_picture(codec, u, plain, b);
    codec.decode(values, 40.0f, c);
    CHECK(a == b);
    CHECK(a == c);

    // 衰落：一成的格點落在谷底，等化後是很大的亂數（雜訊變異數也照實標出來），其餘的格點是 15 dB
    LearnedUnpacked faded = u;
    Rng rng(5);
    for (std::size_t i = 0; i + 1 < faded.values.size(); i += 2) {
        const float var = rng.uniform() < 0.1 ? 30.0f : 0.0316f;
        for (std::size_t k = i; k < i + 2; ++k) {
            faded.noise_var[k] = var;
            faded.values[k] += static_cast<float>(std::sqrt(var / 2.0) * rng.gauss());
        }
    }
    LearnedRxOptions mean;
    mean.median_snr = false;
    mean.attenuate = false;
    const float told = learned_picture(codec, faded, LearnedRxOptions{}, a);  // 預設：中位數，並把很差的格點壓小
    const float told_mean = learned_picture(codec, faded, mean, c);           // 平均值會被那一成拉得很低
    learned_picture(codec, faded, plain, b);
    CHECK_NEAR(told, 15.0, 0.1);
    CHECK(told_mean < 0.0f);
    const double with = test_images::psnr(a, rgb), without = test_images::psnr(b, rgb);
    std::printf("  一成的格點在衰落谷底：壓小 %.1f dB、不壓小 %.1f dB、用平均訊雜比 %.1f dB\n", with, without, test_images::psnr(c, rgb));
    CHECK(with > without + 5.0);
    CHECK(with > model.faded);
}

// ---------------------------------------------------------------- 完整的收發鏈

// 發射 count 幀（同一張畫面）→ 通道 → 接收機。兩邊的模型各自指定，可以是空的。
std::vector<RxFrame> learned_link(const Mode& mode, std::shared_ptr<const LearnedCodec> tx_model,
                                  std::shared_ptr<const LearnedCodec> rx_model, std::span<const std::uint8_t> rgb, int count,
                                  const sim::ChannelParams& params) {
    Transmitter tx;
    tx.set_callsign("N0CALL");
    tx.set_model(std::move(tx_model));
    std::vector<float> signal;
    for (int i = 0; i < count; ++i) {
        const std::vector<float> frame = tx.learned_frame(mode, rgb);
        signal.insert(signal.end(), frame.begin(), frame.end());
    }
    std::vector<float> audio(3000, 0.0f);
    audio.insert(audio.end(), signal.begin(), signal.end());
    audio.insert(audio.end(), 6000, 0.0f);
    sim::Channel channel(params, link_util::mean_power(signal));
    std::vector<float> received;
    channel.process(audio, received);
    Receiver rx;
    rx.set_model(std::move(rx_model));
    return link_util::run_receiver(rx, received);
}

void test_link(const ModelCase& expected, const std::shared_ptr<const LearnedCodec>& model, const std::vector<std::uint8_t>& file) {
    const Mode& mode = *find_mode(expected.mode_id);
    const auto rgb = test_images::load("cat");

    // 發射端沒有模型：編不出幀，幀序號不動
    {
        Transmitter tx;
        CHECK(tx.learned_frame(mode, rgb).empty());
        CHECK_EQ(tx.sequence(), 0);
    }
    // 預覽是編碼後直接解回來的畫面；輸出不超過滿刻度
    {
        Transmitter tx;
        tx.set_model(model);
        std::vector<std::uint8_t> preview;
        const std::vector<float> frame = tx.learned_frame(mode, rgb, &preview);
        CHECK_EQ(static_cast<int>(frame.size()), mode.frame_samples());
        CHECK_EQ(tx.sequence(), 1);
        CHECK_NEAR(test_images::psnr(preview, rgb), expected.clean[2], 0.02);
        float peak = 0.0f;
        for (float v : frame) peak = std::max(peak, std::fabs(v));
        CHECK(peak <= 1.0f);
    }

    auto mean_psnr = [&](const std::vector<RxFrame>& frames, int* pictures) {
        double total = 0.0;
        *pictures = 0;
        for (const RxFrame& f : frames) {
            if (!f.decoded || f.rgb.size() != rgb.size()) continue;
            total += test_images::psnr(f.rgb, rgb);
            ++*pictures;
        }
        return *pictures > 0 ? total / *pictures : 0.0;
    };
    int pictures = 0;

    // 不加雜訊：每一幀都收到，畫質和不經過數據機時幾乎一樣
    sim::ChannelParams clean;
    clean.add_noise = false;
    std::vector<RxFrame> frames = learned_link(mode, model, model, rgb, 2, clean);
    CHECK_EQ(static_cast<int>(frames.size()), 2);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const RxFrame& f = frames[i];
        CHECK(f.decoded && f.header_ok && !f.model_missing);
        CHECK_EQ(f.mode_id, mode.id);
        CHECK(f.callsign == "N0CALL");
        CHECK_EQ(f.sequence, static_cast<int>(i));
    }
    const double clean_psnr = mean_psnr(frames, &pictures);
    CHECK(clean_psnr > expected.clean[2] - 0.6);

    // 白雜訊 10 dB（3 kHz 雜訊頻寬）
    sim::ChannelParams noisy;
    noisy.snr_db = 10.0;
    noisy.seed = 3;
    frames = learned_link(mode, model, model, rgb, 3, noisy);
    const double noisy_psnr = mean_psnr(frames, &pictures);
    CHECK_EQ(pictures, 3);
    CHECK_NEAR(noisy_psnr, expected.link_awgn10, 0.8);

    // 衰落：差通道（2 ms／1 Hz）、15 dB
    sim::ChannelParams poor;
    CHECK(sim::channel_preset("poor", poor));
    poor.snr_db = 15.0;
    poor.seed = 11;
    frames = learned_link(mode, model, model, rgb, 4, poor);
    const double poor_psnr = mean_psnr(frames, &pictures);
    CHECK(pictures >= 3);
    CHECK(poor_psnr > expected.link_poor15);
    std::printf("  模式 %d 的收發鏈（貓）：不加雜訊 %.1f dB、白雜訊 10 dB 時 %.1f dB、差通道 15 dB 時 %.1f dB（%d 張）\n", mode.id,
                clean_psnr, noisy_psnr, poor_psnr, pictures);

    // 接收端沒有模型：幀收得到、標頭也解得出來，只是沒有畫面
    frames = learned_link(mode, model, nullptr, rgb, 2, clean);
    CHECK_EQ(static_cast<int>(frames.size()), 2);
    for (const RxFrame& f : frames) CHECK(!f.decoded && f.model_missing && f.header_ok && f.rgb.empty() && f.callsign == "N0CALL");

    // 兩邊的權重不同
    const std::shared_ptr<const LearnedCodec> other = altered_model(file, *model);
    CHECK(other != nullptr);
    if (other != nullptr) {
        frames = learned_link(mode, model, other, rgb, 2, clean);
        CHECK_EQ(static_cast<int>(frames.size()), 2);
        for (const RxFrame& f : frames) CHECK(!f.decoded && f.model_missing && f.header_ok && f.rgb.empty());
        // 兩邊都換成另一份，就又解得出來
        frames = learned_link(mode, other, other, rgb, 1, clean);
        CHECK(frames.size() == 1 && frames[0].decoded && !frames[0].model_missing);
    }
}

// 黑白剪影：純白或純黑的底上一個反色的人形（頭、身體、兩片翅膀）。大片純色加上銳利的邊緣，和訓練用的照片很不一樣；
// 接收端在衰落時最容易在這種畫面上畫出不存在的東西。每個像素取 4×4 個點，邊緣才有一圈灰階。
std::vector<std::uint8_t> silhouette(bool white_background) {
    auto inside = [](double x, double y) {
        if ((x - 48) * (x - 48) + (y - 16) * (y - 16) < 64) return true;                              // 頭
        if (y >= 22 && y < 70 && std::fabs(x - 48) < (y - 22) * 14.0 / 48.0) return true;             // 身體
        const double wing = std::fabs(x - 48);                                                         // 翅膀：左右對稱
        return wing > 4 && wing < 38 && y > 22 + wing * 0.1 && y < 44 - wing * 0.25 + 4;
    };
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(kImageRgbBytes));
    for (int y = 0; y < kImageHeight; ++y) {
        for (int x = 0; x < kImageWidth; ++x) {
            int hits = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) hits += inside(x + (sx + 0.5) / 4.0, y + (sy + 0.5) / 4.0) ? 1 : 0;
            const int figure = 255 * hits / 16;
            const std::uint8_t v = static_cast<std::uint8_t>(white_background ? 255 - figure : figure);
            const std::size_t i = static_cast<std::size_t>(3 * (y * kImageWidth + x));
            rgb[i] = rgb[i + 1] = rgb[i + 2] = v;
        }
    }
    return rgb;
}

// 原圖是純色（全白或全黑）的地方，解出來的畫面有幾成跑到另一邊去了（白底上冒出黑塊，或黑底上冒出白塊）
double flipped_fraction(const std::vector<std::uint8_t>& original, const std::vector<std::uint8_t>& decoded) {
    int flat = 0, flipped = 0;
    for (std::size_t i = 0; i < original.size(); i += 3) {
        const int luma = (decoded[i] * 299 + decoded[i + 1] * 587 + decoded[i + 2] * 114) / 1000;
        if (original[i] >= 250) {
            ++flat;
            flipped += luma < 128 ? 1 : 0;
        } else if (original[i] <= 5) {
            ++flat;
            flipped += luma >= 128 ? 1 : 0;
        }
    }
    return flat > 0 ? static_cast<double>(flipped) / flat : 0.0;
}

// 使用者實測時看到的情形：黑白的影片在訊號差、有衰落時，畫面上冒出帶點陣的黑塊。
// 第一版的模型訓練時每個值的雜訊一樣大，沒見過「有一部分的值等於沒收到」；第二版訓練時加入衰落，並把各個值的可靠程度交給解碼器。
void test_silhouette_fading(const ModelCase& expected, const std::shared_ptr<const LearnedCodec>& model) {
    const Mode& mode = *find_mode(expected.mode_id);
    sim::ChannelParams poor;
    CHECK(sim::channel_preset("poor", poor));
    double total = 0.0, worst_flip = 0.0;
    int pictures = 0, sent = 0;
    for (double snr : {0.0, 2.0, 5.0}) {
        for (std::uint64_t seed : {3, 7}) {
            poor.snr_db = snr;
            poor.seed = seed;
            Transmitter tx;
            tx.set_callsign("N0CALL");
            tx.set_model(model);
            std::vector<float> signal;
            for (int i = 0; i < 4; ++i) {
                const std::vector<float> frame = tx.learned_frame(mode, silhouette(i % 2 == 0));
                signal.insert(signal.end(), frame.begin(), frame.end());
            }
            sent += 4;
            std::vector<float> audio(3000, 0.0f);
            audio.insert(audio.end(), signal.begin(), signal.end());
            audio.insert(audio.end(), 6000, 0.0f);
            sim::Channel channel(poor, link_util::mean_power(signal));
            std::vector<float> received;
            channel.process(audio, received);
            Receiver rx;
            rx.set_model(model);
            for (const RxFrame& f : link_util::run_receiver(rx, received)) {
                if (!f.decoded || !f.header_ok) continue;
                const std::vector<std::uint8_t> original = silhouette(f.sequence % 2 == 0);
                total += test_images::psnr(f.rgb, original);
                worst_flip = std::max(worst_flip, flipped_fraction(original, f.rgb));
                ++pictures;
            }
        }
    }
    const double mean = pictures > 0 ? total / pictures : 0.0;
    std::printf("  模式 %d，黑白剪影過差通道（0、2、5 dB）：%d 幀收到 %d 張，平均 %.1f dB；純色的地方最多有 %.1f%% 變成另一色\n", mode.id,
                sent, pictures, mean, 100.0 * worst_flip);
    // 模式 11：第一版的權重（結構 1，同樣的幀）在這裡是平均 25.5 dB、純色的地方最多有 2.9% 變成另一色；第二版是 28.9 dB、0%；
    // 現在的第三版（結構和第二版相同，多訓練 5.2 萬步）是 30.3 dB、0%。
    // 模式 13（加寬的模型）：30.6 dB、0%。
    // 模式 14（每張 0.667 秒）：送的值更少，平均 23.5 dB，最差的一張有 0.6% 變色。
    // 模式 12：送的值只有三分之一，衰落很深的幀（量到的訊雜比 −1 dB 上下）邊緣會糊在一起，最差的一張有 1.3% 變色；
    // 平均 24.1 dB。（同樣的畫面與通道，已經移除的類比模式 8 是平均 15.8 dB、最差的一張 3.6%。）
    CHECK(pictures >= sent * 3 / 4);
    CHECK(mean > expected.silhouette);
    CHECK(worst_flip < expected.flipped);
}

// ---------------------------------------------------------------- 兩個模型同時掛著

// 發射機與接收機可以掛好幾個模型：每個模式用實數個數與寬度都和它相同的那一個，一次發射裡換模式也接得上。
// models、files 依 kCases 的順序（模式 11、模式 12）。
void test_two_models(const std::vector<std::shared_ptr<const LearnedCodec>>& models, const std::vector<std::vector<std::uint8_t>>& files) {
    const auto rgb = test_images::load("cat");
    const Mode& long_mode = *find_mode(kCases[0].mode_id);
    const Mode& short_mode = *find_mode(kCases[1].mode_id);

    // 一組模型：每種「實數個數與寬度」留一個，後來的取代先前的；空指標清掉全部
    {
        LearnedModels set;
        CHECK(set.find(long_mode) == nullptr && set.find(short_mode) == nullptr);
        set.set(models[0]);
        CHECK(set.find(long_mode) == models[0].get() && set.find(short_mode) == nullptr);
        set.set(models[1]);
        CHECK(set.find(long_mode) == models[0].get() && set.find(short_mode) == models[1].get());
        const std::shared_ptr<const LearnedCodec> other = altered_model(files[0], *models[0]);
        CHECK(other != nullptr);
        set.set(other);  // 實數個數與寬度都和第一個相同：取代它，另一個不動
        CHECK(set.find(long_mode) == other.get() && set.find(short_mode) == models[1].get());
        set.set(nullptr);
        CHECK(set.find(long_mode) == nullptr && set.find(short_mode) == nullptr);
    }

    // 一次發射裡兩個模式交替，幀與幀直接相接
    Transmitter tx;
    tx.set_callsign("N0CALL");
    for (const auto& model : models) tx.set_model(model);
    CHECK(tx.model_for(long_mode) == models[0].get() && tx.model_for(short_mode) == models[1].get());
    const Mode* const sent[] = {&long_mode, &short_mode, &short_mode, &long_mode};
    std::vector<float> signal;
    for (const Mode* mode : sent) {
        const std::vector<float> frame = tx.learned_frame(*mode, rgb);
        CHECK_EQ(static_cast<int>(frame.size()), mode->frame_samples());
        signal.insert(signal.end(), frame.begin(), frame.end());
    }
    std::vector<float> audio(3000, 0.0f);
    audio.insert(audio.end(), signal.begin(), signal.end());
    audio.insert(audio.end(), 6000, 0.0f);
    sim::ChannelParams clean;
    clean.add_noise = false;
    sim::Channel channel(clean, link_util::mean_power(signal));
    std::vector<float> received;
    channel.process(audio, received);
    auto receive = [&](std::initializer_list<std::size_t> which) {
        Receiver rx;
        for (std::size_t i : which) rx.set_model(models[i]);
        return link_util::run_receiver(rx, received);
    };

    // 接收端兩個模型都有：四幀都有畫面，各用各的模型解
    std::vector<RxFrame> frames = receive({0, 1});
    CHECK_EQ(static_cast<int>(frames.size()), 4);
    for (std::size_t i = 0; i < frames.size() && i < std::size(sent); ++i) {
        const RxFrame& f = frames[i];
        const ModelCase& expected = kCases[sent[i] == &long_mode ? 0 : 1];
        CHECK(f.decoded && f.header_ok && !f.model_missing);
        CHECK_EQ(f.mode_id, sent[i]->id);
        CHECK_EQ(f.sequence, static_cast<int>(i));
        if (f.decoded) CHECK(test_images::psnr(f.rgb, rgb) > expected.clean[2] - 0.6);
    }
    // 接收端只有其中一個：另一個模式的幀照樣收得到、標頭也解得出來，只是沒有畫面
    for (std::size_t only = 0; only < 2; ++only) {
        frames = receive({only});
        CHECK_EQ(static_cast<int>(frames.size()), 4);
        for (std::size_t i = 0; i < frames.size() && i < std::size(sent); ++i) {
            const RxFrame& f = frames[i];
            const bool mine = sent[i]->learned_values == models[only]->value_count();
            CHECK(f.header_ok && f.mode_id == sent[i]->id);
            CHECK_EQ(f.decoded, mine);
            CHECK_EQ(f.model_missing, !mine);
        }
    }
    // 發射端只有其中一個：另一個模式編不出幀，幀序號不動；全部拿掉之後都編不出來
    {
        Transmitter one;
        one.set_model(models[1]);
        CHECK(one.learned_frame(long_mode, rgb).empty());
        CHECK_EQ(one.sequence(), 0);
        CHECK(!one.learned_frame(short_mode, rgb).empty());
        CHECK_EQ(one.sequence(), 1);
        one.set_model(nullptr);
        CHECK(one.learned_frame(short_mode, rgb).empty());
        CHECK_EQ(one.sequence(), 1);
    }
}

// 模式 11 與 13 的幀完全相同、實數一樣多，只差在模型的寬度：兩個模型可以同時掛著，各給各的模式用，
// 接收機由前導符號認出是哪個模式，不會拿錯模型。narrow、wide 是模式 11、13 的模型。
void test_two_widths(const std::shared_ptr<const LearnedCodec>& narrow, const std::shared_ptr<const LearnedCodec>& wide) {
    const auto rgb = test_images::load("cat");
    const Mode& mode11 = *find_mode(11);
    const Mode& mode13 = *find_mode(13);
    CHECK_EQ(narrow->value_count(), wide->value_count());
    CHECK(narrow->base_width() != wide->base_width());
    CHECK(learned_model_fits(mode11, *narrow) && !learned_model_fits(mode13, *narrow));
    CHECK(learned_model_fits(mode13, *wide) && !learned_model_fits(mode11, *wide));
    CHECK(learned_encode(mode13, *narrow, FrameHeader{}, rgb).empty());  // 拿錯模型編不出幀
    CHECK(learned_encode(mode11, *wide, FrameHeader{}, rgb).empty());
    {
        LearnedModels set;
        set.set(narrow);
        CHECK(set.find(mode11) == narrow.get() && set.find(mode13) == nullptr);
        set.set(wide);  // 實數個數相同、寬度不同：不取代，兩個都留著
        CHECK(set.find(mode11) == narrow.get() && set.find(mode13) == wide.get());
    }

    Transmitter tx;
    tx.set_callsign("N0CALL");
    tx.set_model(narrow);
    CHECK(tx.learned_frame(mode13, rgb).empty());  // 只有模式 11 的模型：模式 13 編不出幀
    CHECK_EQ(tx.sequence(), 0);
    tx.set_model(wide);
    const Mode* const sent[] = {&mode11, &mode13, &mode11};
    std::vector<float> signal;
    for (const Mode* mode : sent) {
        const std::vector<float> frame = tx.learned_frame(*mode, rgb);
        CHECK_EQ(static_cast<int>(frame.size()), mode->frame_samples());
        signal.insert(signal.end(), frame.begin(), frame.end());
    }
    std::vector<float> audio(3000, 0.0f);
    audio.insert(audio.end(), signal.begin(), signal.end());
    audio.insert(audio.end(), 6000, 0.0f);
    sim::ChannelParams clean;
    clean.add_noise = false;
    sim::Channel channel(clean, link_util::mean_power(signal));
    std::vector<float> received;
    channel.process(audio, received);

    // 接收端兩個模型都有：三幀都有畫面，而且是各自的模型解的（用錯模型解出來的是 10 dB 以下的雜訊）
    Receiver both;
    both.set_model(narrow);
    both.set_model(wide);
    std::vector<RxFrame> frames = link_util::run_receiver(both, received);
    CHECK_EQ(static_cast<int>(frames.size()), 3);
    for (std::size_t i = 0; i < frames.size() && i < std::size(sent); ++i) {
        const RxFrame& f = frames[i];
        CHECK(f.decoded && f.header_ok && !f.model_missing);
        CHECK_EQ(f.mode_id, sent[i]->id);
        if (f.decoded) CHECK(test_images::psnr(f.rgb, rgb) > 28.0);
    }
    // 接收端只有其中一個：另一個模式的幀收得到、標頭也解得出來，只是沒有畫面
    for (const auto& only : {narrow, wide}) {
        Receiver rx;
        rx.set_model(only);
        frames = link_util::run_receiver(rx, received);
        CHECK_EQ(static_cast<int>(frames.size()), 3);
        for (std::size_t i = 0; i < frames.size() && i < std::size(sent); ++i) {
            const bool mine = learned_model_fits(*sent[i], *only);
            CHECK(frames[i].header_ok && frames[i].mode_id == sent[i]->id);
            CHECK_EQ(frames[i].decoded, mine);
            CHECK_EQ(frames[i].model_missing, !mine);
        }
    }
}

}  // namespace

int main() {
    std::vector<std::shared_ptr<const LearnedCodec>> models;
    std::vector<std::vector<std::uint8_t>> files;
    for (const ModelCase& model : kCases) {
        const std::string model_path = std::string(NBTV_MODEL_DIR) + "/" + model.name + ".bin";
        std::vector<std::uint8_t> file = read_file(model_path);
        if (file.empty()) {
            std::printf("  找不到權重檔 %s，略過模式 %d 的測試\n", model_path.c_str(), model.mode_id);
            continue;
        }
        std::printf("  模式 %d（%s）\n", model.mode_id, model.name);
        test_loader(model, file);
        const std::shared_ptr<const LearnedCodec> codec = LearnedCodec::load(file);
        if (codec == nullptr) continue;
        test_against_pytorch(*codec, read_file(std::string(NBTV_TEST_DATA_DIR) + "/" + model.name + "_ref.bin"));
        test_round_trip(model, *codec);
        test_frame(model, *codec);
        test_link(model, codec, file);
        test_silhouette_fading(model, codec);
        models.push_back(codec);
        files.push_back(std::move(file));
    }
    if (models.size() == std::size(kCases)) {  // 依 kCases 的順序：模式 11、12、13
        test_two_models(models, files);
        test_two_widths(models[0], models[2]);
    }
    return check::finish("learned");
}
