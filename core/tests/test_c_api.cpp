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

// 一個取樣率、一個模式的完整迴路：兩幀，前後各留一段靜音
void loopback(int rate, int mode_id, const char* image_name) {
    const std::vector<std::uint8_t> rgb = test_images::load(image_name);
    nbtv_mode_info mode{};
    CHECK_EQ(nbtv_mode_by_id(mode_id, &mode), 0);
    const bool digital = mode.kind == NBTV_MODE_DIGITAL;
    if (digital && !nbtv_has_image_codec()) return;

    nbtv_tx* tx = nbtv_tx_create(rate);
    CHECK(tx != nullptr);
    CHECK_EQ(nbtv_tx_set_mode(tx, 7), -1);
    CHECK_EQ(nbtv_tx_set_mode(tx, mode_id), 0);
    CHECK_EQ(nbtv_tx_mode(tx), mode_id);
    nbtv_tx_set_callsign(tx, "bx4acp");
    const int frame_samples = nbtv_tx_frame_samples(tx);
    CHECK_EQ(frame_samples, mode.frame_samples * (rate / 12000));

    std::vector<float> audio(static_cast<std::size_t>(rate / 4), 0.0f);  // 0.25 秒靜音
    std::vector<std::uint8_t> preview(NBTV_IMAGE_RGB_BYTES);
    std::vector<float> frame(static_cast<std::size_t>(frame_samples));
    nbtv_tx_frame_info info{};
    CHECK_EQ(nbtv_tx_encode_image(tx, rgb.data(), frame.data(), frame_samples - 1, nullptr, nullptr), -1);  // 容量不足
    for (int i = 0; i < 2; ++i) {
        CHECK_EQ(nbtv_tx_encode_image(tx, rgb.data(), frame.data(), frame_samples, preview.data(), &info), frame_samples);
        CHECK_EQ(info.mode_id, mode_id);
        CHECK_EQ(info.sequence, i);
        if (digital) {
            CHECK(info.image_bytes > mode.image_bytes / 2 && info.image_bytes <= mode.image_bytes);
            CHECK_EQ(info.coefficients, 0);
        } else {
            CHECK_EQ(info.image_bytes, 0);
            CHECK_EQ(info.coefficients, mode.analog_coefficients);
        }
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
    // 預覽與原圖相近
    CHECK(test_images::psnr(preview, rgb) > 20.0);

    nbtv_rx* rx = nbtv_rx_create(rate);
    nbtv_spectrum* sp = nbtv_spectrum_create(rate);
    CHECK(rx != nullptr && sp != nullptr);
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
        CHECK(f.kind == mode.kind);
        CHECK_EQ(f.decoded, 1);
        CHECK_EQ(f.header_ok, 1);
        CHECK(std::strcmp(f.callsign, "BX4ACP") == 0);
        CHECK_EQ(f.sequence, static_cast<int>(i));
        CHECK_NEAR(f.freq_offset_hz, 0.0, 0.5);
        CHECK(f.snr_db > 15.0);
        CHECK_EQ(f.has_image, 1);
        const std::vector<std::uint8_t> got(f.rgb, f.rgb + NBTV_IMAGE_RGB_BYTES);
        if (digital) {
            // 數位：收到的畫面與發射端的預覽完全相同
            CHECK(got == preview);
            CHECK(f.image_bytes > 0 && f.image_bytes <= mode.image_bytes);
            std::vector<std::uint8_t> file(static_cast<std::size_t>(f.image_bytes + NBTV_AVIF_OVERHEAD));
            const int file_n = nbtv_avif_from_payload(f.image_data, f.image_bytes, file.data(), static_cast<int>(file.size()));
            CHECK_EQ(file_n, f.image_bytes + NBTV_AVIF_OVERHEAD);
            CHECK(std::memcmp(file.data() + 4, "ftypavif", 8) == 0);
            CHECK_EQ(nbtv_avif_from_payload(f.image_data, f.image_bytes, file.data(), file_n - 1), -1);
            std::vector<std::uint8_t> again(NBTV_IMAGE_RGB_BYTES);
            CHECK_EQ(nbtv_image_decode(f.image_data, f.image_bytes, again.data()), 0);
            CHECK(again == preview);
        } else {
            CHECK_EQ(f.image_bytes, 0);
            CHECK(test_images::psnr(got, preview) > 24.0);  // 只差發射端的削波失真
        }
    }
    std::printf("  C API 迴路：%d Hz、模式 %d，%zu 幀，收到的畫面對原圖 PSNR %.1f dB\n", rate, mode_id, frames.size(),
                frames.empty() ? 0.0 : test_images::psnr(std::vector<std::uint8_t>(frames[0].rgb, frames[0].rgb + NBTV_IMAGE_RGB_BYTES), rgb));

    // 重設後可以再收一次
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

void test_payload_path() {
    if (!nbtv_has_image_codec()) return;
    const std::vector<std::uint8_t> rgb = test_images::load("rocket");
    nbtv_mode_info mode{};
    nbtv_mode_by_id(0, &mode);
    std::vector<std::uint8_t> payload(NBTV_MAX_IMAGE_BYTES);
    const int n = nbtv_image_encode(rgb.data(), mode.image_bytes, -1, payload.data(), NBTV_MAX_IMAGE_BYTES);
    CHECK(n > 0 && n <= mode.image_bytes);
    CHECK_EQ(nbtv_image_encode(rgb.data(), 20, -1, payload.data(), NBTV_MAX_IMAGE_BYTES), -1);
    CHECK_EQ(nbtv_image_encode(rgb.data(), mode.image_bytes, -1, payload.data(), 10), -1);

    nbtv_tx* tx = nbtv_tx_create(12000);
    nbtv_tx_set_mode(tx, 0);
    nbtv_tx_set_callsign(tx, "N0CALL");
    std::vector<float> audio(3000, 0.0f), frame(static_cast<std::size_t>(nbtv_tx_frame_samples(tx)));
    nbtv_tx_frame_info info{};
    CHECK_EQ(nbtv_tx_encode_payload(tx, payload.data(), mode.image_bytes + 1, frame.data(), static_cast<int>(frame.size()), &info), -1);
    CHECK_EQ(nbtv_tx_encode_payload(tx, payload.data(), n, frame.data(), static_cast<int>(frame.size()), &info), static_cast<int>(frame.size()));
    CHECK_EQ(info.image_bytes, n);
    audio.insert(audio.end(), frame.begin(), frame.end());
    audio.insert(audio.end(), 6000, 0.0f);
    // 類比模式不能直接給酬載
    nbtv_tx_set_mode(tx, 9);
    frame.resize(static_cast<std::size_t>(nbtv_tx_frame_samples(tx)));
    CHECK_EQ(nbtv_tx_encode_payload(tx, payload.data(), n, frame.data(), static_cast<int>(frame.size()), &info), -1);

    nbtv_rx* rx = nbtv_rx_create(12000);
    nbtv_rx_push(rx, audio.data(), static_cast<int>(audio.size()));
    nbtv_rx_frame f;
    CHECK_EQ(nbtv_rx_poll_frame(rx, &f), 1);
    CHECK_EQ(f.decoded, 1);
    CHECK_EQ(f.image_bytes, n);
    CHECK(std::memcmp(f.image_data, payload.data(), static_cast<std::size_t>(n)) == 0);
    CHECK(std::strcmp(f.callsign, "N0CALL") == 0);
    CHECK_EQ(nbtv_rx_poll_frame(rx, &f), 0);
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
    for (int rate : {48000, 12000}) {
        nbtv_tx* tx = nbtv_tx_create(rate);
        nbtv_tx* ref = nbtv_tx_create(rate);
        for (nbtv_tx* t : {tx, ref}) {
            nbtv_tx_set_mode(t, 9);
            nbtv_tx_set_callsign(t, "BX4ACP");
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
}

// 專案 models/ 底下的權重檔；找不到時回傳 NULL
nbtv_model* load_model(const char* name, bool* found) {
    std::vector<std::uint8_t> file;
    const std::string path = std::string(NBTV_MODEL_DIR) + "/" + name + ".bin";
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        std::uint8_t chunk[65536];
        for (std::size_t n = 0; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) file.insert(file.end(), chunk, chunk + n);
        std::fclose(f);
    }
    *found = !file.empty();
    if (file.empty()) {
        std::printf("  找不到權重檔 %s，略過用到它的測試\n", path.c_str());
        return nullptr;
    }
    nbtv_model* model = nbtv_model_load(file.data(), file.size());
    CHECK(model != nullptr);
    return model;
}

// 學習式模式：載入模型、設定給發射機與接收機、走一趟迴路。min_psnr 是「貓」那張圖編了再解回來的下限。
void test_learned_mode(int mode_id, const char* weights, double min_psnr) {
    // 沒有模型、不是模型
    CHECK(nbtv_model_load(nullptr, 0) == nullptr);
    const std::uint8_t junk[64] = {};
    CHECK(nbtv_model_load(junk, sizeof junk) == nullptr);
    nbtv_model_destroy(nullptr);
    CHECK_EQ(nbtv_model_id(nullptr), 0);
    CHECK_EQ(nbtv_model_values(nullptr), 0);
    nbtv_tx_set_model(nullptr, nullptr);
    nbtv_rx_set_model(nullptr, nullptr);

    nbtv_mode_info mode{};
    CHECK_EQ(nbtv_mode_by_id(mode_id, &mode), 0);
    CHECK(mode.kind == NBTV_MODE_LEARNED);
    CHECK(mode.learned_values > 0 && mode.learned_values <= mode.analog_values);
    CHECK_EQ(mode.analog_coefficients, 0);
    CHECK_EQ(mode.image_bytes, 0);

    bool found = false;
    nbtv_model* model = load_model(weights, &found);
    if (model == nullptr) return;
    CHECK_EQ(nbtv_model_values(model), mode.learned_values);
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
        CHECK_EQ(info.image_bytes, 0);
        CHECK_EQ(info.coefficients, 0);
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
        CHECK(f.kind == NBTV_MODE_LEARNED);
        CHECK_EQ(f.mode_id, mode_id);
        CHECK_EQ(f.decoded, 1);
        CHECK_EQ(f.has_image, 1);
        CHECK_EQ(f.model_missing, 0);
        CHECK_EQ(f.header_ok, 1);
        CHECK(std::strcmp(f.callsign, "N0CALL") == 0);
        CHECK_EQ(f.sequence, n);
        CHECK_EQ(f.image_bytes, 0);
        quality = test_images::psnr(std::vector<std::uint8_t>(f.rgb, f.rgb + NBTV_IMAGE_RGB_BYTES), rgb);
        CHECK(quality > min_psnr);
        ++n;
    }
    CHECK_EQ(n, 2);
    std::printf("  C API 迴路：%d Hz、模式 %d，%d 幀，收到的畫面對原圖 PSNR %.1f dB\n", rate, mode_id, n, quality);
    // 沒有模型的接收機：收得到幀與呼號，但沒有畫面
    n = 0;
    while (nbtv_rx_poll_frame(bare, &f)) {
        CHECK(f.kind == NBTV_MODE_LEARNED);
        CHECK_EQ(f.decoded, 0);
        CHECK_EQ(f.has_image, 0);
        CHECK_EQ(f.model_missing, 1);
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

// 兩個學習式模式各有自己的模型：發射機、接收機都設定兩個，一次發射裡換模式，各用各的模型
void test_learned_two_models() {
    bool found[2] = {false, false};
    nbtv_model* models[2] = {load_model("learned_7776_v2", &found[0]), load_model("learned_2592_v2", &found[1])};
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
        CHECK(f.kind == NBTV_MODE_LEARNED);
        CHECK_EQ(f.decoded, 1);
        CHECK_EQ(f.has_image, 1);
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

}  // namespace

int main() {
    test_helpers();
    test_discard_last();
    loopback(12000, 3, "coffee");
    loopback(48000, 5, "cat");
    loopback(48000, 9, "rocket");
    loopback(12000, 8, "coffee");
    test_payload_path();
    test_learned_mode(11, "learned_7776_v2", 27.5);
    test_learned_mode(12, "learned_2592_v2", 23.5);
    test_learned_two_models();
    test_spectrum();
    return check::finish("c_api");
}
