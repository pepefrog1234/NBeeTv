// 執行別的程式（rigctld、rigctl、ffmpeg、ffprobe，還有看門用的自己）：SDL3 的行程 API 包一層。
#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nbtvgui {

class Process {
public:
    struct Options {
        std::vector<std::string> args;   // 第一個是程式的路徑
        bool pipe_stdin = false;         // 我們握著它的標準輸入（寫的那一頭）
        bool pipe_stdout = false;        // 我們讀它的標準輸出
        bool pipe_stderr = false;        // 我們讀它的標準錯誤
    };

    /// 啟動。沒有接管線的標準輸入輸出都接到空的。失敗時回傳空的，error 是原因。
    static std::unique_ptr<Process> spawn(const Options& options, std::string* error);

    /// 還在執行的話強制結束。
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    SDL_IOStream* out() const;   // 沒有接管線時是 nullptr
    SDL_IOStream* err() const;
    /// 它的標準輸入（我們寫的那一頭）；沒有接管線或已經放掉時是 nullptr
    SDL_IOStream* in() const;
    /// 放掉標準輸入寫的那一頭（對方讀到結尾）。
    void close_stdin();

    long pid() const;
    /// 結束了嗎；結束了的話 exit_code 是它的結束碼。
    /// 注意：Windows 上拿不到結束碼，一律是 0（為了不跳出黑色的主控台視窗，行程是以 SDL 的「背景」方式啟動的，
    /// 那種方式不提供結束碼）。要知道對方有沒有做成功，另外看它的輸出（檔案有沒有寫出來、印了什麼）。
    bool exited(int* exit_code = nullptr);
    /// 最多等這麼多秒；時間內結束了回傳 true。
    bool wait(double seconds, int* exit_code = nullptr);
    /// 請它結束（POSIX 是 SIGTERM；Windows 沒有溫和的辦法，一律直接結束）。force：SIGKILL。
    void kill(bool force);

private:
    explicit Process(SDL_Process* handle) : handle_(handle) {}
    SDL_Process* handle_;
    bool stdin_closed_ = false;
};

/// 把一條管線讀到結尾（對方關掉或結束）為止。
std::string read_all(SDL_IOStream* stream);

/// 執行一個程式、等它結束，回傳它印到標準輸出的文字（from_stderr：改收標準錯誤的）。超過時間還沒結束就中止它，回傳空的。
std::optional<std::string> run_and_capture(const std::vector<std::string>& args, double timeout_seconds = 5, bool from_stderr = false);

/// 從管線讀滿 size 個位元組；對方先關掉（或結束）了就回傳 false。
bool read_exact(SDL_IOStream* stream, void* buffer, std::size_t size);
/// 把 size 個位元組全部寫進管線；對方不收了就回傳 false。
bool write_exact(SDL_IOStream* stream, const void* buffer, std::size_t size);

/// 找一個程式放在哪裡（Windows 會補上 .exe）：依序找執行檔自己的目錄、extra_dirs、PATH。找不到回傳空字串。
/// 自己的目錄最先：下載包裡附的程式（Windows 的包附了 ffmpeg）比電腦上另外裝的優先，用的就是和 app 一起檢查過的那一個。
std::string find_program(const std::string& name, const std::vector<std::string>& extra_dirs = {});

/// 以行程編號結束一個不是我們的 Process 物件的行程（看門的程序用）。
void kill_pid(long pid);

/// 這個檔案在 Windows 上有沒有標成「隱藏」（那裡的隱藏是檔案的屬性，不看檔名）。其他系統一律是 false：
/// 那邊隱藏的檔案是名稱以點開頭的，由呼叫的人自己看。
bool has_hidden_attribute(const std::string& path);

}  // namespace nbtvgui
