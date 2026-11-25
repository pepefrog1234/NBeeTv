// PTT 控制：不控制（VOX）、序列埠的 RTS／DTR、電台的 CAT 指令（經序列埠，由 Hamlib 翻譯）、Hamlib 的 rigctld（TCP）。
// key() 可能會卡住一下（開序列埠、連線、等電台回應），要在背景佇列上呼叫。
import CNBTV
import Foundation

enum PTTMode: String, CaseIterable {
    case none      // 不控制，使用電台的 VOX
    case serial    // 序列埠的 RTS 或 DTR
    case cat       // 電台的 CAT 指令，經序列埠（各廠牌的指令由這台電腦上的 Hamlib 翻譯）
    case rigctld   // Hamlib rigctld（TCP）：使用者自己開著的 rigctld
}

enum SerialLine: String {
    case rts, dtr
    var title: String { self == .rts ? "RTS" : "DTR" }
}

struct PTTError: LocalizedError {
    let message: String
    /// 連上了、指令也送出去了，但等到逾時都沒有回覆（和「連不上」「對方馬上掛斷」不一樣：那兩種很快就知道）
    var timedOut = false
    var errorDescription: String? { message }
}

protocol PTTDriver: AnyObject {
    /// 按下（true）或放開（false）PTT。
    func key(_ on: Bool) throws
}

final class NoPTT: PTTDriver {
    func key(_ on: Bool) throws {}
}

/// 序列埠：按下時把指定的控制線拉高，另一條拉低；放開時兩條都拉低並關閉序列埠。
final class SerialPTT: PTTDriver {
    private let path: String
    private let line: SerialLine
    private var fd: Int32 = -1

    init(path: String, line: SerialLine) {
        self.path = path
        self.line = line
    }

    deinit { release() }

    func key(_ on: Bool) throws {
        if !on {
            release()
            return
        }
        guard !path.isEmpty else { throw PTTError(message: L("還沒選擇序列埠")) }
        if fd < 0 {
            fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK)
            if fd < 0 { throw PTTError(message: L("無法開啟 %@：%@", path, String(cString: strerror(errno)))) }
        }
        // 剛開啟時驅動程式通常會把兩條線都拉高，先把不用的那條放掉
        let wanted: Int32 = line == .rts ? 0 : 1
        _ = nbtv_shim_set_modem_line(fd, 1 - wanted, 0)
        if nbtv_shim_set_modem_line(fd, wanted, 1) != 0 {
            let reason = String(cString: strerror(errno))
            release()
            throw PTTError(message: L("無法控制 %@ 的 %@：%@", path, line.title, reason))
        }
    }

    private func release() {
        guard fd >= 0 else { return }
        _ = nbtv_shim_set_modem_line(fd, 0, 0)
        _ = nbtv_shim_set_modem_line(fd, 1, 0)
        close(fd)
        fd = -1
    }

    /// 系統上的序列埠（/dev/cu.*）。
    static func availablePorts() -> [String] {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: "/dev")) ?? []
        return names.filter { $0.hasPrefix("cu.") }.sorted().map { "/dev/" + $0 }
    }
}

/// Hamlib rigctld：每次連線送一個指令，「T 1」按下、「T 0」放開，回覆「RPRT 0」表示成功。
final class RigctldPTT: PTTDriver {
    private let host: String
    private let port: String
    private let timeoutSeconds: Int

    /// timeoutSeconds：連線、等回覆各最多等幾秒
    init(host: String, port: String, timeoutSeconds: Int = 2) {
        self.host = host
        self.port = port
        self.timeoutSeconds = timeoutSeconds
    }

    func key(_ on: Bool) throws {
        let reply = try send(command: "T \(on ? 1 : 0)\n")
        guard reply.contains("RPRT 0") else {
            throw PTTError(message: L("rigctld 回覆「%@」", reply.trimmingCharacters(in: .whitespacesAndNewlines)))
        }
    }

    /// 送出一行指令並讀回覆（測試時也用來查詢狀態）。
    func send(command: String) throws -> String {
        let where_ = "\(host):\(port)"
        var hints = addrinfo()
        hints.ai_family = AF_UNSPEC
        hints.ai_socktype = SOCK_STREAM
        var result: UnsafeMutablePointer<addrinfo>?
        guard getaddrinfo(host, port, &hints, &result) == 0, let first = result else {
            throw PTTError(message: L("找不到 rigctld 的位址 %@", where_))
        }
        defer { freeaddrinfo(result) }

        var lastError = L("無法連線")
        var cursor: UnsafeMutablePointer<addrinfo>? = first
        while let info = cursor {
            cursor = info.pointee.ai_next
            let fd = socket(info.pointee.ai_family, info.pointee.ai_socktype, info.pointee.ai_protocol)
            if fd < 0 { continue }
            defer { close(fd) }
            // 對方若在我們送指令前就斷線，send 要回報錯誤，而不是讓整個 app 收到 SIGPIPE 結束
            var one: Int32 = 1
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
            // 非阻塞連線，最多等 timeoutSeconds
            let flags = fcntl(fd, F_GETFL, 0)
            _ = fcntl(fd, F_SETFL, flags | O_NONBLOCK)
            var ok = connect(fd, info.pointee.ai_addr, info.pointee.ai_addrlen) == 0
            if !ok && errno == EINPROGRESS {
                var pfd = pollfd(fd: fd, events: Int16(POLLOUT), revents: 0)
                if poll(&pfd, 1, Int32(timeoutSeconds * 1000)) > 0 {
                    var soError: Int32 = 0
                    var length = socklen_t(MemoryLayout<Int32>.size)
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &length)
                    ok = soError == 0
                    if !ok { lastError = String(cString: strerror(soError)) }
                } else {
                    lastError = L("連線逾時")
                }
            } else if !ok {
                lastError = String(cString: strerror(errno))
            }
            guard ok else { continue }
            _ = fcntl(fd, F_SETFL, flags)
            var tv = timeval(tv_sec: timeoutSeconds, tv_usec: 0)
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

            let bytes = Array(command.utf8)
            guard Darwin.send(fd, bytes, bytes.count, 0) == bytes.count else {
                throw PTTError(message: L("無法送指令給 rigctld（%@）", where_))
            }
            var reply = ""
            var buffer = [UInt8](repeating: 0, count: 256)
            var timedOut = false
            while !reply.contains("\n") {
                let n = recv(fd, &buffer, buffer.count, 0)
                if n <= 0 {
                    timedOut = n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)
                    break
                }
                reply += String(decoding: buffer[0..<n], as: UTF8.self)
            }
            if reply.isEmpty { throw PTTError(message: L("rigctld（%@）沒有回覆", where_), timedOut: timedOut) }
            return reply
        }
        throw PTTError(message: L("無法連線到 rigctld（%@）：%@", where_, lastError))
    }
}

/// 這台電腦上裝的 Hamlib：找它的程式（rigctl、rigctld）、列出它支援的電台型號。
/// app 自己不帶 Hamlib；沒有裝的話，「電台的 CAT 指令」那一項不能用（其他三項不受影響）。
enum Hamlib {
    struct Model: Hashable {
        let id: Int               // Hamlib 的型號編號（rigctld -m 的參數）
        let manufacturer: String
        let name: String
        /// 「Icom IC-7300」
        var title: String { manufacturer.isEmpty ? name : "\(manufacturer) \(name)" }
    }

    /// 一個型號的序列埠規格（rigctl --dump-caps 的「Serial speed」那一行）。不走序列埠的型號沒有。
    struct SerialCaps: Equatable {
        let minBaud: Int
        let maxBaud: Int              // 沒有指定速率時 Hamlib 用最高的這個
        let hardwareHandshake: Bool   // 要用 RTS／CTS 做流量控制
    }

    /// 設定裡可以選的序列速率
    static let baudRates = [1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200]

    /// Homebrew（Apple 晶片、Intel）、MacPorts、系統的目錄，依序找；從終端機啟動的話 PATH 裡的也算
    private static let directories = ["/opt/homebrew/bin", "/usr/local/bin", "/opt/local/bin", "/usr/bin"]

    static func tool(_ name: String) -> String? {
        var places = directories
        if let path = ProcessInfo.processInfo.environment["PATH"] { places += path.split(separator: ":").map(String.init) }
        return places.map { $0 + "/" + name }.first { FileManager.default.isExecutableFile(atPath: $0) }
    }

    static var rigctld: String? { tool("rigctld") }
    static var rigctl: String? { tool("rigctl") }
    static var isInstalled: Bool { rigctld != nil && rigctl != nil }

    /// 執行一個程式、等它結束，回傳它印到標準輸出的文字。超過時間還沒結束就中止它，回傳 nil。
    static func run(_ path: String, _ arguments: [String], timeout: Double = 5) -> String? {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: path)
        process.arguments = arguments
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = FileHandle.nullDevice
        process.standardInput = FileHandle.nullDevice
        do { try process.run() } catch { return nil }
        // 輸出要邊跑邊讀：等它結束才讀的話，輸出超過管線的緩衝它就卡住了
        final class Box { var data = Data() }
        let box = Box()
        let done = DispatchSemaphore(value: 0)
        DispatchQueue.global(qos: .userInitiated).async {
            box.data = pipe.fileHandleForReading.readDataToEndOfFile()
            done.signal()
        }
        guard done.wait(timeout: .now() + timeout) == .success else {
            process.terminate()
            return nil
        }
        process.waitUntilExit()
        return String(decoding: box.data, as: UTF8.self)
    }

    /// 把「rigctl -l」的清單拆成一個個型號。每一列是「編號、廠牌、型號、版本、狀態、巨集名稱」，欄位用空白對齊；
    /// 廠牌與型號裡面可能有空白（「NET rigctl」「Dummy No VFO」），所以照標題列的欄位位置切，不能用空白分。
    static func parseModels(_ listing: String) -> [Model] {
        let lines = listing.split(whereSeparator: \.isNewline).map(String.init)
        guard let header = lines.first(where: { $0.contains("Mfg") && $0.contains("Model") }),
              let mfg = header.range(of: "Mfg"), let model = header.range(of: "Model") else { return [] }
        let mfgColumn = header.distance(from: header.startIndex, to: mfg.lowerBound)
        let modelColumn = header.distance(from: header.startIndex, to: model.lowerBound)
        var models: [Model] = []
        for line in lines where line != header {
            let characters = Array(line)
            // 最後三欄（版本、狀態、巨集名稱）裡面沒有空白：從後面數回來，前面剩下的就是廠牌與型號
            let tokens = line.split(separator: " ")
            guard tokens.count >= 5, let id = Int(tokens[0]), id > 0, characters.count > modelColumn,
                  let version = line.range(of: " " + tokens[tokens.count - 3] + " ", options: .backwards) else { continue }
            let versionColumn = line.distance(from: line.startIndex, to: version.lowerBound)
            guard versionColumn > modelColumn else { continue }
            let manufacturer = String(characters[mfgColumn..<modelColumn]).trimmingCharacters(in: .whitespaces)
            let name = String(characters[modelColumn..<versionColumn]).trimmingCharacters(in: .whitespaces)
            if !name.isEmpty { models.append(Model(id: id, manufacturer: manufacturer, name: name)) }
        }
        return models
    }

    private static let lock = NSLock()
    private static var cachedModels: [Model] = []
    private static var cachedCaps: [Int: SerialCaps?] = [:]

    /// Hamlib 支援的電台型號。第一次問的時候執行一次「rigctl -l」（約幾十毫秒）；沒有裝 Hamlib 時是空的，下次問會再找一次。
    static func models() -> [Model] {
        lock.lock()
        defer { lock.unlock() }
        if cachedModels.isEmpty, let rigctl, let listing = run(rigctl, ["-l"]) { cachedModels = parseModels(listing) }
        return cachedModels
    }

    /// 選單用：依廠牌分組，廠牌與型號都照名稱排（數字照大小，IC-705 排在 IC-7300 前面）。
    static func modelsByManufacturer() -> [(manufacturer: String, models: [Model])] {
        Dictionary(grouping: models(), by: \.manufacturer)
            .map { (manufacturer: $0.key, models: $0.value.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }) }
            .sorted { $0.manufacturer.localizedStandardCompare($1.manufacturer) == .orderedAscending }
    }

    /// 「rigctl -m 型號 --dump-caps」裡的「Serial speed: 4800..115200 baud, 8N1, ctrl=NONE」
    static func parseSerialCaps(_ dump: String) -> SerialCaps? {
        guard let line = dump.split(whereSeparator: \.isNewline).first(where: { $0.hasPrefix("Serial speed:") }) else { return nil }
        let numbers = line.split(whereSeparator: { !$0.isNumber }).compactMap { Int($0) }
        guard numbers.count >= 2, numbers[0] > 0, numbers[1] >= numbers[0] else { return nil }
        let control = line.range(of: "ctrl=").map { line[$0.upperBound...].uppercased() } ?? ""
        return SerialCaps(minBaud: numbers[0], maxBaud: numbers[1], hardwareHandshake: control.contains("RTS") || control.contains("HARD"))
    }

    /// 那個型號的序列埠規格；不走序列埠的型號（示範用的 Dummy、走網路的）或沒有裝 Hamlib 時是 nil。
    /// 只印出 Hamlib 自己的資料，不會去開任何序列埠。
    static func serialCaps(model: Int) -> SerialCaps? {
        lock.lock()
        if let known = cachedCaps[model] {
            lock.unlock()
            return known
        }
        lock.unlock()
        guard let rigctl, let dump = run(rigctl, ["-m", String(model), "-u"]) else { return nil }
        let caps = parseSerialCaps(dump)
        lock.lock()
        cachedCaps[model] = caps
        lock.unlock()
        return caps
    }
}

/// 電台的 CAT 指令，經序列埠。各廠牌的指令都不一樣，交給 Hamlib 翻譯：按下 PTT 時在背景啟動一個只給自己用的 rigctld
/// （只聽本機的一個臨時連接埠），對它送「T 1」；放開時送「T 0」再把它結束，序列埠也跟著還回去。
///
/// 用指令按下的 PTT 不會因為序列埠關了就放開，所以另外有一個看門的小程序：它的標準輸入接著這個 app 手上的管線，
/// app 不管怎麼結束（當掉、被強制結束），管線一斷它就替我們送「T 0」、把 rigctld 結束，電台才不會一直發射下去。
final class HamlibPTT: PTTDriver {
    private let model: Int
    private let port: String
    private let baud: Int
    private let lock = NSLock()
    private var daemon: Process?
    private var watchdog: (process: Process, pipe: FileHandle)?
    private var errorOutput = Data()   // rigctld 印到標準錯誤的訊息
    private var listening: Int?

    /// rigctld 聽的本機連接埠；沒有在執行時是 nil（測試時用它查詢 PTT 的狀態）
    var tcpPort: Int? {
        lock.lock()
        defer { lock.unlock() }
        return listening
    }

    /// 等 rigctld 把電台開起來，最多等這麼久。電台沒有回應時 Hamlib 自己要重試好幾秒才放棄。
    static var startTimeout = 12.0
    /// 指令被剛啟動的 rigctld 掛斷、重試了幾次（測試用來確認重試這條路有走到）
    private(set) static var retries = 0

    init(model: Int, port: String, baud: Int) {
        self.model = model
        self.port = port
        self.baud = baud
    }

    deinit { stop() }

    func key(_ on: Bool) throws {
        if on {
            let tcp = try start()
            do {
                // 電台開得起來的話這個指令很快就有回覆。開不起來的時候 rigctld 不會結束，也不會馬上回報，而是把指令卡住十幾秒：
                // 等三秒沒有回覆就當作開不起來，結束 rigctld，再從它留下的訊息裡找原因。
                // rigctld 剛開始聽連線的頭一瞬間，偶爾（實測約一成）會把連線直接掛斷、什麼都不回：這種馬上就失敗的再試，最多試兩秒。
                try command("T 1", to: tcp, timeoutSeconds: 3, retryUntil: Date().addingTimeInterval(2))
            } catch {
                stop()
                if let reason = HamlibPTT.openFailureReason(in: errorText()) {
                    throw PTTError(message: reason.isEmpty ? L("Hamlib 開不了電台，請檢查型號、序列埠與速率") : L("Hamlib 開不了電台，請檢查型號、序列埠與速率（%@）", reason))
                }
                throw error
            }
        } else {
            guard let tcp = tcpPort else { return }   // 沒有開起來（或已經放開過了）
            defer { stop() }
            // 放開一定要送到：連線被掛斷這種馬上就知道的失敗也再試
            try command("T 0", to: tcp, timeoutSeconds: 6, retryUntil: Date().addingTimeInterval(2))
        }
    }

    /// rigctld 的參數。caps 是那個型號的序列埠規格（不走序列埠的型號是 nil）。
    static func arguments(model: Int, port: String, baud: Int, tcpPort: Int, caps: Hamlib.SerialCaps?) -> [String] {
        var arguments = ["-m", String(model), "-T", "127.0.0.1", "-t", String(tcpPort)]
        if !port.isEmpty { arguments += ["-r", port] }
        if baud > 0 { arguments += ["-s", String(baud)] }
        if let caps {
            // 序列埠一開，系統會把 RTS、DTR 都拉高，而很多介面拿其中一條當 PTT 或 CW 鍵：rigctld 開著的這段時間要讓它們保持低電位，
            // 不然改用 CAT 之後電台會一直發射。要用 RTS／CTS 做流量控制的型號，RTS 交給 Hamlib 管（硬要指定的話它會拒絕開啟）。
            arguments += ["-C", caps.hardwareHandshake ? "dtr_state=OFF" : "rts_state=OFF,dtr_state=OFF"]
        }
        return arguments
    }

    /// 看門的程序：標準輸入一斷（握著另一頭的行程不在了），就對 rigctld 送「T 0」，再把它結束。
    /// 回傳的 pipe 是另一頭；握著它，看門的就一直等著。
    static func makeWatchdog(tcpPort: Int, pid: Int32) -> (process: Process, pipe: FileHandle)? {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/sh")
        // cat 一直讀到管線斷掉；之後 nc 送出指令（「q」讓 rigctld 馬上掛斷這條連線；連不上的話 nc 立刻結束）
        let script = "cat >/dev/null; printf 'T 0\\nq\\n' | /usr/bin/nc -w 2 127.0.0.1 \"$1\" >/dev/null 2>&1; kill \"$2\" 2>/dev/null"
        process.arguments = ["-c", script, "nbtv-ptt-watchdog", String(tcpPort), String(pid)]
        let pipe = Pipe()
        process.standardInput = pipe
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        do { try process.run() } catch { return nil }
        try? pipe.fileHandleForReading.close()   // 讀的那一頭只給看門的程序用
        return (process, pipe.fileHandleForWriting)
    }

    /// 測試用：模擬 app 突然不在了——不送「T 0」、不結束 rigctld，只把看門程序的管線放掉。
    func debugAbandon() {
        lock.lock()
        let dog = watchdog
        watchdog = nil
        daemon = nil
        listening = nil
        lock.unlock()
        try? dog?.pipe.close()
    }

    /// 啟動 rigctld（已經在執行就沿用），等它把電台開起來、開始聽連線。回傳它聽的連接埠。
    private func start() throws -> Int {
        lock.lock()
        let running = daemon?.isRunning == true ? listening : nil
        lock.unlock()
        if let running { return running }
        stop()
        guard model > 0 else { throw PTTError(message: L("還沒選擇電台型號")) }
        guard let rigctld = Hamlib.rigctld else {
            throw PTTError(message: L("這台電腦沒有裝 Hamlib（找不到 rigctld）；可以用 Homebrew 安裝：brew install hamlib"))
        }
        let caps = Hamlib.serialCaps(model: model)
        if caps != nil, port.isEmpty { throw PTTError(message: L("還沒選擇序列埠")) }
        guard let tcp = HamlibPTT.freePort() else { throw PTTError(message: L("找不到可以用的本機連接埠")) }

        let process = Process()
        process.executableURL = URL(fileURLWithPath: rigctld)
        process.arguments = HamlibPTT.arguments(model: model, port: port, baud: baud, tcpPort: tcp, caps: caps)
        let errors = Pipe()
        process.standardError = errors
        process.standardOutput = FileHandle.nullDevice
        process.standardInput = FileHandle.nullDevice
        errors.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard let self, !data.isEmpty else { return }
            self.lock.lock()
            self.errorOutput.append(data)
            self.lock.unlock()
        }
        do {
            try process.run()
        } catch {
            errors.fileHandleForReading.readabilityHandler = nil
            throw PTTError(message: L("無法啟動 rigctld：%@", error.localizedDescription))
        }
        let dog = HamlibPTT.makeWatchdog(tcpPort: tcp, pid: process.processIdentifier)
        lock.lock()
        daemon = process
        watchdog = dog
        listening = tcp
        errorOutput = Data()
        lock.unlock()
        DebugTap.log("啟動 rigctld（行程 \(process.processIdentifier)）：" + (process.arguments ?? []).joined(separator: " "))

        // rigctld 先試著把電台開起來（序列埠打不開時它自己會重試兩三秒），然後才開始聽連線：等到連得上為止。
        // 連得上不代表電台開起來了——開不起來它也照樣聽連線，要送了指令才知道（見 key）。
        let deadline = Date().addingTimeInterval(HamlibPTT.startTimeout)
        while !HamlibPTT.canConnect(tcp) {
            if !process.isRunning {
                stop()
                let reason = lastErrorLine()
                throw PTTError(message: reason.isEmpty ? L("rigctld 自己結束了") : L("rigctld 自己結束了（%@）", reason))
            }
            if Date() > deadline {
                stop()
                let reason = HamlibPTT.openFailureReason(in: errorText()) ?? ""
                throw PTTError(message: reason.isEmpty ? L("電台一直沒有回應 CAT 指令，請檢查型號、序列埠與速率") : L("電台一直沒有回應 CAT 指令，請檢查型號、序列埠與速率（%@）", reason))
            }
            usleep(30_000)
        }
        return tcp
    }

    /// rigctld 到目前為止印到標準錯誤的訊息。它的訊息是整批寫出來的，通常要等它結束（stop 之後）才看得到。
    private func errorText() -> String {
        lock.lock()
        defer { lock.unlock() }
        return String(decoding: errorOutput, as: UTF8.self)
    }

    /// 從 rigctld 印到標準錯誤的訊息裡找「rig_open: error」。它後面跟著一長串 Hamlib 內部的紀錄，
    /// 裡面講得最清楚的是「Unable to open 裝置 - 原因」，其次是「rig_open returning(-6) IO error」結尾的那個錯誤名稱。
    static func openFailureReason(in text: String) -> String? {
        guard text.contains("rig_open: error") else { return nil }
        let lines = text.split(whereSeparator: \.isNewline).map { $0.trimmingCharacters(in: .whitespaces) }
        if let line = lines.first(where: { $0.contains("Unable to open") }), let range = line.range(of: "Unable to open") {
            return String(line[range.lowerBound...])
        }
        if let line = lines.last(where: { $0.contains("rig_open returning") }), let close = line.range(of: ") ", options: .backwards) {
            return String(line[close.upperBound...])
        }
        return ""
    }

    /// 對本機的 rigctld 送一個指令，回傳它的回覆。連不上、被掛斷這種馬上就知道的失敗，在 retryUntil 之前每 0.1 秒再試一次；
    /// 連上了卻等到逾時都沒有回覆的不再試（那是電台沒有回應，再試只是多等）。
    static func request(_ text: String, port: Int, timeoutSeconds: Int, retryUntil: Date?) throws -> String {
        while true {
            do {
                return try RigctldPTT(host: "127.0.0.1", port: String(port), timeoutSeconds: timeoutSeconds).send(command: text + "\n")
            } catch {
                let timedOut = (error as? PTTError)?.timedOut == true
                guard !timedOut, let retryUntil, Date() < retryUntil else {
                    throw PTTError(message: L("電台沒有回應 CAT 指令，請檢查型號、序列埠與速率"), timedOut: timedOut)
                }
                DebugTap.log("rigctld 沒有接這個指令（\(error.localizedDescription)），再試一次")
                retries += 1
                usleep(100_000)
            }
        }
    }

    private func command(_ text: String, to tcp: Int, timeoutSeconds: Int, retryUntil: Date? = nil) throws {
        let reply = try HamlibPTT.request(text, port: tcp, timeoutSeconds: timeoutSeconds, retryUntil: retryUntil)
        guard reply.contains("RPRT 0") else {
            throw PTTError(message: L("電台沒有接受 PTT 指令（Hamlib 回覆「%@」）", reply.trimmingCharacters(in: .whitespacesAndNewlines)))
        }
    }

    /// rigctld 最後印出來的那一行訊息
    private func lastErrorLine() -> String {
        errorText().split(whereSeparator: \.isNewline).last.map { String($0).trimmingCharacters(in: .whitespaces) } ?? ""
    }

    /// 結束 rigctld 與看門的程序。
    private func stop() {
        lock.lock()
        let process = daemon, dog = watchdog
        daemon = nil
        watchdog = nil
        listening = nil
        lock.unlock()
        // 先讓看門的程序結束，再放掉管線：正常收尾時不需要它再去動 rigctld
        if let dog {
            dog.process.terminate()
            try? dog.pipe.close()
        }
        guard let process else { return }
        let errors = (process.standardError as? Pipe)?.fileHandleForReading
        errors?.readabilityHandler = nil
        if process.isRunning {
            process.terminate()
            // 等它把序列埠關好；兩秒內沒有結束就強制結束
            let deadline = Date().addingTimeInterval(2)
            while process.isRunning, Date() < deadline { usleep(10_000) }
            if process.isRunning {
                kill(process.processIdentifier, SIGKILL)
                process.waitUntilExit()
            }
        }
        // 它結束時才把訊息寫出來：這時候讀到底（已經結束了，不會卡住），之後才查得到開不了電台的原因
        if let rest = errors?.readDataToEndOfFile(), !rest.isEmpty {
            lock.lock()
            errorOutput.append(rest)
            lock.unlock()
        }
    }

    /// 向系統要一個目前沒有人用的本機連接埠。
    static func freePort() -> Int? {
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { return nil }
        defer { close(fd) }
        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET)
        address.sin_addr.s_addr = inet_addr("127.0.0.1")
        address.sin_port = 0
        var length = socklen_t(MemoryLayout<sockaddr_in>.size)
        let bound = withUnsafeMutablePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(fd, $0, length) == 0 && getsockname(fd, $0, &length) == 0 }
        }
        return bound ? Int(UInt16(bigEndian: address.sin_port)) : nil
    }

    /// 本機的這個連接埠連得上嗎？
    static func canConnect(_ port: Int) -> Bool {
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { return false }
        defer { close(fd) }
        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET)
        address.sin_addr.s_addr = inet_addr("127.0.0.1")
        address.sin_port = UInt16(port).bigEndian
        return withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) == 0 }
        }
    }
}
