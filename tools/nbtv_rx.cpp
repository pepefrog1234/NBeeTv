// nbtv-rx：解 WAV 檔裡的訊號，把收到的畫面存成 PPM。
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "args.hpp"
#include "dsp.hpp"
#include "learned_codec.hpp"
#include "learned_merge.hpp"
#include "model_file.hpp"
#include "modes.hpp"
#include "ppm.hpp"
#include "receiver.hpp"
#include "wav.hpp"

using namespace nbtv;
using namespace nbtv::tools;

namespace {

// --json 用：數字印成 JSON 的值（不是有限的數印 null）、字串加上引號並跳脫
std::string json_number(double v, const char* format) {
    if (!std::isfinite(v)) return "null";
    char text[32];
    std::snprintf(text, sizeof text, format, v);
    return text;
}

std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof escaped, "\\u%04x", c);
            out += escaped;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

}  // namespace

int main(int argc, char** argv) {
    const Args args(argc, argv, {"trace", "help", "merge", "json"});
    if (args.positional.empty() || args.has("help")) {
        std::printf(
            "用法：nbtv-rx [選項] 錄音.wav\n"
            "  --out-dir 目錄   收到的畫面存到這裡（預設不存檔，只列出結果）\n"
            "  --model 檔名     權重檔，取代專案 models/ 裡給同一個模式的那一份\n"
            "                   （沒有給的話每個模式都用 models/ 裡的，找不到就不解那個模式的畫面）\n"
            "  --merge          解每一幀之前把前面幾幀收到的值合進來：畫面沒變的地方比較清楚，訊號弱時差別最大。\n"
            "                   用的是 models/ 裡各個模型配的權重；沒有配的模式照原來的方式解\n"
            "  --trace          印出同步與解碼的過程\n"
            "  --json           給程式讀的輸出：每一幀印一行 JSON、印完立刻送出，最後一行是總數\n"
            "                   （frame 是第幾幀，存檔時檔名是 frame_<frame>.ppm；標頭沒解出來時 callsign 是 null）\n"
            "WAV 的取樣率必須是 12000 或 48000。\n");
        return args.has("help") ? 0 : 2;
    }
    std::vector<float> audio;
    int rate = 0;
    if (!read_wav(args.positional[0], audio, rate)) {
        std::fprintf(stderr, "讀不了 %s（需要 16 位元 PCM 或 32 位元浮點的 WAV）\n", args.positional[0].c_str());
        return 1;
    }
    if (rate == 48000) {
        std::vector<float> down;
        Decimator4().process(audio, down);
        audio.swap(down);
    } else if (rate != 12000) {
        std::fprintf(stderr, "取樣率 %d 不支援，請先轉成 12000 或 48000\n", rate);
        return 1;
    }
    const std::string dir = args.str("out-dir", "");
    Receiver rx;
    rx.set_trace(args.has("trace"));
    // 每個模式各一份模型：先載入專案 models/ 裡的，--model 指定的那一份再取代實數個數與寬度都相同的
    auto load_model = [&](const std::string& path, bool required) {
        std::vector<std::uint8_t> bytes;
        std::string error = "讀不到檔案";
        std::shared_ptr<const LearnedCodec> model;
        if (read_file(path, bytes)) model = LearnedCodec::load(bytes, &error);
        const bool loaded = model != nullptr;
        if (loaded) rx.set_model(std::move(model));
        else if (required || !bytes.empty()) std::fprintf(stderr, "%s：%s\n", path.c_str(), error.c_str());
        return loaded || !required;
    };
    for (const Mode& m : kModes) load_model(default_model_path(m.id), false);
    if (args.has("model") && !load_model(args.str("model", ""), true)) return 1;
    if (args.has("merge")) {
        // 合併最近幾幀用的權重，每個模式的模型各配一份；接收機依權重裡記的模型識別碼對應
        int merges = 0;
        for (const Mode& m : kModes) {
            const std::string path = default_merge_path(m.id);
            std::vector<std::uint8_t> bytes;
            if (path.empty() || !read_file(path, bytes)) continue;
            std::string error;
            std::shared_ptr<const LearnedMerge> merge = LearnedMerge::load(bytes, &error);
            if (merge == nullptr) {
                std::fprintf(stderr, "%s：%s\n", path.c_str(), error.c_str());
                continue;
            }
            rx.set_merge(std::move(merge));
            ++merges;
        }
        if (merges == 0) {
            std::fprintf(stderr, "找不到合併最近幾幀用的權重檔（專案的 models/ 底下）\n");
            return 1;
        }
        rx.tuning().learned_merge = true;
    }
    const bool json = args.has("json");
    int index = 0, good = 0;
    auto drain = [&] {
        RxFrame f;
        while (rx.poll(f)) {
            const double at = static_cast<double>(f.start_sample) / kSampleRate;
            if (!dir.empty() && f.decoded) write_ppm(dir + "/frame_" + std::to_string(index) + ".ppm", f.rgb, kImageWidth, kImageHeight);
            if (json) {
                // 畫面先存好才印這一行，另一頭讀到這一行時檔案已經在了
                std::printf("{\"frame\":%d,\"time\":%s,\"mode\":%d,\"picture\":%s,\"header\":%s,\"callsign\":%s,\"sequence\":%d,"
                            "\"snr_db\":%s,\"offset_hz\":%s,\"model_missing\":%s}\n",
                            index, json_number(at, "%.3f").c_str(), f.mode_id, f.decoded ? "true" : "false",
                            f.header_ok ? "true" : "false", f.header_ok ? json_string(f.callsign).c_str() : "null", f.sequence,
                            json_number(f.snr_db, "%.2f").c_str(), json_number(f.freq_offset_hz, "%.2f").c_str(),
                            f.model_missing ? "true" : "false");
                std::fflush(stdout);
            } else {
                std::printf("%7.2f 秒  模式 %2d  %s  呼號 %-9s 序號 %3d  訊雜比 %5.1f dB  頻偏 %+6.1f Hz", at, f.mode_id,
                            f.decoded ? "有畫面" : "沒畫面", f.header_ok ? f.callsign.c_str() : "?", f.sequence, f.snr_db, f.freq_offset_hz);
                if (f.model_missing) std::printf("  沒有模型，或權重和對方的不同");
                std::printf("\n");
            }
            good += f.decoded ? 1 : 0;
            ++index;
        }
    };
    for (std::size_t pos = 0; pos < audio.size(); pos += 1200) {
        rx.push(std::span<const float>(audio).subspan(pos, std::min<std::size_t>(1200, audio.size() - pos)));
        drain();
    }
    if (json) std::printf("{\"end\":true,\"frames\":%d,\"pictures\":%d}\n", index, good);
    else std::printf("共 %d 幀，%d 幀有畫面\n", index, good);
    return 0;
}
