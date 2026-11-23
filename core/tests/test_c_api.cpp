// 只透過 C API 做完整的收發：外殼會用到的每個函式都走一遍。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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

// 專案 models/ 底下的權重檔；找不到時回傳 NULL
nbtv_model* load_model(const char* name) {
    std::vector<std::uint8_t> file;
    const std::string path = std::string(NBTV_MODEL_DIR) + "/" + name + ".bin";
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        std::uint8_t chunk[65536];
        for (std::size_t n = 0; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) file.insert(file.end(), chunk, chunk + n);
        std::fclose(f);
    }
    if (file.empty()) {
        std::printf("  找不到權重檔 %s，略過用到它的測試\n", path.c_str());
        return nullptr;
    }
    nbtv_model* model = nbtv_model_load(file.data(), file.size());
    CHECK(model != nullptr);
    return model;
}

// 一個取樣率、一個模式的完整迴路：兩幀，前後各留一段靜音。min_psnr 是那張圖編了再解回來的下限。
void loopback(int rate, int mode_id, const char* weights, const char* image_name, double min_psnr) {
    const std::vector<std::uint8_t> rgb = test_images::load(image_name);
    nbtv_mode_info mode{};
    CHECK_EQ(nbtv_mode_by_id(mode_id, &mode), 0);
    nbtv_model* model = load_model(weights);
    if (model == nullptr) return;

    nbtv_tx* tx = nbtv_tx_create(rate);
    CHECK(tx != nullptr);
    CHECK_EQ(nbtv_tx_mode(tx), 11);  // 預設是模式表的第一個
    for (int gone : {0, 1, 5, 7, 8, 9, 10, 15}) CHECK_EQ(nbtv_tx_set_mode(tx, gone), -1);  // 沒有定義，或已經移除的編號
    CHECK_EQ(nbtv_tx_mode(tx), 11);
    CHECK_EQ(nbtv_tx_set_mode(tx, mode_id), 0);
    CHECK_EQ(nbtv_tx_mode(tx), mode_id);
    nbtv_tx_set_callsign(tx, "n0call");
    nbtv_tx_set_model(tx, model);
    const int frame_samples = nbtv_tx_frame_samples(tx);
    CHECK_EQ(frame_samples, mode.frame_samples * (rate / 12000));

    std::vector<float> audio(static_cast<std::size_t>(rate / 4), 0.0f);  // 0.25 秒靜音
    std::vector<std::uint8_t> preview(NBTV_IMAGE_RGB_BYTES);
    std::vector<float> frame(static_cast<std::size_t>(frame_samples));
    nbtv_tx_frame_info info{};
    CHECK_EQ(nbtv_tx_encode_image(tx, rgb.data(), frame.data(), frame_samples - 1, nullptr, nullptr), -1);  // 容量不足
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
        const std::vector<std::uint8_t> got(f.rgb, f.rgb + NBTV_IMAGE_RGB_BYTES);
        CHECK(test_images::psnr(got, preview) > min_psnr + 2.0);  // 和預覽只差發射端的削波失真
        CHECK(test_images::psnr(got, rgb) > min_psnr - 0.6);
    }
    std::printf("  C API 迴路：%d Hz、模式 %d，%zu 幀，收到的畫面對原圖 PSNR %.1f dB\n", rate, mode_id, frames.size(),
                frames.empty() ? 0.0 : test_images::psnr(std::vector<std::uint8_t>(frames[0].rgb, frames[0].rgb + NBTV_IMAGE_RGB_BYTES), rgb));

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
        CHECK_EQ(nbtv_tx_discard_last(tx), 0);
        CHECK_EQ(nbtv_tx_discard_last(tx), -1);  // 只能取消一次
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
    nbtv_tx_set_model(nullptr, nullptr);
    nbtv_rx_set_model(nullptr, nullptr);

    nbtv_mode_info mode{};
    CHECK_EQ(nbtv_mode_by_id(mode_id, &mode), 0);
    CHECK(mode.learned_values > 0 && mode.learned_values <= mode.value_capacity);

    nbtv_model* model = load_model(weights);
    if (model == nullptr) return;
    CHECK_EQ(nbtv_model_values(model), mode.learned_values);
    CHECK_EQ(nbtv_model_width(model), mode.learned_width);
    CHECK(nbtv_model_id(model) != 0);

    const std::vector<std::uint8_t> rgb = test_images::load("cat");
    const int rate = 48000;
    nbtv_tx* tx = nbtv_tx_create(rate);
    nbtv_tx_set_callsign(tx, "n0call");
    CHECK_EQ(nbtv_tx_set_mode(tx, mode_id), 0);
    const int frame_samples = nbtv_tx_frame_samples(tx);
    CHECK_EQ(frame_samples, mode.frame_samples * 4);
    std::vector<float> frame(static_cast<std::size_t>(frame_samples));
    std::vector<std::uint8_t> preview(NBTV_IMAGE_RGB_BYTES);
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
        quality = test_images::psnr(std::vector<std::uint8_t>(f.rgb, f.rgb + NBTV_IMAGE_RGB_BYTES), rgb);
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
        CHECK(std::all_of(f.rgb, f.rgb + NBTV_IMAGE_RGB_BYTES, [](std::uint8_t v) { return v == 0; }));  // 沒有畫面時 rgb 是 0
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

}  // namespace

int main() {
    test_helpers();
    test_discard_last();
    // 下限：各個模型對那張圖不加雜訊時的 PSNR（test_learned.cpp 的對照表）減 0.3 dB
    loopback(12000, 11, "261123-1", "coffee", 28.3);
    loopback(48000, 12, "261122-3", "rocket", 24.8);
    loopback(48000, 13, "261122-4", "coffee", 29.2);
    loopback(12000, 14, "261123-3", "rocket", 23.4);
    test_model_handling(11, "261123-1", 28.3);
    test_model_handling(12, "261122-3", 23.5);
    test_model_handling(13, "261122-4", 29.4);
    test_model_handling(14, "261123-3", 21.8);
    test_two_models();
    test_two_widths();
    test_spectrum();
    return check::finish("c_api");
}
