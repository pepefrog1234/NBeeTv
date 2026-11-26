// 端對端：發射 → 通道模擬 → 接收機（同步、通道估測、解碼）。
// 這裡看的是接收機本身：捕獲、頻偏與定時、追蹤、兩次發射之間的銜接。各個模式的畫質在 test_learned.cpp。
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <future>

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
    for (const Mode& m : kModes) {
        const auto rgb = test_images::load("coffee", m.image_width, m.image_height);  // 畫面的大小跟著模式
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
            CHECK(f.image_width == m.image_width && f.image_height == m.image_height && f.rgb.size() == rgb.size());
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

// 兩次發射疊在一起（兩台同時發射，或是兩段錄音被混成一段）：第一次發射連續送三幀，第二次發射在它的第二幀
// 開始之後一個多符號才開始。接收機正在收的那一幀前導符號很明確、收到一半又看到另一個很明確的前導符號時，
// 會先把這一幀收完，再去收另一次發射。曾經出過的問題：回頭去找那個前導符號時，又抓到剛收完的那一幀，
// 於是在兩者之間來回，push() 永遠不會返回。
// 兩邊互相干擾，這裡不要求每一幀都解得出來，只要求：接收機會收完、同一幀不交出來兩次、
// 還沒被蓋到的第一幀一定收得到。
void test_overlapping_transmissions() {
    struct Case {
        int mode_a, mode_b, delay;  // delay：第二次發射在第一次發射的第二幀開始之後幾個取樣開始
        float gain_b;               // 第二次發射的音量是第一次的幾倍
        double snr_db;
    };
    const Case cases[] = {
        {14, 11, 450, 0.7f, 99.0}, {14, 14, 450, 0.7f, 99.0}, {14, 11, 340, 0.7f, 15.0}, {14, 14, 340, 0.7f, 15.0},
        {14, 11, 450, 1.4f, 15.0}, {14, 12, 450, 1.4f, 15.0}, {14, 12, 340, 0.7f, 15.0}, {12, 11, 450, 0.7f, 15.0},
        {11, 14, 397, 1.0f, 15.0}, {13, 14, 520, 0.7f, 99.0},
    };
    int runs = 0, bad = 0;
    for (const Case& c : cases) {
        const Mode& ma = *find_mode(c.mode_a);
        const Mode& mb = *find_mode(c.mode_b);
        const std::vector<float>& a = burst(ma, 3, "AA1AAA");
        const std::vector<float>& b = burst(mb, 1, "BB2BBB");
        const std::size_t start_a = 3000, start_b = start_a + static_cast<std::size_t>(ma.frame_samples() + c.delay);
        std::vector<float> audio(std::max(start_a + a.size(), start_b + b.size()) + 12000, 0.0f);
        for (std::size_t i = 0; i < a.size(); ++i) audio[start_a + i] += a[i];
        for (std::size_t i = 0; i < b.size(); ++i) audio[start_b + i] += c.gain_b * b[i];

        sim::ChannelParams params;
        params.snr_db = c.snr_db;
        params.add_noise = c.snr_db < 90.0;
        params.seed = 77;
        sim::Channel channel(params, mean_power(a));
        std::vector<float> received;
        channel.process(audio, received);

        Receiver rx;
        attach_models(rx);
        // 卡住的話永遠等不到結果，所以放在另一條執行緒跑；超過時限就算失敗，直接結束測試程式
        auto job = std::async(std::launch::async, [&] { return run_receiver(rx, received); });
        if (job.wait_for(std::chrono::seconds(120)) != std::future_status::ready) {
            std::printf("  模式 %d 的第二幀開始後 %d 個取樣出現模式 %d（音量 %.1f 倍、訊雜比 %.0f dB）：接收機 120 秒還沒收完，卡住了\n",
                        c.mode_a, c.delay, c.mode_b, static_cast<double>(c.gain_b), c.snr_db);
            std::fflush(stdout);
            std::_Exit(1);
        }
        const std::vector<RxFrame> frames = job.get();
        bool repeated = false;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            for (std::size_t j = i + 1; j < frames.size(); ++j) {
                if (std::llabs(frames[i].start_sample - frames[j].start_sample) < kSymbolSamples) repeated = true;
            }
        }
        const bool first_frame = !frames.empty() && frames[0].decoded && frames[0].header_ok && frames[0].callsign == "AA1AAA" &&
                                 frames[0].mode_id == ma.id && frames[0].sequence == 0;
        const bool ok = !repeated && first_frame && frames.size() <= 4;  // 總共只送了四幀
        CHECK(ok);
        ++runs;
        if (!ok) {
            ++bad;
            std::printf("  模式 %d 的第二幀開始後 %d 個取樣出現模式 %d（音量 %.1f 倍、訊雜比 %.0f dB）：交出 %zu 幀%s%s\n", c.mode_a, c.delay,
                        c.mode_b, static_cast<double>(c.gain_b), c.snr_db, frames.size(), repeated ? "，同一幀交出來不只一次" : "",
                        first_frame ? "" : "，第一幀沒有收到");
        }
    }
    std::printf("  兩次發射疊在一起：%d 種情形，%d 種有問題\n", runs, bad);
}

// 一次「別的模式」的發射：S1 用的是模式表裡沒有的編號，S0 與導頻和我們的一模一樣。
// 幀長照 payload_symbols，資料格一半的幀放 QPSK（像數位模式）、一半放高斯值（像類比模式）。
std::vector<float> foreign_burst(int id, int payload_symbols, int frames, Rng& rng) {
    const Mode fake{id, payload_symbols, 1, 2, 1, kImageWidth, kImageHeight};  // 只借用幀的算術與 S1 的編號；後面的欄位調變時用不到
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
}

// 別的模式的幀：S1 用的是模式表裡沒有的編號（0–4、8–10：草案裡的數位與類比模式用過，已經移除）。
// S0 與導頻和我們的一模一樣，只有 S1 不同。接收機要認得出那不是自己的模式：不同步、不回報任何幀，
// 也不能因此漏掉緊接著來的、自己認得的發射。
void test_foreign_modes() {
    const Mode& ours = *find_mode(12);
    const std::vector<float>& own = burst(ours, 2, "N0CALL");
    int runs = 0, bad = 0;
    for (int id : {0, 1, 4, 9, 10}) {
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

// 弱訊號下照幀的節拍接著收（RxTuning::follow）。3 kHz 訊雜比 −4 dB 時標頭幾乎都解不出來，前導符號也常常
// 通不過和捕獲時一樣嚴的檢查，但導頻還很清楚、畫面也解得出來：鎖定之後要一幀一幀接著收下去。
// 同時不能因此交出不存在的畫面：發射結束之後的雜訊、緊接著來的別種模式的幀、對方換成另一個模式、
// 頻帶內的單頻載波，都不能變成亂碼。
//
// 這裡的「畫面是真的」用位置來認：依幀開頭的取樣編號找出那裡原來送的是哪一張，模式要對、PSNR 要夠高
// （解錯模式或把雜訊當成一幀，解出來的和原圖無關，只有十來 dB）。
struct Sent {
    const Mode* mode;
    int frames;  // 畫面輪流用三張測試圖（link_util::burst）
};

struct FollowTally {
    int genuine = 0;  // 位置、模式、畫面都對得上的畫面
    int bogus = 0;    // 對不上的畫面（包括落在別種訊號或雜訊上的）
};

FollowTally run_follow(const std::vector<Sent>& sent, const std::vector<float>& after, int tail, double snr_db, std::uint64_t seed, bool follow,
                       std::vector<int>* per_part = nullptr, double tone_hz = 0.0, double tone_db = 0.0) {
    constexpr int kLead = 3000;
    constexpr double kPsnrFloor = 13.5;
    std::vector<float> audio(kLead, 0.0f);
    double power = 0.0;
    for (const Sent& part : sent) {
        const std::vector<float>& signal = burst(*part.mode, part.frames, "N0CALL");
        if (power <= 0.0) power = mean_power(signal);
        audio.insert(audio.end(), signal.begin(), signal.end());
    }
    audio.insert(audio.end(), after.begin(), after.end());
    audio.insert(audio.end(), static_cast<std::size_t>(tail), 0.0f);

    sim::ChannelParams params;
    params.snr_db = snr_db;
    params.add_noise = snr_db < 90.0;
    params.seed = seed;
    params.tone_hz = tone_hz;  // 單頻載波從頭到尾都在，功率相對於訊號
    params.tone_db = tone_db;
    sim::Channel channel(params, power);
    std::vector<float> received;
    channel.process(audio, received);

    Receiver rx;
    rx.tuning().follow = follow;
    attach_models(rx);
    FollowTally tally;
    if (per_part != nullptr) per_part->assign(sent.size(), 0);
    for (const RxFrame& f : run_receiver(rx, received)) {
        if (!f.decoded) continue;
        bool genuine = false;
        std::int64_t begin = kLead;
        for (std::size_t n = 0; n < sent.size() && !genuine; ++n) {
            const std::int64_t length = sent[n].mode->frame_samples();
            const double at = static_cast<double>(f.start_sample - begin) / static_cast<double>(length);
            const long index = std::lround(at);
            if (index >= 0 && index < sent[n].frames && std::fabs(at - static_cast<double>(index)) < 0.2 && f.mode_id == sent[n].mode->id) {
                genuine = test_images::psnr(f.rgb, test_images::load(test_images::kNames[index % 3])) > kPsnrFloor;
                if (genuine && per_part != nullptr) ++(*per_part)[n];
            }
            begin += length * sent[n].frames;
        }
        ++(genuine ? tally.genuine : tally.bogus);
    }
    return tally;
}

void test_follow() {
    const Mode& m12 = *find_mode(12);
    const Mode& m14 = *find_mode(14);

    // 一、連續 16 幀、白雜訊 −4 dB，後面接 8 秒的雜訊。接著收的話幾乎每一幀都交得出來（每次發射的第一幀除外：
    // 它的前導符號證據還不夠強）；關掉的話每一幀都要重新靠前導符號，只剩兩三成。兩邊都不能多出不存在的畫面。
    int with = 0, without = 0, bogus = 0;
    for (std::uint64_t seed : {11u, 12u, 13u}) {
        const FollowTally on = run_follow({{&m12, 16}}, {}, 12000 * 8, -4.0, seed, true);
        const FollowTally off = run_follow({{&m12, 16}}, {}, 12000 * 8, -4.0, seed, false);
        with += on.genuine;
        without += off.genuine;
        bogus += on.bogus + off.bogus;
    }
    CHECK(with >= 39);            // 48 幀裡至少八成
    CHECK(without <= with - 15);  // 差距就是這個功能的用處
    CHECK_EQ(bogus, 0);
    std::printf("  模式 12、AWGN −4 dB、連續 16 幀 × 3 次：照節拍接著收 %d/48 張，不接著收 %d/48 張，多出的畫面 %d 張\n", with, without, bogus);

    // 最短的幀（模式 14，導頻最少）在 −5 dB 也要接得下去
    {
        const FollowTally on = run_follow({{&m14, 16}}, {}, 12000 * 5, -5.0, 21, true);
        CHECK(on.genuine >= 11);
        CHECK_EQ(on.bogus, 0);
        std::printf("  模式 14、AWGN −5 dB、連續 16 幀：%d/16 張，多出的畫面 %d 張\n", on.genuine, on.bogus);
    }

    // 二、我們的發射後面沒有空檔、緊接著別種模式的發射（S0 與導頻一模一樣，照同樣的節拍來，只有 S1 不同）。
    // 編號 1 的 S1 在奇數載波上和模式 12 的很像（24 個裡 15 個相同），是沒有定義的編號裡最容易混淆的；幀長取和模式 12 相同與不同的各一種。
    // 我們的每一幀都要收到，別人的幀一張都不能變成畫面。
    int runs = 0, bad = 0;
    for (int id : {1, 9}) {
        CHECK(find_mode(id) == nullptr);
        for (int payload_symbols : {37, 100}) {
            for (double snr : {99.0, 6.0, 0.0}) {  // 99：不加雜訊
                Rng rng(static_cast<std::uint64_t>(2000 + id * 10 + payload_symbols));
                const std::vector<float> other = foreign_burst(id, payload_symbols, 3, rng);
                const FollowTally r = run_follow({{&m12, 3}}, other, 12000, snr, static_cast<std::uint64_t>(60 + id), true);
                const bool ok = r.bogus == 0 && r.genuine >= (snr > 3.0 ? 3 : 2);
                CHECK(ok);
                ++runs;
                if (!ok) {
                    ++bad;
                    std::printf("  模式 12 × 3 後面緊接別的模式（編號 %d、%d 個酬載符號、%s）：自己的收到 %d 張、多出 %d 張\n", id, payload_symbols,
                                snr < 90.0 ? (snr > 3.0 ? "6 dB" : "0 dB") : "無雜訊", r.genuine, r.bogus);
                }
            }
        }
    }
    std::printf("  我們的發射後面緊接別的模式：%d 種情形，%d 種有問題\n", runs, bad);

    // 三、對方沒有停、直接換成另一個模式（幀長不同，或幀長相同只差在 S1）。換過去之後的幀也要收到，不能有亂碼。
    struct Switch {
        int from, frames_from, to, frames_to;
        double snr_db;
        int least;  // 換過去之後至少要收到幾張
    };
    for (const Switch& c : {Switch{12, 3, 14, 6, 0.0, 4}, Switch{14, 4, 12, 4, -3.0, 2}, Switch{11, 2, 13, 2, 0.0, 1}}) {
        std::vector<int> parts;
        const FollowTally r = run_follow({{find_mode(c.from), c.frames_from}, {find_mode(c.to), c.frames_to}}, {}, 12000, c.snr_db, 70, true, &parts);
        CHECK_EQ(r.bogus, 0);
        CHECK(parts[1] >= c.least);
        std::printf("  模式 %d × %d 直接換成模式 %d × %d（%.0f dB）：收到 %d + %d 張，多出的畫面 %d 張\n", c.from, c.frames_from, c.to, c.frames_to,
                    c.snr_db, parts[0], parts[1], r.bogus);
    }

    // 四、頻帶內一直有一個單頻載波（別台的載波、調諧的訊號）。載波在相隔幾個符號之間是相干的，漏到鄰近幾格的部分也是，
    // 光看導頻的相干值和導頻分不出來；發射結束後只剩載波與雜訊，那裡不能冒出畫面。接收機另外看導頻的能量是不是
    // 分散在整個頻帶（kPilotTopShare）：集中在少數幾個載波上的，照節拍接著收的不接，靠前導符號的要 S1 對得上才算數。
    // 少了這一關，下面 1736.25 與 1642.5 Hz 的兩組會多交出一張不存在的畫面（其餘的頻率在別的亂數下也出現過）；
    // 接著收開或關都要過。
    struct Carrier {
        int mode, frames;
        double snr_db, tone_hz, tone_db;
    };
    runs = bad = 0;
    for (const Carrier& c : {Carrier{12, 3, 10.0, 780.0, -10.0}, Carrier{12, 3, 25.0, 1736.25, -3.0}, Carrier{12, 3, 25.0, 1642.5, -3.0},
                             Carrier{14, 4, 8.0, 1200.0, -3.0}}) {
        for (bool follow : {true, false}) {
            for (std::uint64_t seed : {81u, 82u, 83u}) {
                const FollowTally r = run_follow({{find_mode(c.mode), c.frames}}, {}, 12000 * 3, c.snr_db, seed, follow, nullptr, c.tone_hz, c.tone_db);
                const bool ok = r.bogus == 0 && r.genuine == c.frames;
                CHECK(ok);
                ++runs;
                if (!ok) {
                    ++bad;
                    std::printf("  模式 %d × %d、%.0f dB，加 %.2f Hz 的載波（%+.0f dB）、%s：收到 %d 張、多出 %d 張\n", c.mode, c.frames, c.snr_db,
                                c.tone_hz, c.tone_db, follow ? "接著收" : "不接著收", r.genuine, r.bogus);
                }
            }
        }
    }
    std::printf("  頻帶內有單頻載波、發射結束後只剩載波與雜訊：%d 種情形，%d 種有問題\n", runs, bad);
}

}  // namespace

// ───── 沒有散佈導頻的幀（模式 5、6、7；給 FM 語音通道用）─────
// FM 解調出來的音訊訊雜比高、沒有頻偏、通道在一幀之內不變；會變的是兩邊音效卡的取樣時脈，以及訊號忽強忽弱時的雜訊。

// 白雜訊 12–30 dB：每一幀都要收到、標頭要對，量到的訊雜比接近設定值。
// 模式 5 和模式 15 用同一個模型：畫質要相當（少了導頻，每張快 16%）。另外兩個模式各試一次。
void test_free_awgn() {
    const Mode& m5 = *find_mode(5);
    const Mode& m15 = *find_mode(15);
    const auto rgb = test_images::load("cat", m5.image_width, m5.image_height);
    for (double snr : {30.0, 20.0, 12.0}) {
        sim::ChannelParams p;
        p.snr_db = snr;
        p.seed = 71;
        const LinkResult free = run_link(m5, 3, rgb, p, 5000);
        const LinkResult pilots = run_link(m15, 3, rgb, p, 5000);
        CHECK_EQ(free.decoded, 3);
        CHECK_EQ(pilots.decoded, 3);
        double measured = 0;
        for (const RxFrame& f : free.frames) measured += f.snr_db / static_cast<double>(free.frames.size());
        CHECK_NEAR(measured, snr, 1.5);
        const double a = mean_psnr(free.frames, rgb), b = mean_psnr(pilots.frames, rgb);
        CHECK(a > b - 0.3);
        std::printf("  模式 5 AWGN %.0f dB：%d/3、量到 %.1f dB、PSNR %.2f dB（模式 15：%d/3、%.2f dB）\n", snr, free.decoded, measured, a, pilots.decoded, b);
    }
    for (int id : {6, 7}) {
        const Mode& m = *find_mode(id);
        sim::ChannelParams p;
        p.snr_db = 20.0;
        p.seed = 72;
        const LinkResult r = run_link(m, 2, rgb, p, 5000);
        const LinkResult pilots = run_link(m15, 2, rgb, p, 5000);
        CHECK_EQ(r.decoded, 2);
        // 送的值比較多（23、36 個通道），畫質要比 18 個通道的模式 15 好
        CHECK(mean_psnr(r.frames, rgb) > mean_psnr(pilots.frames, rgb) + 0.3);
        std::printf("  模式 %d AWGN 20 dB：%d/2、PSNR %.2f dB（模式 15：%.2f dB）\n", id, r.decoded, mean_psnr(r.frames, rgb), mean_psnr(pilots.frames, rgb));
    }
}

// 取樣時脈誤差 ±100、±200 ppm：最長的幀（模式 7，4.43 秒）在 200 ppm 下，定時在一幀之內會漂 10 個取樣。
// 每一幀都要收到，畫質和沒有誤差時相當。
void test_free_clock_offset() {
    for (int id : {5, 7}) {
        const Mode& m = *find_mode(id);
        const auto rgb = test_images::load("coffee", m.image_width, m.image_height);
        sim::ChannelParams p;
        p.snr_db = 25.0;
        p.seed = 43;
        const double exact = mean_psnr(run_link(m, 3, rgb, p).frames, rgb);
        for (double ppm : {-200.0, -100.0, 100.0, 200.0}) {
            p.clock_ppm = ppm;
            const LinkResult r = run_link(m, 3, rgb, p);
            CHECK_EQ(r.decoded, 3);
            const double got = mean_psnr(r.frames, rgb);
            CHECK(got > exact - 0.3);
            std::printf("  模式 %d 時脈誤差 %+.0f ppm：%d/3、PSNR %.2f dB（沒有誤差時 %.2f dB）\n", id, ppm, r.decoded, got, exact);
        }
    }
}

// 脈衝雜訊：每秒 2 次、每次 20 ms、比訊號強 20 dB。被打到的符號要降低權重
// （頻帶上方沒用到的頻率聽得到它；收到的能量也比預期多出一大截），否則畫面會被那幾個符號帶壞。
void test_free_impulse_noise() {
    const Mode& m = *find_mode(5);
    const auto rgb = test_images::load("cat", m.image_width, m.image_height);
    sim::ChannelParams p;
    p.snr_db = 25.0;
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
    std::printf("  模式 5、25 dB 加脈衝雜訊：逐符號雜訊估計 %d/4、%.1f dB，單一雜訊值 %d/4、%.1f dB\n", with_map, psnr_map, flat, psnr_flat);
}

// 訊號忽強忽弱時，某一幀開頭的前導符號可能剛好落在訊號最弱的那一刻。連續發射 5 幀，把第三幀的 S0 與 S1 換成雜訊：
// 前導符號確認不到、開頭的參考符號也不能用，但靠前兩幀留下的通道與時脈漂移，加上這一幀結尾的參考符號，照樣要解出來。
void test_free_faded_preamble() {
    const Mode& m = *find_mode(5);
    std::vector<float> signal = burst(m, 5, "N0CALL");
    CHECK_EQ(static_cast<int>(signal.size()), 5 * m.frame_samples());
    if (signal.empty()) return;
    const double power = mean_power(signal);
    Rng rng(909);
    const std::size_t at = static_cast<std::size_t>(2 * m.frame_samples());
    for (int n = 0; n < kPreambleSymbols * kSymbolSamples; ++n) signal[at + static_cast<std::size_t>(n)] = static_cast<float>(std::sqrt(power) * rng.gauss());
    std::vector<float> audio(3000, 0.0f);
    audio.insert(audio.end(), signal.begin(), signal.end());
    audio.insert(audio.end(), 9000, 0.0f);
    for (double ppm : {0.0, 80.0}) {
        sim::ChannelParams params;
        params.snr_db = 25.0;
        params.clock_ppm = ppm;
        params.seed = 606;
        sim::Channel channel(params, power);
        std::vector<float> received;
        channel.process(audio, received);
        Receiver rx;
        attach_models(rx);
        int good = 0, other = 0;
        bool third = false;
        for (const RxFrame& f : run_receiver(rx, received)) {
            const bool mine = f.decoded && f.header_ok && f.callsign == "N0CALL" && f.mode_id == m.id;
            ++(mine ? good : other);
            third = third || (mine && f.sequence == 2);
        }
        CHECK_EQ(good, 5);
        CHECK_EQ(other, 0);
        CHECK(third);
        std::printf("  模式 5 連續 5 幀、第三幀的前導符號換成雜訊（時脈誤差 %+.0f ppm）：收到 %d 張%s\n", ppm, good, third ? "，第三幀也在" : "，第三幀沒收到");
    }
}

// 兩次發射緊接著來、和有導頻的模式互換、送到一半就停。要求和有導頻的模式一樣（test_back_to_back、test_interrupted_transmission）：
// 完整送出的每一幀都要收到，不能多出不存在的畫面。這種幀結尾的參考符號用的是 S1 的另一條序列，
// 接收機不能把它當成下一幀的開頭。
void test_free_bursts() {
    struct Case {
        int mode_a, frames_a, mode_b, frames_b, cut;
        double snr_db;
        std::vector<int> gaps;
    };
    const Case cases[] = {
        {5, 2, 5, 2, 0, 25.0, {0, 66, 700, 4859, 20000}},  // 同一個模式
        {5, 2, 5, 2, 0, 99.0, {120, 8621}},                // 沒有雜訊：兩次發射之間是完全的靜音
        {15, 2, 5, 2, 0, 25.0, {0, 9000}},                 // 有導頻的換成沒有導頻的（同一個模型）
        {5, 2, 15, 2, 0, 25.0, {0, 9000}},                 // 反過來
        {6, 1, 12, 2, 0, 25.0, {300, 6000}},               // 大畫面之後接一般的模式
        {12, 2, 7, 1, 0, 25.0, {300, 6000}},
        {5, 2, 5, 2, 9000, 25.0, {300, 6000, 12000}},      // 第一次發射的最後一幀只送了三成
        {7, 1, 5, 2, 20000, 25.0, {300, 9000}},            // 模式 7 的那一幀送了不到四成就停
    };
    int runs = 0, bad = 0;
    for (const Case& c : cases) {
        const Mode& ma = *find_mode(c.mode_a);
        const Mode& mb = *find_mode(c.mode_b);
        for (int gap : c.gaps) {
            const TwoBursts r = run_two_bursts(ma, c.frames_a, mb, c.frames_b, gap, c.cut, c.snr_db, 700 + static_cast<std::uint64_t>(c.mode_a));
            const bool ok = c.cut == 0 ? (r.first == c.frames_a && r.second == c.frames_b && r.bogus == 0 && r.failed == 0)
                                       : (r.first >= c.frames_a - 1 && r.first <= c.frames_a && r.second == c.frames_b && r.bogus == 0 &&
                                          r.failed <= 1 && r.first + r.failed <= c.frames_a);
            CHECK(ok);
            ++runs;
            if (!ok) {
                ++bad;
                std::printf("  模式 %d×%d（最後一幀%s）→ 空檔 %d → 模式 %d×%d：收到 %d + %d 張，多出 %d 張，回報失敗 %d 次\n", c.mode_a, c.frames_a,
                            c.cut > 0 ? "沒送完" : "送完", gap, c.mode_b, c.frames_b, r.first, r.second, r.bogus, r.failed);
            }
        }
    }
    std::printf("  沒有導頻的幀：兩次發射緊接著來、換模式、中途停止，%d 種情形，%d 種有問題\n", runs, bad);
}

// 送到一半就停的幀，標頭解得出來的話會交出畫面；沒送到的那一截只有雜訊，不能當成收到的值。
// 和有導頻的模式 15（同一個模型，導頻不見了它自然知道那裡沒有訊號）比，畫質不能差太多。
void test_free_truncated_picture() {
    double psnr[2] = {0.0, 0.0};
    int pictures[2] = {0, 0};
    int k = 0;
    for (int id : {5, 15}) {
        const Mode& m = *find_mode(id);
        const auto rgb = test_images::load("rocket", m.image_width, m.image_height);
        Transmitter tx;
        tx.set_callsign("N0CALL");
        attach_models(tx);
        std::vector<float> frame = tx.learned_frame(m, rgb);
        const double power = mean_power(frame);
        frame.resize(frame.size() * 7 / 10);  // 只送出前七成
        std::vector<float> audio(3000, 0.0f);
        audio.insert(audio.end(), frame.begin(), frame.end());
        audio.insert(audio.end(), static_cast<std::size_t>(m.frame_samples()) + 9000, 0.0f);
        sim::ChannelParams params;
        params.snr_db = 25.0;
        params.seed = 818;
        sim::Channel channel(params, power);
        std::vector<float> received;
        channel.process(audio, received);
        Receiver rx;
        attach_models(rx);
        for (const RxFrame& f : run_receiver(rx, received)) {
            if (!f.decoded) continue;
            ++pictures[k];
            psnr[k] = test_images::psnr(f.rgb, rgb);
        }
        ++k;
    }
    CHECK(pictures[0] <= 1 && pictures[1] <= 1);
    CHECK_EQ(pictures[0], pictures[1]);
    if (pictures[0] == 1 && pictures[1] == 1) CHECK(psnr[0] > psnr[1] - 1.5);
    std::printf("  只送出前七成的一幀：模式 5 交出 %d 張（PSNR %.1f dB），模式 15 交出 %d 張（%.1f dB）\n", pictures[0], psnr[0], pictures[1], psnr[1]);
}

int main() {
    test_noise_only();
    if (!have_models()) {
        std::printf("  少了權重檔，略過需要模型的測試\n");
        return check::finish("link");
    }
    test_overlapping_transmissions();
    test_foreign_modes();
    test_follow();
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
    test_free_awgn();
    test_free_clock_offset();
    test_free_impulse_noise();
    test_free_faded_preamble();
    test_free_bursts();
    test_free_truncated_picture();
    return check::finish("link");
}
