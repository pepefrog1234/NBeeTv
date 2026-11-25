// 開發用：--snapshot <目錄>。不開視窗、不碰任何裝置，把幾張版面各算成一張 PNG（對照 macOS 版的畫面、放進說明文件用）。
// 畫面、縮圖、狀態與瀑布圖都是把內附的測試圖真的經過核心收發算出來的。對應 macOS 版的 Snapshot.swift。
// 這裡準備的示範資料也給 --demo（開一個只有示範資料、不碰裝置的視窗）與自我測試用。
#pragma once

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "app.hpp"

namespace nbtvgui::snapshot {

/// 示範用的設定：呼號 BX4ACP、用序列埠的 RTS 控制 PTT。
Settings demo_settings(SourceKind source);
/// 今天的幾點幾分幾秒（本地時間），換成「1970 年起的秒數」。
double time_today(int hour, int minute, int second);

/// 接收中：對方（N0CALL）先後用模式 12、模式 11、模式 13 傳了幾張；目前最後一張收到一半。
/// quick：不經過核心收發，直接把測試圖放進「最近收到」（自我測試用，快很多）。
std::unique_ptr<App> make_receiving_app(bool quick = false);
/// 發射中：我們用模式 11 發射，已送出 8 張，第 9 張送到 1.6 秒。
std::unique_ptr<App> make_transmitting_app(const std::deque<App::ReceivedFrame>& history);
/// 待機：沒有訊號，只有電台通帶內的雜訊；發射來源選影片檔（work_directory 裡做一段示範影片；沒有 ffmpeg 時改用一張圖片）。
std::unique_ptr<App> make_idle_app(const std::string& work_directory);

/// 把六張版面算成 PNG。languages 有不只一種語言時，每種語言各放在 <目錄>/<語言> 裡。回傳結束碼。
int run(const std::string& directory, const std::vector<std::string>& languages);

}  // namespace nbtvgui::snapshot
