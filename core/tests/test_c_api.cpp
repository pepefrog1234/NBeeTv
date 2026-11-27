// 只透過 C API 做完整的收發：外殼會用到的每個函式都走一遍。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "check.hpp"
#include "nbtv/nbtv.h"
#include "test_images.hpp"

namespace {

const double kPi = 3.14159265358979323846;

void test_helpers() {
    char call[NBTV_CALLSIGN_MAX + 1];
    CHECK_EQ(nbtv_callsign_normalize(" bx4acp ", call), 6);
    CHECK(std::strcmp(call, "BX4ACP") == 0);
    CHECK_EQ(nbtv_callsign_normalize("vk2/bx4acp/portable", call), 9);
    CHECK(std::strcmp(call, "VK2/BX4AC") == 0);
    CHECK_EQ(nbtv_callsign_normalize(nullptr, call), 0);

    CHECK(nbtv_tx_create(44100) == nullptr);
    CHECK(nbtv_rx_create(8000) == nullptr);
    CHECK(nbtv_spectrum_create(0) == nullptr);
    // 空指標不會出事
    nbtv_tx_destroy(nullptr);
    nbtv_rx_destroy(nullptr);
    nbtv_spectrum_destroy(nullptr);
    nbtv_rx_push(nullptr, nullptr, 0);
    CHECK_EQ(nbtv_tx_frame_samples(nullptr), 0);
    CHECK_EQ(nbtv_rx_poll_frame(nullptr, nullptr), 0);
}

// 專案 models/ 底下的檔案；找不到時是空的
std::vector<std::uint8_t> read_weights(const char* name) {
    std::vector<std::uint8_t> file;
    const std::string path = std::string(NBTV_MODEL_DIR) + "/" + name + ".bin";
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        std::uint8_t chunk[65536];
        for (std::size_t n = 0; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) file.insert(file.end(), chunk, chunk + n);
        std::fclose(f);
    }
    if (file.empty()) std::printf("  找不到權重檔 %s，略過用到它的測試\n", path.c_str());
    return file;
}

// 專案 models/ 底下的模型；找不到時回傳 NULL
nbtv_model* load_model(const char* name) {
    const std::vector<std::uint8_t> file = read_weights(name);
    if (file.empty()) return nullptr;
    nbtv_model* model = nbtv_model_load(file.data(), file.size());
    CHECK(model != nullptr);
    return model;
}

// 一個取樣率、一個模式的完整迴路：兩幀，前後各留一段靜音。min_psnr 是那張圖編了再解回來的下限。
void loopback(int rate, int mode_id, const char* weights, const char* image_name, double min_psnr) {
    nbtv_mode_info mode{};
    CHECK_EQ(nbtv_mode_by_id(mode_id, &mode), 0);
    // 畫面的大小跟著模式：測試圖是 96×72，模式 15 用的是每個像素重複兩次放大成 192×144 的
    const std::vector<std::uint8_t> rgb = test_images::load(image_name, mode.image_width, mode.image_height);
    const std::size_t rgb_bytes = static_cast<std::size_t>(mode.image_width) * static_cast<std::size_t>(mode.image_height) * 3;
    CHECK(rgb.size() == rgb_bytes && rgb_bytes <= NBTV_IMAGE_MAX_RGB_BYTES);
    nbtv_model* model = load_model(weights);
    if (model == nullptr) return;
    CHECK_EQ(nbtv_model_image_width(model), mode.image_width);
    CHECK_EQ(nbtv_model_image_height(model), mode.image_height);

    nbtv_tx* tx = nbtv_tx_create(rate);
    CHECK(tx != nullptr);
    CHECK_EQ(nbtv_tx_mode(tx), 11);  // 預設是模式表的第一個
    for (int gone : {0, 1, 2, 8, 9, 10, 16}) CHECK_EQ(nbtv_tx_set_mode(tx, gone), -1);  // 沒有定義，或已經移除的編號
    CHECK_EQ(nbtv_tx_mode(tx), 11);
    CHECK_EQ(nbtv_tx_set_mode(tx, mode_id), 0);
    CHECK_EQ(nbtv_tx_mode(tx), mode_id);
    nbtv_tx_set_callsign(tx, "n0call");
    nbtv_tx_set_model(tx, model);
    const int frame_samples = nbtv_tx_frame_samples(tx);
    CHECK_EQ(frame_samples, mode.frame_samples * (rate / 12000));

    std::vector<float> audio(static_cast<std::size_t>(rate / 4), 0.0f);  // 0.25 秒靜音
    std::vector<std::uint8_t> preview(rgb_bytes);
    std::vector<float> frame(static_cast<std::size_t>(frame_samples));
    nbtv_tx_frame_info info{};
    // 還沒編過：沒有通道值可以取
    std::vector<float> values(static_cast<std::size_t>(mode.learned_values));
    CHECK_EQ(nbtv_tx_last_values(tx, values.data(), mode.learned_values), -1);
    CHECK_EQ(nbtv_tx_encode_image(tx, rgb.data(), frame.data(), frame_samples - 1, nullptr, nullptr), -1);  // 容量不足
    CHECK_EQ(nbtv_tx_last_values(tx, values.data(), mode.learned_values), -1);                             // 失敗的那一次也沒有
    CHECK_EQ(nbtv_tx_encode_image(tx, nullptr, frame.data(), frame_samples, nullptr, nullptr), -1);
    CHECK_EQ(nbtv_tx_encode_image(tx, rgb.data(), nullptr, frame_samples, nullptr, nullptr), -1);
    for (int i = 0; i < 2; ++i) {
        CHECK_EQ(nbtv_tx_encode_image(tx, rgb.data(), frame.data(), frame_samples, preview.data(), &info), frame_samples);
        CHECK_EQ(info.mode_id, mode_id);
        CHECK_EQ(info.sequence, i);
        audio.insert(audio.end(), frame.begin(), frame.end());
    }
    std::vector<float> tail(256);
    const int tail_n = nbtv_tx_finish(tx, tail.data(), 256);
    CHECK(rate == 48000 ? tail_n > 0 : tail_n == 0);
    audio.insert(audio.end(), tail.begin(), tail.begin() + tail_n);
    audio.insert(audio.end(), static_cast<std::size_t>(rate / 2), 0.0f);
    float peak = 0;
    for (float v : audio) peak = std::max(peak, std::fabs(v));
    CHECK(peak <= 1.0f && peak > 0.5f);
    // 預覽是編碼後直接解回來的畫面
    CHECK(test_images::psnr(preview, rgb) > min_psnr);
    // 分開做也一樣：取出通道值，另外交給模型解，逐位元組相同（外殼把解碼放到別的執行緒上時走的是這條路）
    CHECK_EQ(nbtv_tx_last_values(tx, values.data(), mode.learned_values - 1), -1);  // 容量不足
    CHECK_EQ(nbtv_tx_last_values(tx, nullptr, mode.learned_values), -1);
    CHECK_EQ(nbtv_tx_last_values(nullptr, values.data(), mode.learned_values), -1);
    CHECK_EQ(nbtv_tx_last_values(tx, values.data(), mode.learned_values), mode.learned_values);
    double power = 0.0;
    for (float v : values) power += static_cast<double>(v) * v;
    CHECK_NEAR(power / mode.learned_values, 0.5, 1e-4);
    std::vector<std::uint8_t> apart(rgb_bytes, 0);
    CHECK_EQ(nbtv_model_decode(model, values.data(), mode.learned_values, apart.data()), 0);
    CHECK(apart == preview);
    CHECK_EQ(nbtv_model_decode(model, values.data(), mode.learned_values - 1, apart.data()), -1);  // 個數不對
    CHECK_EQ(nbtv_model_decode(nullptr, values.data(), mode.learned_values, apart.data()), -1);
    CHECK_EQ(nbtv_model_decode(model, nullptr, mode.learned_values, apart.data()), -1);
    CHECK_EQ(nbtv_model_decode(model, values.data(), mode.learned_values, nullptr), -1);

    nbtv_rx* rx = nbtv_rx_create(rate);
    nbtv_spectrum* sp = nbtv_spectrum_create(rate);
    CHECK(rx != nullptr && sp != nullptr);
    nbtv_rx_set_model(rx, model);
    nbtv_model_destroy(model);  // 發射機與接收機各自留著，之後照樣能用
    std::vector<nbtv_rx_frame> frames;
    bool saw_sync = false, progress_ok = true;
    int rows = 0;
    std::vector<float> row(NBTV_SPECTRUM_BINS);
    double band = 0, outside = 0;
    const std::size_t chunk = static_cast<std::size_t>(rate / 50);  // 20 ms 一塊，像音訊裝置的回呼
    for (std::size_t pos = 0; pos < audio.size(); pos += chunk) {
        const int n = static_cast<int>(std::min(chunk, audio.size() - pos));
        nbtv_rx_push(rx, audio.data() + pos, n);
        nbtv_spectrum_push(sp, audio.data() + pos, n);
        nbtv_rx_status st{};
        nbtv_rx_get_status(rx, &st);
        if (st.synced) {
            saw_sync = true;
            progress_ok = progress_ok && st.mode_id == mode_id && st.frame_symbols_total == mode.frame_symbols &&
                          st.frame_symbols_done >= 0 && st.frame_symbols_done <= st.frame_symbols_total;
        }
        nbtv_rx_frame f;
        while (nbtv_rx_poll_frame(rx, &f)) frames.push_back(f);
        while (nbtv_spectrum_poll(sp, row.data())) {
            ++rows;
            // 訊號頻帶 352–2648 Hz 是第 30–226 點；兩邊各多算兩點（分析窗的主瓣）
            for (int k = 0; k < NBTV_SPECTRUM_BINS; ++k) (k >= 28 && k <= 228 ? band : outside) += std::pow(10.0, row[static_cast<std::size_t>(k)] / 10.0);
        }
    }
    CHECK(saw_sync);
    CHECK(progress_ok);
    CHECK(rows > 20);
    // 能量集中在訊號頻帶內。符號之間沒有加窗，頻帶外還有 sinc 旁瓣，約佔 1%（SSB 發射機的濾波器會再濾掉）
    CHECK(band > 50.0 * outside);
    std::printf("  頻帶外（0–3 kHz 內、訊號頻帶以外）的功率比 %.1f dB\n", 10 * std::log10(outside / band));
    nbtv_rx_status st{};
    nbtv_rx_get_status(rx, &st);
    CHECK_EQ(st.synced, 0);
    CHECK_EQ(st.frames_decoded, 2);
    CHECK_EQ(st.frames_failed, 0);

    CHECK_EQ(static_cast<int>(frames.size()), 2);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const nbtv_rx_frame& f = frames[i];
        CHECK_EQ(f.mode_id, mode_id);
        CHECK_EQ(f.decoded, 1);
        CHECK_EQ(f.model_missing, 0);
        CHECK_EQ(f.header_ok, 1);
        CHECK(std::strcmp(f.callsign, "N0CALL") == 0);
        CHECK_EQ(f.sequence, static_cast<int>(i));
        CHECK_NEAR(f.freq_offset_hz, 0.0, 0.5);
        CHECK(f.snr_db > 15.0);
        CHECK(f.image_width == mode.image_width && f.image_height == mode.image_height);
        const std::vector<std::uint8_t> got(f.rgb, f.rgb + rgb_bytes);
        CHECK(std::all_of(f.rgb + rgb_bytes, f.rgb + sizeof f.rgb, [](std::uint8_t v) { return v == 0; }));  // 畫面之後的部分是 0
        CHECK(test_images::psnr(got, preview) > min_psnr + 2.0);  // 和預覽只差發射端的削波失真
        CHECK(test_images::psnr(got, rgb) > min_psnr - 0.6);
    }
    std::printf("  C API 迴路：%d Hz、模式 %d，%zu 幀，收到的畫面對原圖 PSNR %.1f dB\n", rate, mode_id, frames.size(),
                frames.empty() ? 0.0 : test_images::psnr(std::vector<std::uint8_t>(frames[0].rgb, frames[0].rgb + rgb_bytes), rgb));

    // 重設後可以再收一次（模型還在）
    nbtv_rx_reset(rx);
    nbtv_rx_push(rx, audio.data(), static_cast<int>(audio.size()));
    int again = 0;
    nbtv_rx_frame f;
    while (nbtv_rx_poll_frame(rx, &f)) again += f.decoded;
    CHECK_EQ(again, 2);

    nbtv_spectrum_destroy(sp);
    nbtv_rx_destroy(rx);
    nbtv_tx_destroy(tx);
}

void test_spectrum() {
    for (int rate : {12000, 48000}) {
        nbtv_spectrum* sp = nbtv_spectrum_create(rate);
        std::vector<float> tone(static_cast<std::size_t>(rate));  // 1 秒、1500 Hz、振幅 0.5（−6 dBFS）
        for (std::size_t i = 0; i < tone.size(); ++i) tone[i] = static_cast<float>(0.5 * std::sin(2 * kPi * 1500.0 * static_cast<double>(i) / rate));
        nbtv_spectrum_push(sp, tone.data(), static_cast<int>(tone.size()));
        std::vector<float> row(NBTV_SPECTRUM_BINS);
        int rows = 0;
        while (nbtv_spectrum_poll(sp, row.data())) {
            ++rows;
            if (rows < 3) continue;  // 跳過重取樣濾波器還沒填滿的開頭
            int peak = 0;
            for (int k = 1; k < NBTV_SPECTRUM_BINS; ++k) {
                if (row[static_cast<std::size_t>(k)] > row[static_cast<std::size_t>(peak)]) peak = k;
            }
            CHECK_EQ(peak, 128);  // 1500 Hz ÷ 11.72 Hz
            CHECK_NEAR(row[128], -6.0, 0.3);
            CHECK(row[20] < -60.0);
        }
        CHECK(rows >= 20 && rows <= 24);  // 每秒約 23 列
        nbtv_spectrum_reset(sp);
        CHECK_EQ(nbtv_spectrum_poll(sp, row.data()), 0);
        nbtv_spectrum_destroy(sp);
    }
}

// 推論用幾條執行緒、用哪一種寫法：設定與讀回；不管用幾條，編出來的聲音、預覽與解出來的畫面都逐位元相同
void test_inference_settings() {
    const int before = nbtv_inference_threads();
    CHECK(before >= 1 && before <= 64);
    const std::string kernel = nbtv_inference_kernel();
    CHECK(kernel == "generic" || kernel == "avx2");
    nbtv_set_inference_threads(3);
    CHECK_EQ(nbtv_inference_threads(), 3);
    nbtv_set_inference_threads(1000);
    CHECK_EQ(nbtv_inference_threads(), 64);
    nbtv_set_inference_threads(-1);
    CHECK_EQ(nbtv_inference_threads(), before);

    // 模式 15 的畫面是 192×144，一定會分給別的執行緒
    nbtv_mode_info mode{};
    CHECK_EQ(nbtv_mode_by_id(15, &mode), 0);
    nbtv_model* model = load_model("261125-3");
    if (model == nullptr) return;
    const std::vector<std::uint8_t> rgb = test_images::load("rocket", mode.image_width, mode.image_height);
    const std::size_t rgb_bytes = rgb.size();
    std::vector<float> audio[3];
    std::vector<std::uint8_t> preview[3], picture[3];
    const int settings[3] = {1, 2, 8};
    for (int round = 0; round < 3; ++round) {
        nbtv_set_inference_threads(settings[round]);
        nbtv_tx* tx = nbtv_tx_create(12000);
        nbtv_tx_set_mode(tx, 15);
        nbtv_tx_set_callsign(tx, "N0CALL");
        nbtv_tx_set_model(tx, model);
        const int n = nbtv_tx_frame_samples(tx);
        audio[round].assign(3000, 0.0f);
        std::vector<float> frame(static_cast<std::size_t>(n));
        preview[round].resize(rgb_bytes);
        CHECK_EQ(nbtv_tx_encode_image(tx, rgb.data(), frame.data(), n, preview[round].data(), nullptr), n);
        audio[round].insert(audio[round].end(), frame.begin(), frame.end());
        audio[round].insert(audio[round].end(), 6000, 0.0f);
        nbtv_tx_destroy(tx);

        nbtv_rx* rx = nbtv_rx_create(12000);
        nbtv_rx_set_model(rx, model);
        nbtv_rx_push(rx, audio[round].data(), static_cast<int>(audio[round].size()));
        nbtv_rx_frame f;
        while (nbtv_rx_poll_frame(rx, &f))
            if (f.decoded) picture[round].assign(f.rgb, f.rgb + rgb_bytes);
        nbtv_rx_destroy(rx);
        CHECK(picture[round].size() == rgb_bytes);
    }
    for (int round = 1; round < 3; ++round) {
        CHECK(audio[round] == audio[0]);
        CHECK(preview[round] == preview[0]);
        CHECK(picture[round] == picture[0]);
    }
    nbtv_model_destroy(model);
    nbtv_set_inference_threads(0);
    std::printf("  推論：%s，預設最多 %d 條執行緒；1、2、8 條的結果逐位元相同\n", kernel.c_str(), before);
}

// 來不及的時候先不解畫面（nbtv_rx_set_decode_pictures）：幀照樣回報，只是沒有畫面；再打開之後又有畫面，同步沒有斷。
// merge_weights 不是空指標的話，同時打開合併最近幾幀（略過的那一幀收到的值照樣記著，後面的幀合得進去）。
// 有導頻的幀（模式 12）與沒有導頻的幀（模式 5）在接收機裡是兩條路，各試一次。
void test_skip_pictures(int mode_id, const char* weights, const char* merge_weights, double min_psnr) {
    nbtv_rx_set_decode_pictures(nullptr, 0);  // 空指標不做事
    nbtv_mode_info mode{};
    CHECK_EQ(nbtv_mode_by_id(mode_id, &mode), 0);
    nbtv_model* model = load_model(weights);
    if (model == nullptr) return;
    const std::vector<std::uint8_t> merge_file = merge_weights != nullptr ? read_weights(merge_weights) : std::vector<std::uint8_t>();
    nbtv_merge* merge = merge_file.empty() ? nullptr : nbtv_merge_load(merge_file.data(), merge_file.size());
    const std::vector<std::uint8_t> rgb = test_images::load("coffee", mode.image_width, mode.image_height);
    const std::size_t rgb_bytes = rgb.size();

    const int rate = 12000;
    nbtv_tx* tx = nbtv_tx_create(rate);
    nbtv_tx_set_mode(tx, mode_id);
    nbtv_tx_set_callsign(tx, "N0CALL");
    nbtv_tx_set_model(tx, model);
    const int n = nbtv_tx_frame_samples(tx);
    std::vector<float> audio(3000, 0.0f), frame(static_cast<std::size_t>(n));
    for (int i = 0; i < 5; ++i) {
        CHECK_EQ(nbtv_tx_encode_image(tx, rgb.data(), frame.data(), n, nullptr, nullptr), n);
        audio.insert(audio.end(), frame.begin(), frame.end());
    }
    audio.insert(audio.end(), 6000, 0.0f);
    nbtv_tx_destroy(tx);

    nbtv_rx* rx = nbtv_rx_create(rate);
    nbtv_rx_set_model(rx, model);
    if (merge != nullptr) {
        nbtv_rx_set_merge(rx, merge);
        nbtv_rx_set_merging(rx, 1);
    }
    std::vector<nbtv_rx_frame> frames;
    for (std::size_t pos = 0; pos < audio.size(); pos += 240) {
        // 第一張收到之後關掉，第三張（略過了兩張）之後再打開
        nbtv_rx_set_decode_pictures(rx, frames.size() >= 1 && frames.size() < 3 ? 0 : 1);
        nbtv_rx_push(rx, audio.data() + pos, static_cast<int>(std::min<std::size_t>(240, audio.size() - pos)));
        nbtv_rx_frame f;
        while (nbtv_rx_poll_frame(rx, &f)) frames.push_back(f);
    }
    CHECK_EQ(static_cast<int>(frames.size()), 5);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const nbtv_rx_frame& f = frames[i];
        const bool skipped = i == 1 || i == 2;
        CHECK_EQ(f.mode_id, mode_id);
        CHECK_EQ(f.header_ok, 1);
        CHECK(std::strcmp(f.callsign, "N0CALL") == 0);
        CHECK_EQ(f.sequence, static_cast<int>(i));
        CHECK_EQ(f.model_missing, 0);
        CHECK_EQ(f.skipped, skipped ? 1 : 0);
        CHECK_EQ(f.decoded, skipped ? 0 : 1);
        CHECK(f.snr_db > 15.0);
        const std::vector<std::uint8_t> got(f.rgb, f.rgb + rgb_bytes);
        if (skipped) CHECK(std::all_of(got.begin(), got.end(), [](std::uint8_t v) { return v == 0; }));
        else CHECK(test_images::psnr(got, rgb) > min_psnr);
    }
    nbtv_rx_status st{};
    nbtv_rx_get_status(rx, &st);
    CHECK_EQ(st.frames_decoded, 3);
    CHECK_EQ(st.frames_skipped, 2);
    CHECK_EQ(st.frames_failed, 0);

    // 重設不會改變這個設定，只把計數歸零；沒有模型的模式照樣回報「模型不符」，不算略過
    nbtv_rx_set_decode_pictures(rx, 0);
    nbtv_rx_reset(rx);
    nbtv_rx_push(rx, audio.data(), static_cast<int>(audio.size()));
    int skipped = 0, decoded = 0;
    nbtv_rx_frame f;
    while (nbtv_rx_poll_frame(rx, &f)) {
        skipped += f.skipped;
        decoded += f.decoded;
    }
    CHECK_EQ(skipped, 5);
    CHECK_EQ(decoded, 0);
    nbtv_rx_set_model(rx, nullptr);
    nbtv_rx_reset(rx);
    nbtv_rx_push(rx, audio.data(), static_cast<int>(audio.size()));
    int missing = 0;
    skipped = 0;
    while (nbtv_rx_poll_frame(rx, &f)) {
        skipped += f.skipped;
        missing += f.model_missing;
    }
    CHECK_EQ(skipped, 0);
    CHECK_EQ(missing, 5);

    nbtv_rx_destroy(rx);
    if (merge != nullptr) nbtv_merge_destroy(merge);
    nbtv_model_destroy(model);
}

// 取消已經編好、但決定不送的那一幀：之後的尾巴與幀序號要和「從來沒編過那一幀」完全一樣。
void test_discard_last() {
    const std::vector<std::uint8_t> a = test_images::load("coffee"), b = test_images::load("rocket");
    nbtv_model* model = load_model("261122-3");
    if (model == nullptr) return;
    for (int rate : {48000, 12000}) {
        nbtv_tx* tx = nbtv_tx_create(rate);
        nbtv_tx* ref = nbtv_tx_create(rate);
        for (nbtv_tx* t : {tx, ref}) {
            nbtv_tx_set_mode(t, 12);
            nbtv_tx_set_callsign(t, "N0CALL");
            nbtv_tx_set_model(t, model);
        }
        CHECK_EQ(nbtv_tx_discard_last(tx), -1);  // 還沒編過
        CHECK_EQ(nbtv_tx_discard_last(nullptr), -1);
        const int n = nbtv_tx_frame_samples(tx);
        std::vector<float> first(static_cast<std::size_t>(n)), second(first.size()), expect(first.size());
        nbtv_tx_frame_info info{};
        // 對照組：只編第一幀就結束
        CHECK_EQ(nbtv_tx_encode_image(ref, a.data(), expect.data(), n, nullptr, nullptr), n);
        std::vector<float> ref_tail(256), tail(256);
        const int ref_n = nbtv_tx_finish(ref, ref_tail.data(), 256);

        CHECK_EQ(nbtv_tx_encode_image(tx, a.data(), first.data(), n, nullptr, &info), n);
        CHECK_EQ(info.sequence, 0);
        CHECK_EQ(nbtv_tx_encode_image(tx, b.data(), second.data(), n, nullptr, &info), n);
        CHECK_EQ(info.sequence, 1);
        CHECK(first == expect);
        std::vector<float> values(2592);  // 模式 12 的一張畫面
        std::vector<std::uint8_t> seen(a.size());
        CHECK_EQ(nbtv_tx_last_values(tx, values.data(), 2592), 2592);  // 第二幀（火箭）的通道值
        CHECK_EQ(nbtv_model_decode(model, values.data(), 2592, seen.data()), 0);
        CHECK(test_images::psnr(seen, b) > 24.8 && test_images::psnr(seen, a) < 15.0);
        CHECK_EQ(nbtv_tx_discard_last(tx), 0);
        CHECK_EQ(nbtv_tx_discard_last(tx), -1);  // 只能取消一次
        CHECK_EQ(nbtv_tx_last_values(tx, values.data(), 2592), -1);  // 取消的那一幀，通道值也不再給
        const int tail_n = nbtv_tx_finish(tx, tail.data(), 256);
        CHECK_EQ(tail_n, ref_n);
        CHECK(std::equal(tail.begin(), tail.begin() + tail_n, ref_tail.begin()));
        if (rate == 48000) {
            // 沒取消的話，尾巴是第二幀的；確認這個測試真的分得出來
            CHECK(tail_n > 0);
            CHECK(!std::equal(tail.begin(), tail.begin() + tail_n, second.begin()));
        }
        // 被取消的那一幀的序號會再用一次；重新編出來的和原本那一幀一模一樣（濾波器也回到原來的狀態）
        CHECK_EQ(nbtv_tx_encode_image(tx, a.data(), first.data(), n, nullptr, &info), n);
        CHECK_EQ(info.sequence, 1);
        CHECK_EQ(nbtv_tx_last_values(tx, values.data(), 2592), 2592);  // 重新編了一張（咖啡）：給的是這一張的
        CHECK_EQ(nbtv_model_decode(model, values.data(), 2592, seen.data()), 0);
        CHECK(test_images::psnr(seen, a) > 23.5);
        std::vector<float> again(first.size());
        CHECK_EQ(nbtv_tx_encode_image(tx, b.data(), again.data(), n, nullptr, &info), n);
        CHECK_EQ(info.sequence, 2);
        CHECK_EQ(nbtv_tx_discard_last(tx), 0);
        CHECK_EQ(nbtv_tx_encode_image(tx, b.data(), second.data(), n, nullptr, &info), n);
        CHECK_EQ(info.sequence, 2);
        CHECK(second == again);
        // finish 之後沒有可以取消的幀
        nbtv_tx_finish(tx, tail.data(), 256);
        CHECK_EQ(nbtv_tx_discard_last(tx), -1);
        nbtv_tx_destroy(tx);
        nbtv_tx_destroy(ref);
    }
    nbtv_model_destroy(model);
}

// 模型的各種狀況：還沒設定、設定之後、取消一幀、接收端沒有模型、重設、拿掉。min_psnr 是「貓」那張圖編了再解回來的下限。
void test_model_handling(int mode_id, const char* weights, double min_psnr) {
    // 沒有模型、不是模型
    CHECK(nbtv_model_load(nullptr, 0) == nullptr);
    const std::uint8_t junk[64] = {};
    CHECK(nbtv_model_load(junk, sizeof junk) == nullptr);
    nbtv_model_destroy(nullptr);
    CHECK_EQ(nbtv_model_id(nullptr), 0);
    CHECK_EQ(nbtv_model_values(nullptr), 0);
    CHECK_EQ(nbtv_model_width(nullptr), 0);
    CHECK_EQ(nbtv_model_image_width(nullptr), 0);
    CHECK_EQ(nbtv_model_image_height(nullptr), 0);
    nbtv_tx_set_model(nullptr, nullptr);
    nbtv_rx_set_model(nullptr, nullptr);

    nbtv_mode_info mode{};
    CHECK_EQ(nbtv_mode_by_id(mode_id, &mode), 0);
    CHECK(mode.learned_values > 0 && mode.learned_values <= mode.value_capacity);

    nbtv_model* model = load_model(weights);
    if (model == nullptr) return;
    CHECK_EQ(nbtv_model_values(model), mode.learned_values);
    CHECK_EQ(nbtv_model_width(model), mode.learned_width);
    CHECK_EQ(nbtv_model_image_width(model), mode.image_width);
    CHECK_EQ(nbtv_model_image_height(model), mode.image_height);
    CHECK(nbtv_model_id(model) != 0);

    const std::vector<std::uint8_t> rgb = test_images::load("cat", mode.image_width, mode.image_height);
    const std::size_t rgb_bytes = rgb.size();
    const int rate = 48000;
    nbtv_tx* tx = nbtv_tx_create(rate);
    nbtv_tx_set_callsign(tx, "n0call");
    CHECK_EQ(nbtv_tx_set_mode(tx, mode_id), 0);
    const int frame_samples = nbtv_tx_frame_samples(tx);
    CHECK_EQ(frame_samples, mode.frame_samples * 4);
    std::vector<float> frame(static_cast<std::size_t>(frame_samples));
    std::vector<std::uint8_t> preview(rgb_bytes);
    nbtv_tx_frame_info info{};

    // 還沒設定模型：編不出幀，也沒有可以取消的幀
    CHECK_EQ(nbtv_tx_encode_image(tx, rgb.data(), frame.data(), frame_samples, preview.data(), &info), -1);
    CHECK_EQ(nbtv_tx_discard_last(tx), -1);

    nbtv_rx* rx = nbtv_rx_create(rate);
    nbtv_rx* bare = nbtv_rx_create(rate);  // 這一個不設定模型
    nbtv_tx_set_model(tx, model);
    nbtv_rx_set_model(rx, model);
    nbtv_model_destroy(model);  // 發射機與接收機各自留著，之後照樣能用

    std::vector<float> audio(static_cast<std::size_t>(rate / 4), 0.0f);
    for (int i = 0; i < 2; ++i) {
        CHECK_EQ(nbtv_tx_encode_image(tx, rgb.data(), frame.data(), frame_samples, preview.data(), &info), frame_samples);
        CHECK_EQ(info.mode_id, mode_id);
        CHECK_EQ(info.sequence, i);
        audio.insert(audio.end(), frame.begin(), frame.end());
    }
    CHECK(test_images::psnr(preview, rgb) > min_psnr);  // 預覽是編碼後直接解回來的畫面
    // 多編一幀再取消：幀序號回到 2
    CHECK_EQ(nbtv_tx_encode_image(tx, rgb.data(), frame.data(), frame_samples, nullptr, &info), frame_samples);
    CHECK_EQ(nbtv_tx_discard_last(tx), 0);
    CHECK_EQ(nbtv_tx_encode_image(tx, rgb.data(), frame.data(), frame_samples, nullptr, &info), frame_samples);
    CHECK_EQ(info.sequence, 2);
    CHECK_EQ(nbtv_tx_discard_last(tx), 0);
    std::vector<float> tail(256);
    const int tail_n = nbtv_tx_finish(tx, tail.data(), 256);
    audio.insert(audio.end(), tail.begin(), tail.begin() + tail_n);
    audio.insert(audio.end(), static_cast<std::size_t>(rate / 2), 0.0f);

    for (nbtv_rx* r : {rx, bare}) {
        for (std::size_t pos = 0; pos < audio.size(); pos += 4096) {
            nbtv_rx_push(r, audio.data() + pos, static_cast<int>(std::min<std::size_t>(4096, audio.size() - pos)));
        }
    }
    nbtv_rx_frame f;
    int n = 0;
    double quality = 0.0;
    while (nbtv_rx_poll_frame(rx, &f)) {
        CHECK_EQ(f.mode_id, mode_id);
        CHECK_EQ(f.decoded, 1);
        CHECK_EQ(f.model_missing, 0);
        CHECK_EQ(f.header_ok, 1);
        CHECK(std::strcmp(f.callsign, "N0CALL") == 0);
        CHECK_EQ(f.sequence, n);
        CHECK(f.image_width == mode.image_width && f.image_height == mode.image_height);
        quality = test_images::psnr(std::vector<std::uint8_t>(f.rgb, f.rgb + rgb_bytes), rgb);
        CHECK(quality > min_psnr);
        ++n;
    }
    CHECK_EQ(n, 2);
    std::printf("  C API 迴路：%d Hz、模式 %d，%d 幀，收到的畫面對原圖 PSNR %.1f dB\n", rate, mode_id, n, quality);
    // 沒有模型的接收機：收得到幀與呼號，但沒有畫面
    n = 0;
    while (nbtv_rx_poll_frame(bare, &f)) {
        CHECK_EQ(f.decoded, 0);
        CHECK_EQ(f.model_missing, 1);
        CHECK(std::all_of(f.rgb, f.rgb + sizeof f.rgb, [](std::uint8_t v) { return v == 0; }));  // 沒有畫面時 rgb 是 0
        CHECK(f.image_width == mode.image_width && f.image_height == mode.image_height);          // 畫面多大還是知道的
        CHECK_EQ(f.header_ok, 1);
        CHECK(std::strcmp(f.callsign, "N0CALL") == 0);
        ++n;
    }
    CHECK_EQ(n, 2);
    nbtv_rx_status st{};
    nbtv_rx_get_status(rx, &st);
    CHECK_EQ(st.frames_decoded, 2);
    nbtv_rx_get_status(bare, &st);
    CHECK_EQ(st.frames_decoded, 0);
    CHECK_EQ(st.frames_failed, 2);

    // 重設之後模型還在；拿掉模型之後就又編不出來、解不出畫面
    nbtv_rx_reset(rx);
    nbtv_rx_push(rx, audio.data(), static_cast<int>(audio.size()));
    n = 0;
    while (nbtv_rx_poll_frame(rx, &f)) n += f.decoded;
    CHECK_EQ(n, 2);
    nbtv_tx_set_model(tx, nullptr);
    CHECK_EQ(nbtv_tx_encode_image(tx, rgb.data(), frame.data(), frame_samples, nullptr, nullptr), -1);
    nbtv_rx_set_model(rx, nullptr);
    nbtv_rx_reset(rx);
    nbtv_rx_push(rx, audio.data(), static_cast<int>(audio.size()));
    n = 0;
    while (nbtv_rx_poll_frame(rx, &f)) n += f.model_missing;
    CHECK_EQ(n, 2);

    nbtv_rx_destroy(bare);
    nbtv_rx_destroy(rx);
    nbtv_tx_destroy(tx);
}

// 兩個模式各有自己的模型：發射機、接收機都設定兩個，一次發射裡換模式，各用各的模型
// 合併最近幾幀：同一張畫面在很弱的訊號下連續送 5 幀，接收機打開合併之後，後面的幀比不合併的接近原圖。
// 這個測試只用 C API，雜訊自己產生（固定的種子，每次的結果相同）。
void test_merging(int mode_id, const char* weights, const char* merge_weights) {
    // 沒有權重、不是這種權重檔、空指標
    CHECK(nbtv_merge_load(nullptr, 0) == nullptr);
    const std::uint8_t junk[64] = {};
    CHECK(nbtv_merge_load(junk, sizeof junk) == nullptr);
    nbtv_merge_destroy(nullptr);
    CHECK_EQ(nbtv_merge_model_id(nullptr), 0);
    CHECK_EQ(nbtv_merge_frames(nullptr), 0);
    nbtv_rx_set_merge(nullptr, nullptr);
    nbtv_rx_set_merging(nullptr, 1);

    const std::vector<std::uint8_t> merge_file = read_weights(merge_weights);
    nbtv_model* model = merge_file.empty() ? nullptr : load_model(weights);
    if (model == nullptr) return;
    // 畫面模型的權重檔不是合併用的
    const std::vector<std::uint8_t> model_file = read_weights(weights);
    CHECK(nbtv_merge_load(model_file.data(), model_file.size()) == nullptr);
    nbtv_merge* merge = nbtv_merge_load(merge_file.data(), merge_file.size());
    CHECK(merge != nullptr);
    if (merge == nullptr) {
        nbtv_model_destroy(model);
        return;
    }
    CHECK_EQ(nbtv_merge_model_id(merge), nbtv_model_id(model));  // 配的就是這個模式的模型
    CHECK_EQ(nbtv_merge_frames(merge), 4);

    const std::vector<std::uint8_t> rgb = test_images::load("cat");
    const int rate = 12000, count = 5;
    nbtv_tx* tx = nbtv_tx_create(rate);
    nbtv_tx_set_callsign(tx, "n0call");
    CHECK_EQ(nbtv_tx_set_mode(tx, mode_id), 0);
    nbtv_tx_set_model(tx, model);
    const int frame_samples = nbtv_tx_frame_samples(tx);
    std::vector<float> frame(static_cast<std::size_t>(frame_samples)), signal;
    for (int i = 0; i < count; ++i) {
        CHECK_EQ(nbtv_tx_encode_image(tx, rgb.data(), frame.data(), frame_samples, nullptr, nullptr), frame_samples);
        signal.insert(signal.end(), frame.begin(), frame.end());
    }
    nbtv_tx_destroy(tx);
    // 白雜訊，訊雜比 3 dB（訊號的平均功率 ÷ 3 kHz 頻寬內的雜訊功率）
    double power = 0;
    for (float v : signal) power += static_cast<double>(v) * v;
    power /= static_cast<double>(signal.size());
    const double sigma = std::sqrt(power / std::pow(10.0, 0.3) * (rate / 2.0) / 3000.0);
    std::vector<float> audio(static_cast<std::size_t>(rate / 4), 0.0f);
    audio.insert(audio.end(), signal.begin(), signal.end());
    audio.insert(audio.end(), static_cast<std::size_t>(rate / 2), 0.0f);
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    const auto uniform = [&state] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return (static_cast<double>(state >> 11) + 0.5) / 9007199254740992.0;
    };
    for (float& v : audio) {
        // 兩個亂數分開取：寫在同一個式子裡的話，先算乘號哪一邊 C++ 沒有規定，不同的編譯器做出來的雜訊就不一樣
        const double u1 = uniform();
        const double u2 = uniform();
        v += static_cast<float>(sigma * std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2));
    }

    // 同一段音訊收一次，回傳各幀的畫面（沒解出來的是空的）
    const auto receive = [&](nbtv_rx* rx) {
        std::vector<std::vector<std::uint8_t>> pictures;
        const std::size_t chunk = static_cast<std::size_t>(rate / 50);
        for (std::size_t pos = 0; pos < audio.size(); pos += chunk) {
            nbtv_rx_push(rx, audio.data() + pos, static_cast<int>(std::min(chunk, audio.size() - pos)));
            nbtv_rx_frame f;
            while (nbtv_rx_poll_frame(rx, &f))
                pictures.push_back(f.decoded ? std::vector<std::uint8_t>(f.rgb, f.rgb + NBTV_IMAGE_RGB_BYTES) : std::vector<std::uint8_t>());
        }
        return pictures;
    };
    // 第 4 幀起（前面 3 幀都在）對原圖的 PSNR 的平均；幀數不對或有沒解出來的回傳 0
    const auto late_psnr = [&](const std::vector<std::vector<std::uint8_t>>& pictures) {
        if (static_cast<int>(pictures.size()) != count) return 0.0;
        double total = 0;
        for (int i = 3; i < count; ++i) {
            if (pictures[static_cast<std::size_t>(i)].empty()) return 0.0;
            total += test_images::psnr(pictures[static_cast<std::size_t>(i)], rgb);
        }
        return total / (count - 3);
    };

    nbtv_rx* rx = nbtv_rx_create(rate);
    nbtv_rx_set_model(rx, model);
    nbtv_model_destroy(model);
    const auto plain = receive(rx);
    CHECK_EQ(static_cast<int>(plain.size()), count);
    CHECK(late_psnr(plain) > 0.0);

    // 設定了權重但沒有打開：和原來一模一樣
    nbtv_rx_set_merge(rx, merge);
    nbtv_merge_destroy(merge);  // 接收機自己留著，之後照樣能用
    nbtv_rx_reset(rx);
    CHECK(receive(rx) == plain);

    // 打開：第一幀前面沒有東西可以合，和原來一樣；後面的幀比較接近原圖
    nbtv_rx_set_merging(rx, 1);
    nbtv_rx_reset(rx);
    const auto merged = receive(rx);
    CHECK_EQ(static_cast<int>(merged.size()), count);
    if (!plain.empty() && !merged.empty()) CHECK(merged[0] == plain[0]);
    CHECK(late_psnr(merged) > late_psnr(plain) + 0.3);
    // 重設不會改變這個設定，只會忘掉前面收到的幀：再收一次，結果相同
    nbtv_rx_reset(rx);
    CHECK(receive(rx) == merged);

    // 關掉、或把權重拿掉：又和原來一樣
    nbtv_rx_set_merging(rx, 0);
    nbtv_rx_reset(rx);
    CHECK(receive(rx) == plain);
    nbtv_rx_set_merging(rx, 1);
    nbtv_rx_set_merge(rx, nullptr);
    nbtv_rx_reset(rx);
    CHECK(receive(rx) == plain);
    std::printf("  C API 合併最近幾幀：模式 %d、白雜訊 3 dB、同一張畫面，第 4–5 幀對原圖 PSNR %.2f dB（不合併 %.2f dB）\n", mode_id,
                late_psnr(merged), late_psnr(plain));
    nbtv_rx_destroy(rx);
}

void test_two_models() {
    nbtv_model* models[2] = {load_model("261123-1"), load_model("261122-3")};
    if (models[0] == nullptr || models[1] == nullptr) {
        for (nbtv_model* m : models) nbtv_model_destroy(m);
        return;
    }
    CHECK(nbtv_model_values(models[0]) != nbtv_model_values(models[1]));
    const int rate = 12000;
    nbtv_tx* tx = nbtv_tx_create(rate);
    nbtv_rx* rx = nbtv_rx_create(rate);
    nbtv_rx* partial = nbtv_rx_create(rate);  // 這一個只有模式 12 的模型
    for (nbtv_model* m : models) {
        nbtv_tx_set_model(tx, m);
        nbtv_rx_set_model(rx, m);
    }
    nbtv_rx_set_model(partial, models[1]);
    nbtv_rx_set_model(partial, models[1]);  // 同一個再設定一次沒有影響
    for (nbtv_model* m : models) nbtv_model_destroy(m);

    const std::vector<std::uint8_t> rgb = test_images::load("cat");
    const int ids[] = {12, 11, 12};
    std::vector<float> audio(static_cast<std::size_t>(rate / 4), 0.0f);
    for (int id : ids) {
        CHECK_EQ(nbtv_tx_set_mode(tx, id), 0);
        std::vector<float> frame(static_cast<std::size_t>(nbtv_tx_frame_samples(tx)));
        nbtv_tx_frame_info info{};
        CHECK_EQ(nbtv_tx_encode_image(tx, rgb.data(), frame.data(), static_cast<int>(frame.size()), nullptr, &info),
                 static_cast<int>(frame.size()));
        CHECK_EQ(info.mode_id, id);
        audio.insert(audio.end(), frame.begin(), frame.end());
    }
    audio.insert(audio.end(), static_cast<std::size_t>(rate / 2), 0.0f);
    for (nbtv_rx* r : {rx, partial}) nbtv_rx_push(r, audio.data(), static_cast<int>(audio.size()));

    nbtv_rx_frame f;
    int n = 0;
    while (nbtv_rx_poll_frame(rx, &f)) {
        if (n < 3) CHECK_EQ(f.mode_id, ids[n]);
        CHECK_EQ(f.decoded, 1);
        CHECK_EQ(f.model_missing, 0);
        CHECK_EQ(f.sequence, n);
        // 用錯模型的話解出來的是雜訊（10 dB 以下）
        CHECK(test_images::psnr(std::vector<std::uint8_t>(f.rgb, f.rgb + NBTV_IMAGE_RGB_BYTES), rgb) > 20.0);
        ++n;
    }
    CHECK_EQ(n, 3);
    n = 0;
    while (nbtv_rx_poll_frame(partial, &f)) {
        if (n < 3) {
            CHECK_EQ(f.mode_id, ids[n]);
            CHECK_EQ(f.decoded, ids[n] == 12 ? 1 : 0);
            CHECK_EQ(f.model_missing, ids[n] == 12 ? 0 : 1);
            CHECK_EQ(f.header_ok, 1);
        }
        ++n;
    }
    CHECK_EQ(n, 3);
    // 全部拿掉之後，兩個模式都編不出幀
    nbtv_tx_set_model(tx, nullptr);
    std::vector<float> frame(static_cast<std::size_t>(rate * 3));
    for (int id : {11, 12}) {
        CHECK_EQ(nbtv_tx_set_mode(tx, id), 0);
        CHECK_EQ(nbtv_tx_encode_image(tx, rgb.data(), frame.data(), static_cast<int>(frame.size()), nullptr, nullptr), -1);
    }
    nbtv_rx_destroy(partial);
    nbtv_rx_destroy(rx);
    nbtv_tx_destroy(tx);
}

// 模式 11 與 13 的實數一樣多，模型的寬度不同：各用各的模型，設定其中一個不會讓另一個模式也能用
void test_two_widths() {
    nbtv_model* narrow = load_model("261123-1");
    nbtv_model* wide = load_model("261122-4");
    if (narrow == nullptr || wide == nullptr) {
        nbtv_model_destroy(narrow);
        nbtv_model_destroy(wide);
        return;
    }
    CHECK_EQ(nbtv_model_values(narrow), nbtv_model_values(wide));
    CHECK(nbtv_model_width(narrow) != nbtv_model_width(wide));
    CHECK(nbtv_model_id(narrow) != nbtv_model_id(wide));

    const int rate = 12000;
    const std::vector<std::uint8_t> rgb = test_images::load("cat");
    nbtv_tx* tx = nbtv_tx_create(rate);
    nbtv_rx* rx = nbtv_rx_create(rate);
    nbtv_rx* partial = nbtv_rx_create(rate);  // 這一個只有模式 11 的模型
    nbtv_tx_set_model(tx, narrow);
    CHECK_EQ(nbtv_tx_set_mode(tx, 13), 0);
    std::vector<float> frame(static_cast<std::size_t>(nbtv_tx_frame_samples(tx)));
    // 只有模式 11 的模型：模式 13 編不出幀
    CHECK_EQ(nbtv_tx_encode_image(tx, rgb.data(), frame.data(), static_cast<int>(frame.size()), nullptr, nullptr), -1);
    nbtv_tx_set_model(tx, wide);  // 不會取代模式 11 的那一個
    nbtv_rx_set_model(rx, narrow);
    nbtv_rx_set_model(rx, wide);
    nbtv_rx_set_model(partial, narrow);
    nbtv_model_destroy(narrow);
    nbtv_model_destroy(wide);

    const int ids[] = {13, 11};
    std::vector<float> audio(static_cast<std::size_t>(rate / 4), 0.0f);
    for (int id : ids) {
        CHECK_EQ(nbtv_tx_set_mode(tx, id), 0);
        nbtv_tx_frame_info info{};
        CHECK_EQ(nbtv_tx_encode_image(tx, rgb.data(), frame.data(), static_cast<int>(frame.size()), nullptr, &info),
                 static_cast<int>(frame.size()));
        CHECK_EQ(info.mode_id, id);
        audio.insert(audio.end(), frame.begin(), frame.end());
    }
    audio.insert(audio.end(), static_cast<std::size_t>(rate / 2), 0.0f);
    for (nbtv_rx* r : {rx, partial}) nbtv_rx_push(r, audio.data(), static_cast<int>(audio.size()));

    nbtv_rx_frame f;
    int n = 0;
    while (nbtv_rx_poll_frame(rx, &f)) {
        if (n < 2) CHECK_EQ(f.mode_id, ids[n]);
        CHECK_EQ(f.decoded, 1);
        CHECK_EQ(f.model_missing, 0);
        // 用錯模型的話解出來的是雜訊（10 dB 以下）
        CHECK(test_images::psnr(std::vector<std::uint8_t>(f.rgb, f.rgb + NBTV_IMAGE_RGB_BYTES), rgb) > 28.0);
        ++n;
    }
    CHECK_EQ(n, 2);
    n = 0;
    while (nbtv_rx_poll_frame(partial, &f)) {
        if (n < 2) {
            CHECK_EQ(f.mode_id, ids[n]);
            CHECK_EQ(f.decoded, ids[n] == 11 ? 1 : 0);
            CHECK_EQ(f.model_missing, ids[n] == 11 ? 0 : 1);
            CHECK_EQ(f.header_ok, 1);
        }
        ++n;
    }
    CHECK_EQ(n, 2);
    nbtv_rx_destroy(partial);
    nbtv_rx_destroy(rx);
    nbtv_tx_destroy(tx);
}

// 壞掉的取樣（NaN、無限大、極大的值）不可以把接收機弄壞：以前只要一個 NaN，搜尋同步訊號用的滑動平均就永遠是 NaN，
// 之後什麼幀都收不到；音量與頻譜的讀數也變成非數值，外殼把它們轉成整數時會當掉。
void test_bad_samples(int rate) {
    nbtv_model* model = load_model("261122-3");
    if (model == nullptr) return;
    nbtv_tx* tx = nbtv_tx_create(rate);
    nbtv_rx* rx = nbtv_rx_create(rate);
    nbtv_spectrum* sp = nbtv_spectrum_create(rate);
    nbtv_tx_set_model(tx, model);
    nbtv_rx_set_model(rx, model);
    nbtv_model_destroy(model);
    CHECK_EQ(nbtv_tx_set_mode(tx, 12), 0);
    nbtv_tx_set_callsign(tx, "N0CALL");
    const std::vector<std::uint8_t> rgb = test_images::load("rocket");
    std::vector<float> frame(static_cast<std::size_t>(nbtv_tx_frame_samples(tx)));
    CHECK_EQ(nbtv_tx_encode_image(tx, rgb.data(), frame.data(), static_cast<int>(frame.size()), nullptr, nullptr), static_cast<int>(frame.size()));

    std::uint32_t seed = 12345;
    const auto noise = [&seed](std::size_t n) {
        std::vector<float> x(n);
        for (float& v : x) {
            seed = seed * 1664525u + 1013904223u;
            v = (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) * 0.01f;
        }
        return x;
    };
    int decoded = 0, rows = 0;
    bool finite = true;
    std::vector<float> row(NBTV_SPECTRUM_BINS);
    const auto feed = [&](const std::vector<float>& x) {
        const std::size_t chunk = static_cast<std::size_t>(rate / 50);
        for (std::size_t pos = 0; pos < x.size(); pos += chunk) {
            const int n = static_cast<int>(std::min(chunk, x.size() - pos));
            nbtv_rx_push(rx, x.data() + pos, n);
            nbtv_spectrum_push(sp, x.data() + pos, n);
            nbtv_rx_status st{};
            nbtv_rx_get_status(rx, &st);
            finite = finite && std::isfinite(st.input_level_db) && std::isfinite(st.freq_offset_hz) && (!st.snr_valid || std::isfinite(st.snr_db));
            nbtv_rx_frame f;
            while (nbtv_rx_poll_frame(rx, &f)) {
                decoded += f.decoded;
                finite = finite && std::isfinite(f.snr_db) && std::isfinite(f.freq_offset_hz);
            }
            while (nbtv_spectrum_poll(sp, row.data())) {
                ++rows;
                for (float v : row) finite = finite && std::isfinite(v);
            }
        }
    };

    feed(noise(static_cast<std::size_t>(rate)));
    feed(frame);
    feed(noise(static_cast<std::size_t>(rate)));
    CHECK_EQ(decoded, 1);

    // 各種壞值：單獨一個、連續一段，夾在雜訊中間
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    const float big = std::numeric_limits<float>::max();
    for (float bad : {nan, inf, -inf, big, -big, 1e30f}) {
        std::vector<float> x = noise(static_cast<std::size_t>(rate) / 2);
        x[x.size() / 3] = bad;
        std::fill(x.begin() + static_cast<std::ptrdiff_t>(x.size() / 2), x.begin() + static_cast<std::ptrdiff_t>(x.size() / 2) + 200, bad);
        feed(x);
    }
    // 幀的中間也有壞值：這一幀解不解得出來都可以，但不可以交出非有限的讀數
    std::vector<float> damaged = frame;
    damaged[damaged.size() / 2] = nan;
    damaged[damaged.size() / 2 + 1] = inf;
    feed(noise(static_cast<std::size_t>(rate) / 2));
    feed(damaged);
    // 連續一長串極大的值（被限制在 ±100）會讓搜尋同步訊號用的平均值偏掉幾秒（量到約 6 秒），之後自己恢復：等 10 秒
    feed(noise(static_cast<std::size_t>(rate) * 10));
    const int before = decoded;

    // 之後照樣收得到幀，讀數也都是有限值
    feed(frame);
    feed(noise(static_cast<std::size_t>(rate)));
    CHECK_EQ(decoded, before + 1);
    CHECK(finite);
    CHECK(rows > 100);
    nbtv_rx_status st{};
    nbtv_rx_get_status(rx, &st);
    CHECK(st.input_level_db < -30.0f);  // 音量的讀數也回到只有雜訊時該有的大小

    // 只有非有限值（沒有極大的有限值）的話不用等：下一幀馬上收得到
    nbtv_rx_reset(rx);
    feed(noise(static_cast<std::size_t>(rate)));
    feed(std::vector<float>(300, nan));
    feed(std::vector<float>(300, inf));
    const int clean_start = decoded;
    feed(frame);
    feed(noise(static_cast<std::size_t>(rate)));
    CHECK_EQ(decoded, clean_start + 1);
    CHECK(finite);
    std::printf("  壞掉的取樣（NaN、無限大、極大值）：%d Hz，之後照樣收到幀，讀數都是有限值（音量 %.1f dB）\n", rate, static_cast<double>(st.input_level_db));

    nbtv_spectrum_destroy(sp);
    nbtv_rx_destroy(rx);
    nbtv_tx_destroy(tx);
}

}  // namespace

// 語音與畫面共用 FM 語音通道，即時用的那一種接法：發射機直接給畫面那一半，語音那一半另外做、播放時相加；
// 接收機直接收合成的訊號。語音用一個單音代替（核心不含語音的編解碼器）。
void test_duplex(int mode_id, const char* weights, double min_psnr) {
    // 不對的參數、空指標
    CHECK(nbtv_duplex_tx_create(2) == nullptr);
    CHECK(nbtv_duplex_rx_create(NBTV_DUPLEX_OFF) == nullptr);
    CHECK_NEAR(nbtv_duplex_slow_factor(7), 0.0, 0.0);
    CHECK_NEAR(nbtv_duplex_slow_factor(NBTV_DUPLEX_STANDARD), 1.5, 1e-12);
    CHECK_NEAR(nbtv_duplex_picture_high_hz(NBTV_DUPLEX_NARROW), 2610.56, 0.01);
    CHECK_NEAR(nbtv_duplex_voice_low_hz(NBTV_DUPLEX_STANDARD), 360.0, 1e-9);
    CHECK_NEAR(nbtv_duplex_voice_high_hz(NBTV_DUPLEX_NARROW), 1220.0, 1e-9);
    CHECK_NEAR(nbtv_duplex_band_shift_hz(NBTV_DUPLEX_STANDARD), 1650.5, 1e-9);
    nbtv_duplex_tx_destroy(nullptr);
    nbtv_duplex_rx_destroy(nullptr);
    nbtv_duplex_tx_reset(nullptr);
    nbtv_duplex_rx_reset(nullptr);
    nbtv_duplex_tx_push_picture(nullptr, nullptr, 0);
    nbtv_duplex_tx_push_voice(nullptr, nullptr, 0);
    nbtv_duplex_rx_push(nullptr, nullptr, 0);
    nbtv_duplex_rx_enable(nullptr, 1, 1);
    float four[4] = {1, 1, 1, 1};
    nbtv_duplex_tx_read(nullptr, four, 4);  // 沒有物件：靜音
    CHECK(four[0] == 0.0f && four[3] == 0.0f);
    CHECK_EQ(nbtv_duplex_tx_pending_picture(nullptr), 0);
    CHECK_EQ(nbtv_duplex_rx_read_picture(nullptr, four, 4), 0);
    CHECK_EQ(nbtv_duplex_rx_read_voice(nullptr, four, 2), 0);
    CHECK_EQ(nbtv_tx_set_duplex(nullptr, NBTV_DUPLEX_STANDARD, 0.0f), -1);
    CHECK_EQ(nbtv_rx_set_duplex(nullptr, NBTV_DUPLEX_STANDARD), -1);
    {
        nbtv_tx* slow = nbtv_tx_create(12000);  // 要 48 kHz 的才行
        nbtv_rx* slow_rx = nbtv_rx_create(12000);
        CHECK_EQ(nbtv_tx_set_duplex(slow, NBTV_DUPLEX_STANDARD, 0.0f), -1);
        CHECK_EQ(nbtv_rx_set_duplex(slow_rx, NBTV_DUPLEX_NARROW), -1);
        CHECK_EQ(nbtv_tx_set_duplex(slow, NBTV_DUPLEX_OFF, 0.0f), 0);
        CHECK_EQ(nbtv_rx_set_duplex(slow_rx, NBTV_DUPLEX_OFF), 0);
        nbtv_tx_destroy(slow);
        nbtv_rx_destroy(slow_rx);
    }

    nbtv_mode_info mode{};
    CHECK_EQ(nbtv_mode_by_id(mode_id, &mode), 0);
    const std::vector<std::uint8_t> rgb = test_images::load("coffee", mode.image_width, mode.image_height);
    nbtv_model* model = load_model(weights);
    if (model == nullptr) return;
    const int rate = NBTV_DUPLEX_RATE;

    for (int layout : {NBTV_DUPLEX_STANDARD, NBTV_DUPLEX_NARROW}) {
        const double slow = nbtv_duplex_slow_factor(layout);
        nbtv_tx* tx = nbtv_tx_create(rate);
        nbtv_tx_set_mode(tx, mode_id);
        nbtv_tx_set_callsign(tx, "N0CALL");
        nbtv_tx_set_model(tx, model);
        const int plain_samples = nbtv_tx_frame_samples(tx);
        CHECK_EQ(nbtv_tx_set_duplex(tx, 5, 0.0f), -1);
        CHECK_EQ(nbtv_tx_frame_samples(tx), plain_samples);
        CHECK_EQ(nbtv_tx_set_duplex(tx, layout, 0.0f), 0);
        const int capacity = nbtv_tx_frame_samples(tx);
        CHECK(capacity >= static_cast<int>(plain_samples * slow) && capacity <= static_cast<int>(plain_samples * slow) + 16);

        // 三幀，接著第四幀編了又取消，最後取尾巴
        std::vector<float> air(static_cast<std::size_t>(rate / 4), 0.0f), frame(static_cast<std::size_t>(capacity));
        std::vector<int> counts;
        for (int i = 0; i < 4; ++i) {
            CHECK_EQ(nbtv_tx_encode_image(tx, rgb.data(), frame.data(), capacity - 1, nullptr, nullptr), -1);  // 容量不足
            const int n = nbtv_tx_encode_image(tx, rgb.data(), frame.data(), capacity, nullptr, nullptr);
            CHECK(n > 0 && n <= capacity && std::abs(n - plain_samples * slow) <= 8.0);
            if (i == 3) {
                CHECK_EQ(nbtv_tx_discard_last(tx), 0);
                CHECK_EQ(nbtv_tx_discard_last(tx), -1);
                break;
            }
            counts.push_back(n);
            air.insert(air.end(), frame.begin(), frame.begin() + n);
        }
        std::vector<float> tail(static_cast<std::size_t>(nbtv_tx_tail_samples(tx)));
        const int tail_n = nbtv_tx_finish(tx, tail.data(), static_cast<int>(tail.size()));
        CHECK(tail_n > 400 && tail_n < static_cast<int>(tail.size()));
        air.insert(air.end(), tail.begin(), tail.begin() + tail_n);
        air.insert(air.end(), static_cast<std::size_t>(rate / 2), 0.0f);
        CHECK_EQ(nbtv_tx_finish(tx, tail.data(), static_cast<int>(tail.size())), 0);  // 已經取過了

        // 取消的那一幀不留痕跡：只編三幀的另一個發射機給的完全相同
        {
            nbtv_tx* other = nbtv_tx_create(rate);
            nbtv_tx_set_mode(other, mode_id);
            nbtv_tx_set_callsign(other, "N0CALL");
            nbtv_tx_set_model(other, model);
            CHECK_EQ(nbtv_tx_set_duplex(other, layout, 0.0f), 0);
            std::vector<float> same(static_cast<std::size_t>(rate / 4), 0.0f);
            for (int i = 0; i < 3; ++i) {
                const int n = nbtv_tx_encode_image(other, rgb.data(), frame.data(), capacity, nullptr, nullptr);
                same.insert(same.end(), frame.begin(), frame.begin() + n);
            }
            const int n = nbtv_tx_finish(other, tail.data(), static_cast<int>(tail.size()));
            same.insert(same.end(), tail.begin(), tail.begin() + n);
            same.insert(same.end(), static_cast<std::size_t>(rate / 2), 0.0f);
            CHECK(same == air);
            // 關掉之後回到平常的樣子
            CHECK_EQ(nbtv_tx_set_duplex(other, NBTV_DUPLEX_OFF, 0.0f), 0);
            CHECK_EQ(nbtv_tx_frame_samples(other), plain_samples);
            CHECK_EQ(nbtv_tx_encode_image(other, rgb.data(), frame.data(), capacity, nullptr, nullptr), plain_samples);
            nbtv_tx_destroy(other);
        }

        // 語音那一半：只推語音的另一個物件。這裡用 1500 Hz 的單音（搬到空中是 790 Hz），大小是語音訊號平常的大小
        nbtv_duplex_tx* voice = nbtv_duplex_tx_create(layout);
        CHECK(voice != nullptr);
        nbtv_duplex_tx_set_voice_gain_db(voice, 0.0f);
        const std::size_t voice_count = air.size() / 6;
        std::vector<float> iq(2 * voice_count);
        for (std::size_t n = 0; n < voice_count; ++n) {
            iq[2 * n] = static_cast<float>(0.7 * std::cos(2.0 * kPi * 1500.0 * static_cast<double>(n) / NBTV_DUPLEX_VOICE_RATE));
            iq[2 * n + 1] = static_cast<float>(0.7 * std::sin(2.0 * kPi * 1500.0 * static_cast<double>(n) / NBTV_DUPLEX_VOICE_RATE));
        }
        nbtv_duplex_tx_push_voice(voice, iq.data(), static_cast<int>(voice_count));
        CHECK_EQ(nbtv_duplex_tx_pending_picture(voice), 0);
        CHECK(std::abs(nbtv_duplex_tx_pending_voice(voice) - static_cast<int>(air.size())) <= 6);
        std::vector<float> voice_air(air.size());
        nbtv_duplex_tx_read(voice, voice_air.data(), static_cast<int>(voice_air.size()));
        nbtv_duplex_tx_destroy(voice);
        float peak = 0.0f;
        for (std::size_t i = 0; i < air.size(); ++i) {
            air[i] += voice_air[i];
            peak = std::max(peak, std::fabs(air[i]));
        }
        CHECK(peak < 1.0f && peak > 0.4f);

        // 接收：打開了的接收機收得到三幀；沒打開的收不到任何東西
        nbtv_rx* rx = nbtv_rx_create(rate);
        nbtv_rx* plain = nbtv_rx_create(rate);
        nbtv_rx_set_model(rx, model);
        nbtv_rx_set_model(plain, model);
        CHECK_EQ(nbtv_rx_set_duplex(rx, layout), 0);
        std::vector<nbtv_rx_frame> frames;
        int plain_frames = 0;
        for (std::size_t pos = 0; pos < air.size(); pos += 960) {
            const int n = static_cast<int>(std::min<std::size_t>(960, air.size() - pos));
            nbtv_rx_push(rx, air.data() + pos, n);
            nbtv_rx_push(plain, air.data() + pos, n);
            nbtv_rx_frame f;
            while (nbtv_rx_poll_frame(rx, &f)) frames.push_back(f);
            while (nbtv_rx_poll_frame(plain, &f)) ++plain_frames;
        }
        CHECK_EQ(static_cast<int>(frames.size()), 3);
        CHECK_EQ(plain_frames, 0);
        double psnr = 0.0;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const nbtv_rx_frame& f = frames[i];
            CHECK(f.decoded == 1 && f.header_ok == 1 && f.mode_id == mode_id && f.sequence == static_cast<int>(i));
            CHECK(std::strcmp(f.callsign, "N0CALL") == 0);
            const std::vector<std::uint8_t> got(f.rgb, f.rgb + rgb.size());
            psnr += test_images::psnr(got, rgb) / static_cast<double>(frames.size());
        }
        CHECK(psnr > min_psnr);
        // 重設之後再收一次也一樣；關掉之後就收不到這種訊號
        nbtv_rx_reset(rx);
        nbtv_rx_push(rx, air.data(), static_cast<int>(air.size()));
        int again = 0;
        nbtv_rx_frame f;
        while (nbtv_rx_poll_frame(rx, &f)) again += f.decoded;
        CHECK_EQ(again, 3);
        CHECK_EQ(nbtv_rx_set_duplex(rx, NBTV_DUPLEX_OFF), 0);
        nbtv_rx_push(rx, air.data(), static_cast<int>(air.size()));
        CHECK_EQ(nbtv_rx_poll_frame(rx, &f), 0);
        nbtv_rx_destroy(rx);
        nbtv_rx_destroy(plain);

        // 語音那一路：只開語音的分離器，單音回到 1500 Hz、大小和空中的份量相當；畫面那一路沒有東西
        nbtv_duplex_rx* split = nbtv_duplex_rx_create(layout);
        nbtv_duplex_rx_enable(split, 0, 1);
        nbtv_duplex_rx_push(split, air.data(), static_cast<int>(air.size()));
        CHECK_EQ(nbtv_duplex_rx_available_picture(split), 0);
        const int got_voice = nbtv_duplex_rx_available_voice(split);
        CHECK(std::abs(got_voice - static_cast<int>(air.size() / 6)) <= 2);
        std::vector<float> back(2 * static_cast<std::size_t>(got_voice));
        CHECK_EQ(nbtv_duplex_rx_read_voice(split, back.data(), got_voice), got_voice);
        CHECK_EQ(nbtv_duplex_rx_available_voice(split), 0);
        nbtv_duplex_rx_destroy(split);
        double tone_re = 0.0, tone_im = 0.0, total = 0.0;
        const std::size_t from = static_cast<std::size_t>(got_voice) / 4, to = 3 * static_cast<std::size_t>(got_voice) / 4;
        for (std::size_t n = from; n < to; ++n) {
            const double angle = -2.0 * kPi * 1500.0 * static_cast<double>(n) / NBTV_DUPLEX_VOICE_RATE;
            const double re = back[2 * n], im = back[2 * n + 1];
            tone_re += re * std::cos(angle) - im * std::sin(angle);
            tone_im += re * std::sin(angle) + im * std::cos(angle);
            total += re * re + im * im;
        }
        const double span = static_cast<double>(to - from);
        const double tone = (tone_re * tone_re + tone_im * tone_im) / (span * span), all = total / span;
        CHECK(tone > 0.01 && tone > 0.999 * all * 0.98);  // 幾乎全部的功率都在那個單音上（畫面漏過來的很少）
        std::printf("  C API 語音＋畫面（%s、模式 %d）：%zu 幀，PSNR %.1f dB；每幀 %d 個取樣（平常 %d）、尾巴 %d 個、合成後峰值 %.2f；"
                    "語音那一路單音以外的功率 %.1f dB\n",
                    layout == NBTV_DUPLEX_NARROW ? "窄版面" : "標準版面", mode_id, frames.size(), psnr, counts.empty() ? 0 : counts[0], plain_samples, tail_n,
                    static_cast<double>(peak), 10.0 * std::log10(std::max(all - tone, 1e-12) / all));
        nbtv_tx_destroy(tx);
    }
    nbtv_model_destroy(model);
}

int main() {
    test_helpers();
    test_inference_settings();
    test_skip_pictures(12, "261122-3", nullptr, 23.5);
    test_skip_pictures(12, "261122-3", "261124-8", 23.5);
    test_skip_pictures(5, "261125-3", nullptr, 26.0);  // 幀裡沒有導頻的模式
    test_discard_last();
    // 下限：各個模型對那張圖不加雜訊時的 PSNR（test_learned.cpp 的對照表）減 0.3 dB
    loopback(12000, 11, "261123-1", "coffee", 28.3);
    loopback(48000, 12, "261122-3", "rocket", 24.8);
    loopback(48000, 13, "261122-4", "coffee", 29.2);
    loopback(12000, 14, "261123-4", "rocket", 23.7);
    loopback(48000, 15, "261125-3", "rocket", 29.0);  // FM 用的大畫面：192×144
    loopback(48000, 5, "261125-3", "cat", 28.4);      // 沒有導頻的幀（和模式 15 同一個模型）
    loopback(12000, 3, "261126-4", "rocket", 25.9);   // 同一種幀裡最快的：96×72，加寬的模型
    loopback(48000, 4, "261126-5", "coffee", 24.8);   // 同一種幀的 1 秒大畫面，加寬的模型
    test_model_handling(11, "261123-1", 28.3);
    test_model_handling(12, "261122-3", 23.5);
    test_model_handling(13, "261122-4", 29.4);
    test_model_handling(14, "261123-4", 22.0);
    test_model_handling(15, "261125-3", 27.0);
    test_merging(14, "261123-4", "261124-7");
    test_merging(12, "261122-3", "261124-8");
    test_two_models();
    test_two_widths();
    test_duplex(3, "261126-4", 24.2);  // 語音與畫面共用 FM 語音通道（下限：那張圖直接收是 24.7 dB）
    test_spectrum();
    test_bad_samples(12000);
    test_bad_samples(48000);
    return check::finish("c_api");
}
