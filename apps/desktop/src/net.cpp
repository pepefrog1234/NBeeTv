#include "net.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static const socket_t kNoSocket = INVALID_SOCKET;
static void close_socket(socket_t s) { closesocket(s); }
static int last_error() { return WSAGetLastError(); }
static bool in_progress(int error) { return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS; }
static bool would_block(int error) { return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK; }
static std::string error_text(int error) {
    char buffer[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(error), 0, buffer, sizeof buffer, nullptr);
    std::string text = buffer;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text.empty() ? "error " + std::to_string(error) : text;
}
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using socket_t = int;
static const socket_t kNoSocket = -1;
static void close_socket(socket_t s) { close(s); }
static int last_error() { return errno; }
static bool in_progress(int error) { return error == EINPROGRESS; }
static bool would_block(int error) { return error == EAGAIN || error == EWOULDBLOCK; }
static std::string error_text(int error) { return std::strerror(error); }
#endif

namespace nbtvgui::net {

void startup() {
#if defined(_WIN32)
    static bool started = false;
    if (!started) {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
        started = true;
    }
#endif
}

namespace {

void set_blocking(socket_t s, bool blocking) {
#if defined(_WIN32)
    u_long mode = blocking ? 0 : 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    const int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
#endif
}

void set_timeouts(socket_t s, int seconds) {
#if defined(_WIN32)
    const DWORD ms = static_cast<DWORD>(seconds) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
#else
    timeval tv{};
    tv.tv_sec = seconds;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#if defined(SO_NOSIGPIPE)
    // 對方若在我們送指令前就斷線，send 要回報錯誤，而不是讓整個程式收到 SIGPIPE 結束
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
#endif
}

/// 非阻塞連線，最多等 timeout_seconds 秒。失敗時 reason 是原因。
bool connect_with_timeout(socket_t s, const sockaddr* address, int length, int timeout_seconds, std::string& reason) {
    set_blocking(s, false);
    bool ok = connect(s, address, static_cast<socklen_t>(length)) == 0;
    if (!ok) {
        const int error = last_error();
        if (in_progress(error)) {
#if defined(_WIN32)
            WSAPOLLFD pfd{};
            pfd.fd = s;
            pfd.events = POLLOUT;
            const int ready = WSAPoll(&pfd, 1, timeout_seconds * 1000);
#else
            pollfd pfd{};
            pfd.fd = s;
            pfd.events = POLLOUT;
            const int ready = poll(&pfd, 1, timeout_seconds * 1000);
#endif
            if (ready > 0) {
                int so_error = 0;
                socklen_t size = sizeof so_error;
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &size);
                ok = so_error == 0;
                if (!ok) reason = error_text(so_error);
            } else {
                reason = "timeout";
            }
        } else {
            reason = error_text(error);
        }
    }
    set_blocking(s, true);
    return ok;
}

sockaddr_in loopback(int port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<unsigned short>(port));
    return address;
}

}  // namespace

Reply request(const std::string& host, const std::string& port, const std::string& text, int timeout_seconds) {
    startup();
    Reply reply;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &list) != 0 || list == nullptr) {
        reply.failure = Reply::Failure::resolve;
        return reply;
    }
    reply.failure = Reply::Failure::connect;
    for (addrinfo* info = list; info != nullptr; info = info->ai_next) {
        const socket_t s = socket(info->ai_family, info->ai_socktype, info->ai_protocol);
        if (s == kNoSocket) continue;
        std::string reason;
        if (!connect_with_timeout(s, info->ai_addr, static_cast<int>(info->ai_addrlen), timeout_seconds, reason)) {
            reply.detail = reason;
            close_socket(s);
            continue;
        }
        set_timeouts(s, timeout_seconds);
#if defined(MSG_NOSIGNAL)
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0;
#endif
        if (send(s, text.data(), static_cast<int>(text.size()), flags) != static_cast<int>(text.size())) {
            reply.failure = Reply::Failure::send;
            close_socket(s);
            break;
        }
        char buffer[256];
        while (reply.text.find('\n') == std::string::npos) {
            const int n = static_cast<int>(recv(s, buffer, sizeof buffer, 0));
            if (n <= 0) {
                reply.timed_out = n < 0 && would_block(last_error());
                break;
            }
            reply.text.append(buffer, static_cast<std::size_t>(n));
        }
        close_socket(s);
        reply.failure = reply.text.empty() ? Reply::Failure::no_reply : Reply::Failure::none;
        break;
    }
    freeaddrinfo(list);
    return reply;
}

int free_local_port() {
    startup();
    const socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == kNoSocket) return 0;
    sockaddr_in address = loopback(0);
    socklen_t length = sizeof address;
    int port = 0;
    if (bind(s, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0 && getsockname(s, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
        port = ntohs(address.sin_port);
    }
    close_socket(s);
    return port;
}

bool can_connect_local(int port) {
    startup();
    const socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == kNoSocket) return false;
    const sockaddr_in address = loopback(port);
    std::string reason;
    const bool ok = connect_with_timeout(s, reinterpret_cast<const sockaddr*>(&address), sizeof address, 1, reason);
    close_socket(s);
    return ok;
}

// ---------------------------------------------------------------- 自我測試用的假 rigctld

struct LineServer::Impl {
    socket_t listener = kNoSocket;
    int port = 0;
    std::function<std::string(const std::string&)> reply;
    std::atomic<bool> stopping{false};
    mutable std::mutex mutex;
    std::vector<std::string> lines;
    std::thread thread;

    void serve() {
        while (!stopping.load()) {
            // 每 50 毫秒看一次要不要結束
#if defined(_WIN32)
            WSAPOLLFD pfd{};
            pfd.fd = listener;
            pfd.events = POLLIN;
            const int ready = WSAPoll(&pfd, 1, 50);
#else
            pollfd pfd{};
            pfd.fd = listener;
            pfd.events = POLLIN;
            const int ready = poll(&pfd, 1, 50);
#endif
            if (ready <= 0) continue;
            const socket_t client = accept(listener, nullptr, nullptr);
            if (client == kNoSocket) continue;
            set_blocking(client, true);
            set_timeouts(client, 2);
            std::string text;
            char buffer[256];
            while (text.find('\n') == std::string::npos) {
                const int n = static_cast<int>(recv(client, buffer, sizeof buffer, 0));
                if (n <= 0) break;
                text.append(buffer, static_cast<std::size_t>(n));
            }
            const std::string line = text.substr(0, text.find('\n'));
            {
                std::lock_guard<std::mutex> lock(mutex);
                lines.push_back(line);
            }
            const std::string answer = reply ? reply(line) : std::string();
#if defined(MSG_NOSIGNAL)
            const int flags = MSG_NOSIGNAL;
#else
            const int flags = 0;
#endif
            if (!answer.empty()) send(client, answer.data(), static_cast<int>(answer.size()), flags);
            close_socket(client);
        }
    }
};

LineServer::LineServer(std::function<std::string(const std::string&)> reply) : impl_(std::make_unique<Impl>()) {
    startup();
    impl_->reply = std::move(reply);
    impl_->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (impl_->listener == kNoSocket) return;
    sockaddr_in address = loopback(0);
    socklen_t length = sizeof address;
    if (bind(impl_->listener, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || listen(impl_->listener, 8) != 0 ||
        getsockname(impl_->listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        close_socket(impl_->listener);
        impl_->listener = kNoSocket;
        return;
    }
    impl_->port = ntohs(address.sin_port);
    impl_->thread = std::thread([impl = impl_.get()] { impl->serve(); });
}

LineServer::~LineServer() {
    impl_->stopping = true;
    if (impl_->thread.joinable()) impl_->thread.join();
    if (impl_->listener != kNoSocket) close_socket(impl_->listener);
}

int LineServer::port() const { return impl_->port; }

std::vector<std::string> LineServer::received() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->lines;
}

}  // namespace nbtvgui::net
