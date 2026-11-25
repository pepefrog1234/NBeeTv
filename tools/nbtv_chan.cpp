// nbtv-chan：把 WAV 檔送過模擬的短波通道（Watterson 衰落、雜訊、頻偏、取樣時脈誤差）。
#include <cmath>
#include <cstdio>
#include <vector>

#include "args.hpp"
#include "channel.hpp"
#include "dsp.hpp"
#include "startup.hpp"
#include "wav.hpp"

using namespace nbtv;
using namespace nbtv::tools;

int main(int argc, char** argv) {
    tool_startup();
    const Args args(argc, argv, {"help"});
    if (args.positional.size() < 2 || args.has("help")) {
        std::printf(
            "用法：nbtv-chan [選項] 輸入.wav 輸出.wav\n"
            "  --channel 名稱   awgn（預設）、poor（2 ms／1 Hz）、moderate（1 ms／0.5 Hz）、slow（0.5 ms／0.1 Hz）\n"
            "  --snr dB         訊雜比，3 kHz 雜訊頻寬（預設 10）；訊號功率取檔案裡有聲音的那一段\n"
            "  --offset Hz      頻率偏移\n"
            "  --ppm N          接收端取樣時脈誤差\n"
            "  --seed N         亂數種子\n"
            "輸出一律是 12 kHz。\n");
        return args.has("help") ? 0 : 2;
    }
    std::vector<float> audio;
    int rate = 0;
    if (!read_wav(args.positional[0], audio, rate) || (rate != 12000 && rate != 48000)) {
        std::fprintf(stderr, "讀不了 %s（需要 12000 或 48000 Hz 的 WAV）\n", args.positional[0].c_str());
        return 1;
    }
    if (rate == 48000) {
        std::vector<float> down;
        Decimator4().process(audio, down);
        audio.swap(down);
    }
    sim::ChannelParams p;
    if (!sim::channel_preset(args.str("channel", "awgn"), p)) {
        std::fprintf(stderr, "沒有這個通道\n");
        return 2;
    }
    p.snr_db = args.num("snr", 10.0);
    p.freq_offset_hz = args.num("offset", 0.0);
    p.clock_ppm = args.num("ppm", 0.0);
    p.seed = static_cast<std::uint64_t>(args.integer("seed", 1));
    // 訊號功率：第一個到最後一個有聲音的取樣之間
    std::size_t first = audio.size(), last = 0;
    for (std::size_t i = 0; i < audio.size(); ++i) {
        if (std::fabs(audio[i]) > 1e-4f) {
            first = std::min(first, i);
            last = i;
        }
    }
    double power = 0.0;
    for (std::size_t i = first; i <= last && i < audio.size(); ++i) power += static_cast<double>(audio[i]) * audio[i];
    power = last > first ? power / static_cast<double>(last - first + 1) : 1e-6;
    sim::Channel channel(p, power);
    std::vector<float> out;
    channel.process(audio, out);
    // 雜訊加上去之後可能超過滿刻度，整體縮小到不削波
    float peak = 0.0f;
    for (float v : out) peak = std::max(peak, std::fabs(v));
    if (peak > 0.99f) {
        for (float& v : out) v *= 0.99f / peak;
    }
    if (!write_wav(args.positional[1], out, 12000)) {
        std::fprintf(stderr, "寫不了 %s\n", args.positional[1].c_str());
        return 1;
    }
    std::printf("%s：通道 %s、訊雜比 %.1f dB、頻偏 %+.1f Hz、時脈 %+.0f ppm\n", args.positional[1].c_str(), args.str("channel", "awgn").c_str(),
                p.snr_db, p.freq_offset_hz, p.clock_ppm);
    return 0;
}
