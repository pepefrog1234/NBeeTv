// 自我測試（--selftest）：不開視窗、不碰任何真的裝置，把收發的流程、PTT、檔案來源、介面逐項跑一次。
// 對應 macOS 版的 SelfTest.swift（挑了和平台有關、最容易出錯的那些）。CI 在每個平台上建置完都會跑。
#pragma once

#include <string>

namespace nbtvgui::selftest {

struct Options {
    /// 只跑名稱是這個的那一項；空字串表示全部
    std::string only;
    /// 放暫存檔的目錄；空字串表示在系統的暫存目錄裡開一個，跑完刪掉
    std::string work_directory;
    /// 這個執行檔自己的路徑（有幾項要另外啟動一個自己）
    std::string executable;
};

/// 回傳結束碼：全部通過是 0，有失敗的是 1。
int run(const Options& options);

}  // namespace nbtvgui::selftest
