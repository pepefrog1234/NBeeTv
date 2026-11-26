// 端對端測試共用：發射 → 通道模擬 → 接收。
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "channel.hpp"
#include "learned_codec.hpp"
#include "model_file.hpp"
#include "modes.hpp"
#include "receiver.hpp"
#include "test_images.hpp"
#include "transmitter.hpp"

namespace link_util {

using namespace nbtv;

inline double mean_power(std::span<const float> x) {
    double acc = 0.0;
    for (float v : x) acc += static_cast<double>(v) * v;
    return x.empty() ? 0.0 : acc / static_cast<double>(x.size());
}

// 專案 models/ 底下每個模式各一份的權重，依模式表的順序；讀不到的那一格是空指標。只讀一次。
inline const std::vector<std::shared_ptr<const LearnedCodec>>& models() {
    static const std::vector<std::shared_ptr<const LearnedCodec>> loaded = [] {
        std::vector<std::shared_ptr<const LearnedCodec>> all;
        for (const Mode& m : kModes) {
            const std::string path = tools::default_model_path(m.id);
            std::vector<std::uint8_t> bytes;
            std::shared_ptr<const LearnedCodec> model;
            if (!path.empty() && tools::read_file(path, bytes)) model = LearnedCodec::load(bytes);
            if (model == nullptr && !path.empty()) std::printf("  找不到權重檔 %s\n", path.c_str());
            all.push_back(std::move(model));
        }
        return all;
    }();
    return loaded;
}

// 模式表裡每個需要模型的模式都有權重：沒有的話，用到模型的測試只能略過
inline bool have_models() {
    const auto& all = models();
    for (std::size_t i = 0; i < kModes.size(); ++i) {
        if (!tools::default_model_path(kModes[i].id).empty() && all[i] == nullptr) return false;
    }
    return true;
}

template <typename Endpoint>  // Transmitter 或 Receiver
void attach_models(Endpoint& endpoint) {
    for (const auto& model : models()) {
        if (model != nullptr) endpoint.set_model(model);
    }
}

struct LinkResult {
    std::vector<RxFrame> frames;
    RxStatus final_status;
    int decoded = 0;  // 有畫面、標頭也正確（呼號與模式都對）的幀數
};

// 把一段音訊（已含前後的靜音）交給接收機；每次餵 chunk 個取樣，模擬音訊裝置的節奏。
inline std::vector<RxFrame> run_receiver(Receiver& rx, std::span<const float> audio, std::size_t chunk = 1024) {
    std::vector<RxFrame> frames;
    for (std::size_t pos = 0; pos < audio.size(); pos += chunk) {
        rx.push(audio.subspan(pos, std::min(chunk, audio.size() - pos)));
        RxFrame f;
        while (rx.poll(f)) frames.push_back(std::move(f));
    }
    return frames;
}

// 發射 count 幀（同一張畫面），前面空 lead 個取樣、後面空 tail 個取樣，過通道後接收。
inline LinkResult run_link(const Mode& mode, int count, std::span<const std::uint8_t> rgb, const sim::ChannelParams& params,
                           int lead = 3000, int tail = 6000, bool noise_map = true) {
    Transmitter tx;
    tx.set_callsign("N0CALL");
    attach_models(tx);
    std::vector<float> signal;
    for (int i = 0; i < count; ++i) {
        const auto frame = tx.learned_frame(mode, rgb);
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
    attach_models(rx);
    LinkResult result;
    result.frames = run_receiver(rx, received);
    result.final_status = rx.status();
    for (const RxFrame& f : result.frames) {
        result.decoded += (f.decoded && f.header_ok && f.callsign == "N0CALL" && f.mode_id == mode.id) ? 1 : 0;
    }
    return result;
}

// 有畫面的幀對原圖的平均 PSNR；一張也沒有時是 0
inline double mean_psnr(const std::vector<RxFrame>& frames, const std::vector<std::uint8_t>& rgb) {
    double total = 0.0;
    int pictures = 0;
    for (const RxFrame& f : frames) {
        if (!f.decoded || f.rgb.size() != rgb.size()) continue;
        total += test_images::psnr(f.rgb, rgb);
        ++pictures;
    }
    return pictures > 0 ? total / pictures : 0.0;
}

// 一次發射的音訊：frames 幀，畫面輪流用三張測試圖，呼號是 call。同樣的參數只編一次（編碼要跑類神經網路）。
inline const std::vector<float>& burst(const Mode& mode, int frames, const std::string& call) {
    static std::map<std::pair<std::pair<int, int>, std::string>, std::vector<float>> cache;
    auto [slot, fresh] = cache.try_emplace({{mode.id, frames}, call});
    if (fresh) {
        Transmitter tx;
        tx.set_callsign(call);
        attach_models(tx);
        for (int i = 0; i < frames; ++i) {
            const auto f = tx.learned_frame(mode, test_images::load(test_images::kNames[i % 3], mode.image_width, mode.image_height));
            slot->second.insert(slot->second.end(), f.begin(), f.end());
        }
    }
    return slot->second;
}

struct TwoBursts {
    int first = 0;   // 第一次發射收到的幀數（有畫面、標頭正確）
    int second = 0;  // 第二次發射收到的幀數
    int bogus = 0;   // 不是任何一次發射送的、卻被當成畫面交出來的幀數（包括標頭對不上的畫面）
    int failed = 0;  // 「有一幀沒解出來」的通知次數
};

// 兩次發射（呼號不同）之間隔 gap 個取樣的靜音。cut > 0 時，第一次發射的最後一幀只送前 cut 個取樣（中途停止）。
// snr_db ≥ 90 表示不加雜訊，靜音就是完全的 0。
inline TwoBursts run_two_bursts(const Mode& ma, int na, const Mode& mb, int nb, int gap, int cut, double snr_db, std::uint64_t seed) {
    std::vector<float> a = burst(ma, na, "AA1AAA");
    const std::vector<float>& b = burst(mb, nb, "BB2BBB");
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
    attach_models(rx);
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
