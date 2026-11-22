// 核心 C API（nbtv.h）的 Swift 包裝。這裡的物件都不是執行緒安全的：
// 同一個物件只能在同一條佇列上使用，由呼叫端（RadioEngine、AppModel）負責。
import CNBTV
import CoreGraphics
import Foundation

enum NBTV {
    static let imageWidth = 96
    static let imageHeight = 72
    static let rgbBytes = imageWidth * imageHeight * 3   // 20736
    static let maxImageBytes = 1473
    static let spectrumBins = 257                        // 0–3000 Hz
    static let spectrumMaxHz = 3000.0
    static let avifOverhead = 277
    static let sampleRate = 48000                        // 外殼與核心之間一律用 48 kHz
    static let totalCoefficients = 20736                 // 類比模式三個平面的係數總數
    static let bandLowHz = 352.0                         // 訊號頻帶
    static let bandHighHz = 2648.0

    static var version: String { String(cString: nbtv_version_string()) }
    static var hasImageCodec: Bool { nbtv_has_image_codec() != 0 }

    /// 把輸入整理成可以傳送的呼號（轉大寫、去掉不能用的字元、最多 9 個字元）。
    static func normalizeCallsign(_ text: String) -> String {
        var buffer = [CChar](repeating: 0, count: 16)
        _ = nbtv_callsign_normalize(text, &buffer)
        return String(cString: buffer)
    }

    /// 影像酬載 → 標準的 .avif 檔。
    static func avifFile(fromPayload payload: Data) -> Data? {
        var out = [UInt8](repeating: 0, count: payload.count + avifOverhead)
        let n = payload.withUnsafeBytes { src in
            nbtv_avif_from_payload(src.bindMemory(to: UInt8.self).baseAddress, Int32(payload.count), &out, Int32(out.count))
        }
        return n > 0 ? Data(out[0..<Int(n)]) : nil
    }

    /// 96×72×3 的 RGB → CGImage。
    static func image(fromRGB rgb: Data) -> CGImage? {
        guard rgb.count == rgbBytes, let provider = CGDataProvider(data: rgb as CFData),
              let space = CGColorSpace(name: CGColorSpace.sRGB) else { return nil }
        return CGImage(width: imageWidth, height: imageHeight, bitsPerComponent: 8, bitsPerPixel: 24, bytesPerRow: imageWidth * 3,
                       space: space, bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue), provider: provider,
                       decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    }
}

/// 一個傳輸模式，資料取自 nbtv_mode_at()。
struct ModeInfo: Identifiable, Hashable {
    let id: Int
    let isDigital: Bool
    let frameSeconds: Double
    let frameSymbols: Int
    let payloadSymbols: Int
    let bitsPerCell: Int
    let rateNum: Int
    let rateDen: Int
    let imageBytes: Int
    let analogCoefficients: Int
    var analogQuality = ""  // 粗略／中等／細緻，依酬載長短排出來

    /// 「數位 1」「類比 9」
    var name: String { (isDigital ? "數位 " : "類比 ") + String(id) }

    /// 「QPSK 1/2」或「中等」
    var detail: String {
        guard isDigital else { return analogQuality }
        let modulation = bitsPerCell == 2 ? "QPSK" : "\(1 << bitsPerCell)QAM"
        return "\(modulation) \(rateNum)/\(rateDen)"
    }

    /// 「數位 1 · QPSK 1/2」
    var label: String { detail.isEmpty ? name : "\(name) · \(detail)" }

    /// 「5.39 秒」
    var secondsText: String { String(format: "%.2f 秒", frameSeconds) }

    /// 「888 位元組」或「35% 係數」
    var contentText: String {
        if isDigital { return "\(imageBytes) 位元組" }
        let percent = Int((Double(analogCoefficients) / Double(NBTV.totalCoefficients) * 100).rounded())
        return "\(percent)% 係數"
    }

    /// 右側欄模式鈕下面的說明
    var summary: String {
        isDigital ? "每張 \(secondsText) · 影像上限 \(imageBytes) 位元組" : "每張 \(secondsText) · 傳 \(contentText)"
    }
}

enum Modes {
    static let all: [ModeInfo] = {
        var list: [ModeInfo] = []
        var info = nbtv_mode_info()
        for index in 0..<nbtv_mode_count() where nbtv_mode_at(index, &info) == 0 {
            list.append(ModeInfo(id: Int(info.id), isDigital: info.kind == NBTV_MODE_DIGITAL, frameSeconds: info.frame_seconds,
                                 frameSymbols: Int(info.frame_symbols), payloadSymbols: Int(info.payload_symbols),
                                 bitsPerCell: Int(info.bits_per_cell), rateNum: Int(info.code_rate_num), rateDen: Int(info.code_rate_den),
                                 imageBytes: Int(info.image_bytes), analogCoefficients: Int(info.analog_coefficients)))
        }
        // 類比模式依每幀的長短命名
        let names = ["粗略", "中等", "細緻"]
        let analogOrder = list.filter { !$0.isDigital }.sorted { $0.payloadSymbols < $1.payloadSymbols }.map(\.id)
        for i in list.indices {
            if let rank = analogOrder.firstIndex(of: list[i].id), analogOrder.count == names.count { list[i].analogQuality = names[rank] }
        }
        return list
    }()

    static var digital: [ModeInfo] { all.filter(\.isDigital) }
    static var analog: [ModeInfo] { all.filter { !$0.isDigital } }
    static func find(_ id: Int) -> ModeInfo? { all.first { $0.id == id } }
    static let defaultID = 1
}

/// 接收機解出來的一幀。
struct DecodedFrame {
    let modeID: Int
    let isDigital: Bool
    let decoded: Bool
    let headerOK: Bool
    let callsign: String
    let sequence: Int
    let snrDb: Double
    let offsetHz: Double
    let rgb: Data?       // 有畫面時是 96×72×3
    let payload: Data?   // 數位：影像酬載（AV1 資料）
}

final class CoreReceiver {
    private let handle: OpaquePointer
    private let frame: UnsafeMutablePointer<nbtv_rx_frame>

    init(sampleRate: Int = NBTV.sampleRate) {
        guard let h = nbtv_rx_create(Int32(sampleRate)), let f = nbtv_shim_frame_alloc() else { fatalError("無法建立接收機") }
        handle = h
        frame = f
    }

    deinit {
        nbtv_rx_destroy(handle)
        nbtv_shim_frame_free(frame)
    }

    func reset() { nbtv_rx_reset(handle) }

    func push(_ samples: [Float]) {
        samples.withUnsafeBufferPointer { nbtv_rx_push(handle, $0.baseAddress, Int32($0.count)) }
    }

    func status() -> nbtv_rx_status {
        var s = nbtv_rx_status()
        nbtv_rx_get_status(handle, &s)
        return s
    }

    func pollFrame() -> DecodedFrame? {
        guard nbtv_rx_poll_frame(handle, frame) == 1 else { return nil }
        let hasImage = frame.pointee.has_image != 0
        let imageBytes = Int(frame.pointee.image_bytes)
        return DecodedFrame(
            modeID: Int(frame.pointee.mode_id),
            isDigital: frame.pointee.kind == NBTV_MODE_DIGITAL,
            decoded: frame.pointee.decoded != 0,
            headerOK: frame.pointee.header_ok != 0,
            callsign: String(cString: nbtv_shim_frame_callsign(frame)),
            sequence: Int(frame.pointee.sequence),
            snrDb: Double(frame.pointee.snr_db),
            offsetHz: Double(frame.pointee.freq_offset_hz),
            rgb: hasImage ? Data(bytes: nbtv_shim_frame_rgb(frame), count: NBTV.rgbBytes) : nil,
            payload: imageBytes > 0 ? Data(bytes: nbtv_shim_frame_image_data(frame), count: imageBytes) : nil)
    }
}

/// 發射端做好的一幀。
struct EncodedFrame {
    let samples: [Float]   // 48 kHz
    let preview: Data      // 沒有雜訊時對方看到的畫面，96×72×3
    let modeID: Int
    let sequence: Int
    let imageBytes: Int    // 數位：壓縮後的位元組數
    let coefficients: Int  // 類比：傳送的係數個數
}

final class CoreTransmitter {
    private let handle: OpaquePointer

    init(sampleRate: Int = NBTV.sampleRate) {
        guard let h = nbtv_tx_create(Int32(sampleRate)) else { fatalError("無法建立發射機") }
        handle = h
    }

    deinit { nbtv_tx_destroy(handle) }

    @discardableResult
    func setMode(_ id: Int) -> Bool { nbtv_tx_set_mode(handle, Int32(id)) == 0 }
    func setCallsign(_ callsign: String) { nbtv_tx_set_callsign(handle, callsign) }
    var frameSamples: Int { Int(nbtv_tx_frame_samples(handle)) }
    /// 影像編碼器的速度檔 0（最慢、畫質最好）到 10；負值是核心的預設值。
    func setEncoderSpeed(_ speed: Int) { nbtv_tx_set_encoder_speed(handle, Int32(speed)) }

    /// 把一張 96×72 的畫面做成一幀音訊。失敗（例如畫面壓不進去）回傳 nil。
    func encode(rgb: Data) -> EncodedFrame? {
        guard rgb.count == NBTV.rgbBytes else { return nil }
        let count = frameSamples
        var samples = [Float](repeating: 0, count: count)
        var preview = [UInt8](repeating: 0, count: NBTV.rgbBytes)
        var info = nbtv_tx_frame_info()
        let written = rgb.withUnsafeBytes { src in
            nbtv_tx_encode_image(handle, src.bindMemory(to: UInt8.self).baseAddress, &samples, Int32(count), &preview, &info)
        }
        guard Int(written) == count else { return nil }
        return EncodedFrame(samples: samples, preview: Data(preview), modeID: Int(info.mode_id), sequence: Int(info.sequence),
                            imageBytes: Int(info.image_bytes), coefficients: Int(info.coefficients))
    }

    /// 取消最近編好、但決定不送的那一幀：發射機回到編它之前的狀態（幀序號、重取樣濾波器）。
    /// 沒有可以取消的幀時回傳 false。
    @discardableResult
    func discardLast() -> Bool { nbtv_tx_discard_last(handle) == 0 }

    /// 發射結束時取出重取樣濾波器裡剩下的尾巴。
    func finish() -> [Float] {
        var tail = [Float](repeating: 0, count: 256)
        let n = nbtv_tx_finish(handle, &tail, Int32(tail.count))
        return Array(tail[0..<Int(max(n, 0))])
    }
}

final class CoreSpectrum {
    private let handle: OpaquePointer

    init(sampleRate: Int = NBTV.sampleRate) {
        guard let h = nbtv_spectrum_create(Int32(sampleRate)) else { fatalError("無法建立頻譜分析") }
        handle = h
    }

    deinit { nbtv_spectrum_destroy(handle) }

    func reset() { nbtv_spectrum_reset(handle) }

    func push(_ samples: [Float]) {
        samples.withUnsafeBufferPointer { nbtv_spectrum_push(handle, $0.baseAddress, Int32($0.count)) }
    }

    /// 取出一列頻譜（dBFS，257 點），沒有的話回傳 nil。
    func poll() -> [Float]? {
        var row = [Float](repeating: 0, count: NBTV.spectrumBins)
        return nbtv_spectrum_poll(handle, &row) == 1 ? row : nil
    }
}
