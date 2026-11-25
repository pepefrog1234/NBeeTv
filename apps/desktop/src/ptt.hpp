// PTT 控制：不控制（VOX）、序列埠的 RTS／DTR、電台的 CAT 指令（經序列埠，由 Hamlib 翻譯）、Hamlib 的 rigctld（TCP）。
// 對應 macOS 版的 PTT.swift，行為照它的。key() 可能會卡住一下（開序列埠、連線、等電台回應），要在背景的 Worker 上呼叫。
#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "process.hpp"
#include "serial.hpp"

namespace nbtvgui {

enum class PTTMode {
    none,      // 不控制，使用電台的 VOX
    serial,    // 序列埠的 RTS 或 DTR
    cat,       // 電台的 CAT 指令，經序列埠（各廠牌的指令由這台電腦上的 Hamlib 翻譯）
    rigctld,   // Hamlib rigctld（TCP）：使用者自己開著的 rigctld
};
const char* to_string(PTTMode mode);
PTTMode ptt_mode_from(const std::string& text);

enum class SerialLine { rts, dtr };
inline const char* title(SerialLine line) { return line == SerialLine::rts ? "RTS" : "DTR"; }

struct PTTError {
    std::string message;
    /// 連上了、指令也送出去了，但等到逾時都沒有回覆（和「連不上」「對方馬上掛斷」不一樣：那兩種很快就知道）
    bool timed_out = false;
    /// 指令可能已經到了電台、PTT 也許還按著：按下的指令送到了卻沒有得到確認，之後補送的放開也沒有成功；或是放開的指令失敗了。
    /// 外殼看到這個要提醒使用者去看電台。
    bool may_be_keyed = false;
};
/// 沒有值表示成功。
using PTTResult = std::optional<PTTError>;

class PTTDriver {
public:
    virtual ~PTTDriver() = default;
    /// 按下（true）或放開（false）PTT。
    virtual PTTResult key(bool on) = 0;
};

class NoPTT : public PTTDriver {
public:
    PTTResult key(bool) override { return std::nullopt; }
};

/// 序列埠：按下時把指定的控制線拉高，另一條拉低；放開時兩條都拉低並關閉序列埠。
class SerialPTT : public PTTDriver {
public:
    SerialPTT(std::string path, SerialLine line) : path_(std::move(path)), line_(line) {}
    PTTResult key(bool on) override;

private:
    std::string path_;
    SerialLine line_;
    SerialPort port_;
};

/// Hamlib rigctld：每次連線送一個指令，「T 1」按下、「T 0」放開，回覆「RPRT 0」表示成功。
class RigctldPTT : public PTTDriver {
public:
    /// timeout_seconds：連線、等回覆各最多等幾秒
    RigctldPTT(std::string host, std::string port, int timeout_seconds = 2) : host_(std::move(host)), port_(std::move(port)), timeout_seconds_(timeout_seconds) {}
    PTTResult key(bool on) override;
    /// 送出一行指令並讀回覆（測試時也用來查詢狀態）。失敗時回傳錯誤。
    PTTResult send(const std::string& command, std::string& reply);

private:
    std::string host_, port_;
    int timeout_seconds_;
};

/// 這台電腦上裝的 Hamlib：找它的程式（rigctl、rigctld）、列出它支援的電台型號。
/// app 自己不帶 Hamlib；沒有裝的話，「電台的 CAT 指令」那一項不能用（其他三項不受影響）。
namespace hamlib {

struct Model {
    int id = 0;                // Hamlib 的型號編號（rigctld -m 的參數）
    std::string manufacturer;
    std::string name;
    std::string title() const { return manufacturer.empty() ? name : manufacturer + " " + name; }   // 「Icom IC-7300」
};

/// 一個型號的序列埠規格（rigctl --dump-caps 的「Serial speed」那一行）。不走序列埠的型號沒有。
struct SerialCaps {
    int min_baud = 0;
    int max_baud = 0;               // 沒有指定速率時 Hamlib 用最高的這個
    bool hardware_handshake = false; // 要用 RTS／CTS 做流量控制
    bool operator==(const SerialCaps& other) const { return min_baud == other.min_baud && max_baud == other.max_baud && hardware_handshake == other.hardware_handshake; }
};

/// 設定裡可以選的序列速率
const std::vector<int>& baud_rates();

/// rigctld、rigctl 放在哪裡；找不到是空字串。
std::string tool(const std::string& name);
bool installed();

std::vector<Model> parse_models(const std::string& listing);
/// Hamlib 支援的電台型號。第一次問的時候執行一次「rigctl -l」；沒有裝 Hamlib 時是空的，下次問會再找一次。
std::vector<Model> models();
/// 選單用：依廠牌分組，廠牌與型號都照名稱排（數字照大小，IC-705 排在 IC-7300 前面）。
std::vector<std::pair<std::string, std::vector<Model>>> models_by_manufacturer();

std::optional<SerialCaps> parse_serial_caps(const std::string& dump);
/// 那個型號的序列埠規格；不走序列埠的型號或沒有裝 Hamlib 時是空的。只印出 Hamlib 自己的資料，不會去開任何序列埠。
std::optional<SerialCaps> serial_caps(int model);

}  // namespace hamlib

/// 電台的 CAT 指令，經序列埠。按下 PTT 時在背景啟動一個只給自己用的 rigctld（只聽本機的一個臨時連接埠），對它送「T 1」；
/// 放開時送「T 0」再把它結束，序列埠也跟著還回去。
///
/// 用指令按下的 PTT 不會因為序列埠關了就放開，所以另外有一個看門的程序（就是這個執行檔自己，加上 --ptt-watchdog）：
/// 它的標準輸入接著我們手上的管線，這個 app 不管怎麼結束（當掉、被強制結束），管線一斷它就替我們送「T 0」、把 rigctld 結束。
class HamlibPTT : public PTTDriver {
public:
    HamlibPTT(int model, std::string port, int baud) : model_(model), port_(std::move(port)), baud_(baud) {}
    ~HamlibPTT() override { stop(); }
    PTTResult key(bool on) override;

    /// rigctld 聽的本機連接埠；沒有在執行時是空的（測試時用它查詢 PTT 的狀態）
    std::optional<int> tcp_port() const;
    /// 測試用：模擬 app 突然不在了——不送「T 0」、不結束 rigctld，只把看門程序的管線放掉。
    void debug_abandon();

    /// rigctld 的參數。caps 是那個型號的序列埠規格（不走序列埠的型號是空的）。
    static std::vector<std::string> arguments(int model, const std::string& port, int baud, int tcp_port, const std::optional<hamlib::SerialCaps>& caps);
    /// 從 rigctld 印到標準錯誤的訊息裡找開不了電台的原因；不是開不了電台時回傳空的。
    static std::optional<std::string> open_failure_reason(const std::string& text);
    static PTTResult press(int tcp_port);
    static PTTResult release(int tcp_port);

    /// 等 rigctld 把電台開起來，最多等這麼久。電台沒有回應時 Hamlib 自己要重試好幾秒才放棄。
    static double start_timeout;

private:
    PTTResult start(int& tcp_port);
    void stop();
    std::string error_text() const;

    const int model_;
    const std::string port_;
    const int baud_;
    mutable std::mutex mutex_;
    std::unique_ptr<Process> daemon_;
    std::unique_ptr<Process> watchdog_;
    std::thread error_reader_;
    std::string error_output_;   // rigctld 印到標準錯誤的訊息
    std::optional<int> listening_;
};

/// 看門程序的主程式（nbtv-gui --ptt-watchdog 連接埠 行程編號）：等標準輸入斷掉，然後對 rigctld 送「T 0」、把它結束。
int ptt_watchdog_main(const std::string& tcp_port, const std::string& pid);

}  // namespace nbtvgui
