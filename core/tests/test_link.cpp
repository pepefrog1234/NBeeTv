// 端對端：發射 → 通道模擬 → 接收機（同步、通道估測、解碼）。
// 這裡看的是接收機本身：捕獲、頻偏與定時、追蹤、兩次發射之間的銜接。各個模式的畫質在 test_learned.cpp。
#include <algorithm>
#include <cstdio>

#include "check.hpp"
#include "link_util.hpp"
#include "ofdm_tx.hpp"
#include "rng.hpp"
#include "test_images.hpp"

using namespace nbtv;
using namespace link_util;

namespace {

// 乾淨的通道：每個模式連續三幀都要收到，頻偏與訊雜比的讀數合理
void test_clean() {
    const auto rgb = test_images::load("coffee");
    for (const Mode& m : kModes) {
        sim::ChannelParams p;
        p.add_noise = false;
        const LinkResult r = run_link(m, 3, rgb, p);
        CHECK_EQ(r.decoded, 3);
        CHECK_EQ(static_cast<int>(r.frames.size()), 3);
        for (std::size_t i = 0; i < r.frames.size(); ++i) {
            const RxFrame& f = r.frames[i];
            CHECK_EQ(f.mode_id, m.id);
            CHECK_EQ(f.sequence, static_cast<int>(i));
            CHECK(!f.model_missing);
            CHECK_NEAR(f.freq_offset_hz, 0.0, 0.3);
            CHECK(f.snr_db > 18.0);  // 只剩削波失真
        }
        CHECK(!r.final_status.synced);  // 訊號結束後回到搜尋
        std::printf("  模式 %d 乾淨通道：%d/3，量到的訊雜比 %.1f dB\n", m.id, r.decoded, r.frames.empty() ? 0.0 : r.frames[0].snr_db);
    }
}

// 白雜訊：每一幀都要收到（標頭也要解得出來），量到的訊雜比要接近設定值
void test_awgn() {
    const auto rgb = test_images::load("rocket");
    struct Point {
        int mode;
        double snr_db;
    };
    for (const Point& pt : {Point{11, 2.5}, Point{12, 6.5}, Point{13, 12.5}, Point{14, 2.5}, Point{14, 16.5}}) {
        const Mode& m = *find_mode(pt.mode);
        sim::ChannelParams p;
        p.snr_db = pt.snr_db;
        p.seed = 7 + static_cast<std::uint64_t>(pt.mode);
        const LinkResult r = run_link(m, 3, rgb, p, 5000);
        CHECK_EQ(r.decoded, 3);
        double snr = 0;
        for (const RxFrame& f : r.frames) snr += f.snr_db / static_cast<double>(r.frames.size());
        CHECK_NEAR(snr, pt.snr_db, 1.0);
        std::printf("  模式 %d AWGN %.1f dB：%d/3，量到 %.1f dB\n", pt.mode, pt.snr_db, r.decoded, snr);
    }
}

// 頻率偏移 ±150 Hz 內都要能捕獲，讀數要對
void test_frequency_offset() {
    const auto rgb = test_images::load("cat");
    const Mode& m = *find_mode(12);
    for (double offset : {-150.0, -93.75, -47.0, -20.0, 0.4, 46.0, 100.0, 150.0}) {
        sim::ChannelParams p;
        p.snr_db = 12.0;
        p.freq_offset_hz = offset;
        p.seed = 31;
        const LinkResult r = run_link(m, 2, rgb, p);
        CHECK_EQ(r.decoded, 2);
        if (!r.frames.empty()) CHECK_NEAR(r.frames[0].freq_offset_hz, offset, 0.5);
    }
}

// 取樣時脈誤差 ±100 ppm：最長的幀（2.72 秒）裡定時會漂 3 個取樣
void test_clock_offset() {
    const auto rgb = test_images::load("cat");
    for (double ppm : {-100.0, 100.0}) {
        for (int mode : {11, 14}) {
            const Mode& m = *find_mode(mode);
            sim::ChannelParams p;
            p.snr_db = mode == 11 ? 4.0 : 20.0;
            p.clock_ppm = ppm;
            p.seed = 41;
            const LinkResult r = run_link(m, 3, rgb, p);
            CHECK_EQ(r.decoded, 3);
        }
    }
}

// 只有雜訊：不能交出任何幀，也不能一直停在「已同步」
void test_noise_only() {
    Rng rng(55);
    std::vector<float> noise(12000 * 40);
    for (float& v : noise) v = static_cast<float>(0.1 * rng.gauss());
    Receiver rx;
    attach_models(rx);
    int synced_chunks = 0, chunks = 0;
    for (std::size_t pos = 0; pos < noise.size(); pos += 1200) {
        rx.push(std::span<const float>(noise).subspan(pos, 1200));
        synced_chunks += rx.status().synced ? 1 : 0;
        ++chunks;
    }
    RxFrame f;
    CHECK(!rx.poll(f));
    CHECK(synced_chunks < chunks / 10);
    CHECK_NEAR(rx.status().input_level_db, -20.0, 0.5);
    std::printf("  純雜訊 40 秒：%d/%d 個時段誤判為同步\n", synced_chunks, chunks);
}

// 衰落通道（2 ms／1 Hz），訊雜比充裕時應該幾乎都收得到
void test_fading() {
    const auto rgb = test_images::load("cat");
    const Mode& m = *find_mode(11);
    sim::ChannelParams p;
    sim::channel_preset("poor", p);
    p.snr_db = 14.0;
    p.seed = 77;
    const LinkResult r = run_link(m, 6, rgb, p);
    CHECK(r.decoded >= 5);
    std::printf("  模式 11 差通道 14 dB：%d/6\n", r.decoded);
}

// 弱訊號：0 dB、頻偏 130 Hz 下仍要從第一幀就鎖定（S0 的頻偏與定時估計在這裡都很吵，
// 靠 S1 與導頻的驗證把它們修正回來）
void test_weak_signal() {
    const auto rgb = test_images::load("rocket");
    const Mode& m = *find_mode(13);
    sim::ChannelParams p;
    p.snr_db = 0.0;
    p.freq_offset_hz = 130.0;
    int pictures = 0, headers = 0;
    for (std::uint64_t seed : {4u, 38u, 44u}) {
        p.seed = seed;
        const LinkResult r = run_link(m, 2, rgb, p, 3000 + static_cast<int>(seed) * 37);
        headers += r.decoded;
        for (const RxFrame& f : r.frames) {
            pictures += f.decoded ? 1 : 0;
            CHECK_NEAR(f.freq_offset_hz, 130.0, 1.0);
        }
    }
    CHECK_EQ(pictures, 6);
    CHECK(headers >= 5);
    std::printf("  模式 13、AWGN 0 dB、頻偏 130 Hz：%d/6 張有畫面，其中 %d 張標頭正確\n", pictures, headers);
}

// 脈衝雜訊（靜電突波）：每秒 2 次、每次 20 ms、比訊號強 20 dB。被打到的符號要靠逐符號的雜訊估計降低權重，
// 否則那幾個符號上的值會當成和其他的一樣可靠，畫面就被它們帶壞。
void test_impulse_noise() {
    const auto rgb = test_images::load("cat");
    const Mode& m = *find_mode(11);
    sim::ChannelParams p;
    p.snr_db = 14.0;
    p.impulse_rate = 2.0;
    p.impulse_db = 20.0;
    p.impulse_ms = 20.0;
    int with_map = 0, flat = 0;
    double psnr_map = 0.0, psnr_flat = 0.0;
    for (std::uint64_t seed : {3u, 5u}) {
        p.seed = seed;
        const LinkResult a = run_link(m, 2, rgb, p, 4000);
        const LinkResult b = run_link(m, 2, rgb, p, 4000, 6000, false);
        with_map += a.decoded;
        flat += b.decoded;
        psnr_map += mean_psnr(a.frames, rgb) / 2.0;
        psnr_flat += mean_psnr(b.frames, rgb) / 2.0;
    }
    CHECK_EQ(with_map, 4);
    CHECK(psnr_map > psnr_flat + 1.0);
    std::printf("  模式 11、14 dB 加脈衝雜訊：逐符號雜訊估計 %d/4、%.1f dB，單一雜訊值 %d/4、%.1f dB\n", with_map, psnr_map, flat, psnr_flat);
}

// 幾乎沒有細節的畫面：全黑、全白、純色，或是鏡頭被蓋住時那種只有一點雜訊的黑畫面。
// 不論畫面內容是什麼，每一幀都要收到、標頭要對、畫面要還原。
void test_flat_pictures() {
    auto flat = [](int r, int g, int b) {
        std::vector<std::uint8_t> rgb(kImageRgbBytes);
        for (std::size_t i = 0; i < rgb.size(); i += 3) {
            rgb[i] = static_cast<std::uint8_t>(r);
            rgb[i + 1] = static_cast<std::uint8_t>(g);
            rgb[i + 2] = static_cast<std::uint8_t>(b);
        }
        return rgb;
    };
    struct Picture {
        const char* name;
        std::vector<std::uint8_t> rgb;
    };
    std::vector<Picture> pictures = {{"全黑", flat(0, 0, 0)}, {"全白", flat(255, 255, 255)}, {"中灰", flat(128, 128, 128)}, {"純紅", flat(200, 30, 30)}};
    Rng rng(4242);
    std::vector<std::uint8_t> covered(kImageRgbBytes);
    for (auto& v : covered) v = static_cast<std::uint8_t>(rng.below(3));
    pictures.push_back({"蓋住的鏡頭", covered});

    int runs = 0, bad = 0;
    double worst_psnr = 99.0;
    for (int id : {11, 12, 14}) {
        const Mode& m = *find_mode(id);
        for (const Picture& pic : pictures) {
            for (double snr : {99.0, 40.0, 12.0}) {  // 99：不加雜訊
                sim::ChannelParams p;
                p.add_noise = snr < 90.0;
                p.snr_db = snr;
                p.seed = static_cast<std::uint64_t>(500 + id);
                const LinkResult r = run_link(m, 2, pic.rgb, p);
                double psnr = 99.0;
                for (const RxFrame& f : r.frames) psnr = std::min(psnr, f.decoded ? test_images::psnr(f.rgb, pic.rgb) : 0.0);
                const bool ok = r.frames.size() == 2 && r.decoded == 2 && psnr > (snr < 20.0 ? 24.0 : 28.0);
                ++runs;
                CHECK(ok);
                if (ok) {
                    worst_psnr = std::min(worst_psnr, psnr);
                } else {
                    ++bad;
                    std::printf("  模式 %d 送「%s」（%s）：收到 %zu 幀、標頭正確 %d 幀（應為 2），PSNR %.1f dB\n", id, pic.name,
                                snr < 90.0 ? (snr > 20.0 ? "40 dB" : "12 dB") : "無雜訊", r.frames.size(), r.decoded, psnr);
                }
            }
        }
    }
    std::printf("  沒有細節的畫面：%d 種情形，%d 種有問題（通過的裡面最低的 PSNR %.1f dB）\n", runs, bad, worst_psnr);
}

// 一次發射結束後，隔一小段時間又有另一次發射（換人發射、停了再開始）。不論空檔多長，兩次發射的每一幀
// 都要收到，也不能多出不存在的畫面。這裡的空檔長度都是曾經出過問題的：
//   - 接收機以為對方還在送、硬收一幀的那段時間裡，新的發射開始了（0.1–1 秒）
//   - 新的前導符號離原本預期的位置只差幾十到一百多個取樣（S0 每 128 點重複，位置差 128 點時分不出來）
//   - 完全的靜音（取樣全是 0）讓滑動相關的分母變成 0
void test_back_to_back() {
    struct Case {
        int mode_a, frames_a, mode_b, frames_b;
        double snr_db;
        std::uint64_t seed;
        std::vector<int> gaps;
    };
    const Case cases[] = {
        // 短的幀接長的幀
        {12, 1, 11, 2, 14.0, 808, {0, 66, 80, 120, 156, 200, 400, 800, 1160, 1880, 2160, 2840, 6560, 7720, 8760, 9000, 9520, 9600, 9640, 9720,
                                   10520, 12480, 13520}},
        // 同一個模式
        {11, 2, 11, 2, 14.0, 803, {0, 66, 160, 320, 640, 4859, 9600, 20000, 32640}},
        // 幀完全相同、只差在 S1 序列的兩個模式
        {11, 1, 13, 2, 14.0, 809, {10509, 15481}},
        // 沒有雜訊：兩次發射之間是完全的靜音
        {14, 1, 14, 2, 99.0, 805, {66, 69, 153, 156, 162, 2000, 8621}},
        {12, 2, 12, 2, 99.0, 808, {120, 8621, 8695}},
        // 訊雜比 6 dB
        {12, 3, 12, 3, 6.0, 24, {12587}},
    };
    int runs = 0, bad = 0;
    for (const Case& c : cases) {
        const Mode& ma = *find_mode(c.mode_a);
        const Mode& mb = *find_mode(c.mode_b);
        for (int gap : c.gaps) {
            const TwoBursts r = run_two_bursts(ma, c.frames_a, mb, c.frames_b, gap, 0, c.snr_db, c.seed);
            const bool ok = r.first == c.frames_a && r.second == c.frames_b && r.bogus == 0 && r.failed == 0;
            CHECK(ok);
            ++runs;
            if (!ok) {
                ++bad;
                std::printf("  模式 %d×%d → 空檔 %d 個取樣 → 模式 %d×%d：收到 %d + %d 張，多出 %d 張，回報失敗 %d 次\n", c.mode_a, c.frames_a, gap,
                            c.mode_b, c.frames_b, r.first, r.second, r.bogus, r.failed);
            }
        }
    }
    std::printf("  兩次發射緊接著來：%d 種空檔長度，%d 種有問題\n", runs, bad);
}

// 第一次發射送到一半就停（最後一幀只送出前面一部分），接著另一次發射開始。
// 那個不完整的幀可能解得出來（標頭有重複與迴旋碼保護，禁得起少一截），也可能回報成沒解出來；
// 但第二次發射的每一幀都要收到，而且不能把不完整的幀或別的東西當成畫面交出來。
void test_interrupted_transmission() {
    struct Case {
        int mode_a, mode_b, cut;
        double snr_db;
        std::vector<int> gaps;
    };
    const Case cases[] = {
        {12, 11, 5000, 14.0, {300, 5550, 6660, 6808, 6919, 7548}},  // 模式 12 只送了四成
        {11, 13, 20000, 14.0, {300, 5000, 11865, 12091}},  // 模式 11 送了六成，第二次發射在它「該結束」之前就開始
        {13, 11, 9000, 14.0, {300, 4859, 12000}},          // 模式 13 只送了不到三成
        {14, 14, 3000, 8.0, {300, 2310, 4620}},            // 模式 14 送了不到四成
    };
    int runs = 0, bad = 0;
    for (const Case& c : cases) {
        const Mode& ma = *find_mode(c.mode_a);
        const Mode& mb = *find_mode(c.mode_b);
        for (int gap : c.gaps) {
            const TwoBursts r = run_two_bursts(ma, 2, mb, 2, gap, c.cut, c.snr_db, 900 + static_cast<std::uint64_t>(c.mode_a));
            const bool ok = r.first >= 1 && r.first <= 2 && r.second == 2 && r.bogus == 0 && r.failed <= 1 && r.first + r.failed <= 2;
            CHECK(ok);
            ++runs;
            if (!ok) {
                ++bad;
                std::printf("  模式 %d（最後一幀只送 %d 個取樣）→ 空檔 %d → 模式 %d：收到 %d + %d 張，多出 %d 張，回報失敗 %d 次\n", c.mode_a, c.cut,
                            gap, c.mode_b, r.first, r.second, r.bogus, r.failed);
            }
        }
    }
    std::printf("  發射中途停止後接著另一次發射：%d 種情形，%d 種有問題\n", runs, bad);
}

// 別的模式的幀：S1 用的是模式表裡沒有的編號（0–5、8–10 是已經移除的數位與類比模式用過的，6、7、15 沒有用過）。
// S0 與導頻和我們的一模一樣，只有 S1 不同。接收機要認得出那不是自己的模式：不同步、不回報任何幀，
// 也不能因此漏掉緊接著來的、自己認得的發射。
void test_foreign_modes() {
    // 一次「別的模式」的發射：幀長照 payload_symbols，資料格一半的幀放 QPSK（像數位模式）、一半放高斯值（像類比模式）
    auto foreign_burst = [](int id, int payload_symbols, int frames, Rng& rng) {
        const Mode fake{id, payload_symbols, 1, 2, 1};  // 只借用幀的算術與 S1 的編號；後三個欄位調變時用不到
        std::vector<float> signal;
        for (int i = 0; i < frames; ++i) {
            std::vector<cf> cells(static_cast<std::size_t>(fake.data_cells()));
            for (cf& c : cells) {
                c = i % 2 == 0 ? cf(rng.bit() ? -0.70710678f : 0.70710678f, rng.bit() ? -0.70710678f : 0.70710678f) : cf(rng.cgauss());
            }
            const auto frame = ofdm_modulate(fake, cells, kDefaultPaprDb);
            signal.insert(signal.end(), frame.begin(), frame.end());
        }
        return signal;
    };
    const Mode& ours = *find_mode(12);
    const std::vector<float>& own = burst(ours, 2, "N0CALL");
    int runs = 0, bad = 0;
    for (int id : {0, 4, 7, 9, 15}) {
        CHECK(find_mode(id) == nullptr);
        for (int payload_symbols : {100, 37}) {
            for (double snr : {99.0, 12.0, 3.0}) {  // 99：不加雜訊
                for (bool followed : {false, true}) {  // 後面有沒有緊接著我們認得的發射
                    Rng rng(static_cast<std::uint64_t>(1000 + id * 10 + payload_symbols));
                    const std::vector<float> other = foreign_burst(id, payload_symbols, 3, rng);
                    std::vector<float> audio(3000, 0.0f);
                    audio.insert(audio.end(), other.begin(), other.end());
                    if (followed) {
                        audio.insert(audio.end(), 700, 0.0f);
                        audio.insert(audio.end(), own.begin(), own.end());
                    }
                    audio.insert(audio.end(), 12000, 0.0f);
                    sim::ChannelParams params;
                    params.snr_db = snr;
                    params.add_noise = snr < 90.0;
                    params.seed = static_cast<std::uint64_t>(40 + id);
                    sim::Channel channel(params, mean_power(other));
                    std::vector<float> received;
                    channel.process(audio, received);

                    Receiver rx;
                    attach_models(rx);
                    int good = 0, extra = 0, synced_on_foreign = 0;
                    const std::size_t foreign_end = 3000 + other.size();
                    for (std::size_t pos = 0; pos < received.size(); pos += 1024) {
                        rx.push(std::span<const float>(received).subspan(pos, std::min<std::size_t>(1024, received.size() - pos)));
                        if (pos + 1024 <= foreign_end && rx.status().synced) ++synced_on_foreign;
                        RxFrame f;
                        while (rx.poll(f)) {
                            const bool mine = f.decoded && f.header_ok && f.callsign == "N0CALL" && f.mode_id == ours.id;
                            ++(mine ? good : extra);
                        }
                    }
                    const bool ok = extra == 0 && synced_on_foreign == 0 && good == (followed ? 2 : 0);
                    CHECK(ok);
                    ++runs;
                    if (!ok) {
                        ++bad;
                        std::printf("  別的模式（編號 %d、%d 個酬載符號、%s）%s：自己的收到 %d 張、多出 %d 幀、誤判同步 %d 個時段\n", id, payload_symbols,
                                    snr < 90.0 ? (snr > 5.0 ? "12 dB" : "3 dB") : "無雜訊", followed ? "後面接著模式 12 的發射" : "", good, extra,
                                    synced_on_foreign);
                    }
                }
            }
        }
    }
    std::printf("  別的模式的幀：%d 種情形，%d 種有問題\n", runs, bad);
}

}  // namespace

int main() {
    test_noise_only();
    if (!have_models()) {
        std::printf("  少了權重檔，略過需要模型的測試\n");
        return check::finish("link");
    }
    test_foreign_modes();
    test_back_to_back();
    test_interrupted_transmission();
    test_weak_signal();
    test_impulse_noise();
    test_flat_pictures();
    test_clean();
    test_awgn();
    test_frequency_offset();
    test_clock_offset();
    test_fading();
    return check::finish("link");
}
