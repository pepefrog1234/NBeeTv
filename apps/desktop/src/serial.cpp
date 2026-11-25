#include "serial.hpp"

#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace nbtvgui {

#if defined(_WIN32)

namespace {

std::string system_error(DWORD code) {
    char buffer[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buffer, sizeof buffer, nullptr);
    std::string text = buffer;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.')) text.pop_back();
    return text.empty() ? "error " + std::to_string(code) : text;
}

}  // namespace

std::vector<std::string> serial_ports() {
    // 系統把目前有的 COM 埠列在登錄檔的這個位置（裝置管理員看到的就是它）
    std::vector<std::string> ports;
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) != ERROR_SUCCESS) return ports;
    for (DWORD index = 0;; ++index) {
        char name[256], value[256];
        DWORD name_size = sizeof name, value_size = sizeof value, type = 0;
        if (RegEnumValueA(key, index, name, &name_size, nullptr, &type, reinterpret_cast<BYTE*>(value), &value_size) != ERROR_SUCCESS) break;
        if (type != REG_SZ || value_size == 0) continue;
        value[std::min<DWORD>(value_size, sizeof value - 1)] = '\0';
        ports.emplace_back(value);
    }
    RegCloseKey(key);
    // COM3 排在 COM10 前面
    std::sort(ports.begin(), ports.end(), [](const std::string& a, const std::string& b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    return ports;
}

bool SerialPort::open(const std::string& path, std::string* error) {
    close();
    // COM10 以上要寫成 \\.\COM10；COM1–9 這樣寫也可以
    const std::string device = path.rfind("\\\\", 0) == 0 ? path : "\\\\.\\" + path;
    HANDLE handle = CreateFileA(device.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (error != nullptr) *error = system_error(GetLastError());
        return false;
    }
    // 系統開埠時會照預設把 DTR、RTS 拉高：改成由我們自己控制，並先都放掉
    DCB dcb{};
    dcb.DCBlength = sizeof dcb;
    if (GetCommState(handle, &dcb)) {
        dcb.fDtrControl = DTR_CONTROL_DISABLE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        SetCommState(handle, &dcb);
    }
    EscapeCommFunction(handle, CLRRTS);
    EscapeCommFunction(handle, CLRDTR);
    handle_ = handle;
    return true;
}

bool SerialPort::is_open() const { return handle_ != nullptr; }

bool SerialPort::set_line(bool rts, bool high, std::string* error) {
    if (handle_ == nullptr) return false;
    const DWORD function = rts ? (high ? SETRTS : CLRRTS) : (high ? SETDTR : CLRDTR);
    if (EscapeCommFunction(static_cast<HANDLE>(handle_), function)) return true;
    if (error != nullptr) *error = system_error(GetLastError());
    return false;
}

void SerialPort::close() {
    if (handle_ == nullptr) return;
    EscapeCommFunction(static_cast<HANDLE>(handle_), CLRRTS);
    EscapeCommFunction(static_cast<HANDLE>(handle_), CLRDTR);
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
}

#else

std::vector<std::string> serial_ports() {
    std::vector<std::string> ports;
    std::error_code ignored;
#if defined(__APPLE__)
    for (const auto& entry : std::filesystem::directory_iterator("/dev", ignored)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("cu.", 0) == 0) ports.push_back("/dev/" + name);
    }
#else
    // USB 轉序列埠、USB CDC、樹莓派的 UART、藍牙序列埠；主機板上的 ttyS* 只列真的接著硬體的
    // （沒有硬體的那些，/sys 裡它的裝置掛在 platform 底下）
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/tty", ignored)) {
        const std::string name = entry.path().filename().string();
        const bool usual = name.rfind("ttyUSB", 0) == 0 || name.rfind("ttyACM", 0) == 0 || name.rfind("ttyAMA", 0) == 0 || name.rfind("rfcomm", 0) == 0;
        bool onboard = false;
        if (name.rfind("ttyS", 0) == 0) {
            const std::filesystem::path subsystem = std::filesystem::read_symlink(entry.path() / "device" / "subsystem", ignored);
            onboard = !ignored && !subsystem.empty() && subsystem.filename() != "platform";
            ignored.clear();
        }
        if ((usual || onboard) && std::filesystem::exists("/dev/" + name, ignored)) ports.push_back("/dev/" + name);
    }
#endif
    std::sort(ports.begin(), ports.end(), [](const std::string& a, const std::string& b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    return ports;
}

bool SerialPort::open(const std::string& path, std::string* error) {
    close();
    fd_ = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
        if (error != nullptr) *error = std::strerror(errno);
        return false;
    }
    return true;
}

bool SerialPort::is_open() const { return fd_ >= 0; }

bool SerialPort::set_line(bool rts, bool high, std::string* error) {
    if (fd_ < 0) return false;
    int bit = rts ? TIOCM_RTS : TIOCM_DTR;
    if (ioctl(fd_, high ? TIOCMBIS : TIOCMBIC, &bit) == 0) return true;
    if (error != nullptr) *error = std::strerror(errno);
    return false;
}

void SerialPort::close() {
    if (fd_ < 0) return;
    int bits = TIOCM_RTS | TIOCM_DTR;
    ioctl(fd_, TIOCMBIC, &bits);
    ::close(fd_);
    fd_ = -1;
}

#endif

}  // namespace nbtvgui
