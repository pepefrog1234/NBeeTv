// nbtv-duplex：語音與畫面一起送（實驗性）的檔案版：把畫面的數據機訊號與語音的訊號合成一個音檔，
// 或把錄回來的音檔分成兩路。三種版面：FM 語音通道分頻的兩種，和短波 SSB 的「疊加」。
// 只處理檔案，不碰音訊裝置。除了 demux 把 12 kHz 的錄音先換成 48 kHz 的那一步，只用核心的 C API。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "args.hpp"
#include "duplex.hpp"
#include "nbtv/nbtv.h"
#include "startup.hpp"
#include "wav.hpp"

using namespace nbtv::tools;

namespace {

void usage() {
    std::printf(
        "用法：\n"
        "  nbtv-duplex mux [選項] --out 合成.wav        把畫面與語音合成送給電台的音檔（48 kHz）\n"
        "      --picture 畫面.wav   畫面的數據機訊號：nbtv-tx --rate 12000 做的（一定要 12 kHz）\n"
        "      --voice 語音.iq      語音的訊號：8 kHz 的複數（32 位元浮點，I、Q 交錯）、中心 1500 Hz，\n"
        "                           nbtv-voice encode 給的就是這種格式（overlay 要用 --codec v1 編的）。\n"
        "                           兩個至少要給一個\n"
        "      --voice-gain dB      語音比畫面大幾 dB（預設 0：兩者功率相同）\n"
        "      --lead 秒、--tail 秒 開頭、結尾的靜音（預設各 0.5）\n"
        "  nbtv-duplex demux [選項] 錄音.wav            把收到的音檔分成兩路（48 kHz；12 kHz 的也可以，例如網路 SDR 的錄音）\n"
        "      --picture 畫面.wav   畫面那一路（12 kHz）：交給 nbtv-rx，要加上這裡印出來的 --band-shift\n"
        "      --voice 語音.iq      語音那一路：8 kHz 的複數、中心 1500 Hz，交給語音的解碼器\n"
        "  nbtv-duplex info                             印出三種版面的數字\n"
        "共用的選項：\n"
        "      --layout standard|narrow|overlay   版面（預設 standard）\n"
        "          standard  給 FM：語音（RADE V2）佔 360–1220 Hz，畫面放慢 1.5 倍、佔 1335–2866 Hz\n"
        "          narrow    給 FM：畫面放慢 1.8 倍、只佔到 2611 Hz，給音訊上緣比較窄的電台\n"
        "          overlay   疊加，給短波的 SSB：畫面原樣（不放慢、不移頻），語音（RADE V1，728–2228 Hz）原樣加在\n"
        "                    同一個頻帶上。兩邊互相當成雜訊，功率要差不多（--voice-gain 留在 0）才都解得出來；\n"
        "                    demux 給的畫面就是整段音訊（直接給 nbtv-rx 也一樣），語音是整段音訊的解析訊號\n");
}

const char* layout_name(int layout) {
    return layout == NBTV_DUPLEX_OVERLAY ? "overlay" : layout == NBTV_DUPLEX_NARROW ? "narrow" : "standard";
}

bool parse_layout(const Args& args, int& layout) {
    const std::string name = args.str("layout", "standard");
    for (int candidate : {NBTV_DUPLEX_STANDARD, NBTV_DUPLEX_NARROW, NBTV_DUPLEX_OVERLAY}) {
        if (name == layout_name(candidate)) {
            layout = candidate;
            return true;
        }
    }
    std::fprintf(stderr, "--layout 只能是 standard、narrow 或 overlay\n");
    return false;
}

// 8 kHz 的複數訊號：32 位元浮點，I、Q 交錯，沒有檔頭
bool read_iq(const std::string& path, std::vector<float>& iq) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    float buf[4096];
    for (std::size_t n; (n = std::fread(buf, sizeof(float), 4096, f)) > 0;) iq.insert(iq.end(), buf, buf + n);
    std::fclose(f);
    iq.resize(iq.size() / 2 * 2);
    return true;
}

bool write_iq(const std::string& path, const std::vector<float>& iq) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const bool ok = iq.empty() || std::fwrite(iq.data(), sizeof(float), iq.size(), f) == iq.size();
    return std::fclose(f) == 0 && ok;
}

double level_db(double rms) { return 20.0 * std::log10(std::max(rms, 1e-9)); }

int info() {
    for (int layout : {NBTV_DUPLEX_STANDARD, NBTV_DUPLEX_NARROW, NBTV_DUPLEX_OVERLAY}) {
        if (nbtv_duplex_slow_factor(layout) > 1.0) {
            std::printf("%-8s 畫面放慢 %.1f 倍，佔 %.0f–%.0f Hz；", layout_name(layout), nbtv_duplex_slow_factor(layout),
                        nbtv_duplex_picture_low_hz(layout), nbtv_duplex_picture_high_hz(layout));
        } else {
            std::printf("%-8s 畫面原樣，佔 %.0f–%.0f Hz；", layout_name(layout), nbtv_duplex_picture_low_hz(layout), nbtv_duplex_picture_high_hz(layout));
        }
        std::printf("語音（RADE V%d）佔 %.0f–%.0f Hz；nbtv-rx 的 --band-shift 是 %.1f\n", nbtv_duplex_voice_codec(layout),
                    nbtv_duplex_voice_low_hz(layout), nbtv_duplex_voice_high_hz(layout), nbtv_duplex_band_shift_hz(layout));
    }
    return 0;
}

int mux(const Args& args, int layout) {
    if (!args.has("out") || (!args.has("picture") && !args.has("voice"))) {
        usage();
        return 2;
    }
    std::vector<float> picture, voice;
    if (args.has("picture")) {
        int rate = 0;
        if (!read_wav(args.str("picture", ""), picture, rate)) {
            std::fprintf(stderr, "讀不了 %s（需要 16 位元 PCM 或 32 位元浮點的 WAV）\n", args.str("picture", "").c_str());
            return 1;
        }
        if (rate != NBTV_DUPLEX_PICTURE_RATE) {
            std::fprintf(stderr, "畫面的音檔要是 12 kHz 的（nbtv-tx --rate 12000），這個是 %d Hz\n", rate);
            return 1;
        }
    }
    if (args.has("voice") && !read_iq(args.str("voice", ""), voice)) {
        std::fprintf(stderr, "讀不了 %s\n", args.str("voice", "").c_str());
        return 1;
    }
    nbtv_duplex_tx* tx = nbtv_duplex_tx_create(layout);
    nbtv_duplex_tx_set_voice_gain_db(tx, static_cast<float>(args.num("voice-gain", 0.0)));
    // 兩路後面各補一小段靜音，把濾波器裡的尾巴推出來
    picture.resize(picture.size() + NBTV_DUPLEX_PICTURE_RATE / 20, 0.0f);
    voice.resize(voice.size() + 2 * (NBTV_DUPLEX_VOICE_RATE / 20), 0.0f);
    if (args.has("picture")) nbtv_duplex_tx_push_picture(tx, picture.data(), static_cast<int>(picture.size()));
    if (args.has("voice")) nbtv_duplex_tx_push_voice(tx, voice.data(), static_cast<int>(voice.size() / 2));
    const int picture_samples = nbtv_duplex_tx_pending_picture(tx), voice_samples = nbtv_duplex_tx_pending_voice(tx);
    const int content = std::max(picture_samples, voice_samples);
    const int lead = static_cast<int>(std::lround(std::max(0.0, args.num("lead", 0.5)) * NBTV_DUPLEX_RATE));
    const int tail = static_cast<int>(std::lround(std::max(0.0, args.num("tail", 0.5)) * NBTV_DUPLEX_RATE));
    std::vector<float> out(static_cast<std::size_t>(lead + content + tail), 0.0f);
    nbtv_duplex_tx_read(tx, out.data() + lead, content);
    nbtv_duplex_tx_destroy(tx);

    double energy = 0.0;
    float peak = 0.0f;
    long clipped = 0;
    for (int i = 0; i < content; ++i) {
        const float v = out[static_cast<std::size_t>(lead + i)];
        energy += static_cast<double>(v) * v;
        peak = std::max(peak, std::fabs(v));
        clipped += std::fabs(v) >= 1.0f ? 1 : 0;
    }
    if (!write_wav(args.str("out", ""), out, NBTV_DUPLEX_RATE)) {
        std::fprintf(stderr, "寫不了 %s\n", args.str("out", "").c_str());
        return 1;
    }
    std::printf("%s：%.2f 秒（畫面 %.2f 秒、語音 %.2f 秒），均方根 %.1f dBFS、峰值 %.3f、削到的取樣 %ld 個\n", args.str("out", "").c_str(),
                static_cast<double>(out.size()) / NBTV_DUPLEX_RATE, static_cast<double>(picture_samples) / NBTV_DUPLEX_RATE,
                static_cast<double>(voice_samples) / NBTV_DUPLEX_RATE, level_db(std::sqrt(energy / std::max(content, 1))), static_cast<double>(peak), clipped);
    return 0;
}

int demux(const Args& args, int layout) {
    if (args.positional.size() < 2 || (!args.has("picture") && !args.has("voice"))) {
        usage();
        return 2;
    }
    std::vector<float> audio;
    int rate = 0;
    if (!read_wav(args.positional[1], audio, rate)) {
        std::fprintf(stderr, "讀不了 %s（需要 16 位元 PCM 或 32 位元浮點的 WAV）\n", args.positional[1].c_str());
        return 1;
    }
    if (rate == NBTV_DUPLEX_PICTURE_RATE) {
        // 12 kHz 的錄音：先換成 48 kHz（3 kHz 以下原樣，補零多出來的鏡像壓掉）
        std::vector<float> up;
        nbtv::duplex_detail::Resampler(NBTV_DUPLEX_RATE / NBTV_DUPLEX_PICTURE_RATE, 1, NBTV_DUPLEX_PICTURE_RATE, 3000.0, 9000.0).process(audio, up);
        audio.swap(up);
    } else if (rate != NBTV_DUPLEX_RATE) {
        std::fprintf(stderr, "取樣率 %d 不支援，請先轉成 48000 或 12000\n", rate);
        return 1;
    }
    nbtv_duplex_rx* rx = nbtv_duplex_rx_create(layout);
    std::vector<float> picture, voice, block(4096), iq(2 * 4096);
    // 一段一段地給，和即時收訊時一樣；最後補一小段靜音把濾波器裡的尾巴推出來
    audio.resize(audio.size() + NBTV_DUPLEX_RATE / 10, 0.0f);
    for (std::size_t pos = 0; pos < audio.size(); pos += 4800) {
        nbtv_duplex_rx_push(rx, audio.data() + pos, static_cast<int>(std::min<std::size_t>(4800, audio.size() - pos)));
        for (int n; (n = nbtv_duplex_rx_read_picture(rx, block.data(), static_cast<int>(block.size()))) > 0;) picture.insert(picture.end(), block.begin(), block.begin() + n);
        for (int n; (n = nbtv_duplex_rx_read_voice(rx, iq.data(), static_cast<int>(iq.size() / 2))) > 0;) voice.insert(voice.end(), iq.begin(), iq.begin() + 2 * n);
    }
    nbtv_duplex_rx_destroy(rx);
    if (args.has("picture")) {
        // 存成 16 位元時不要削到：畫面那一路的大小和收到的音訊差不多，但濾波之後峰值可能高一點
        float peak = 0.0f;
        for (float v : picture) peak = std::max(peak, std::fabs(v));
        if (peak > 0.98f) {
            for (float& v : picture) v *= 0.98f / peak;
        }
        if (!write_wav(args.str("picture", ""), picture, NBTV_DUPLEX_PICTURE_RATE)) {
            std::fprintf(stderr, "寫不了 %s\n", args.str("picture", "").c_str());
            return 1;
        }
        if (nbtv_duplex_band_shift_hz(layout) != 0.0) {
            std::printf("%s：畫面那一路，%.2f 秒（12 kHz）。解它：nbtv-rx --band-shift %.1f %s\n", args.str("picture", "").c_str(),
                        static_cast<double>(picture.size()) / NBTV_DUPLEX_PICTURE_RATE, nbtv_duplex_band_shift_hz(layout), args.str("picture", "").c_str());
        } else {
            std::printf("%s：畫面那一路，%.2f 秒（12 kHz；沒有搬過，語音還疊在上面）。解它：nbtv-rx %s\n", args.str("picture", "").c_str(),
                        static_cast<double>(picture.size()) / NBTV_DUPLEX_PICTURE_RATE, args.str("picture", "").c_str());
        }
    }
    if (args.has("voice")) {
        if (!write_iq(args.str("voice", ""), voice)) {
            std::fprintf(stderr, "寫不了 %s\n", args.str("voice", "").c_str());
            return 1;
        }
        std::printf("%s：語音那一路，%.2f 秒（8 kHz 的複數、中心 1500 Hz）。解它：nbtv-voice decode --codec v%d %s 語音.wav\n",
                    args.str("voice", "").c_str(), static_cast<double>(voice.size() / 2) / NBTV_DUPLEX_VOICE_RATE, nbtv_duplex_voice_codec(layout),
                    args.str("voice", "").c_str());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    tool_startup();
    const Args args(argc, argv, {"help"});
    if (args.positional.empty() || args.has("help")) {
        usage();
        return args.has("help") ? 0 : 2;
    }
    int layout = NBTV_DUPLEX_STANDARD;
    if (!parse_layout(args, layout)) return 2;
    const std::string& command = args.positional[0];
    if (command == "info") return info();
    if (command == "mux") return mux(args, layout);
    if (command == "demux") return demux(args, layout);
    usage();
    return 2;
}
