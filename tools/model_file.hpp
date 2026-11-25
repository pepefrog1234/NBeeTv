// 命令列工具共用：讀入各個模式的權重檔。
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace nbtv::tools {

// 執行檔所在的目錄（結尾沒有斜線）。工具在 main() 開頭用 tool_startup() 填進來（startup.hpp）；
// 沒有填的時候（單元測試）是空字串，那就不找執行檔旁邊。
inline std::string& program_dir() {
    static std::string dir;
    return dir;
}

// 權重檔 <名稱>.bin 的路徑。依序找下面幾個目錄，回傳第一個有這個檔案的：
//   1. 環境變數 NBTV_MODEL_DIR 指的目錄（有設的話）；
//   2. 執行檔旁邊的 models/——下載來的發行包（-DNBTV_INSTALL_FLAT=ON 裝出來的目錄）是這樣放；
//   3. 執行檔上一層的 share/nbtv/models/——一般的安裝配置（執行檔在 bin/ 裡）；
//   4. 建置時專案的 models/——自己從原始碼建置、直接執行建置目錄裡的那一份時。
// 都沒有時回傳第一個候選的路徑，錯誤訊息裡看得到原本該放在哪裡。
inline std::string model_file_path(const char* name) {
    const std::string file = std::string(name) + ".bin";
    std::vector<std::string> dirs;
    if (const char* env = std::getenv("NBTV_MODEL_DIR"); env != nullptr && *env != '\0') dirs.emplace_back(env);
    if (!program_dir().empty()) {
        dirs.push_back(program_dir() + "/models");
        dirs.push_back(program_dir() + "/../share/nbtv/models");
    }
    dirs.emplace_back(NBTV_MODEL_DIR);
    for (const std::string& dir : dirs) {
        const std::string path = dir + "/" + file;
        if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
            std::fclose(f);
            return path;
        }
    }
    return dirs.front() + "/" + file;
}

// 沒有用 --model 指定時用的權重檔，每個模式各一份。
// 權重檔以模型訓練完成的日期命名：「年月日-當天第幾個完成的」，例如 261123-1 是 2026 年 11 月 23 日完成的第一個。
// 各個模型的來歷見 tools/learned/README.md 的一覽表。沒有這個模式的權重時回傳空字串。
inline std::string default_model_path(int mode_id) {
    const char* name = mode_id == 11 ? "261123-1" : mode_id == 12 ? "261122-3" : mode_id == 13 ? "261122-4" : mode_id == 14 ? "261123-4" : nullptr;
    return name != nullptr ? model_file_path(name) : std::string();
}

// 接收端合併最近幾幀用的權重檔（core/src/learned_merge.hpp），每個模式的模型各配一份；沒有的模式回傳空字串。
inline std::string default_merge_path(int mode_id) {
    const char* name = mode_id == 11 ? "261124-10" : mode_id == 12 ? "261124-8" : mode_id == 13 ? "261124-9" : mode_id == 14 ? "261124-7" : nullptr;
    return name != nullptr ? model_file_path(name) : std::string();
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
