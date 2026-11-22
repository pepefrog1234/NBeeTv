// 命令列工具共用：讀入學習式模式的權重檔。
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace nbtv::tools {

// 沒有用 --model 指定時用的權重檔（專案的 models/ 底下）
inline std::string default_model_path() { return std::string(NBTV_MODEL_DIR) + "/learned_7776_v2.bin"; }

inline bool read_file(const std::string& path, std::vector<std::uint8_t>& bytes) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    std::uint8_t chunk[65536];
    for (std::size_t n = 0; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) bytes.insert(bytes.end(), chunk, chunk + n);
    std::fclose(f);
    return !bytes.empty();
}

}  // namespace nbtv::tools
