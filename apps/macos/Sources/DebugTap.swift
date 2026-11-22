// 除錯用的紀錄：把 app 送出去的音訊、聽到的音訊，以及收發流程的每一步寫到檔案裡，
// 用來查「發射端到底送了什麼、接收端到底聽到什麼」。平常不開；把使用者預設值 debugTapDirectory
// 設成一個目錄才啟用（下次啟動 app 生效），用完記得刪掉這個設定：
//
//   defaults write tw.hf-nbtv.mac debugTapDirectory -string /某個/目錄
//   defaults delete tw.hf-nbtv.mac debugTapDirectory
//
// 目錄裡會有：
//   app1-log.txt（第二個 app 是 app2-log.txt）   每一步的時間與狀態
//   app1-tx-時分秒.f32                            每次發射排進輸出佇列的取樣（乘上輸出音量之前）
//   app1-rx-時分秒.f32                            每次開始接收後交給接收機的取樣
// 音訊檔沒有檔頭：32 位元浮點、48 kHz、單聲道。每個檔案最多寫 maxSeconds 秒，超過就不再寫。
import Foundation

enum DebugTap {
    static let directory: URL? = {
        guard let path = UserDefaults.standard.string(forKey: "debugTapDirectory"), !path.isEmpty else { return nil }
        let url = URL(fileURLWithPath: path, isDirectory: true)
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }()
    static var isEnabled: Bool { directory != nil }

    static let maxSeconds = 900.0
    private static let queue = DispatchQueue(label: "nbtv.debug.tap", qos: .utility)
    private static let tag = "app" + (AppIdentity.instance ?? "1")
    private static var logHandle: FileHandle?   // 只在 queue 上用

    private static func stamp(_ format: String) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = format
        return formatter.string(from: Date())
    }

    private static func open(_ name: String, append: Bool) -> FileHandle? {
        guard let url = directory?.appendingPathComponent(name) else { return nil }
        if !append || !FileManager.default.fileExists(atPath: url.path) { FileManager.default.createFile(atPath: url.path, contents: nil) }
        guard let handle = try? FileHandle(forWritingTo: url) else { return nil }
        if append { handle.seekToEndOfFile() }
        return handle
    }

    /// 在紀錄檔加一行（帶時間）。沒有啟用時什麼都不做。
    static func log(_ text: @autoclosure () -> String) {
        guard isEnabled else { return }
        let line = "\(stamp("HH:mm:ss.SSS")) \(text())\n"
        queue.async {
            if logHandle == nil { logHandle = open("\(tag)-log.txt", append: true) }
            logHandle?.write(Data(line.utf8))
        }
    }

    /// 一個音訊檔。write 可以在任何佇列上呼叫，實際寫檔在背景佇列上。
    final class Stream {
        let name: String
        private var handle: FileHandle?   // 只在 queue 上用
        private var written = 0
        private var full = false

        init?(kind: String) {
            guard DebugTap.isEnabled else { return nil }
            name = "\(DebugTap.tag)-\(kind)-\(DebugTap.stamp("HHmmss")).f32"
            let fileName = name
            DebugTap.queue.async { self.handle = DebugTap.open(fileName, append: false) }
            DebugTap.log("開始記錄音訊：\(fileName)")
        }

        func write(_ samples: [Float]) {
            guard !samples.isEmpty else { return }
            DebugTap.queue.async { [self] in
                guard let handle, !full else { return }
                if Double(written + samples.count) > DebugTap.maxSeconds * Double(NBTV.sampleRate) {
                    full = true
                    DebugTap.log("\(name) 已達 \(Int(DebugTap.maxSeconds)) 秒的上限，之後不再寫")
                    return
                }
                samples.withUnsafeBufferPointer { handle.write(Data(buffer: $0)) }
                written += samples.count
            }
        }

        func close() {
            DebugTap.queue.async { [self] in
                try? handle?.close()
                handle = nil
            }
        }
    }
}
