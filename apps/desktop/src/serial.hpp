// 序列埠的控制線（RTS、DTR）：PTT 用。只動控制線，不收送資料。
// Windows 用 Win32（COM 埠），其他用 termios 的 ioctl。
#pragma once

#include <string>
#include <vector>

namespace nbtvgui {

/// 系統上的序列埠：Windows 是「COM3」，Linux 是「/dev/ttyUSB0」，macOS 是「/dev/cu.…」。
std::vector<std::string> serial_ports();

class SerialPort {
public:
    SerialPort() = default;
    ~SerialPort() { close(); }
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    /// 開啟（不改速率等設定）。失敗時 error 是系統給的原因。
    bool open(const std::string& path, std::string* error);
    bool is_open() const;
    /// 把 RTS（rts 是 true）或 DTR 拉高或拉低。
    bool set_line(bool rts, bool high, std::string* error);
    /// 兩條線都拉低之後關閉。
    void close();

private:
#if defined(_WIN32)
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

}  // namespace nbtvgui
