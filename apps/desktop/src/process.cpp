#include "process.hpp"

#include <chrono>
#include <cstdlib>
#include <future>
#include <thread>

#include "model_file.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace nbtvgui {

std::unique_ptr<Process> Process::spawn(const Options& options, std::string* error) {
    std::vector<const char*> argv;
    for (const std::string& arg : options.args) argv.push_back(arg.c_str());
    argv.push_back(nullptr);

    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, options.pipe_stdin ? SDL_PROCESS_STDIO_APP : SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, options.pipe_stdout ? SDL_PROCESS_STDIO_APP : SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, options.pipe_stderr ? SDL_PROCESS_STDIO_APP : SDL_PROCESS_STDIO_NULL);
#if defined(_WIN32)
    // 視窗程式啟動主控台程式時，Windows 會替它開一個黑色的主控台視窗；「背景」的行程不會。
    // （POSIX 上「背景」是另一回事：會脫離我們、拿不到結束碼，所以只在 Windows 設。）
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
#endif
    SDL_Process* handle = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (handle == nullptr) {
        if (error != nullptr) *error = SDL_GetError();
        return nullptr;
    }
    return std::unique_ptr<Process>(new Process(handle));
}

Process::~Process() {
    if (!exited()) {
        kill(true);
        SDL_WaitProcess(handle_, true, nullptr);
    }
    SDL_DestroyProcess(handle_);
}

SDL_IOStream* Process::out() const {
    return static_cast<SDL_IOStream*>(SDL_GetPointerProperty(SDL_GetProcessProperties(handle_), SDL_PROP_PROCESS_STDOUT_POINTER, nullptr));
}

SDL_IOStream* Process::err() const {
    return static_cast<SDL_IOStream*>(SDL_GetPointerProperty(SDL_GetProcessProperties(handle_), SDL_PROP_PROCESS_STDERR_POINTER, nullptr));
}

SDL_IOStream* Process::in() const { return stdin_closed_ ? nullptr : SDL_GetProcessInput(handle_); }

void Process::close_stdin() {
    if (stdin_closed_) return;
    stdin_closed_ = true;
    if (SDL_IOStream* in = SDL_GetProcessInput(handle_)) SDL_CloseIO(in);
}

long Process::pid() const {
    return static_cast<long>(SDL_GetNumberProperty(SDL_GetProcessProperties(handle_), SDL_PROP_PROCESS_PID_NUMBER, 0));
}

bool Process::exited(int* exit_code) { return SDL_WaitProcess(handle_, false, exit_code); }

bool Process::wait(double seconds, int* exit_code) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (!exited(exit_code)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

void Process::kill(bool force) {
#if defined(_WIN32)
    (void)force;
    SDL_KillProcess(handle_, true);
#else
    SDL_KillProcess(handle_, force);
#endif
}

std::string read_all(SDL_IOStream* stream) {
    std::string text;
    if (stream == nullptr) return text;
    char buffer[4096];
    for (;;) {
        const size_t n = SDL_ReadIO(stream, buffer, sizeof buffer);
        if (n > 0) {
            text.append(buffer, n);
            continue;
        }
        // 管線暫時沒有資料（SDL 的管線可能是非阻塞的）：等一下再讀；讀到結尾或出錯才結束
        if (SDL_GetIOStatus(stream) == SDL_IO_STATUS_NOT_READY) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        break;
    }
    return text;
}

bool read_exact(SDL_IOStream* stream, void* buffer, std::size_t size) {
    if (stream == nullptr) return false;
    auto* bytes = static_cast<char*>(buffer);
    std::size_t done = 0;
    while (done < size) {
        const size_t n = SDL_ReadIO(stream, bytes + done, size - done);
        if (n > 0) {
            done += n;
        } else if (SDL_GetIOStatus(stream) == SDL_IO_STATUS_NOT_READY) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } else {
            return false;
        }
    }
    return true;
}

bool write_exact(SDL_IOStream* stream, const void* buffer, std::size_t size) {
    if (stream == nullptr) return false;
    const auto* bytes = static_cast<const char*>(buffer);
    std::size_t done = 0;
    while (done < size) {
        const size_t n = SDL_WriteIO(stream, bytes + done, size - done);
        if (n > 0) {
            done += n;
        } else if (SDL_GetIOStatus(stream) == SDL_IO_STATUS_NOT_READY) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } else {
            return false;
        }
    }
    return true;
}

std::optional<std::string> run_and_capture(const std::vector<std::string>& args, double timeout_seconds, bool from_stderr) {
    Process::Options options;
    options.args = args;
    options.pipe_stdout = !from_stderr;
    options.pipe_stderr = from_stderr;
    std::unique_ptr<Process> process = Process::spawn(options, nullptr);
    if (!process) return std::nullopt;
    // 輸出要邊跑邊讀：等它結束才讀的話，輸出超過管線的緩衝它就卡住了
    std::future<std::string> output = std::async(std::launch::async, [stream = from_stderr ? process->err() : process->out()] { return read_all(stream); });
    if (output.wait_for(std::chrono::duration<double>(timeout_seconds)) != std::future_status::ready) {
        process->kill(true);   // 管線跟著斷，讀的那一邊就會結束
        output.wait();
        return std::nullopt;
    }
    std::string text = output.get();
    process->wait(1.0);
    return text;
}

std::string find_program(const std::string& name, const std::vector<std::string>& extra_dirs) {
#if defined(_WIN32)
    const std::string file = name + ".exe";
    const char separator = ';';
#else
    const std::string file = name;
    const char separator = ':';
#endif
    std::vector<std::string> dirs = extra_dirs;
    if (!nbtv::tools::program_dir().empty()) dirs.push_back(nbtv::tools::program_dir());
    if (const char* path = std::getenv("PATH")) {
        const std::string all = path;
        std::size_t start = 0;
        while (start <= all.size()) {
            const std::size_t end = all.find(separator, start);
            const std::string dir = all.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!dir.empty()) dirs.push_back(dir);
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    for (const std::string& dir : dirs) {
        const std::string candidate = dir + "/" + file;
        SDL_PathInfo info{};
        if (SDL_GetPathInfo(candidate.c_str(), &info) && info.type == SDL_PATHTYPE_FILE) return candidate;
    }
    return {};
}

void kill_pid(long pid) {
    if (pid <= 0) return;
#if defined(_WIN32)
    if (HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid))) {
        TerminateProcess(handle, 1);
        CloseHandle(handle);
    }
#else
    ::kill(static_cast<pid_t>(pid), SIGTERM);
#endif
}

}  // namespace nbtvgui
