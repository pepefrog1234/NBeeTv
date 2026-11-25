// 找系統上的字型檔。介面有五種語言，中文、日文的字要靠系統的字型：
// Windows 用內建的（Segoe UI、微軟正黑體、微軟雅黑、Yu Gothic），Linux 問 fontconfig（fc-match），找不到再試幾個常見的位置。
// 介面（Dear ImGui）與畫布（印在送出畫面上的字）用的都是這裡找到的檔案。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace nbtvgui {

struct FontFile {
    std::string path;
    int index = 0;   // .ttc 裡的第幾個字型
};

/// 每一組的第一個是主要的字型，後面的是它沒有的字（中文、日文）要去找的字型，依序找。
struct FontSet {
    std::vector<FontFile> regular;
    std::vector<FontFile> bold;
    std::vector<FontFile> mono;
    std::vector<FontFile> mono_bold;   // 等寬的粗體：呼號、訊雜比、發射的時間這些要醒目的數字
    /// 找到的字型裡有沒有涵蓋中文、日文的
    bool has_cjk = false;
};

/// language 是介面的語言（zh-Hant、zh-Hans、ja、es、en）：同一個漢字在繁體、簡體、日文的寫法不同，優先用那個語言的字型。
FontSet find_fonts(const std::string& language);

/// 一個字型檔的內容，唯讀地對應到記憶體：用到哪一塊，系統才真的去讀那一塊。中文、日文的字型檔一個就是一二十 MB，
/// 介面的四組字型（一般、粗體、等寬、等寬粗體）各自要接上好幾個；整個讀進來會佔掉一兩百 MB，對應的方式幾乎不佔。
struct FontData {
    const unsigned char* bytes = nullptr;
    std::size_t size = 0;
};
/// 同一個檔案只對應一次，之後一直留著（到程式結束），所以拿到的指標可以一直用。開不了回傳 nullptr。
const FontData* font_data(const std::string& path);

}  // namespace nbtvgui
