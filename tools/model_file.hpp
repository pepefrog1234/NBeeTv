// 命令列工具共用：讀入學習式模式的權重檔。
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace nbtv::tools {

// 沒有用 --model 指定時用的權重檔（專案的 models/ 底下）。每個學習式模式各一份，檔名帶著它的實數個數
// （模式的 learned_values）、是不是加寬的模型（learned_width 是 64）與版本：
// 模式 11 是 learned_7776_v3.bin，模式 12 是 learned_2592_v2.bin，模式 13 是 learned_7776_wide_v1.bin。
inline std::string default_model_path(int values, int width) {
    const char* version = width == 64 ? "wide_v1" : values == 7776 ? "v3" : "v2";
    return std::string(NBTV_MODEL_DIR) + "/learned_" + std::to_string(values) + "_" + version + ".bin";
}

inline bool read_file(const std::string& path, std::vector<std::uint8_t>& bytes) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    std::uint8_t chunk[65536];
    for (std::size_t n = 0; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) bytes.insert(bytes.end(), chunk, chunk + n);
    std::fclose(f);
    return !bytes.empty();
}

}  // namespace nbtv::tools
