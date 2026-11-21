// PTT 控制：不控制（VOX）、序列埠的 RTS／DTR、Hamlib 的 rigctld（TCP）。
// key() 可能會卡住一下（開序列埠、連線），要在背景佇列上呼叫。
import CNBTV
import Foundation

enum PTTMode: String, CaseIterable {
    case none      // 不控制，使用電台的 VOX
    case serial    // 序列埠的 RTS 或 DTR
    case rigctld   // Hamlib rigctld（TCP）
}

enum SerialLine: String {
    case rts, dtr
    var title: String { self == .rts ? "RTS" : "DTR" }
}

struct PTTError: LocalizedError {
    let message: String
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
        guard !path.isEmpty else { throw PTTError(message: "還沒選擇序列埠") }
        if fd < 0 {
            fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK)
            if fd < 0 { throw PTTError(message: "無法開啟 \(path)：\(String(cString: strerror(errno)))") }
        }
        // 剛開啟時驅動程式通常會把兩條線都拉高，先把不用的那條放掉
        let wanted: Int32 = line == .rts ? 0 : 1
        _ = nbtv_shim_set_modem_line(fd, 1 - wanted, 0)
        if nbtv_shim_set_modem_line(fd, wanted, 1) != 0 {
            let reason = String(cString: strerror(errno))
            release()
            throw PTTError(message: "無法控制 \(path) 的 \(line.title)：\(reason)")
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
    private let timeoutSeconds = 2

    init(host: String, port: String) {
        self.host = host
        self.port = port
    }

    func key(_ on: Bool) throws {
        let reply = try send(command: "T \(on ? 1 : 0)\n")
        guard reply.contains("RPRT 0") else {
            throw PTTError(message: "rigctld 回覆「\(reply.trimmingCharacters(in: .whitespacesAndNewlines))」")
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
            throw PTTError(message: "找不到 rigctld 的位址 \(where_)")
        }
        defer { freeaddrinfo(result) }

        var lastError = "無法連線"
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
                    lastError = "連線逾時"
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
                throw PTTError(message: "無法送指令給 rigctld（\(where_)）")
            }
            var reply = ""
            var buffer = [UInt8](repeating: 0, count: 256)
            while !reply.contains("\n") {
                let n = recv(fd, &buffer, buffer.count, 0)
                if n <= 0 { break }
                reply += String(decoding: buffer[0..<n], as: UTF8.self)
            }
            if reply.isEmpty { throw PTTError(message: "rigctld（\(where_)）沒有回覆") }
            return reply
        }
        throw PTTError(message: "無法連線到 rigctld（\(where_)）：\(lastError)")
    }
}
