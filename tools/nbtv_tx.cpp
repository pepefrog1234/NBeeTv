// nbtv-tx：把 96×72 的 PPM 影像做成可以直接播放給電台的 WAV 檔。只用核心的 C API。
#include <cstdio>
#include <vector>

#include "args.hpp"
#include "model_file.hpp"
#include "nbtv/nbtv.h"
#include "ppm.hpp"
#include "wav.hpp"

using namespace nbtv::tools;

int main(int argc, char** argv) {
    const Args args(argc, argv);
    if (args.positional.empty() || args.has("help")) {
        std::printf(
            "用法：nbtv-tx [選項] 影像.ppm [影像2.ppm …]\n"
            "  --out 檔名      輸出的 WAV（預設 nbtv.wav）\n"
            "  --mode N        模式編號：數位 0–5、類比 8–10、學習式 11–12（預設 1）\n"
            "  --model 檔名    學習式模式的權重檔（預設用專案 models/ 裡給那個模式的）\n"
            "  --call 呼號     最多 9 個字元\n"
            "  --frames N      總共送幾幀，影像輪流使用（預設：每張影像一幀）\n"
            "  --rate N        取樣率 12000 或 48000（預設 48000）\n"
            "  --lead 秒       開頭的靜音（預設 0.5）\n"
            "  --tail 秒       結尾的靜音（預設 0.5）\n"
            "  --papr dB       包絡峰均比目標；0 用預設值，負值不削波\n"
            "  --speed N       影像編碼器的速度檔 0–10（預設 3）\n"
            "影像必須是 96×72 的二進位 PPM，例如：ffmpeg -i 照片.jpg -vf scale=96:72 -pix_fmt rgb24 影像.ppm\n");
        return args.has("help") ? 0 : 2;
    }
    const int rate = args.integer("rate", 48000);
    nbtv_tx* tx = nbtv_tx_create(rate);
    if (tx == nullptr) {
        std::fprintf(stderr, "取樣率只能是 12000 或 48000\n");
        return 2;
    }
    const int mode_id = args.integer("mode", 1);
    nbtv_mode_info mode{};
    if (nbtv_tx_set_mode(tx, mode_id) != 0 || nbtv_mode_by_id(mode_id, &mode) != 0) {
        std::fprintf(stderr, "沒有模式 %d\n", mode_id);
        return 2;
    }
    if (mode.kind == NBTV_MODE_LEARNED) {
        const std::string model_path = args.str("model", default_model_path(mode.learned_values));
        std::vector<std::uint8_t> bytes;
        nbtv_model* model = read_file(model_path, bytes) ? nbtv_model_load(bytes.data(), bytes.size()) : nullptr;
        if (model == nullptr || nbtv_model_values(model) != mode.learned_values) {
            std::fprintf(stderr, "學習式模式需要權重檔：%s 讀不到、不是權重檔，或不是給這個模式用的\n", model_path.c_str());
            return 1;
        }
        nbtv_tx_set_model(tx, model);
        nbtv_model_destroy(model);  // 發射機自己留著一份
    }
    nbtv_tx_set_callsign(tx, args.str("call", "").c_str());
    nbtv_tx_set_encoder_speed(tx, args.integer("speed", -1));
    nbtv_tx_set_papr_db(tx, static_cast<float>(args.num("papr", 0.0)));

    std::vector<std::vector<std::uint8_t>> images;
    for (const std::string& path : args.positional) {
        std::vector<std::uint8_t> rgb;
        int w = 0, h = 0;
        if (!read_ppm(path, rgb, &w, &h) || w != NBTV_IMAGE_WIDTH || h != NBTV_IMAGE_HEIGHT) {
            std::fprintf(stderr, "%s 不是 96×72 的 PPM\n", path.c_str());
            return 1;
        }
        images.push_back(std::move(rgb));
    }
    const int frames = args.integer("frames", static_cast<int>(images.size()));
    std::vector<float> audio(static_cast<std::size_t>(args.num("lead", 0.5) * rate), 0.0f);
    std::vector<float> frame(static_cast<std::size_t>(nbtv_tx_frame_samples(tx)));
    for (int i = 0; i < frames; ++i) {
        nbtv_tx_frame_info info{};
        const int n = nbtv_tx_encode_image(tx, images[static_cast<std::size_t>(i) % images.size()].data(), frame.data(),
                                           static_cast<int>(frame.size()), nullptr, &info);
        if (n < 0) {
            std::fprintf(stderr, "第 %d 幀編碼失敗%s\n", i, nbtv_has_image_codec() ? "" : "（這個建置沒有 libavif，數位模式不能用）");
            return 1;
        }
        audio.insert(audio.end(), frame.begin(), frame.begin() + n);
        if (mode.kind == NBTV_MODE_DIGITAL) std::printf("第 %d 幀：序號 %d，影像 %d／%d 位元組\n", i, info.sequence, info.image_bytes, mode.image_bytes);
        else if (mode.kind == NBTV_MODE_LEARNED) std::printf("第 %d 幀：序號 %d，%d 個實數\n", i, info.sequence, mode.learned_values);
        else std::printf("第 %d 幀：序號 %d，%d 個係數\n", i, info.sequence, info.coefficients);
    }
    std::vector<float> tail(256);
    const int tail_n = nbtv_tx_finish(tx, tail.data(), 256);
    audio.insert(audio.end(), tail.begin(), tail.begin() + tail_n);
    audio.insert(audio.end(), static_cast<std::size_t>(args.num("tail", 0.5) * rate), 0.0f);
    nbtv_tx_destroy(tx);

    const std::string out = args.str("out", "nbtv.wav");
    if (!write_wav(out, audio, rate)) {
        std::fprintf(stderr, "寫不了 %s\n", out.c_str());
        return 1;
    }
    std::printf("%s：模式 %d，%d 幀，每幀 %.2f 秒，共 %.1f 秒\n", out.c_str(), mode_id, frames, mode.frame_seconds,
                static_cast<double>(audio.size()) / rate);
    return 0;
}
