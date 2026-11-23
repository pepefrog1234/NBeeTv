// nbtv-learned：用模型把 96×72 的 PPM 編成通道值、加白雜訊、再解回來，印出畫質與花的時間。
// 不經過數據機，只看「畫面 ↔ 通道值」這一段（等於理想的等化）；走完整收發鏈的是模式 11 到 14，用 nbtv-tx、nbtv-rx、nbtv-sim。
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "args.hpp"
#include "learned_codec.hpp"
#include "learned_frame.hpp"
#include "model_file.hpp"
#include "modes.hpp"
#include "ppm.hpp"
#include "rng.hpp"
#include "types.hpp"

using namespace nbtv;
using namespace nbtv::tools;

namespace {

double psnr(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    double mse = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double d = static_cast<double>(a[i]) - b[i];
        mse += d * d;
    }
    mse /= static_cast<double>(a.size());
    return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
    const Args args(argc, argv);
    if (args.positional.empty() || args.has("help")) {
        std::printf(
            "用法：nbtv-learned [選項] 影像.ppm [影像2.ppm …]\n"
            "  --mode N        用哪個模式的模型：11（2.72 秒）、12（1.04 秒）、13（2.72 秒，加寬的模型）\n"
            "                  或 14（0.67 秒，加寬的模型），預設 11\n"
            "  --model 檔名    直接指定權重檔（預設是專案 models/ 裡給那個模式的）\n"
            "  --snr dB        加白雜訊的格點訊雜比；不給就不加雜訊\n"
            "  --seed N        雜訊的亂數種子（預設 1）\n"
            "  --out 檔名      解出來的畫面（PPM；只給一張影像時可用）\n"
            "  --values 檔名   編出來的通道值，float32、小端序（只給一張影像時可用）\n"
            "影像必須是 96×72 的二進位 PPM，例如：ffmpeg -i 照片.jpg -vf scale=96:72 -pix_fmt rgb24 影像.ppm\n");
        return args.has("help") ? 0 : 2;
    }
    const Mode* mode = find_mode(args.integer("mode", 11));
    if (mode == nullptr) {
        std::fprintf(stderr, "沒有這個模式\n");
        return 2;
    }
    const std::string model_path = args.str("model", default_model_path(mode->id));
    std::vector<std::uint8_t> file;
    if (!read_file(model_path, file)) {
        std::fprintf(stderr, "讀不到權重檔 %s\n", model_path.c_str());
        return 1;
    }
    std::string error;
    auto t0 = std::chrono::steady_clock::now();
    const auto codec = LearnedCodec::load(file, &error);
    if (codec == nullptr) {
        std::fprintf(stderr, "%s：%s\n", model_path.c_str(), error.c_str());
        return 1;
    }
    if (!learned_model_fits(*mode, *codec)) {
        std::fprintf(stderr, "%s 不是給模式 %d 用的模型（實數個數或各層的寬度不同）\n", model_path.c_str(), mode->id);
        return 1;
    }
    std::printf("模型 %08X：一張畫面 %d 個實數（%d 個格點），第一層 %d 個通道；載入 %.0f ms\n", static_cast<unsigned>(codec->model_id()),
                codec->value_count(), codec->value_count() / 2, codec->base_width(), ms_since(t0));
    if ((args.has("out") || args.has("values")) && args.positional.size() != 1) {
        std::fprintf(stderr, "--out 和 --values 只能在給一張影像時使用\n");
        return 2;
    }

    const bool add_noise = args.has("snr");
    const double snr_db = args.num("snr", 0.0);
    const double sigma = add_noise ? std::sqrt(std::pow(10.0, -snr_db / 10.0) / 2.0) : 0.0;  // 每個實數的雜訊標準差
    Rng rng(static_cast<std::uint64_t>(args.integer("seed", 1)));
    if (add_noise) std::printf("格點訊雜比 %.1f dB 的白雜訊\n", snr_db);
    else std::printf("不加雜訊（告訴解碼器的訊雜比是 %.0f dB）\n", static_cast<double>(kLearnedMaxSnrDb));

    std::vector<float> values(static_cast<std::size_t>(codec->value_count()));
    std::vector<std::uint8_t> decoded(static_cast<std::size_t>(kImageRgbBytes));
    double total = 0.0;
    for (const std::string& path : args.positional) {
        std::vector<std::uint8_t> rgb;
        int w = 0, h = 0;
        if (!read_ppm(path, rgb, &w, &h) || w != kImageWidth || h != kImageHeight) {
            std::fprintf(stderr, "%s 不是 96×72 的 PPM\n", path.c_str());
            return 1;
        }
        t0 = std::chrono::steady_clock::now();
        codec->encode(rgb, values);
        const double encode_ms = ms_since(t0);
        if (args.has("values")) {
            std::FILE* f = std::fopen(args.str("values", "").c_str(), "wb");
            if (f == nullptr || std::fwrite(values.data(), sizeof(float), values.size(), f) != values.size()) {
                std::fprintf(stderr, "寫不出 %s\n", args.str("values", "").c_str());
                return 1;
            }
            std::fclose(f);
        }
        for (float& v : values) v += static_cast<float>(sigma * rng.gauss());
        t0 = std::chrono::steady_clock::now();
        codec->decode(values, add_noise ? static_cast<float>(snr_db) : kLearnedMaxSnrDb, decoded);
        const double decode_ms = ms_since(t0);
        const double quality = psnr(decoded, rgb);
        total += quality;
        std::printf("%s：PSNR %.2f dB；編碼 %.0f ms、解碼 %.0f ms\n", path.c_str(), quality, encode_ms, decode_ms);
        if (args.has("out") && !write_ppm(args.str("out", ""), decoded, kImageWidth, kImageHeight)) {
            std::fprintf(stderr, "寫不出 %s\n", args.str("out", "").c_str());
            return 1;
        }
    }
    const std::size_t count = args.positional.size();
    if (count > 1) std::printf("平均 PSNR %.2f dB（%zu 張）\n", total / static_cast<double>(count), count);
    return 0;
}
