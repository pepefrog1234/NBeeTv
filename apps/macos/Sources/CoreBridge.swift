// 核心 C API（nbtv.h）的 Swift 包裝。這裡的物件都不是執行緒安全的：
// 同一個物件只能在同一條佇列上使用，由呼叫端（RadioEngine、AppModel）負責。
import CNBTV
import CoreGraphics
import Foundation

enum NBTV {
    static let imageWidth = 96
    static let imageHeight = 72
    static let rgbBytes = imageWidth * imageHeight * 3   // 20736
    static let spectrumBins = 257                        // 0–3000 Hz
    static let spectrumMaxHz = 3000.0
    static let sampleRate = 48000                        // 外殼與核心之間一律用 48 kHz
    static let bandLowHz = 352.0                         // 訊號頻帶
    static let bandHighHz = 2648.0

    static var version: String { String(cString: nbtv_version_string()) }

    /// 把輸入整理成可以傳送的呼號（轉大寫、去掉不能用的字元、最多 9 個字元）。
    static func normalizeCallsign(_ text: String) -> String {
        var buffer = [CChar](repeating: 0, count: 16)
        _ = nbtv_callsign_normalize(text, &buffer)
        return String(cString: buffer)
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

/// 各個模式用的模型（畫面的編解碼是類神經網路）：每個模式各一份權重，放在 app 的 Resources 裡，第一次用到時載入
/// （小的約 0.1 秒，加寬的約 0.35 秒），之後所有的發射機、接收機共用；核心載入之後不會再改動它們，所以可以同時在不同的佇列上使用。
enum LearnedModel {
    /// Resources 裡的權重檔（不含副檔名）。檔名就是模型的名稱：「訓練完成的年月日-當天第幾個完成的」。
    /// 哪一份給哪個模式用由核心決定：模型的實數個數與寬度都和模式的相同就是。
    /// 依序是模式 11、12、13、14 用的；後兩份是加寬的大型網路（各 46 MB）。
    static let resourceNames = ["261123-1", "261122-3", "261122-4", "261123-4"]

    /// 載入成功的模型與它的名稱。找不到或損壞的那一份不在裡面：對應的模式不會列在選單裡，收到那種幀也不會有畫面。
    static let loaded: [(name: String, handle: OpaquePointer)] = resourceNames.compactMap { name in
        guard let url = Bundle.main.url(forResource: name, withExtension: "bin"),
              let data = try? Data(contentsOf: url, options: .mappedIfSafe),
              let handle = data.withUnsafeBytes({ nbtv_model_load($0.bindMemory(to: UInt8.self).baseAddress, $0.count) }) else { return nil }
        return (name, handle)
    }
    static let handles: [OpaquePointer] = loaded.map(\.handle)

    private static func find(values: Int, width: Int) -> (name: String, handle: OpaquePointer)? {
        loaded.first { Int(nbtv_model_values($0.handle)) == values && Int(nbtv_model_width($0.handle)) == width }
    }
    /// 「一張畫面變成 values 個實數、第一層 width 個通道」的那個模型（模式的 learned_values 與 learned_width）；沒有的話是 nil
    static func handle(values: Int, width: Int) -> OpaquePointer? { find(values: values, width: width)?.handle }
    /// 那個模型的名稱，例如「261123-1」
    static func name(values: Int, width: Int) -> String? { find(values: values, width: width)?.name }
    /// 載入成功的各個模型的識別碼（順序同 loaded）
    static let ids: [UInt32] = handles.map { nbtv_model_id($0) }
    /// 那個模型的識別碼；沒有的話是 nil
    static func id(values: Int, width: Int) -> UInt32? { handle(values: values, width: width).map { nbtv_model_id($0) } }
    /// 那個模型的權重檔識別碼；收發兩邊要相同才解得出畫面
    static func idText(values: Int, width: Int) -> String? {
        handle(values: values, width: width).map { String(format: "%08X", nbtv_model_id($0)) }
    }
}

/// 接收端「合併最近幾幀」用的權重（核心的 nbtv_merge）：解每一幀之前，畫面沒變的地方把前面幾幀收到的值合進來，
/// 訊號弱時畫面比較清楚也比較穩定。每個畫面模型各配一份，放在 app 的 Resources 裡；和模型一樣第一次用到時載入，
/// 之後所有的接收機共用。少了哪一份，用那個模型的模式就照原來的方式解。
enum MergeWeights {
    /// Resources 裡的權重檔（不含副檔名）。檔名是它自己的名稱（訓練完成的年月日-當天第幾個完成的）；
    /// 配的是哪一個畫面模型記在檔案裡，由核心對應。
    static let resourceNames = ["261124-10", "261124-8", "261124-9", "261124-7"]

    static let loaded: [(name: String, handle: OpaquePointer)] = resourceNames.compactMap { name in
        guard let url = Bundle.main.url(forResource: name, withExtension: "bin"),
              let data = try? Data(contentsOf: url, options: .mappedIfSafe),
              let handle = data.withUnsafeBytes({ nbtv_merge_load($0.bindMemory(to: UInt8.self).baseAddress, $0.count) }) else { return nil }
        return (name, handle)
    }
    static let handles: [OpaquePointer] = loaded.map(\.handle)

    /// 各份權重配的畫面模型的識別碼（順序同 loaded）
    static let modelIDs: [UInt32] = handles.map { nbtv_merge_model_id($0) }
    /// 這個畫面模型（以識別碼表示）有沒有配合併用的權重
    static func covers(modelID: UInt32) -> Bool { modelIDs.contains(modelID) }
}

/// 一個傳輸模式，資料取自 nbtv_mode_at()。
struct ModeInfo: Identifiable, Hashable {
    let id: Int
    let frameSeconds: Double
    let frameSymbols: Int
    let payloadSymbols: Int
    let learnedValues: Int  // 一張畫面變成幾個實數
    let learnedWidth: Int   // 模型第一層的通道數
    var modelName = ""      // 用的模型的名稱（訓練完成的年月日-當天第幾個），例如「261123-1」
    var quality = ""        // 快速／粗略／中等／細緻，依每幀的長短
    var largeModel = false  // 用的是加寬的大型網路（運算量約四倍）

    /// 「模式 11」
    var name: String { "模式 \(id)" }

    /// 「中等」
    var detail: String { quality }

    /// 「模式 11 · 中等」
    var label: String { detail.isEmpty ? name : "\(name) · \(detail)" }

    /// 「2.72 秒」
    var secondsText: String { String(format: "%.2f 秒", frameSeconds) }

    /// 「類神經網路」或「大型網路」
    var contentText: String { largeModel ? "大型網路" : "類神經網路" }

    /// 右側欄模式鈕下面的說明
    var summary: String {
        "每張 \(secondsText) · \(largeModel ? "大型的類神經網路" : "類神經網路")編碼，實驗中" + (modelName.isEmpty ? "" : " · 模型 \(modelName)")
    }

    /// 依每幀的酬載符號數取的名字：愈長的幀放得下愈多的值，畫面愈細，但每張也愈久。
    /// 目前的模式是 23（快速）、37（粗略）、100（中等）個符號。
    static func qualityName(payloadSymbols: Int) -> String {
        payloadSymbols < 30 ? "快速" : payloadSymbols < 60 ? "粗略" : payloadSymbols < 150 ? "中等" : "細緻"
    }
}

enum Modes {
    /// app 裡有模型的模式，依核心模式表的順序。少了哪一份權重檔，那個模式就不在裡面（選單不列、也不能發射）。
    static let all: [ModeInfo] = {
        var list: [ModeInfo] = []
        var info = nbtv_mode_info()
        for index in 0..<nbtv_mode_count() where nbtv_mode_at(index, &info) == 0 {
            let values = Int(info.learned_values), width = Int(info.learned_width)
            guard LearnedModel.handle(values: values, width: width) != nil else { continue }
            list.append(ModeInfo(id: Int(info.id), frameSeconds: info.frame_seconds, frameSymbols: Int(info.frame_symbols),
                                 payloadSymbols: Int(info.payload_symbols), learnedValues: values, learnedWidth: width,
                                 modelName: LearnedModel.name(values: values, width: width) ?? "",
                                 quality: ModeInfo.qualityName(payloadSymbols: Int(info.payload_symbols))))
        }
        // 模型比其他模式寬的標成「大型」（模式 13、14）
        let narrowest = list.map(\.learnedWidth).min() ?? 0
        for i in list.indices { list[i].largeModel = list[i].learnedWidth > narrowest }
        return list
    }()

    /// 選單的順序：依每幀的長短由快到慢；一樣長的，模型小的在前（模式編號是 14、12、11、13）
    static let menuOrder: [ModeInfo] = all.sorted {
        ($0.payloadSymbols, $0.learnedWidth, $0.id) < ($1.payloadSymbols, $1.learnedWidth, $1.id)
    }
    static func find(_ id: Int) -> ModeInfo? { all.first { $0.id == id } }
    /// 沒有選過（或原本選的模式已經不存在）時用的模式：模式 11；app 裡沒有它的模型的話，用手上有的第一個。
    static let defaultID: Int = find(11)?.id ?? all.first?.id ?? 11
}

/// 接收機交出來的一幀。
struct DecodedFrame {
    let modeID: Int
    let decoded: Bool       // 有畫面
    let modelMissing: Bool  // 收到了一幀，但權重和對方的不同（或 app 裡沒有那個模式的權重檔），所以沒有畫面
    let headerOK: Bool
    let callsign: String
    let sequence: Int
    let snrDb: Double
    let offsetHz: Double
    let rgb: Data?          // 有畫面時是 96×72×3
}

final class CoreReceiver {
    private let handle: OpaquePointer
    private let frame: UnsafeMutablePointer<nbtv_rx_frame>

    init(sampleRate: Int = NBTV.sampleRate) {
        guard let h = nbtv_rx_create(Int32(sampleRate)), let f = nbtv_shim_frame_alloc() else { fatalError("無法建立接收機") }
        handle = h
        frame = f
        for model in LearnedModel.handles { nbtv_rx_set_model(handle, model) }
        for merge in MergeWeights.handles { nbtv_rx_set_merge(handle, merge) }
    }

    deinit {
        nbtv_rx_destroy(handle)
        nbtv_shim_frame_free(frame)
    }

    func reset() { nbtv_rx_reset(handle) }

    /// 要不要把最近幾幀合起來解（預設不要）。只對有配合併用權重的模型有作用；重設不會改變這個設定。
    func setMerging(_ on: Bool) { nbtv_rx_set_merging(handle, on ? 1 : 0) }

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
        let decoded = frame.pointee.decoded != 0
        return DecodedFrame(
            modeID: Int(frame.pointee.mode_id),
            decoded: decoded,
            modelMissing: frame.pointee.model_missing != 0,
            headerOK: frame.pointee.header_ok != 0,
            callsign: String(cString: nbtv_shim_frame_callsign(frame)),
            sequence: Int(frame.pointee.sequence),
            snrDb: Double(frame.pointee.snr_db),
            offsetHz: Double(frame.pointee.freq_offset_hz),
            rgb: decoded ? Data(bytes: nbtv_shim_frame_rgb(frame), count: NBTV.rgbBytes) : nil)
    }
}

/// 發射端做好的一幀。
struct EncodedFrame {
    let samples: [Float]   // 48 kHz
    let preview: Data      // 沒有雜訊時對方看到的畫面，96×72×3
    let modeID: Int
    let sequence: Int
}

final class CoreTransmitter {
    private let handle: OpaquePointer

    init(sampleRate: Int = NBTV.sampleRate) {
        guard let h = nbtv_tx_create(Int32(sampleRate)) else { fatalError("無法建立發射機") }
        handle = h
        for model in LearnedModel.handles { nbtv_tx_set_model(handle, model) }
    }

    deinit { nbtv_tx_destroy(handle) }

    @discardableResult
    func setMode(_ id: Int) -> Bool { nbtv_tx_set_mode(handle, Int32(id)) == 0 }
    func setCallsign(_ callsign: String) { nbtv_tx_set_callsign(handle, callsign) }
    var frameSamples: Int { Int(nbtv_tx_frame_samples(handle)) }

    /// 把一張 96×72 的畫面做成一幀音訊。失敗（目前的模式沒有模型）回傳 nil。
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
        return EncodedFrame(samples: samples, preview: Data(preview), modeID: Int(info.mode_id), sequence: Int(info.sequence))
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
