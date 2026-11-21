// nbtv-rx：解 WAV 檔裡的訊號，把收到的畫面存成 PPM（數位模式另存標準的 .avif）。
#include <cstdio>
#include <string>
#include <vector>

#include "args.hpp"
#include "av1_payload.hpp"
#include "dsp.hpp"
#include "image_codec.hpp"
#include "ppm.hpp"
#include "receiver.hpp"
#include "wav.hpp"

using namespace nbtv;
using namespace nbtv::tools;

int main(int argc, char** argv) {
    const Args args(argc, argv, {"trace", "help"});
    if (args.positional.empty() || args.has("help")) {
        std::printf(
            "用法：nbtv-rx [選項] 錄音.wav\n"
            "  --out-dir 目錄   收到的畫面存到這裡（預設不存檔，只列出結果）\n"
            "  --trace          印出同步與解碼的過程\n"
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
    int index = 0, good = 0;
    auto drain = [&] {
        RxFrame f;
        while (rx.poll(f)) {
            const double at = static_cast<double>(f.start_sample) / kSampleRate;
            std::printf("%7.2f 秒  模式 %2d  %s  呼號 %-9s 序號 %3d  訊雜比 %5.1f dB  頻偏 %+6.1f Hz", at, f.mode_id,
                        f.decoded ? "成功" : "失敗", f.header_ok ? f.callsign.c_str() : "?", f.sequence, f.snr_db, f.freq_offset_hz);
            std::vector<std::uint8_t> rgb;
            if (f.decoded && f.is_digital) {
                const std::size_t n = av1_payload_length(f.image_data);
                std::printf("  影像 %zu 位元組", n);
                rgb.resize(kImageRgbBytes);
                if (!image_decode(f.image_data, rgb)) rgb.clear();
                if (!dir.empty()) {
                    const auto file = avif_from_payload(f.image_data);
                    const std::string path = dir + "/frame_" + std::to_string(index) + ".avif";
                    if (FILE* out = std::fopen(path.c_str(), "wb")) {
                        std::fwrite(file.data(), 1, file.size(), out);
                        std::fclose(out);
                    }
                }
            } else if (f.decoded) {
                rgb = f.rgb;
            }
            if (!dir.empty() && !rgb.empty()) write_ppm(dir + "/frame_" + std::to_string(index) + ".ppm", rgb, kImageWidth, kImageHeight);
            std::printf("\n");
            good += f.decoded ? 1 : 0;
            ++index;
        }
    };
    for (std::size_t pos = 0; pos < audio.size(); pos += 1200) {
        rx.push(std::span<const float>(audio).subspan(pos, std::min<std::size_t>(1200, audio.size() - pos)));
        drain();
    }
    std::printf("共 %d 幀，解出 %d 幀\n", index, good);
    return 0;
}
