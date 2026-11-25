// 一點點 TCP：對 rigctld 送一行指令、讀一行回覆（每次一條新的連線），以及找一個沒有人用的本機連接埠。
// Windows 用 Winsock，其他用 BSD sockets。
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nbtvgui::net {

struct Reply {
    enum class Failure {
        none,         // 成功，text 是回覆
        resolve,      // 找不到那個位址
        connect,      // 連不上；detail 是原因
        send,         // 連上了但指令送不出去
        no_reply,     // 送出去了，沒有等到回覆；timed_out 分得出是等到逾時還是對方直接掛斷
    };
    Failure failure = Failure::none;
    std::string text;
    std::string detail;
    bool timed_out = false;
    bool ok() const { return failure == Failure::none; }
};

/// 程式開始時呼叫一次（Windows 要先啟動 Winsock）。
void startup();

/// 連到 host:port，送出 text，讀到回覆裡出現換行為止。連線、等回覆各最多等 timeout_seconds 秒。
Reply request(const std::string& host, const std::string& port, const std::string& text, int timeout_seconds);

/// 向系統要一個目前沒有人用的本機連接埠；要不到時回傳 0。
int free_local_port();

/// 本機的這個連接埠連得上嗎？
bool can_connect_local(int port);

/// 自我測試用的假 rigctld：聽本機的一個臨時連接埠；每條連線讀一行、記下來、照 reply 回一行，然後掛斷。
class LineServer {
public:
    /// reply：收到一行指令（不含換行）時要回什麼；回空字串表示什麼都不回、直接掛斷。在伺服器自己的執行緒上呼叫。
    explicit LineServer(std::function<std::string(const std::string&)> reply);
    ~LineServer();
    LineServer(const LineServer&) = delete;
    LineServer& operator=(const LineServer&) = delete;

    /// 聽的連接埠；沒有開成功是 0。
    int port() const;
    /// 到目前為止收到的每一行，照順序。
    std::vector<std::string> received() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nbtvgui::net
