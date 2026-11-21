// 端對端測試共用：發射 → 通道模擬 → 接收。
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "channel.hpp"
#include "modes.hpp"
#include "receiver.hpp"
#include "rng.hpp"
#include "transmitter.hpp"

namespace link_util {

using namespace nbtv;

inline std::vector<std::uint8_t> random_bytes(Rng& rng, int n) {
    std::vector<std::uint8_t> v(static_cast<std::size_t>(n));
    for (auto& b : v) b = static_cast<std::uint8_t>(rng.below(256));
    return v;
}

inline double mean_power(std::span<const float> x) {
    double acc = 0.0;
    for (float v : x) acc += static_cast<double>(v) * v;
    return x.empty() ? 0.0 : acc / static_cast<double>(x.size());
}

struct LinkResult {
    std::vector<RxFrame> frames;
    RxStatus final_status;
    int decoded = 0;  // 解出來且內容正確的幀數
};

// 把一段音訊（已含前後的靜音）送過通道再交給接收機；每次餵 chunk 個取樣，模擬音訊裝置的節奏。
inline std::vector<RxFrame> run_receiver(Receiver& rx, std::span<const float> audio, std::size_t chunk = 1024) {
    std::vector<RxFrame> frames;
    for (std::size_t pos = 0; pos < audio.size(); pos += chunk) {
        rx.push(audio.subspan(pos, std::min(chunk, audio.size() - pos)));
        RxFrame f;
        while (rx.poll(f)) frames.push_back(std::move(f));
    }
    return frames;
}

// 發射 count 幀數位模式（內容是亂數），前面空 lead 個取樣、後面空 tail 個取樣，過通道後接收。
// 回傳每一幀送出的酬載，供比對。
inline LinkResult run_digital_link(const Mode& mode, int count, const sim::ChannelParams& params, std::uint64_t seed,
                                   int lead = 3000, int tail = 6000, bool noise_map = true) {
    Rng rng(seed);
    Transmitter tx;
    tx.set_callsign("BX4ACP");
    std::vector<std::vector<std::uint8_t>> payloads;
    std::vector<float> signal;
    for (int i = 0; i < count; ++i) {
        payloads.push_back(random_bytes(rng, mode.image_bytes()));
        const auto frame = tx.digital_frame(mode, payloads.back());
        signal.insert(signal.end(), frame.begin(), frame.end());
    }
    const double power = mean_power(signal);
    std::vector<float> audio(static_cast<std::size_t>(lead), 0.0f);
    audio.insert(audio.end(), signal.begin(), signal.end());
    audio.insert(audio.end(), static_cast<std::size_t>(tail), 0.0f);

    sim::Channel channel(params, power);
    std::vector<float> received;
    channel.process(audio, received);

    Receiver rx;
    rx.tuning().noise_map = noise_map;
    LinkResult result;
    result.frames = run_receiver(rx, received);
    result.final_status = rx.status();
    for (const RxFrame& f : result.frames) {
        if (!f.decoded || f.sequence < 0 || f.sequence >= count) continue;
        if (f.image_data == payloads[static_cast<std::size_t>(f.sequence)] && f.callsign == "BX4ACP") ++result.decoded;
    }
    return result;
}

// 發射 count 幀類比模式（同一張畫面），過通道後接收；回傳收到的幀。
inline LinkResult run_analog_link(const Mode& mode, int count, std::span<const std::uint8_t> rgb, const sim::ChannelParams& params,
                                  int lead = 3000, int tail = 6000) {
    Transmitter tx;
    tx.set_callsign("BX4ACP");
    std::vector<float> signal;
    for (int i = 0; i < count; ++i) {
        const auto frame = tx.analog_frame(mode, rgb);
        signal.insert(signal.end(), frame.begin(), frame.end());
    }
    const double power = mean_power(signal);
    std::vector<float> audio(static_cast<std::size_t>(lead), 0.0f);
    audio.insert(audio.end(), signal.begin(), signal.end());
    audio.insert(audio.end(), static_cast<std::size_t>(tail), 0.0f);
    sim::Channel channel(params, power);
    std::vector<float> received;
    channel.process(audio, received);
    Receiver rx;
    LinkResult result;
    result.frames = run_receiver(rx, received);
    result.final_status = rx.status();
    for (const RxFrame& f : result.frames) result.decoded += (f.decoded && f.header_ok) ? 1 : 0;
    return result;
}

// 能量集中在少數幾個 DCT 係數的測試圖（漸層加棋盤格）。類比模式送這種畫面時，少數幾個格點會比其他的
// 大上幾十倍，酬載聽起來像幾個單音——這是同步判定最容易被騙的內容。
inline std::vector<std::uint8_t> test_pattern(int variant) {
    std::vector<std::uint8_t> rgb(kImageRgbBytes);
    for (int y = 0; y < kImageHeight; ++y) {
        for (int x = 0; x < kImageWidth; ++x) {
            const std::size_t i = static_cast<std::size_t>(3 * (y * kImageWidth + x));
            rgb[i] = static_cast<std::uint8_t>((x * 2 + variant * 40) & 0xFF);
            rgb[i + 1] = static_cast<std::uint8_t>((y * 3 + variant * 17) & 0xFF);
            rgb[i + 2] = static_cast<std::uint8_t>(((x / 12 + y / 12) % 2) * 200 + 20);
        }
    }
    return rgb;
}

struct TwoBursts {
    int first = 0;   // 第一次發射解出來的幀數
    int second = 0;  // 第二次發射解出來的幀數
    int bogus = 0;   // 不是任何一次發射送的、卻被當成畫面交出來的幀數
    int failed = 0;  // 「有一幀沒解出來」的通知次數
};

// 兩次發射（呼號不同）之間隔 gap 個取樣的靜音。cut > 0 時，第一次發射的最後一幀只送前 cut 個取樣（中途停止）。
// 數位模式送亂數酬載，類比模式送 test_pattern。snr_db ≥ 90 表示不加雜訊，靜音就是完全的 0。
inline TwoBursts run_two_bursts(const Mode& ma, int na, const Mode& mb, int nb, int gap, int cut, double snr_db, std::uint64_t seed) {
    Rng rng(seed * 77 + 5);
    auto burst = [&](const Mode& m, int frames, const char* call) {
        Transmitter tx;
        tx.set_callsign(call);
        std::vector<float> signal;
        for (int i = 0; i < frames; ++i) {
            const auto f = m.is_digital() ? tx.digital_frame(m, random_bytes(rng, m.image_bytes())) : tx.analog_frame(m, test_pattern(i));
            signal.insert(signal.end(), f.begin(), f.end());
        }
        return signal;
    };
    std::vector<float> a = burst(ma, na, "AA1AAA");
    const std::vector<float> b = burst(mb, nb, "BB2BBB");
    const double power = mean_power(a);
    if (cut > 0) a.resize(a.size() - static_cast<std::size_t>(ma.frame_samples()) + static_cast<std::size_t>(cut));

    std::vector<float> audio(3000, 0.0f);
    audio.insert(audio.end(), a.begin(), a.end());
    audio.insert(audio.end(), static_cast<std::size_t>(gap), 0.0f);
    audio.insert(audio.end(), b.begin(), b.end());
    audio.insert(audio.end(), static_cast<std::size_t>(std::max(ma.frame_samples(), mb.frame_samples()) + 18000), 0.0f);

    sim::ChannelParams params;
    params.snr_db = snr_db;
    params.add_noise = snr_db < 90.0;
    params.seed = seed * 1000 + static_cast<std::uint64_t>(gap);
    sim::Channel channel(params, power);
    std::vector<float> received;
    channel.process(audio, received);

    Receiver rx;
    TwoBursts result;
    for (const RxFrame& f : run_receiver(rx, received)) {
        const bool good = f.decoded && f.header_ok;
        if (good && f.callsign == "AA1AAA" && f.mode_id == ma.id) ++result.first;
        else if (good && f.callsign == "BB2BBB" && f.mode_id == mb.id) ++result.second;
        else if (!f.decoded) ++result.failed;
        else ++result.bogus;
    }
    return result;
}

}  // namespace link_util
