// 命令列工具用的 WAV 讀寫：寫 16 位元 PCM 單聲道；讀 16 位元 PCM 或 32 位元浮點，多聲道時取第一個聲道。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace nbtv::tools {

inline bool write_wav(const std::string& path, const std::vector<float>& samples, int sample_rate) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(samples.size() * 2);
    auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + data_bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);  // PCM
    u16(1);  // 單聲道
    u32(static_cast<std::uint32_t>(sample_rate));
    u32(static_cast<std::uint32_t>(sample_rate) * 2);
    u16(2);
    u16(16);
    std::fwrite("data", 1, 4, f);
    u32(data_bytes);
    for (float s : samples) {
        const std::int16_t v = static_cast<std::int16_t>(std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f));
        std::fwrite(&v, 2, 1, f);
    }
    std::fclose(f);
    return true;
}

inline bool read_wav(const std::string& path, std::vector<float>& samples, int& sample_rate) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    std::vector<std::uint8_t> bytes;
    std::uint8_t buf[65536];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) return false;
    auto u16 = [&](std::size_t at) { return static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1] << 8)); };
    auto u32 = [&](std::size_t at) { return static_cast<std::uint32_t>(u16(at)) | (static_cast<std::uint32_t>(u16(at + 2)) << 16); };
    int format = 0, channels = 0, bits = 0;
    sample_rate = 0;
    samples.clear();
    for (std::size_t pos = 12; pos + 8 <= bytes.size();) {
        const std::uint32_t size = u32(pos + 4);
        const std::size_t body = pos + 8;
        if (body + size > bytes.size()) break;
        if (std::memcmp(&bytes[pos], "fmt ", 4) == 0 && size >= 16) {
            format = u16(body);
            channels = u16(body + 2);
            sample_rate = static_cast<int>(u32(body + 4));
            bits = u16(body + 14);
            if (format == 0xFFFE && size >= 26) format = u16(body + 24);  // WAVE_FORMAT_EXTENSIBLE
        } else if (std::memcmp(&bytes[pos], "data", 4) == 0) {
            if (channels < 1 || !((format == 1 && bits == 16) || (format == 3 && bits == 32))) return false;
            const std::size_t frame = static_cast<std::size_t>(channels) * static_cast<std::size_t>(bits / 8);
            for (std::size_t at = body; at + frame <= body + size; at += frame) {
                if (format == 1) {
                    samples.push_back(static_cast<float>(static_cast<std::int16_t>(u16(at))) / 32768.0f);
                } else {
                    float v;
                    std::memcpy(&v, &bytes[at], 4);
                    samples.push_back(v);
                }
            }
            return sample_rate > 0;
        }
        pos = body + size + (size & 1u);
    }
    return false;
}

}  // namespace nbtv::tools
