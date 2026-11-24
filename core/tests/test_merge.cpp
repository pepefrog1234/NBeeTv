// 接收端合併最近幾幀（learned_merge）：權重檔的解析、C++ 的計算和 PyTorch 算出來的是否一致，
// 以及接到接收機之後，過完整的收發鏈的效果。
// 每個畫面模型配一份權重（models/）與一份參考答案（data/…_ref.bin，由 tools/learned/export_merge.py 產生）。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "bits.hpp"
#include "channel.hpp"
#include "check.hpp"
#include "learned_codec.hpp"
#include "learned_merge.hpp"
#include "link_util.hpp"
#include "model_file.hpp"
#include "modes.hpp"
#include "receiver.hpp"
#include "rng.hpp"
#include "test_images.hpp"
#include "transmitter.hpp"

using namespace nbtv;

namespace {

// 一份合併用的權重。換了權重要一起更新。
struct MergeCase {
    int mode_id;
    const char* name;        // models/<name>.bin 與 data/<name>_ref.bin
    std::uint32_t merge_id;  // 這個權重檔的識別碼
    std::uint32_t model_id;  // 它配的畫面模型的識別碼
    // 以下是 C++ 這邊量到的數字留一點餘裕之後的門檻：完整的收發鏈、白雜訊 3 dB（3 kHz 雜訊頻寬）、貓那一張
    double still_gain;       // 同一張畫面連續送：第 4 幀起，合併比不合併高多少（下限）
    double moving_loss;      // 每一幀換一張畫面：合併比不合併最多低多少（上限）
};
const MergeCase kCases[] = {
    // 量到的：同一張畫面 +2.32 dB，每幀換畫面 +0.73 dB
    {11, "261124-10", 0x0D7460A5u, 0xD6ED61D0u, 2.0, 0.2},
    // 量到的：同一張畫面 +1.87 dB，每幀換畫面 +0.49 dB（三張測試圖下緣的字幕相同，那一塊合得進來）
    {12, "261124-8", 0x1759AED1u, 0xBD805526u, 1.5, 0.2},
    // 量到的：同一張畫面 +2.59 dB，每幀換畫面 +0.72 dB
    {13, "261124-9", 0x2D0E0346u, 0x8D00009Au, 2.2, 0.2},
    // 量到的：同一張畫面 +1.63 dB，每幀換畫面 +0.13 dB
    {14, "261124-7", 0x48037DABu, 0xB60B3C50u, 1.3, 0.2},
};

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

// 改過內容之後把結尾的檢查碼補對，用來測「檢查碼對、內容不對」的情形
void fix_crc(std::vector<std::uint8_t>& file) {
    const std::uint32_t crc = crc32(std::span<const std::uint8_t>(file).first(file.size() - 4));
    for (int i = 0; i < 4; ++i) file[file.size() - 4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(crc >> (8 * i));
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::vector<std::uint8_t> bytes;
    tools::read_file(path, bytes);
    return bytes;
}

// ---------------------------------------------------------------- 權重檔

void test_loader(const MergeCase& expected, const std::vector<std::uint8_t>& file, const std::vector<std::uint8_t>& model_file) {
    std::string error;
    const auto merge = LearnedMerge::load(file, &error);
    CHECK(merge != nullptr);
    CHECK(error.empty());
    if (merge == nullptr) return;
    CHECK_EQ(merge->merge_id(), expected.merge_id);
    CHECK_EQ(merge->model_id(), expected.model_id);
    CHECK_EQ(merge->value_count(), find_mode(expected.mode_id)->learned_values);
    CHECK_EQ(merge->frames(), 4);

    // 不是權重檔
    CHECK(LearnedMerge::load({}, &error) == nullptr);
    CHECK(!error.empty());
    CHECK(LearnedMerge::load({}) == nullptr);  // 不要原因也可以
    const std::vector<std::uint8_t> junk(100, 0x55);
    CHECK(LearnedMerge::load(junk) == nullptr);
    // 畫面模型的權重檔不是合併用的；反過來也一樣
    CHECK(LearnedMerge::load(model_file) == nullptr);
    CHECK(LearnedCodec::load(file) == nullptr);

    // 截斷在各種位置
    for (std::size_t keep : {std::size_t{8}, std::size_t{35}, std::size_t{36}, std::size_t{1000}, file.size() - 5, file.size() - 1}) {
        const std::vector<std::uint8_t> cut(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(keep));
        CHECK(LearnedMerge::load(cut) == nullptr);
    }
    // 截斷之後檢查碼剛好補對：長度和內容對不上，也要拒絕
    {
        std::vector<std::uint8_t> cut(file.begin(), file.begin() + 5000);
        fix_crc(cut);
        CHECK(LearnedMerge::load(cut, &error) == nullptr);
    }
    // 資料中間壞了一個位元
    {
        std::vector<std::uint8_t> bad = file;
        bad[bad.size() / 2] ^= 0x01;
        CHECK(LearnedMerge::load(bad, &error) == nullptr);
    }
    // 以下都把檢查碼補對，看結構上的檢查
    {
        std::vector<std::uint8_t> bad = file;  // 格式版本
        bad[8] = 2;
        fix_crc(bad);
        CHECK(LearnedMerge::load(bad) == nullptr);
    }
    {
        std::vector<std::uint8_t> bad = file;  // 這一版不認得的結構編號
        bad[12] = 2;
        fix_crc(bad);
        CHECK(LearnedMerge::load(bad) == nullptr);
    }
    {
        std::vector<std::uint8_t> bad = file;  // 潛在通道數和張量的形狀對不上
        bad[16] = static_cast<std::uint8_t>(bad[16] + 1);
        fix_crc(bad);
        CHECK(LearnedMerge::load(bad, &error) == nullptr);
        CHECK(error.find("estimator.0.weight") != std::string::npos);
    }
    {
        std::vector<std::uint8_t> bad = file;  // 第一個張量的名稱（位移 34 起）改掉一個字
        bad[34] = 'x';
        fix_crc(bad);
        CHECK(LearnedMerge::load(bad, &error) == nullptr);
        CHECK(error.find("estimator.0.weight") != std::string::npos);
    }
    {
        std::vector<std::uint8_t> bad = file;  // 結尾多出資料
        bad.insert(bad.end() - 4, {1, 2, 3, 4});
        fix_crc(bad);
        CHECK(LearnedMerge::load(bad) == nullptr);
    }
    {
        std::vector<std::uint8_t> other = file;  // 配的是另一個畫面模型（位移 28 起的 4 個位元組）：檔案本身是好的
        other[28] ^= 0x01;
        fix_crc(other);
        const auto loaded = LearnedMerge::load(other);
        CHECK(loaded != nullptr);
        if (loaded != nullptr) CHECK(loaded->model_id() != expected.model_id);
    }
}

// ---------------------------------------------------------------- 和 PyTorch 對照

// 參考答案檔的格式見 tools/learned/export_merge.py。
void test_against_pytorch(const LearnedMerge& merge, const std::vector<std::uint8_t>& ref) {
    CHECK(ref.size() > 28);
    if (ref.size() <= 28) return;
    CHECK(std::memcmp(ref.data(), "NBTVMRF1", 8) == 0);
    CHECK_EQ(le32(ref.data() + 8), merge.merge_id());
    CHECK_EQ(le32(ref.data() + 12), merge.model_id());
    const std::size_t cases = le32(ref.data() + 16), count = le32(ref.data() + 20), frames = le32(ref.data() + 24);
    CHECK_EQ(static_cast<int>(count), merge.value_count());
    CHECK_EQ(static_cast<int>(frames), merge.frames());
    const std::size_t per_case = 4 + (2 * frames + 2) * count * 4;
    CHECK_EQ(ref.size(), 28 + cases * per_case);
    if (static_cast<int>(count) != merge.value_count() || static_cast<int>(frames) != merge.frames() || ref.size() != 28 + cases * per_case)
        return;

    double worst_value = 0.0, worst_var = 0.0;
    for (std::size_t n = 0; n < cases; ++n) {
        const std::uint8_t* p = ref.data() + 28 + n * per_case;
        const std::uint32_t mask = le32(p);
        std::vector<float> values(frames * count), noise(frames * count), want(count), want_var(count), got(count), got_var(count);
        std::memcpy(values.data(), p + 4, frames * count * 4);
        std::memcpy(noise.data(), p + 4 + frames * count * 4, frames * count * 4);
        std::memcpy(want.data(), p + 4 + 2 * frames * count * 4, count * 4);
        std::memcpy(want_var.data(), p + 4 + (2 * frames + 1) * count * 4, count * 4);
        std::vector<LearnedMerge::Earlier> earlier(frames - 1);
        for (std::size_t j = 1; j < frames; ++j) {
            if ((mask & (1u << (j - 1))) == 0) continue;
            earlier[j - 1].values = std::span<const float>(values.data() + j * count, count);
            earlier[j - 1].noise_var = std::span<const float>(noise.data() + j * count, count);
        }
        CHECK(merge.merge({values.data(), count}, {noise.data(), count}, earlier, got, got_var));
        for (std::size_t i = 0; i < count; ++i) {
            worst_value = std::max(worst_value, static_cast<double>(std::fabs(got[i] - want[i])));
            worst_var = std::max(worst_var, std::fabs(static_cast<double>(got_var[i]) / static_cast<double>(want_var[i]) - 1.0));
        }
    }
    // 兩邊都是單精度，只差在運算的順序；量到的差距在 1e-6 上下
    CHECK(worst_value < 1e-4);
    CHECK(worst_var < 1e-4);
    std::printf("  和 PyTorch 對照 %zu 個情形：合併後的值最多差 %.1e，雜訊變異數最多差 %.1e（相對）\n", cases, worst_value, worst_var);
}

// ---------------------------------------------------------------- 邊界情形

void test_edges(const LearnedMerge& merge) {
    const std::size_t n = static_cast<std::size_t>(merge.value_count());
    const std::size_t slots = static_cast<std::size_t>(merge.frames() - 1);
    std::vector<float> values(n), noise(n, 0.1f), out(n), out_var(n);
    for (std::size_t i = 0; i < n; ++i) values[i] = 0.7f * std::sin(0.37f * static_cast<float>(i));

    // 前面沒有幀：結果就是這一幀原來的值
    CHECK(merge.merge(values, noise, {}, out, out_var));
    CHECK(out == values && out_var == noise);
    std::vector<LearnedMerge::Earlier> none(slots);
    CHECK(merge.merge(values, noise, none, out, out_var));
    CHECK(out == values && out_var == noise);

    // 前面每一幀都和這一幀一模一樣：不管閘開多少，加權平均出來的值不變，回報的雜訊變異數不會變大。
    // （實際上不會有這種輸入：聲稱有雜訊，幾幀卻一模一樣。閘開多少由下一段合理的輸入來看。）
    std::vector<LearnedMerge::Earlier> same(slots);
    for (auto& e : same) e = {values, noise};
    CHECK(merge.merge(values, noise, same, out, out_var));
    double worst = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        worst = std::max(worst, static_cast<double>(std::fabs(out[i] - values[i])));
        CHECK(out_var[i] <= noise[i] * 1.0001f);
    }
    CHECK(worst < 1e-5);

    // 同一組值、各幀各自加上聲稱的那麼大的雜訊（每個實數的變異數是 σ²/2）：畫面沒變，前面幾幀應該大多合進來，
    // 合併後的值比這一幀原來的接近沒有雜訊的值，回報的雜訊變異數也變小
    {
        Rng rng(7);
        std::vector<std::vector<float>> noisy(slots + 1, std::vector<float>(n));
        for (auto& frame : noisy)
            for (std::size_t i = 0; i < n; ++i) frame[i] = values[i] + static_cast<float>(rng.gauss() * std::sqrt(0.05));
        std::vector<LearnedMerge::Earlier> earlier(slots);
        for (std::size_t j = 0; j < slots; ++j) earlier[j] = {noisy[j + 1], noise};
        std::vector<float> merged(n), merged_var(n);
        CHECK(merge.merge(noisy[0], noise, earlier, merged, merged_var));
        double error_before = 0.0, error_after = 0.0, total_before = 0.0, total_after = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            error_before += static_cast<double>(noisy[0][i] - values[i]) * (noisy[0][i] - values[i]);
            error_after += static_cast<double>(merged[i] - values[i]) * (merged[i] - values[i]);
            total_before += noise[i];
            total_after += merged_var[i];
        }
        CHECK(error_after < 0.5 * error_before);
        CHECK(total_after < 0.5 * total_before);
        std::printf("  同一組值、各幀各自的雜訊（10 dB）：合併後的誤差剩 %.0f%%，回報的雜訊變異數剩 %.0f%%\n",
                    100.0 * error_after / error_before, 100.0 * total_after / total_before);
    }

    // 給得比需要的多：多的不看
    std::vector<LearnedMerge::Earlier> more(slots + 2);
    for (auto& e : more) e = {values, noise};
    std::vector<float> out2(n), out2_var(n);
    CHECK(merge.merge(values, noise, more, out2, out2_var));
    CHECK(out2 == out && out2_var == out_var);

    // 前面的幀裡有非數值、或雜訊變異數不是正的：那一幀當作沒收到
    std::vector<float> broken = values, zero_noise = noise;
    broken[n / 2] = std::numeric_limits<float>::quiet_NaN();
    zero_noise[3] = 0.0f;
    std::vector<LearnedMerge::Earlier> bad(slots);
    bad[0] = {broken, noise};
    if (slots > 1) bad[1] = {values, zero_noise};
    CHECK(merge.merge(values, noise, bad, out2, out2_var));
    CHECK(out2 == values && out2_var == noise);
    // 這一幀本身有非數值：不合併，原樣交回去
    CHECK(merge.merge(broken, noise, same, out2, out2_var));
    CHECK(std::isnan(out2[n / 2]) && out2[0] == broken[0] && out2_var == noise);

    // 長度不對
    std::vector<float> small(n - 1);
    CHECK(!merge.merge(small, noise, same, out, out_var));
    CHECK(!merge.merge(values, small, same, out, out_var));
    CHECK(!merge.merge(values, noise, same, small, out_var));
    CHECK(!merge.merge(values, noise, same, out, small));
    // 前面某一幀的長度不對：那一幀當作沒收到
    std::vector<LearnedMerge::Earlier> short_one(slots);
    short_one[0] = {small, small};
    CHECK(merge.merge(values, noise, short_one, out2, out2_var));
    CHECK(out2 == values && out2_var == noise);
}

// ---------------------------------------------------------------- 完整的收發鏈

struct Received {
    std::vector<float> audio;  // 過完通道的音訊
};

// 發射一串畫面（每幀一張）→ 通道
Received transmit(const Mode& mode, const std::shared_ptr<const LearnedCodec>& model, const std::vector<std::vector<std::uint8_t>>& pictures,
                  const sim::ChannelParams& params) {
    Transmitter tx;
    tx.set_callsign("N0CALL");
    tx.set_model(model);
    std::vector<float> signal;
    for (const auto& rgb : pictures) {
        const std::vector<float> frame = tx.learned_frame(mode, rgb);
        signal.insert(signal.end(), frame.begin(), frame.end());
    }
    std::vector<float> audio(3000, 0.0f);
    audio.insert(audio.end(), signal.begin(), signal.end());
    audio.insert(audio.end(), 6000, 0.0f);
    sim::Channel channel(params, link_util::mean_power(signal));
    Received out;
    channel.process(audio, out.audio);
    return out;
}

std::vector<RxFrame> receive(const Received& received, const std::shared_ptr<const LearnedCodec>& model,
                             const std::shared_ptr<const LearnedMerge>& merge, bool merging) {
    Receiver rx;
    rx.set_model(model);
    rx.set_merge(merge);
    rx.tuning().learned_merge = merging;
    return link_util::run_receiver(rx, received.audio);
}

// 每一幀對它那一張原圖的 PSNR 的平均（從第 first 幀算起）；幀數和畫面數不同時回傳 0
double mean_psnr(const std::vector<RxFrame>& frames, const std::vector<std::vector<std::uint8_t>>& pictures, std::size_t first = 0) {
    if (frames.size() != pictures.size() || first >= frames.size()) return 0.0;
    double total = 0.0;
    for (std::size_t i = first; i < frames.size(); ++i) {
        if (!frames[i].decoded) return 0.0;
        total += test_images::psnr(frames[i].rgb, pictures[i]);
    }
    return total / static_cast<double>(frames.size() - first);
}

bool same_pictures(const std::vector<RxFrame>& a, const std::vector<RxFrame>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].decoded != b[i].decoded || a[i].rgb != b[i].rgb) return false;
    return true;
}

void test_link(const MergeCase& expected, const std::shared_ptr<const LearnedCodec>& model, const std::shared_ptr<const LearnedMerge>& merge,
               const std::vector<std::uint8_t>& merge_file) {
    const Mode& mode = *find_mode(expected.mode_id);
    const std::vector<std::uint8_t> cat = test_images::load("cat"), coffee = test_images::load("coffee"), rocket = test_images::load("rocket");
    sim::ChannelParams weak;
    weak.snr_db = 3.0;
    weak.seed = 5;

    // 同一張畫面連續 7 幀，訊號很弱：第一幀前面沒有東西可以合，和不合併一模一樣；之後愈來愈清楚
    const std::vector<std::vector<std::uint8_t>> still(7, cat);
    const Received still_audio = transmit(mode, model, still, weak);
    const std::vector<RxFrame> plain = receive(still_audio, model, nullptr, false);
    const std::vector<RxFrame> merged = receive(still_audio, model, merge, true);
    CHECK_EQ(plain.size(), still.size());
    CHECK_EQ(merged.size(), still.size());
    if (plain.size() != still.size() || merged.size() != still.size()) return;
    CHECK(plain[0].decoded && merged[0].decoded && plain[0].rgb == merged[0].rgb);
    const double still_plain = mean_psnr(plain, still, 3), still_merged = mean_psnr(merged, still, 3);
    CHECK(still_plain > 0.0);
    CHECK(still_merged - still_plain > expected.still_gain);
    for (std::size_t i = 0; i < merged.size(); ++i) {  // 合併只改畫面，其他回報的東西不變
        CHECK_EQ(merged[i].sequence, plain[i].sequence);
        CHECK(merged[i].header_ok == plain[i].header_ok && merged[i].start_sample == plain[i].start_sample);
        CHECK_NEAR(merged[i].snr_db, plain[i].snr_db, 1e-6);
    }

    // 沒有打開、沒有設定權重、或設定的是配另一個畫面模型的權重：和不合併一模一樣
    CHECK(same_pictures(receive(still_audio, model, merge, false), plain));
    CHECK(same_pictures(receive(still_audio, model, nullptr, true), plain));
    {
        std::vector<std::uint8_t> other = merge_file;
        other[28] ^= 0x01;
        fix_crc(other);
        const std::shared_ptr<const LearnedMerge> foreign = LearnedMerge::load(other);
        CHECK(foreign != nullptr);
        CHECK(same_pictures(receive(still_audio, model, foreign, true), plain));
    }

    // 重設之後不記得前面的幀：同一段音訊再收一次，結果和第一次一模一樣（第一幀又是沒有東西可以合的）
    {
        Receiver rx;
        rx.set_model(model);
        rx.set_merge(merge);
        rx.tuning().learned_merge = true;
        const std::vector<RxFrame> first = link_util::run_receiver(rx, still_audio.audio);
        rx.reset();
        const std::vector<RxFrame> second = link_util::run_receiver(rx, still_audio.audio);
        CHECK(same_pictures(first, merged));
        CHECK(same_pictures(second, merged));
        // 中途關掉：之後的幀和不合併一樣
        rx.reset();
        rx.tuning().learned_merge = false;
        CHECK(same_pictures(link_util::run_receiver(rx, still_audio.audio), plain));
    }

    // 每一幀換一張畫面：合併幫不上忙，但也不能幫倒忙
    const std::vector<std::vector<std::uint8_t>> moving = {cat, coffee, rocket, cat, coffee, rocket, cat};
    const Received moving_audio = transmit(mode, model, moving, weak);
    const std::vector<RxFrame> moving_plain_frames = receive(moving_audio, model, nullptr, false);
    const double moving_plain = mean_psnr(moving_plain_frames, moving);
    const double moving_merged = mean_psnr(receive(moving_audio, model, merge, true), moving);
    CHECK(moving_plain > 0.0);
    CHECK(moving_merged > moving_plain - expected.moving_loss);

    // 訊號很好：合不合併幾乎一樣
    sim::ChannelParams clean;
    clean.add_noise = false;
    const std::vector<std::vector<std::uint8_t>> few(4, cat);
    const Received clean_audio = transmit(mode, model, few, clean);
    const double clean_plain = mean_psnr(receive(clean_audio, model, nullptr, false), few);
    const double clean_merged = mean_psnr(receive(clean_audio, model, merge, true), few);
    CHECK(clean_plain > 0.0);
    CHECK_NEAR(clean_merged, clean_plain, 0.15);

    // 差通道（2 ms／1 Hz）、8 dB：衰落吃掉的值可以由前面幾幀補回來
    sim::ChannelParams poor;
    CHECK(sim::channel_preset("poor", poor));
    poor.snr_db = 8.0;
    poor.seed = 11;
    const std::vector<std::vector<std::uint8_t>> many(9, cat);
    const Received poor_audio = transmit(mode, model, many, poor);
    const std::vector<RxFrame> poor_plain_frames = receive(poor_audio, model, nullptr, false);
    const std::vector<RxFrame> poor_merged_frames = receive(poor_audio, model, merge, true);
    double poor_plain = 0.0, poor_merged = 0.0;
    int poor_pictures = 0;
    CHECK_EQ(poor_plain_frames.size(), poor_merged_frames.size());
    for (std::size_t i = 0; i < poor_plain_frames.size() && i < poor_merged_frames.size(); ++i) {
        if (!poor_plain_frames[i].decoded || !poor_merged_frames[i].decoded) continue;
        poor_plain += test_images::psnr(poor_plain_frames[i].rgb, cat);
        poor_merged += test_images::psnr(poor_merged_frames[i].rgb, cat);
        ++poor_pictures;
    }
    CHECK(poor_pictures >= 6);
    if (poor_pictures > 0) {
        poor_plain /= poor_pictures;
        poor_merged /= poor_pictures;
    }
    CHECK(poor_merged > poor_plain);
    std::printf("  模式 %d 的收發鏈（貓，合併／不合併）：白雜訊 3 dB 同一張畫面 %.2f／%.2f dB（%+.2f）、每幀換畫面 %.2f／%.2f dB（%+.2f）、"
                "不加雜訊 %.2f／%.2f dB、差通道 8 dB %.2f／%.2f dB（%+.2f，%d 張）\n",
                mode.id, still_merged, still_plain, still_merged - still_plain, moving_merged, moving_plain, moving_merged - moving_plain,
                clean_merged, clean_plain, poor_merged, poor_plain, poor_merged - poor_plain, poor_pictures);
}

}  // namespace

int main() {
    for (const MergeCase& c : kCases) {
        const std::string merge_path = std::string(NBTV_MODEL_DIR) + "/" + c.name + ".bin";
        const std::vector<std::uint8_t> file = read_file(merge_path);
        const std::vector<std::uint8_t> model_file = read_file(tools::default_model_path(c.mode_id));
        if (file.empty() || model_file.empty()) {
            std::printf("  找不到權重檔 %s 或模式 %d 的模型，略過\n", merge_path.c_str(), c.mode_id);
            continue;
        }
        std::printf("  模式 %d（%s）\n", c.mode_id, c.name);
        test_loader(c, file, model_file);
        const std::shared_ptr<const LearnedMerge> merge = LearnedMerge::load(file);
        const std::shared_ptr<const LearnedCodec> model = LearnedCodec::load(model_file);
        if (merge == nullptr || model == nullptr) continue;
        CHECK_EQ(merge->model_id(), model->model_id());
        test_against_pytorch(*merge, read_file(std::string(NBTV_TEST_DATA_DIR) + "/" + c.name + "_ref.bin"));
        test_edges(*merge);
        test_link(c, model, merge, file);
    }
    return check::finish("merge");
}
