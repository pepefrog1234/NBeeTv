// 使用者設定，存在 UserDefaults。
import Combine
import Foundation

final class AppSettings: ObservableObject {
    private let store: UserDefaults?

    /// 我的呼號（寫進每張畫面的標頭）
    @Published var callsign: String { didSet { store?.set(callsign, forKey: "callsign") } }
    /// 音訊裝置的 UID；空字串表示用系統預設
    @Published var inputDeviceUID: String { didSet { store?.set(inputDeviceUID, forKey: "inputDeviceUID") } }
    @Published var outputDeviceUID: String { didSet { store?.set(outputDeviceUID, forKey: "outputDeviceUID") } }
    /// 輸出音量（dB，−40…0）
    @Published var outputLevelDb: Double { didSet { store?.set(outputLevelDb, forKey: "outputLevelDb") } }
    @Published var pttMode: PTTMode { didSet { store?.set(pttMode.rawValue, forKey: "pttMode") } }
    @Published var serialPort: String { didSet { store?.set(serialPort, forKey: "serialPort") } }
    @Published var serialLine: SerialLine { didSet { store?.set(serialLine.rawValue, forKey: "serialLine") } }
    @Published var rigHost: String { didSet { store?.set(rigHost, forKey: "rigHost") } }
    @Published var rigPort: String { didSet { store?.set(rigPort, forKey: "rigPort") } }
    /// 發射模式的編號
    @Published var txModeID: Int { didSet { store?.set(txModeID, forKey: "txModeID") } }
    @Published var sourceKind: SourceKind { didSet { store?.set(sourceKind.rawValue, forKey: "sourceKind") } }
    @Published var scaleMode: ScaleMode { didSet { store?.set(scaleMode.rawValue, forKey: "scaleMode") } }
    @Published var cameraID: String { didSet { store?.set(cameraID, forKey: "cameraID") } }
    @Published var loopVideo: Bool { didSet { store?.set(loopVideo, forKey: "loopVideo") } }
    /// 上次選的影片檔的路徑（空字串：沒有）。下次啟動時自動開啟；定時發射播的就是這個影片
    @Published var videoPath: String { didSet { store?.set(videoPath, forKey: "videoPath") } }
    /// 定時發射：每隔幾分鐘一次，對齊整點（15 就是每小時的 00、15、30、45 分）。
    /// 開關本身不存在這裡：每次開 app 都是關的，要自己再打開（見 AppModel.scheduleArmed）
    @Published var scheduleIntervalMinutes: Int { didSet { store?.set(scheduleIntervalMinutes, forKey: "scheduleIntervalMinutes") } }
    /// 定時發射：到了時間正在收別人的畫面時，那一次不發射
    @Published var scheduleSkipWhenBusy: Bool { didSet { store?.set(scheduleSkipWhenBusy, forKey: "scheduleSkipWhenBusy") } }
    /// 在收到的畫面與縮圖的左上角標出那一張的訊雜比
    @Published var showFrameSNR: Bool { didSet { store?.set(showFrameSNR, forKey: "showFrameSNR") } }
    /// 接收時把最近幾幀合起來解：畫面沒變的地方雜訊被平均掉，訊號弱時比較清楚也比較穩定
    @Published var mergeFrames: Bool { didSet { store?.set(mergeFrames, forKey: "mergeFrames") } }
    /// 測試用：不是 nil 時，發射用它給的 PTT 驅動，不看 pttMode（自我測試用來確認 PTT 有按下、有放開）。
    var debugPTTDriver: (() -> PTTDriver)?

    /// store 傳 nil 時只存在記憶體裡（示範與測試用）。
    init(store: UserDefaults? = .standard) {
        self.store = store
        callsign = store?.string(forKey: "callsign") ?? ""
        inputDeviceUID = store?.string(forKey: "inputDeviceUID") ?? ""
        outputDeviceUID = store?.string(forKey: "outputDeviceUID") ?? ""
        outputLevelDb = (store?.object(forKey: "outputLevelDb") as? Double) ?? -6
        pttMode = PTTMode(rawValue: store?.string(forKey: "pttMode") ?? "") ?? .none
        serialPort = store?.string(forKey: "serialPort") ?? ""
        serialLine = SerialLine(rawValue: store?.string(forKey: "serialLine") ?? "") ?? .rts
        rigHost = store?.string(forKey: "rigHost") ?? "127.0.0.1"
        rigPort = store?.string(forKey: "rigPort") ?? "4532"
        let mode = (store?.object(forKey: "txModeID") as? Int) ?? Modes.defaultID
        txModeID = Modes.find(mode) != nil ? mode : Modes.defaultID
        sourceKind = SourceKind(rawValue: store?.string(forKey: "sourceKind") ?? "") ?? .camera
        scaleMode = ScaleMode(rawValue: store?.string(forKey: "scaleMode") ?? "") ?? .fill
        cameraID = store?.string(forKey: "cameraID") ?? ""
        loopVideo = (store?.object(forKey: "loopVideo") as? Bool) ?? true
        videoPath = store?.string(forKey: "videoPath") ?? ""
        let interval = (store?.object(forKey: "scheduleIntervalMinutes") as? Int) ?? TransmitSchedule.defaultInterval
        scheduleIntervalMinutes = TransmitSchedule.intervals.contains(interval) ? interval : TransmitSchedule.defaultInterval
        scheduleSkipWhenBusy = (store?.object(forKey: "scheduleSkipWhenBusy") as? Bool) ?? true
        showFrameSNR = (store?.object(forKey: "showFrameSNR") as? Bool) ?? false
        mergeFrames = (store?.object(forKey: "mergeFrames") as? Bool) ?? true
    }

    /// 底列與設定視窗顯示的 PTT 說明
    var pttSummary: String {
        switch pttMode {
        case .none: return "不控制（VOX）"
        case .serial: return "序列埠 \(serialLine.title)"
        case .rigctld: return "rigctld \(rigHost):\(rigPort)"
        }
    }

    func makePTTDriver() -> PTTDriver {
        if let debugPTTDriver { return debugPTTDriver() }
        switch pttMode {
        case .none: return NoPTT()
        case .serial: return SerialPTT(path: serialPort, line: serialLine)
        case .rigctld: return RigctldPTT(host: rigHost, port: rigPort)
        }
    }

    var outputGain: Float { Float(pow(10.0, outputLevelDb / 20.0)) }
}
