#include "ptt.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>

#include "i18n.hpp"
#include "net.hpp"
#include "startup.hpp"

namespace nbtvgui {

namespace {

std::string trimmed(const std::string& text) {
    std::size_t first = 0, last = text.size();
    while (first < last && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) --last;
    return text.substr(first, last - first);
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find_first_of("\r\n", start);
        if (end == std::string::npos) end = text.size();
        if (end > start) lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

double now_seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

}  // namespace

const char* to_string(PTTMode mode) {
    switch (mode) {
    case PTTMode::serial: return "serial";
    case PTTMode::cat: return "cat";
    case PTTMode::rigctld: return "rigctld";
    case PTTMode::none: break;
    }
    return "none";
}

PTTMode ptt_mode_from(const std::string& text) {
    return text == "serial" ? PTTMode::serial : text == "cat" ? PTTMode::cat : text == "rigctld" ? PTTMode::rigctld : PTTMode::none;
}

// ---------------------------------------------------------------- 序列埠

PTTResult SerialPTT::key(bool on) {
    if (!on) {
        port_.close();
        return std::nullopt;
    }
    if (path_.empty()) return PTTError{L("還沒選擇序列埠")};
    std::string reason;
    if (!port_.is_open() && !port_.open(path_, &reason)) return PTTError{LF("無法開啟 %@：%@", {path_, reason})};
    // 剛開啟時驅動程式通常會把兩條線都拉高，先把不用的那條放掉
    const bool rts = line_ == SerialLine::rts;
    port_.set_line(!rts, false, nullptr);
    if (!port_.set_line(rts, true, &reason)) {
        port_.close();
        return PTTError{LF("無法控制 %@ 的 %@：%@", {path_, title(line_), reason})};
    }
    return std::nullopt;
}

// ---------------------------------------------------------------- rigctld

PTTResult RigctldPTT::send(const std::string& command, std::string& reply) {
    const std::string where = host_ + ":" + port_;
    const net::Reply result = net::request(host_, port_, command, timeout_seconds_);
    switch (result.failure) {
    case net::Reply::Failure::none:
        reply = result.text;
        return std::nullopt;
    case net::Reply::Failure::resolve:
        return PTTError{LF("找不到 rigctld 的位址 %@", {where})};
    case net::Reply::Failure::connect:
        return PTTError{LF("無法連線到 rigctld（%@）：%@", {where, result.detail == "timeout" ? L("連線逾時") : result.detail.empty() ? L("無法連線") : result.detail})};
    case net::Reply::Failure::send:
        return PTTError{LF("無法送指令給 rigctld（%@）", {where})};
    case net::Reply::Failure::no_reply:
        break;
    }
    PTTError error{LF("rigctld（%@）沒有回覆", {where})};
    error.timed_out = result.timed_out;
    return error;
}

PTTResult RigctldPTT::key(bool on) {
    std::string reply;
    if (PTTResult error = send(on ? "T 1\n" : "T 0\n", reply)) return error;
    if (reply.find("RPRT 0") == std::string::npos) return PTTError{LF("rigctld 回覆「%@」", {trimmed(reply)})};
    return std::nullopt;
}

// ---------------------------------------------------------------- Hamlib

namespace hamlib {

const std::vector<int>& baud_rates() {
    static const std::vector<int> rates = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
    return rates;
}

std::string tool(const std::string& name) {
    std::vector<std::string> dirs;
#if defined(_WIN32)
    // 官方的安裝程式裝在「Program Files\hamlib-w64-版本\bin」
    for (const char* variable : {"ProgramFiles", "ProgramW6432", "ProgramFiles(x86)"}) {
        const char* base = std::getenv(variable);
        if (base == nullptr || *base == '\0') continue;
        std::error_code ignored;
        for (const auto& entry : std::filesystem::directory_iterator(base, ignored)) {
            std::string folder = entry.path().filename().string();
            std::transform(folder.begin(), folder.end(), folder.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (folder.rfind("hamlib", 0) == 0) dirs.push_back((entry.path() / "bin").string());
        }
    }
#else
    dirs = {"/opt/homebrew/bin", "/usr/local/bin", "/opt/local/bin", "/usr/bin"};
#endif
    return find_program(name, dirs);
}

bool installed() { return !tool("rigctld").empty() && !tool("rigctl").empty(); }

// 「rigctl -l」的清單：每一列是「編號、廠牌、型號、版本、狀態、巨集名稱」，欄位用空白對齊；
// 廠牌與型號裡面可能有空白（「NET rigctl」「Dummy No VFO」），所以照標題列的欄位位置切，不能用空白分。
std::vector<Model> parse_models(const std::string& listing) {
    std::vector<Model> models;
    const std::vector<std::string> lines = lines_of(listing);
    const auto header = std::find_if(lines.begin(), lines.end(), [](const std::string& line) {
        return line.find("Mfg") != std::string::npos && line.find("Model") != std::string::npos;
    });
    if (header == lines.end()) return models;
    const std::size_t mfg_column = header->find("Mfg"), model_column = header->find("Model");
    for (const std::string& line : lines) {
        if (&line == &*header) continue;
        std::vector<std::string> tokens;
        std::size_t pos = 0;
        while (pos < line.size()) {
            while (pos < line.size() && line[pos] == ' ') ++pos;
            const std::size_t end = line.find(' ', pos);
            if (pos < line.size()) tokens.push_back(line.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
            pos = end == std::string::npos ? line.size() : end;
        }
        if (tokens.size() < 5 || line.size() <= model_column) continue;
        const int id = std::atoi(tokens[0].c_str());
        if (id <= 0 || tokens[0].find_first_not_of("0123456789") != std::string::npos) continue;
        // 最後三欄（版本、狀態、巨集名稱）裡面沒有空白：從後面數回來，前面剩下的就是廠牌與型號
        const std::size_t version_column = line.rfind(" " + tokens[tokens.size() - 3] + " ");
        if (version_column == std::string::npos || version_column <= model_column) continue;
        Model model;
        model.id = id;
        model.manufacturer = trimmed(line.substr(mfg_column, model_column - mfg_column));
        model.name = trimmed(line.substr(model_column, version_column - model_column));
        if (!model.name.empty()) models.push_back(model);
    }
    return models;
}

namespace {

std::mutex& cache_mutex() {
    static std::mutex mutex;
    return mutex;
}

/// 數字照大小、字母不分大小寫的比較：「IC-705」排在「IC-7300」前面。
bool natural_less(const std::string& a, const std::string& b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const bool da = std::isdigit(static_cast<unsigned char>(a[i])) != 0, db = std::isdigit(static_cast<unsigned char>(b[j])) != 0;
        if (da && db) {
            std::size_t ie = i, je = j;
            while (ie < a.size() && std::isdigit(static_cast<unsigned char>(a[ie]))) ++ie;
            while (je < b.size() && std::isdigit(static_cast<unsigned char>(b[je]))) ++je;
            std::string na = a.substr(i, ie - i), nb = b.substr(j, je - j);
            na.erase(0, std::min(na.find_first_not_of('0'), na.size() - 1));
            nb.erase(0, std::min(nb.find_first_not_of('0'), nb.size() - 1));
            if (na.size() != nb.size()) return na.size() < nb.size();
            if (na != nb) return na < nb;
            i = ie;
            j = je;
        } else {
            const int ca = std::tolower(static_cast<unsigned char>(a[i])), cb = std::tolower(static_cast<unsigned char>(b[j]));
            if (ca != cb) return ca < cb;
            ++i;
            ++j;
        }
    }
    return a.size() - i < b.size() - j;
}

}  // namespace

std::vector<Model> models() {
    static std::vector<Model> cached;
    std::lock_guard<std::mutex> lock(cache_mutex());
    if (cached.empty()) {
        const std::string rigctl = tool("rigctl");
        if (!rigctl.empty()) {
            if (const std::optional<std::string> listing = run_and_capture({rigctl, "-l"})) cached = parse_models(*listing);
        }
    }
    return cached;
}

std::vector<std::pair<std::string, std::vector<Model>>> models_by_manufacturer() {
    std::map<std::string, std::vector<Model>> groups;
    for (const Model& model : models()) groups[model.manufacturer].push_back(model);
    std::vector<std::pair<std::string, std::vector<Model>>> sorted;
    for (auto& [manufacturer, list] : groups) {
        std::sort(list.begin(), list.end(), [](const Model& a, const Model& b) { return natural_less(a.name, b.name); });
        sorted.emplace_back(manufacturer, std::move(list));
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return natural_less(a.first, b.first); });
    return sorted;
}

// 「rigctl -m 型號 --dump-caps」裡的「Serial speed: 4800..115200 baud, 8N1, ctrl=NONE」
std::optional<SerialCaps> parse_serial_caps(const std::string& dump) {
    for (const std::string& line : lines_of(dump)) {
        if (line.rfind("Serial speed:", 0) != 0) continue;
        std::vector<int> numbers;
        for (std::size_t i = 0; i < line.size();) {
            if (!std::isdigit(static_cast<unsigned char>(line[i]))) {
                ++i;
                continue;
            }
            std::size_t end = i;
            while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end]))) ++end;
            numbers.push_back(std::atoi(line.substr(i, end - i).c_str()));
            i = end;
        }
        if (numbers.size() < 2 || numbers[0] <= 0 || numbers[1] < numbers[0]) return std::nullopt;
        std::string control;
        if (const std::size_t at = line.find("ctrl="); at != std::string::npos) control = line.substr(at + 5);
        std::transform(control.begin(), control.end(), control.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        SerialCaps caps;
        caps.min_baud = numbers[0];
        caps.max_baud = numbers[1];
        caps.hardware_handshake = control.find("RTS") != std::string::npos || control.find("HARD") != std::string::npos;
        return caps;
    }
    return std::nullopt;
}

std::optional<SerialCaps> serial_caps(int model) {
    static std::map<int, std::optional<SerialCaps>> cached;
    {
        std::lock_guard<std::mutex> lock(cache_mutex());
        if (const auto known = cached.find(model); known != cached.end()) return known->second;
    }
    const std::string rigctl = tool("rigctl");
    if (rigctl.empty()) return std::nullopt;
    const std::optional<std::string> dump = run_and_capture({rigctl, "-m", std::to_string(model), "-u"});
    if (!dump) return std::nullopt;
    const std::optional<SerialCaps> caps = parse_serial_caps(*dump);
    std::lock_guard<std::mutex> lock(cache_mutex());
    cached[model] = caps;
    return caps;
}

}  // namespace hamlib

// ---------------------------------------------------------------- 電台的 CAT 指令

double HamlibPTT::start_timeout = 12.0;

namespace {

/// 對本機的 rigctld 送一個指令，回傳它的回覆。連不上、被掛斷這種馬上就知道的失敗，在 retry_until 之前每 0.1 秒再試一次；
/// 連上了卻等到逾時都沒有回覆的不再試（那是電台沒有回應，再試只是多等）。
PTTResult rig_request(const std::string& text, int tcp_port, int timeout_seconds, std::optional<double> retry_until, std::string& reply) {
    for (;;) {
        const PTTResult error = RigctldPTT("127.0.0.1", std::to_string(tcp_port), timeout_seconds).send(text + "\n", reply);
        if (!error) return std::nullopt;
        if (error->timed_out || !retry_until || now_seconds() >= *retry_until) {
            PTTError failure{L("電台沒有回應 CAT 指令，請檢查型號、序列埠與速率")};
            failure.timed_out = error->timed_out;
            return failure;
        }
        sleep_ms(100);
    }
}

PTTResult rig_command(const std::string& text, int tcp_port, int timeout_seconds, std::optional<double> retry_until = std::nullopt) {
    std::string reply;
    if (PTTResult error = rig_request(text, tcp_port, timeout_seconds, retry_until, reply)) return error;
    if (reply.find("RPRT 0") == std::string::npos) {
        // 指令確實送到了，電台卻回報失敗：PTT 有沒有動說不準
        PTTError failure{LF("電台沒有接受 PTT 指令（Hamlib 回覆「%@」）", {trimmed(reply)})};
        failure.may_be_keyed = true;
        return failure;
    }
    return std::nullopt;
}

/// rigctld 印到標準錯誤的訊息：讀的那條執行緒和 HamlibPTT 共用，HamlibPTT 先不在了也沒關係。
struct ErrorBuffer {
    std::mutex mutex;
    std::string text;
};

std::map<const HamlibPTT*, std::shared_ptr<ErrorBuffer>>& error_buffers() {
    static std::map<const HamlibPTT*, std::shared_ptr<ErrorBuffer>> buffers;
    return buffers;
}

std::mutex& error_buffers_mutex() {
    static std::mutex mutex;
    return mutex;
}

/// 模擬「app 突然不在了」時放掉的行程：留著不動，由看門的程序去收。
std::vector<std::unique_ptr<Process>>& abandoned() {
    static std::vector<std::unique_ptr<Process>> list;
    return list;
}

}  // namespace

// 對 rigctld 送「T 1」。
// 電台開得起來的話這個指令很快就有回覆。開不起來的時候 rigctld 不會結束，也不會馬上回報，而是把指令卡住十幾秒：
// 等三秒沒有回覆就當作失敗（key 之後會結束 rigctld，再從它留下的訊息裡找原因）。
// rigctld 剛開始聽連線的頭一瞬間，偶爾會把連線直接掛斷、什麼都不回：這種馬上就失敗的再試，最多試兩秒。
//
// 失敗不代表電台沒有按下：指令可能已經到了（回覆晚到、弄丟，或電台回報失敗卻其實按下了）。接著 rigctld 就要收掉，
// 之後沒有人會再叫電台放開，所以回報失敗之前先補送一次「T 0」。指令有可能到了電台（等到逾時沒有回覆，或電台回報失敗）
// 而補送的那一次又沒成功的話，錯誤上標著 may_be_keyed；根本連不上、一連上就被掛斷的，指令沒有送到，不標。
PTTResult HamlibPTT::press(int tcp_port) {
    PTTResult error = rig_command("T 1", tcp_port, 3, now_seconds() + 2);
    if (!error) return std::nullopt;
    const bool delivered = error->timed_out || error->may_be_keyed;
    const bool released = !rig_command("T 0", tcp_port, 1);
    error->may_be_keyed = delivered && !released;
    return error;
}

// 對 rigctld 送「T 0」。放開一定要送到：連線被掛斷這種馬上就知道的失敗也再試；等到逾時、或電台回報失敗的，
// 最後再補送一次（之後 rigctld 就要收掉了，沒有別的機會）。還是不行的話，錯誤上標著 may_be_keyed。
PTTResult HamlibPTT::release(int tcp_port) {
    PTTResult error = rig_command("T 0", tcp_port, 6, now_seconds() + 2);
    if (!error) return std::nullopt;
    if (!rig_command("T 0", tcp_port, 1)) return std::nullopt;
    error->may_be_keyed = true;
    return error;
}

std::vector<std::string> HamlibPTT::arguments(int model, const std::string& port, int baud, int tcp_port, const std::optional<hamlib::SerialCaps>& caps) {
    std::vector<std::string> arguments = {"-m", std::to_string(model), "-T", "127.0.0.1", "-t", std::to_string(tcp_port)};
    if (!port.empty()) {
        arguments.push_back("-r");
        arguments.push_back(port);
    }
    if (baud > 0) {
        arguments.push_back("-s");
        arguments.push_back(std::to_string(baud));
    }
    if (caps) {
        // 序列埠一開，系統會把 RTS、DTR 都拉高，而很多介面拿其中一條當 PTT 或 CW 鍵：rigctld 開著的這段時間要讓它們保持低電位，
        // 不然改用 CAT 之後電台會一直發射。要用 RTS／CTS 做流量控制的型號，RTS 交給 Hamlib 管（硬要指定的話它會拒絕開啟）。
        arguments.push_back("-C");
        arguments.push_back(caps->hardware_handshake ? "dtr_state=OFF" : "rts_state=OFF,dtr_state=OFF");
    }
    return arguments;
}

// 從 rigctld 印到標準錯誤的訊息裡找「rig_open: error」。它後面跟著一長串 Hamlib 內部的紀錄，
// 裡面講得最清楚的是「Unable to open 裝置 - 原因」，其次是「rig_open returning(-6) IO error」結尾的那個錯誤名稱。
std::optional<std::string> HamlibPTT::open_failure_reason(const std::string& text) {
    if (text.find("rig_open: error") == std::string::npos) return std::nullopt;
    const std::vector<std::string> lines = lines_of(text);
    for (const std::string& raw : lines) {
        const std::string line = trimmed(raw);
        if (const std::size_t at = line.find("Unable to open"); at != std::string::npos) return line.substr(at);
    }
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        const std::string line = trimmed(*it);
        if (line.find("rig_open returning") == std::string::npos) continue;
        if (const std::size_t close = line.rfind(") "); close != std::string::npos) return line.substr(close + 2);
        break;
    }
    return std::string();
}

std::optional<int> HamlibPTT::tcp_port() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return listening_;
}

std::string HamlibPTT::error_text() const {
    std::shared_ptr<ErrorBuffer> buffer;
    {
        std::lock_guard<std::mutex> lock(error_buffers_mutex());
        if (const auto found = error_buffers().find(this); found != error_buffers().end()) buffer = found->second;
    }
    if (!buffer) return error_output_;
    std::lock_guard<std::mutex> lock(buffer->mutex);
    return buffer->text;
}

PTTResult HamlibPTT::key(bool on) {
    if (on) {
        int tcp = 0;
        if (PTTResult error = start(tcp)) return error;
        if (PTTResult error = press(tcp)) {
            stop();
            // 電台根本沒有開起來的話，指令到不了它那裡，不可能按著
            if (const std::optional<std::string> reason = open_failure_reason(error_text())) {
                return PTTError{reason->empty() ? L("Hamlib 開不了電台，請檢查型號、序列埠與速率") : LF("Hamlib 開不了電台，請檢查型號、序列埠與速率（%@）", {*reason})};
            }
            return error;
        }
        return std::nullopt;
    }
    const std::optional<int> tcp = tcp_port();
    if (!tcp) return std::nullopt;   // 沒有開起來（或已經放開過了）
    const PTTResult result = release(*tcp);
    stop();
    return result;
}

PTTResult HamlibPTT::start(int& tcp_port) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (daemon_ && !daemon_->exited() && listening_) {
            tcp_port = *listening_;
            return std::nullopt;
        }
    }
    stop();
    if (model_ <= 0) return PTTError{L("還沒選擇電台型號")};
    const std::string rigctld = hamlib::tool("rigctld");
    if (rigctld.empty()) return PTTError{L("這台電腦沒有裝 Hamlib（找不到 rigctld）")};
    const std::optional<hamlib::SerialCaps> caps = hamlib::serial_caps(model_);
    if (caps && port_.empty()) return PTTError{L("還沒選擇序列埠")};
    const int tcp = net::free_local_port();
    if (tcp == 0) return PTTError{L("找不到可以用的本機連接埠")};

    Process::Options options;
    options.args = arguments(model_, port_, baud_, tcp, caps);
    options.args.insert(options.args.begin(), rigctld);
    options.pipe_stderr = true;
    std::string reason;
    std::unique_ptr<Process> process = Process::spawn(options, &reason);
    if (!process) return PTTError{LF("無法啟動 rigctld：%@", {reason})};

    // 它印到標準錯誤的訊息邊跑邊收（它結束時才把大部分的訊息寫出來）
    auto buffer = std::make_shared<ErrorBuffer>();
    std::thread reader([buffer, stream = process->err()] {
        char chunk[1024];
        for (;;) {
            const size_t n = stream != nullptr ? SDL_ReadIO(stream, chunk, sizeof chunk) : 0;
            if (n > 0) {
                std::lock_guard<std::mutex> lock(buffer->mutex);
                buffer->text.append(chunk, n);
            } else if (stream != nullptr && SDL_GetIOStatus(stream) == SDL_IO_STATUS_NOT_READY) {
                sleep_ms(10);
            } else {
                return;
            }
        }
    });

    // 看門的程序：這個執行檔自己。它的標準輸入接著我們手上的管線
    Process::Options dog_options;
    dog_options.args = {nbtv::tools::executable_path(), "--ptt-watchdog", std::to_string(tcp), std::to_string(process->pid())};
    dog_options.pipe_stdin = true;
    std::unique_ptr<Process> dog = dog_options.args[0].empty() ? nullptr : Process::spawn(dog_options, nullptr);

    Process* running = process.get();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        daemon_ = std::move(process);
        watchdog_ = std::move(dog);
        error_reader_ = std::move(reader);
        listening_ = tcp;
        error_output_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(error_buffers_mutex());
        error_buffers()[this] = buffer;
    }

    // rigctld 先試著把電台開起來（序列埠打不開時它自己會重試兩三秒），然後才開始聽連線：等到連得上為止。
    // 連得上不代表電台開起來了——開不起來它也照樣聽連線，要送了指令才知道（見 key）。
    const double deadline = now_seconds() + start_timeout;
    while (!net::can_connect_local(tcp)) {
        if (running->exited()) {
            stop();
            const std::vector<std::string> lines = lines_of(error_text());
            const std::string last = lines.empty() ? std::string() : trimmed(lines.back());
            return PTTError{last.empty() ? L("rigctld 自己結束了") : LF("rigctld 自己結束了（%@）", {last})};
        }
        if (now_seconds() > deadline) {
            stop();
            const std::string why = open_failure_reason(error_text()).value_or(std::string());
            return PTTError{why.empty() ? L("電台一直沒有回應 CAT 指令，請檢查型號、序列埠與速率") : LF("電台一直沒有回應 CAT 指令，請檢查型號、序列埠與速率（%@）", {why})};
        }
        sleep_ms(30);
    }
    tcp_port = tcp;
    return std::nullopt;
}

void HamlibPTT::stop() {
    std::unique_ptr<Process> process, dog;
    std::thread reader;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        process = std::move(daemon_);
        dog = std::move(watchdog_);
        reader = std::move(error_reader_);
        listening_.reset();
    }
    // 先讓看門的程序結束，再放掉管線：正常收尾時不需要它再去動 rigctld
    if (dog) {
        dog->kill(true);
        dog->close_stdin();
        dog.reset();
    }
    if (process) {
        if (!process->exited()) {
            process->kill(false);
            // 等它把序列埠關好；兩秒內沒有結束就強制結束
            if (!process->wait(2.0)) {
                process->kill(true);
                process->wait(2.0);
            }
        }
    }
    // 它結束時才把訊息寫出來：讀的那條執行緒這時候讀得到結尾，之後才查得到開不了電台的原因
    if (reader.joinable()) reader.join();
    {
        std::lock_guard<std::mutex> lock(error_buffers_mutex());
        if (const auto found = error_buffers().find(this); found != error_buffers().end()) {
            std::lock_guard<std::mutex> inner(found->second->mutex);
            error_output_ = found->second->text;
            error_buffers().erase(found);
        }
    }
    process.reset();
}

void HamlibPTT::debug_abandon() {
    std::unique_ptr<Process> process, dog;
    std::thread reader;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        process = std::move(daemon_);
        dog = std::move(watchdog_);
        reader = std::move(error_reader_);
        listening_.reset();
    }
    if (reader.joinable()) reader.detach();
    if (dog) dog->close_stdin();
    if (dog) abandoned().push_back(std::move(dog));
    if (process) abandoned().push_back(std::move(process));
}

int ptt_watchdog_main(const std::string& tcp_port, const std::string& pid) {
    // 一直讀到管線斷掉：握著另一頭的行程（app）不在了
#if defined(_WIN32)
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    char buffer[64];
    DWORD got = 0;
    while (input != nullptr && input != INVALID_HANDLE_VALUE && ReadFile(input, buffer, sizeof buffer, &got, nullptr) && got > 0) {}
#else
    char buffer[64];
    while (std::fread(buffer, 1, sizeof buffer, stdin) > 0) {}
#endif
    // 「q」讓 rigctld 馬上掛斷這條連線；連不上的話立刻就回來
    net::request("127.0.0.1", tcp_port, "T 0\nq\n", 2);
    kill_pid(std::atol(pid.c_str()));
    return 0;
}

}  // namespace nbtvgui
