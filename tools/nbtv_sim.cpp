// nbtv-sim：發射 → 通道模擬 → 接收，統計掉幀率（數位）或畫質（類比、學習式）。
// 每一輪是一次連續發射（幾幀），前面先放一段只有雜訊的時間；每一輪用不同的通道亂數。
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "args.hpp"
#include "channel.hpp"
#include "learned_codec.hpp"
#include "learned_frame.hpp"
#include "model_file.hpp"
#include "modes.hpp"
#include "ppm.hpp"
#include "receiver.hpp"
#include "rng.hpp"
#include "transmitter.hpp"

using namespace nbtv;
using namespace nbtv::tools;

namespace {

struct Totals {
    long sent = 0, decoded = 0, received = 0;
    double snr_sum = 0, psnr_sum = 0;
    long psnr_count = 0;
};

double psnr(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    double mse = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double d = static_cast<double>(a[i]) - b[i];
        mse += d * d;
    }
    mse /= static_cast<double>(a.size());
    return mse <= 0 ? 99.0 : 10 * std::log10(255.0 * 255.0 / mse);
}

bool g_noise_map = true;
std::shared_ptr<const LearnedCodec> g_model;  // 學習式模式用；載入後不再改變，各執行緒共用
bool g_learned_median = true, g_learned_attenuate = true, g_learned_floor = true;

Totals run_round(const Mode& mode, sim::ChannelParams params, int burst, const std::vector<std::uint8_t>& image, std::uint64_t seed,
                 float papr_db) {
    Rng rng(seed);
    Transmitter tx;
    tx.set_callsign("BX4ACP");
    tx.set_papr_db(papr_db);
    tx.set_model(g_model);
    std::vector<std::vector<std::uint8_t>> payloads;
    std::vector<float> signal;
    for (int i = 0; i < burst; ++i) {
        std::vector<float> frame;
        if (mode.is_digital()) {
            std::vector<std::uint8_t> payload(static_cast<std::size_t>(mode.image_bytes()));
            for (auto& b : payload) b = static_cast<std::uint8_t>(rng.below(256));
            frame = tx.digital_frame(mode, payload);
            payloads.push_back(std::move(payload));
        } else if (mode.is_learned()) {
            frame = tx.learned_frame(mode, image);
        } else {
            frame = tx.analog_frame(mode, image);
        }
        signal.insert(signal.end(), frame.begin(), frame.end());
    }
    double power = 0;
    for (float v : signal) power += static_cast<double>(v) * v;
    power /= static_cast<double>(signal.size());

    // 開頭 0.5–1.5 秒只有雜訊（長度隨機，讓幀的起點落在任意位置），結尾再留 1 秒
    std::vector<float> audio(6000 + rng.below(12000), 0.0f);
    audio.insert(audio.end(), signal.begin(), signal.end());
    audio.insert(audio.end(), 12000, 0.0f);
    params.seed = seed * 7919 + 13;
    sim::Channel channel(params, power);
    std::vector<float> received;
    channel.process(audio, received);

    Receiver rx;
    rx.tuning().noise_map = g_noise_map;
    rx.tuning().learned_median_snr = g_learned_median;
    rx.tuning().learned_attenuate = g_learned_attenuate;
    rx.tuning().learned_floor = g_learned_floor;
    rx.set_model(g_model);
    Totals t;
    t.sent = burst;
    for (std::size_t pos = 0; pos < received.size(); pos += 1024) {
        rx.push(std::span<const float>(received).subspan(pos, std::min<std::size_t>(1024, received.size() - pos)));
        RxFrame f;
        while (rx.poll(f)) {
            if (mode.is_digital()) {
                if (f.decoded && f.sequence >= 0 && f.sequence < burst && f.image_data == payloads[static_cast<std::size_t>(f.sequence)]) {
                    ++t.decoded;
                    t.snr_sum += f.snr_db;
                }
            } else if (f.decoded && f.rgb.size() == image.size()) {
                ++t.received;
                t.decoded += f.header_ok ? 1 : 0;
                t.snr_sum += f.snr_db;
                t.psnr_sum += psnr(f.rgb, image);
                ++t.psnr_count;
            }
        }
    }
    return t;
}

}  // namespace

int main(int argc, char** argv) {
    const Args args(argc, argv, {"help", "flat-noise", "learned-mean", "learned-plain", "learned-nofloor"});
    g_noise_map = !args.has("flat-noise");
    g_learned_median = !args.has("learned-mean");
    g_learned_attenuate = !args.has("learned-plain");
    g_learned_floor = !args.has("learned-nofloor");
    if (args.has("help")) {
        std::printf(
            "用法：nbtv-sim [選項]\n"
            "  --mode N          模式編號（預設 1）\n"
            "  --channel 名稱    awgn、poor、moderate、slow（預設 awgn）\n"
            "  --snr 清單        訊雜比，可用逗號列出多個，例如 3,4,5（3 kHz 雜訊頻寬）\n"
            "  --frames N        每個訊雜比送幾幀（預設 200）\n"
            "  --burst N         每次連續發射幾幀（預設 4）\n"
            "  --offset Hz       頻率偏移；--ppm N 取樣時脈誤差\n"
            "  --tone Hz         單頻載波干擾的頻率；--tone-db dB 它相對於訊號的功率（預設 0）\n"
            "  --impulse N       每秒 N 次脈衝雜訊；--impulse-db dB 脈衝期間的雜訊功率（預設 20）；--impulse-ms 持續時間（預設 20）\n"
            "  --image 檔.ppm    類比與學習式模式用的畫面（96×72）\n"
            "  --model 檔名      學習式模式的權重檔（預設用專案 models/ 裡給那個模式的）\n"
            "  --learned-mean    學習式：告訴解碼器的訊雜比用各格點雜訊的平均值，不用中位數（比較用）\n"
            "  --learned-plain   學習式：雜訊特別大的格點不壓小（比較用）\n"
            "  --learned-nofloor 學習式：訊雜比低於解碼器認得的下限時不另外處理（比較用）\n"
            "  --papr dB         包絡峰均比目標（0 用預設值，負值不削波）\n"
            "  --flat-noise      接收機不用逐符號、逐載波的雜訊估計（比較用）\n"
            "  --threads N       執行緒數（預設用全部核心）\n"
            "  --seed N          亂數種子\n");
        return 0;
    }
    const Mode* mode = find_mode(args.integer("mode", 1));
    if (mode == nullptr) {
        std::fprintf(stderr, "沒有這個模式\n");
        return 2;
    }
    sim::ChannelParams params;
    const std::string channel_name = args.str("channel", "awgn");
    if (!sim::channel_preset(channel_name, params)) {
        std::fprintf(stderr, "沒有這個通道\n");
        return 2;
    }
    params.freq_offset_hz = args.num("offset", 0.0);
    params.clock_ppm = args.num("ppm", 0.0);
    params.tone_hz = args.num("tone", 0.0);
    params.tone_db = args.num("tone-db", 0.0);
    params.impulse_rate = args.num("impulse", 0.0);
    params.impulse_db = args.num("impulse-db", 20.0);
    params.impulse_ms = args.num("impulse-ms", 20.0);
    std::vector<double> snrs;
    {
        std::stringstream ss(args.str("snr", "10"));
        for (std::string item; std::getline(ss, item, ',');) snrs.push_back(std::atof(item.c_str()));
    }
    const int burst = std::max(1, args.integer("burst", 4));
    const int rounds = std::max(1, (args.integer("frames", 200) + burst - 1) / burst);
    const unsigned threads = static_cast<unsigned>(std::max(1, args.integer("threads", static_cast<int>(std::thread::hardware_concurrency()))));
    const std::uint64_t seed = static_cast<std::uint64_t>(args.integer("seed", 1));
    const float papr = static_cast<float>(args.num("papr", 0.0));

    std::vector<std::uint8_t> image(kImageRgbBytes, 128);
    if (!mode->is_digital()) {
        int w = 0, h = 0;
        if (!args.has("image") || !read_ppm(args.str("image", ""), image, &w, &h) || w != kImageWidth || h != kImageHeight) {
            std::fprintf(stderr, "類比與學習式模式需要 --image 指定 96×72 的 PPM\n");
            return 2;
        }
    }
    if (mode->is_learned()) {
        const std::string model_path = args.str("model", default_model_path(mode->learned_values, mode->learned_width));
        std::vector<std::uint8_t> bytes;
        std::string error = "讀不到檔案";
        if (read_file(model_path, bytes)) g_model = LearnedCodec::load(bytes, &error);
        if (g_model == nullptr || !learned_model_fits(*mode, *g_model)) {
            std::fprintf(stderr, "學習式模式需要權重檔 %s：%s\n", model_path.c_str(), g_model == nullptr ? error.c_str() : "不是給這個模式用的");
            return 2;
        }
    }

    std::printf("模式 %d，通道 %s", mode->id, channel_name.c_str());
    if (params.freq_offset_hz != 0.0) std::printf("，頻偏 %+.1f Hz", params.freq_offset_hz);
    if (params.clock_ppm != 0.0) std::printf("，時脈 %+.0f ppm", params.clock_ppm);
    if (params.tone_hz > 0.0) std::printf("，載波干擾 %.0f Hz／%+.0f dB", params.tone_hz, params.tone_db);
    if (params.impulse_rate > 0.0) std::printf("，脈衝雜訊每秒 %.1f 次／%+.0f dB／%.0f ms", params.impulse_rate, params.impulse_db, params.impulse_ms);
    std::printf("；每點 %d 幀（每次連續 %d 幀）\n", rounds * burst, burst);
    std::printf(mode->is_digital() ? "  訊雜比   送出   解出   掉幀率   量到的訊雜比\n" : "  訊雜比   送出   收到   標頭正確   平均 PSNR   量到的訊雜比\n");
    for (double snr : snrs) {
        params.snr_db = snr;
        Totals total;
        std::mutex lock;
        std::atomic<int> next{0};
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; ++t) {
            pool.emplace_back([&] {
                for (int r; (r = next.fetch_add(1)) < rounds;) {
                    const Totals one = run_round(*mode, params, burst, image, seed * 1000003ULL + static_cast<std::uint64_t>(r), papr);
                    const std::lock_guard<std::mutex> guard(lock);
                    total.sent += one.sent;
                    total.decoded += one.decoded;
                    total.received += one.received;
                    total.snr_sum += one.snr_sum;
                    total.psnr_sum += one.psnr_sum;
                    total.psnr_count += one.psnr_count;
                }
            });
        }
        for (std::thread& th : pool) th.join();
        if (mode->is_digital()) {
            std::printf("  %5.1f   %5ld  %5ld   %5.1f%%   %6.1f dB\n", snr, total.sent, total.decoded,
                        100.0 * static_cast<double>(total.sent - total.decoded) / static_cast<double>(total.sent),
                        total.decoded > 0 ? total.snr_sum / static_cast<double>(total.decoded) : 0.0);
        } else {
            std::printf("  %5.1f   %5ld  %5ld   %7.1f%%   %7.1f dB   %6.1f dB\n", snr, total.sent, total.received,
                        100.0 * static_cast<double>(total.decoded) / static_cast<double>(total.sent),
                        total.psnr_count > 0 ? total.psnr_sum / static_cast<double>(total.psnr_count) : 0.0,
                        total.psnr_count > 0 ? total.snr_sum / static_cast<double>(total.psnr_count) : 0.0);
        }
        std::fflush(stdout);
    }
    return 0;
}
