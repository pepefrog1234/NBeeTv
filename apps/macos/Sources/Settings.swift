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
    /// 用電台的 CAT 指令控制 PTT 時的電台型號：Hamlib 的型號編號（0：還沒選）與選的時候看到的名稱（「Icom IC-7300」）。
    /// 名稱另外存一份，沒有裝 Hamlib 的時候畫面上才寫得出選的是哪一部
    @Published var rigModel: Int { didSet { store?.set(rigModel, forKey: "rigModel") } }
    @Published var rigModelName: String { didSet { store?.set(rigModelName, forKey: "rigModelName") } }
    /// CAT 的序列速率（bps）；0：用 Hamlib 替那個型號定的預設
    @Published var serialBaud: Int { didSet { store?.set(serialBaud, forKey: "serialBaud") } }
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
    /// 畫布：送出去的每一張畫面上要不要印字、印什麼、印在上面還是下面（見 Canvas.swift）
    @Published var canvasEnabled: Bool { didSet { store?.set(canvasEnabled, forKey: "canvasEnabled") } }
    @Published var canvasText: String { didSet { store?.set(canvasText, forKey: "canvasText") } }
    @Published var canvasPosition: CanvasPosition { didSet { store?.set(canvasPosition.rawValue, forKey: "canvasPosition") } }
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
        rigModel = (store?.object(forKey: "rigModel") as? Int) ?? 0
        rigModelName = store?.string(forKey: "rigModelName") ?? ""
        let baud = (store?.object(forKey: "serialBaud") as? Int) ?? 0
        serialBaud = Hamlib.baudRates.contains(baud) ? baud : 0
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
        canvasEnabled = (store?.object(forKey: "canvasEnabled") as? Bool) ?? false
        canvasText = store?.string(forKey: "canvasText") ?? ""
        canvasPosition = CanvasPosition(rawValue: store?.string(forKey: "canvasPosition") ?? "") ?? .top
    }

    /// 底列與設定視窗顯示的 PTT 說明
    var pttSummary: String {
        switch pttMode {
        case .none: return L("不控制（VOX）")
        case .serial: return L("序列埠 %@", serialLine.title)
        case .cat: return rigModel > 0 ? "CAT \(rigModelTitle)" : L("CAT（還沒選電台型號）")
        case .rigctld: return "rigctld \(rigHost):\(rigPort)"
        }
    }

    /// 選的電台型號的名稱；還沒選的時候是空的
    var rigModelTitle: String {
        guard rigModel > 0 else { return "" }
        return rigModelName.isEmpty ? L("型號 %d", rigModel) : rigModelName
    }

    /// 畫布目前的內容；沒有打開（或沒有字）的時候是 nil
    var canvasStyle: CanvasStyle? {
        let text = canvasText.trimmingCharacters(in: .whitespacesAndNewlines)
        return canvasEnabled && !text.isEmpty ? CanvasStyle(text: text, position: canvasPosition) : nil
    }

    func makePTTDriver() -> PTTDriver {
        if let debugPTTDriver { return debugPTTDriver() }
        switch pttMode {
        case .none: return NoPTT()
        case .serial: return SerialPTT(path: serialPort, line: serialLine)
        case .cat: return HamlibPTT(model: rigModel, port: serialPort, baud: serialBaud)
        case .rigctld: return RigctldPTT(host: rigHost, port: rigPort)
        }
    }

    var outputGain: Float { Float(pow(10.0, outputLevelDb / 20.0)) }
}
