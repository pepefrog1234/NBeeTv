// nbtv-voice：即時語音的編解碼器（FreeDV 的 RADE V2）的檔案版：語音的 WAV ↔ 語音的訊號（8 kHz 的複數）。
// 和 nbtv-duplex、nbtv-tx、nbtv-rx 接起來，就能只用檔案把「語音與畫面共用 FM 語音通道」整條路走一遍。
// 只處理檔案，不碰音訊裝置。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "args.hpp"
#include "duplex.hpp"
#include "nbtv/voice.h"
#include "startup.hpp"
#include "wav.hpp"

using namespace nbtv::tools;

namespace {

void usage() {
    std::printf(
        "用法：\n"
        "  nbtv-voice encode 語音.wav 訊號.iq   語音 → 語音的訊號。WAV 要是 48 kHz 或 16 kHz（16 位元 PCM 或 32 位元浮點，\n"
        "                                       多聲道取第一個聲道）\n"
        "  nbtv-voice decode 訊號.iq 語音.wav   語音的訊號 → 語音（48 kHz 的 WAV）\n"
        "  nbtv-voice info                      印出編解碼器的名稱\n"
        "訊號是 8 kHz 的複數（32 位元浮點，I、Q 交錯，沒有檔頭）、中心 1500 Hz：nbtv-duplex mux 的 --voice 要的、\n"
        "demux 的 --voice 給的就是這種檔案。\n");
}

bool read_floats(const std::string& path, std::vector<float>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    float buf[4096];
    for (std::size_t n; (n = std::fread(buf, sizeof(float), 4096, f)) > 0;) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}

bool write_floats(const std::string& path, const std::vector<float>& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const bool ok = data.empty() || std::fwrite(data.data(), sizeof(float), data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}

int encode(const std::string& in, const std::string& out) {
    std::vector<float> speech;
    int rate = 0;
    if (!read_wav(in, speech, rate)) {
        std::fprintf(stderr, "讀不了 %s（需要 16 位元 PCM 或 32 位元浮點的 WAV）\n", in.c_str());
        return 1;
    }
    if (rate == 16000) {
        std::vector<float> up;
        nbtv::duplex_detail::Resampler(3, 1, 16000.0, 7400.0, 8600.0).process(speech, up);
        speech.swap(up);
    } else if (rate != NBTV_VOICE_SPEECH_RATE) {
        std::fprintf(stderr, "取樣率 %d 不支援，請先轉成 48000 或 16000\n", rate);
        return 1;
    }
    nbtv_voice_tx* tx = nbtv_voice_tx_create();
    if (tx == nullptr) {
        std::fprintf(stderr, "語音的編解碼器開不起來\n");
        return 1;
    }
    std::vector<float> iq, block(2 * 4096);
    auto drain = [&] {
        for (int n; (n = nbtv_voice_tx_read(tx, block.data(), 4096)) > 0;) iq.insert(iq.end(), block.begin(), block.begin() + 2 * n);
    };
    // 一小段一小段地給，和即時發射時一樣
    for (std::size_t pos = 0; pos < speech.size(); pos += 960) {
        nbtv_voice_tx_push(tx, speech.data() + pos, static_cast<int>(std::min<std::size_t>(960, speech.size() - pos)));
        drain();
    }
    nbtv_voice_tx_finish(tx);
    drain();
    nbtv_voice_tx_destroy(tx);
    if (!write_floats(out, iq)) {
        std::fprintf(stderr, "寫不了 %s\n", out.c_str());
        return 1;
    }
    double energy = 0.0;
    for (float v : iq) energy += static_cast<double>(v) * v;
    std::printf("%s：語音 %.2f 秒 → 訊號 %.2f 秒（含結尾的「結束」標記），均方根 %.3f\n", out.c_str(),
                static_cast<double>(speech.size()) / NBTV_VOICE_SPEECH_RATE, static_cast<double>(iq.size() / 2) / NBTV_VOICE_SIGNAL_RATE,
                iq.empty() ? 0.0 : std::sqrt(energy / static_cast<double>(iq.size() / 2)));
    return 0;
}

int decode(const std::string& in, const std::string& out) {
    std::vector<float> iq;
    if (!read_floats(in, iq)) {
        std::fprintf(stderr, "讀不了 %s\n", in.c_str());
        return 1;
    }
    iq.resize(iq.size() / 2 * 2);
    nbtv_voice_rx* rx = nbtv_voice_rx_create();
    if (rx == nullptr) {
        std::fprintf(stderr, "語音的編解碼器開不起來\n");
        return 1;
    }
    // 後面補一秒靜音，讓最後一段也解得出來
    iq.resize(iq.size() + 2 * NBTV_VOICE_SIGNAL_RATE, 0.0f);
    std::vector<float> speech, block(4800);
    long synced_blocks = 0, blocks = 0;
    double snr = 0.0;
    nbtv_voice_rx_status status{};
    for (std::size_t pos = 0; pos < iq.size() / 2; pos += 160) {
        nbtv_voice_rx_push(rx, iq.data() + 2 * pos, static_cast<int>(std::min<std::size_t>(160, iq.size() / 2 - pos)));
        for (int n; (n = nbtv_voice_rx_read(rx, block.data(), static_cast<int>(block.size()))) > 0;) speech.insert(speech.end(), block.begin(), block.begin() + n);
        nbtv_voice_rx_get_status(rx, &status);
        ++blocks;
        if (status.synced) {
            ++synced_blocks;
            snr += status.snr_db;
        }
    }
    nbtv_voice_rx_destroy(rx);
    if (!write_wav(out, speech, NBTV_VOICE_SPEECH_RATE)) {
        std::fprintf(stderr, "寫不了 %s\n", out.c_str());
        return 1;
    }
    std::printf("%s：訊號 %.2f 秒 → 語音 %.2f 秒；同步的時間 %.0f%%", out.c_str(), static_cast<double>(iq.size() / 2) / NBTV_VOICE_SIGNAL_RATE - 1.0,
                static_cast<double>(speech.size()) / NBTV_VOICE_SPEECH_RATE, blocks > 0 ? 100.0 * static_cast<double>(synced_blocks) / static_cast<double>(blocks) : 0.0);
    if (synced_blocks > 0) std::printf("、訊雜比平均 %.1f dB、時脈差 %+.0f ppm", snr / static_cast<double>(synced_blocks), static_cast<double>(status.clock_ppm));
    std::printf("、收到「結束」的標記 %d 次\n", status.overs_ended);
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
    const std::string& command = args.positional[0];
    if (command == "info") {
        if (nbtv_voice_available()) std::printf("%s\n", nbtv_voice_description());
        else std::printf("這份程式沒有把語音的編解碼器編進來（建置時要加 -DNBTV_VOICE=ON）\n");
        return 0;
    }
    if ((command != "encode" && command != "decode") || args.positional.size() != 3) {
        usage();
        return 2;
    }
    if (!nbtv_voice_available()) {
        std::fprintf(stderr, "這份程式沒有把語音的編解碼器編進來（建置時要加 -DNBTV_VOICE=ON）\n");
        return 1;
    }
    return command == "encode" ? encode(args.positional[1], args.positional[2]) : decode(args.positional[1], args.positional[2]);
}
