// 命令列工具共用：讀入各個模式的權重檔。
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace nbtv::tools {

// 沒有用 --model 指定時用的權重檔（專案的 models/ 底下），每個模式各一份。
// 權重檔以模型訓練完成的日期命名：「年月日-當天第幾個完成的」，例如 261123-1 是 2026 年 11 月 23 日完成的第一個。
// 各個模型的來歷見 tools/learned/README.md 的一覽表。沒有這個模式的權重時回傳空字串。
inline std::string default_model_path(int mode_id) {
    const char* name = mode_id == 11 ? "261123-1" : mode_id == 12 ? "261122-3" : mode_id == 13 ? "261122-4" : mode_id == 14 ? "261123-4" : nullptr;
    return name != nullptr ? std::string(NBTV_MODEL_DIR) + "/" + name + ".bin" : std::string();
}

// 接收端合併最近幾幀用的權重檔（core/src/learned_merge.hpp），每個模式的模型各配一份；沒有的模式回傳空字串。
inline std::string default_merge_path(int mode_id) {
    const char* name = mode_id == 11 ? "261124-10" : mode_id == 12 ? "261124-8" : mode_id == 13 ? "261124-9" : mode_id == 14 ? "261124-7" : nullptr;
    return name != nullptr ? std::string(NBTV_MODEL_DIR) + "/" + name + ".bin" : std::string();
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
