// 介面的狀態與收發流程。這個物件只在主執行緒上使用；
// 花時間的事（影像編碼、PTT、縮圖）丟到各自的背景佇列，做完再回主執行緒。
import AppKit
import AVFoundation
import Combine
import CoreAudio
import CoreGraphics
import Foundation
import UniformTypeIdentifiers

/// 給其他執行緒（鏡頭的回呼）放最新一格畫面用的盒子。
final class LatestFrameBox {
    private let lock = NSLock()
    private var buffer: CVPixelBuffer?
    private var turns = 0
    private var counter = 0
    private var cameraEnabled = false

    /// 換來源時呼叫：清掉舊的畫面，並決定之後收不收鏡頭的畫面
    /// （停止擷取之後，鏡頭的回呼還可能再來幾格，不能讓它蓋掉新來源的畫面）。
    func reset(acceptCamera: Bool) {
        lock.lock()
        buffer = nil
        turns = 0
        counter += 1
        cameraEnabled = acceptCamera
        lock.unlock()
    }

    /// 鏡頭的回呼用（在鏡頭的佇列上）。
    func setFromCamera(_ newBuffer: CVPixelBuffer) {
        lock.lock()
        if cameraEnabled {
            buffer = newBuffer
            turns = 0
            counter += 1
        }
        lock.unlock()
    }

    /// turns：這一格要順時針轉幾個 90 度才是正的（影片檔才會不是 0）。
    func set(_ newBuffer: CVPixelBuffer?, turns newTurns: Int = 0) {
        lock.lock()
        buffer = newBuffer
        turns = newTurns
        counter += 1
        lock.unlock()
    }

    func get() -> (buffer: CVPixelBuffer?, turns: Int, version: Int) {
        lock.lock()
        defer { lock.unlock() }
        return (buffer, turns, counter)
    }
}

final class AppModel: ObservableObject {
    enum LastResult { case none, ok, failed, modelMissing }
    enum TxPhase { case idle, starting, transmitting, stopping }

    /// 收到的一張畫面。
    struct ReceivedFrame: Identifiable {
        let id = UUID()
        let image: CGImage
        let modeID: Int
        let callsign: String
        let sequence: Int    // 幀序號；不知道時是 -1
        let time: Date
        let snrDb: Double    // 收到這一張時接收機量到的訊雜比（3 kHz 雜訊頻寬）
    }

    let settings: AppSettings
    /// false 時不碰任何裝置（算示範圖用）。
    let live: Bool

    // 接收
    @Published var synced = false
    @Published var syncCallsign = ""
    @Published var snrDb: Double?
    @Published var offsetHz: Double?
    @Published var rxModeID: Int?
    @Published var lastResult: LastResult = .none
    @Published var inputLevelDb = -120.0
    @Published var history: [ReceivedFrame] = []
    @Published var selectedID: UUID?
    @Published var waterfall: CGImage?
    /// 正在把一段畫面存成影片檔（見 saveDisplayedSequence）
    @Published private(set) var exportingVideo = false

    // 發射
    @Published var txPhase: TxPhase = .idle
    @Published var txModeID = Modes.defaultID
    @Published var txElapsed = 0.0
    @Published var txFramesSent = 0
    @Published var txFrameNumber = 0
    @Published var txFrameProgress = 0.0
    @Published var txPreview: CGImage?

    // 發射來源
    @Published var sourcePreview: CGImage?
    @Published var sourceSize: CGSize?
    @Published var sourceMessage: String?
    @Published var cameras: [CameraInfo] = []
    @Published var videoName: String?
    @Published var videoDuration = 0.0
    @Published var videoPosition = 0.0
    @Published var videoPlaying = false

    // 定時發射（流程見後面「定時發射」那一段）
    /// 開關。不存進設定：每次開 app 都是關的，要自己再打開，重開 app 之後才不會在沒有人注意的時候自己發射。
    @Published private(set) var scheduleArmed = false
    /// 下一次的時刻；沒開的時候是 nil
    @Published private(set) var scheduleNextFire: Date?
    /// 離下一次還有幾秒。只在最後一分鐘裡有值（畫面上倒數用）
    @Published private(set) var scheduleCountdown: Int?
    /// 最近一次到時間的結果：「12:00 送出 37 張」「12:15 沒有發射：正在發射」。開關關掉之後也留著
    @Published private(set) var scheduleLastEvent: String?
    /// 目前這一次發射是定時發射開始的
    @Published private(set) var scheduledRun = false
    /// 定時發射的面板是否開著
    @Published var showScheduleMenu = false

    // 裝置
    @Published var inputDevices: [AudioDevice] = []
    @Published var outputDevices: [AudioDevice] = []
    @Published var serialPorts: [String] = []
    @Published var inputDeviceName = "—"
    @Published var outputDeviceName = "—"
    /// 要讓使用者看到的訊息（裝置或 PTT 出錯等），顯示在底列。
    @Published var notice: String?
    /// PTT 的指令沒有成功、電台可能還按著發射時的警告（主視窗疊一條顯示，要使用者自己按掉，見 PTTWarningBanner）；平常是 nil。
    @Published var pttWarning: String?
    /// 發射模式的選單是否開著
    @Published var showModeMenu = false

    /// 開發用：重要的流程轉折會經由它印出來（見 NBTVApp.swift 的 DebugOptions）。
    static var trace: ((String) -> Void)?

    let waterfallStore = WaterfallStore()
    private let engine: RadioEngine?
    private let camera = CameraSource()
    private let video = VideoFileSource()
    private let latestFrame = LatestFrameBox()
    private var previewVersion = -1
    private var previewScale: ScaleMode?
    // 畫布（印在送出畫面上的字）：圖層照設定算好留著，設定改了才重算（見 currentCanvas）
    private var canvasLayer: CanvasLayer?
    private var canvasBuilt: CanvasStyle?
    private var canvasVersion = 0
    private var previewCanvasVersion = 0
    private var previewTime = Date.distantPast
    private var sourceEpoch = 0   // 每換一次來源加一；換來源之前開始算的預覽晚到時靠它認出來

    // 畫面每 0.1 秒更新一次（uiTick）。其他佇列與計時器的結果先放在這裡，到時候一起反映到 @Published 的屬性上：
    // 每改一次，整個視窗就要重排一次，一秒重排幾十次很浪費。
    private var queuedUpdates: [RxUpdate] = []
    private var queuedMonitorRows: [[Float]] = []
    private var pendingPreview: (image: CGImage?, size: CGSize)?
    private struct TransmitDisplay {
        let number: Int
        let preview: CGImage?
        let progress: Double
        let sent: Int
        let elapsed: Double
    }
    private var txDisplay: TransmitDisplay?
    private var lastSourceRGB: Data?
    private var scrubbing = false
    private let previewQueue = DispatchQueue(label: "nbtv.preview", qos: .userInitiated)
    private let exportQueue = DispatchQueue(label: "nbtv.export", qos: .userInitiated)
    private var uiTimer: Timer?
    private var deviceTimer: Timer?
    private var cancellables = Set<AnyCancellable>()
    private var receiveWanted = false           // 已經取得麥克風權限，應該要在接收
    private var activeInputID: AudioDeviceID?   // 目前收音的裝置；沒有開成功時是 nil
    private var activeInputRate = 0.0           // 開始接收時那個裝置的取樣率

    // 發射流程
    private struct OnAirFrame {
        let start: Int64
        let end: Int64
        let samples: [Float]
        let preview: CGImage?
        let number: Int
    }
    private let transmitter = CoreTransmitter()  // 只在 txQueue 上使用
    private let txQueue = DispatchQueue(label: "nbtv.tx", qos: .userInitiated)
    private let pttQueue = DispatchQueue(label: "nbtv.ptt", qos: .userInitiated)
    private var pttDriver: PTTDriver?
    private var onAir: [OnAirFrame] = []
    private var txSession = 0           // 每次開始或結束發射就加一；晚到的非同步結果靠它認出自己已經過期
    private var txLead: Int64 = 0       // 開頭的靜音（等電台切到發射）
    private var txEnd: Int64 = 0        // 已排進輸出佇列的最後位置
    private var txMonitored: Int64 = 0  // 已交給頻譜分析的位置
    private var txNextNumber = 1
    private var txPreparing = false
    private var txTailState = 0         // 0：還沒排尾巴，1：正在取，2：已排入
    private var txLastRGB: Data?        // 這次發射最近送出的那一格；來源一時沒有畫面時重送它
    private var txTimer: Timer?
    private var txTap: DebugTap.Stream?        // 除錯用：排進輸出佇列的取樣原樣記到檔案（見 DebugTap）
    private var txTapLogged = Date.distantPast

    // 定時發射
    /// 現在幾點。測試時換成假的時鐘，不必真的等到整點。
    var clock: () -> Date = { Date() }
    private var scheduleLabel = ""                   // 這一次定時發射的時刻（「12:15」）
    private var scheduleRestoreSource: SourceKind?   // 定時發射結束後要換回去的來源；不必換時是 nil
    private var scheduleLimit: Double?               // 這一次定時發射最多送幾秒；超過就自動停止（見 fireSchedule）
    private var scheduleCutShort = false             // 這一次是超過上限、自動停下來的
    private var scheduleActivity: NSObjectProtocol?  // 開著的時候握著：不讓電腦因為閒置而睡眠
    private var lastSignalTime = Date.distantPast    // 接收機最近一次同步著（或交出畫面）是什麼時候，見 receivingSomeone
    /// 到了時間卻晚了這麼久才輪到（電腦剛從睡眠醒來、時鐘被往前調）：那一次不發射
    static let scheduleLateLimit = 5.0
    /// 定時發射最多送「影片的長度＋兩張＋這麼多秒」。正常是影片播完再送一到兩張就結束；影片卡住、
    /// 或中途被換成別的來源時，旁邊沒有人按停止，靠這個上限收尾。（測試時改小）
    static var scheduleMarginSeconds = 5.0

    static let pttLeadSeconds = 0.3      // 按下 PTT 後等多久才開始送音訊
    /// 輸出裝置超過這麼多秒沒有來取取樣，就當成它壞了（被拔掉、沒有回應）：立刻停止發射、放開 PTT。
    /// 發射的每一步都是照「已經播到哪裡」在走的，裝置不來取就永遠等不到，PTT 會一直按著。（測試時改小）
    static var outputStallSeconds = 3.0
    /// 「結束發射中」要過這麼久，再按一次發射鈕才算「立刻停止」（免得連按兩下就把最後一張切掉）
    static let abortGuardSeconds = 0.5
    private var txPulledFrames: Int64 = -1   // 最近一次看到輸出裝置總共取了幾格
    private var txPulledAt = 0.0             // 那個數字從什麼時候起沒再變（系統開機後的秒數，不受時鐘調整影響）
    private var txStoppingAt = 0.0           // 什麼時候進到「結束發射中」
    static let prepareLeadSeconds = 0.6  // 目前這張剩多久時開始準備下一張（至少）
    /// 最近準備一張（取畫面＋編碼）花了多久；偶爾特別慢的一次會讓它維持高一陣子。
    private var txPrepareSeconds = 0.0
    /// 目前這張剩多久時開始準備下一張。大型網路的編碼加預覽在慢的機器上可能要大半秒，
    /// 所以照實際花的時間提早，下一張才趕得及在目前這張播完之前排進去。
    private var prepareLead: Double { max(AppModel.prepareLeadSeconds, txPrepareSeconds * 1.5 + 0.25) }
    /// 「最近收到」最多留幾張（都在記憶體裡，一張約 23 KB）；超過就丟掉最舊的。
    static let historyLimit = 5000

    init(settings: AppSettings, live: Bool) {
        self.settings = settings
        self.live = live
        engine = live ? RadioEngine() : nil
        txModeID = settings.txModeID
        // 設定改了，用到設定的畫面也要重畫
        settings.objectWillChange.sink { [weak self] in self?.objectWillChange.send() }.store(in: &cancellables)
        // 定時發射開著的時候改了間隔：下一次的時刻重新算
        settings.$scheduleIntervalMinutes.dropFirst().sink { [weak self] value in self?.rescheduleNext(interval: value) }.store(in: &cancellables)
        guard live, let engine else { return }

        engine.onRxUpdate = { [weak self] update in self?.queuedUpdates.append(update) }
        engine.onMonitorRows = { [weak self] rows in self?.queuedMonitorRows += rows }
        camera.onFrame = { [latestFrame] buffer in latestFrame.setFromCamera(buffer) }
        camera.onInterrupted = { [weak self] message in
            DispatchQueue.main.async {
                // 鏡頭沒了：不再顯示（也不再發射）它留下的最後一格
                guard let self, self.settings.sourceKind == .camera else { return }
                self.clearSourceFrame()
                self.sourceMessage = message
            }
        }

        // 電腦要睡眠了（蓋上筆電、選「睡眠」）：見 systemWillSleep
        NSWorkspace.shared.notificationCenter.publisher(for: NSWorkspace.willSleepNotification)
            .sink { [weak self] _ in self?.systemWillSleep() }.store(in: &cancellables)

        // 換了輸入裝置：refreshDevices 發現該用的裝置和正在用的不同，會改從新的裝置接收
        settings.$inputDeviceUID.dropFirst().sink { [weak self] _ in
            DispatchQueue.main.async { self?.refreshDevices() }
        }.store(in: &cancellables)
        settings.$outputDeviceUID.dropFirst().sink { [weak self] _ in
            DispatchQueue.main.async { self?.refreshDevices() }
        }.store(in: &cancellables)
        settings.$outputLevelDb.sink { [weak self] value in
            self?.engine?.output.gain = Float(pow(10.0, value / 20.0))
        }.store(in: &cancellables)
        settings.$loopVideo.sink { [weak self] value in
            guard let self else { return }
            self.video.loop = value && !self.scheduledRun   // 定時發射中一律只播一次
        }.store(in: &cancellables)
        settings.$txModeID.sink { [weak self] value in
            if self?.txPhase == .idle { self?.txModeID = value }
        }.store(in: &cancellables)
    }

    // MARK: - 啟動與結束

    /// 開始接收、打開發射來源。app 啟動後呼叫一次。
    func start() {
        guard live else { return }
        refreshDevices()
        DebugTap.log("啟動：輸入「\(inputDeviceName)」、輸出「\(outputDeviceName)」、來源 \(settings.sourceKind.rawValue)、模式 \(settings.txModeID)、PTT \(settings.pttMode.rawValue)、輸出音量 \(settings.outputLevelDb) dB")
        if ProcessInfo.processInfo.environment["NBTV_NO_DEVICES"] == nil {
            requestAudioAccessAndReceive()
            restartSource()
        }
        reopenLastVideo()
        let timer = Timer(timeInterval: 0.1, repeats: true) { [weak self] _ in self?.uiTick() }
        RunLoop.main.add(timer, forMode: .common)
        uiTimer = timer
        let slow = Timer(timeInterval: 5, repeats: true) { [weak self] _ in self?.refreshDevices() }
        RunLoop.main.add(slow, forMode: .common)
        deviceTimer = slow
    }

    /// app 結束前呼叫：停止輸出並放開 PTT（同步做完才回來）。
    func shutdown() {
        txSession += 1
        txTimer?.invalidate()
        engine?.output.stop()
        if let driver = pttDriver {
            pttQueue.sync { try? driver.key(false) }
            pttDriver = nil
        }
        engine?.stopReceiving()
        camera.stop()
        video.close()
        if let activity = scheduleActivity {
            ProcessInfo.processInfo.endActivity(activity)
            scheduleActivity = nil
        }
    }

    /// 電腦即將睡眠。睡著之後音訊就停了，但用指令按下的 PTT（電台的 CAT 指令、rigctld）不會自己放開，
    /// 電台會空著發射到電腦醒來為止：所以正在發射的話立刻停止，而且等 PTT 真的放開了才回來
    /// （系統會等這個通知處理完才睡）。
    func systemWillSleep() {
        guard txPhase != .idle else { return }
        failTransmit(L("電腦進入睡眠，已停止發射並放開 PTT"))
        pttQueue.sync {}   // 放開 PTT 排在這條佇列上，等它做完
    }

    /// PTT 的指令沒有成功，電台可能還按著：記下來並在主視窗上顯示警告（底列的一行字太容易沒看到，而且下一次發射就被清掉）。
    private func warnPTTMayBeKeyed(_ reason: String) {
        DebugTap.log("警告：PTT 可能還按著（\(reason)）")
        AppModel.trace?("警告：PTT 可能還按著（\(reason)）")
        pttWarning = L("PTT 的指令沒有成功（%@），電台可能還在發射。請看一下電台；還在發射的話，直接用電台上的按鍵或開關停下來。", reason)
    }

    // MARK: - 裝置

    /// 重新列出系統上的裝置（啟動時、每 5 秒，以及打開裝置選單之前）。
    func refreshDevices() {
        inputDevices = AudioDevices.inputs()
        outputDevices = AudioDevices.outputs()
        serialPorts = SerialPTT.availablePorts()
        cameras = CameraSource.devices()
        let input = AudioDevices.resolve(uid: settings.inputDeviceUID, input: true)
        inputDeviceName = input?.name ?? L("沒有輸入裝置")
        outputDeviceName = AudioDevices.resolve(uid: settings.outputDeviceUID, input: false)?.name ?? L("沒有輸出裝置")

        // 該用的輸入裝置和正在用的不一樣了（改了設定、裝置被拔掉或插回來、系統預設換了、取樣率改了，
        // 或是先前沒開成功）：改從現在該用的那一個接收。
        guard receiveWanted, txPhase == .idle else { return }
        let rate = input.map { AudioDevices.sampleRate($0.id) } ?? 0
        if input?.id != activeInputID || (activeInputID != nil && rate != activeInputRate) {
            AppModel.trace?("輸入裝置改變，重新開始接收")
            engine?.stopReceiving()
            startReceiving()
        }
    }

    private func requestAudioAccessAndReceive() {
        let denied = L("沒有麥克風權限，無法接收（系統設定 → 隱私權與安全性 → 麥克風）")
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized:
            receiveWanted = true
            startReceiving()
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .audio) { [weak self] granted in
                DispatchQueue.main.async {
                    guard let self else { return }
                    if granted {
                        self.receiveWanted = true
                        self.startReceiving()
                    } else {
                        self.notice = denied
                    }
                }
            }
        default:
            notice = denied
        }
    }

    /// 從設定的輸入裝置開始接收（接收機會清成初始狀態）。發射中或還沒有麥克風權限時不做事。
    private func startReceiving() {
        guard let engine, receiveWanted, txPhase == .idle else { return }
        activeInputID = nil
        clearSyncState()
        guard let device = AudioDevices.resolve(uid: settings.inputDeviceUID, input: true) else {
            notice = L("找不到音訊輸入裝置")
            return
        }
        do {
            try engine.startReceiving(deviceID: device.id)
            activeInputID = device.id
            activeInputRate = AudioDevices.sampleRate(device.id)
            inputDeviceName = device.name
            DebugTap.log("開始接收：輸入「\(device.name)」，裝置取樣率 \(Int(activeInputRate)) Hz")
        } catch {
            DebugTap.log("開始接收失敗：\(error.localizedDescription)")
            notice = L("無法開啟輸入裝置「%@」：%@", device.name, error.localizedDescription)
        }
    }

    // MARK: - 接收

    private func clearSyncState() {
        synced = false
        syncCallsign = ""
        snrDb = nil
        offsetHz = nil
        rxModeID = nil
        lastResult = .none
    }

    func apply(_ update: RxUpdate) {
        guard txPhase == .idle || !live else { return }
        if update.synced || !update.frames.isEmpty { lastSignalTime = clock() }
        inputLevelDb = update.inputLevelDb
        for frame in update.frames { add(frame, time: Date()) }
        if update.synced {
            synced = true
            rxModeID = update.modeID
            snrDb = update.snrValid ? update.snrDb : nil
            offsetHz = update.offsetHz
        } else if synced || update.frames.isEmpty {
            clearSyncState()
        }
        appendSpectrum(update.spectrumRows, reference: update.synced ? .hold : .noise)
    }

    func add(_ frame: DecodedFrame, time: Date) {
        guard frame.decoded, let rgb = frame.rgb, let image = NBTV.image(fromRGB: rgb) else {
            // 幀收到了，但對方用的是另一份權重（或 app 裡沒有那個模式的權重檔）
            lastResult = frame.modelMissing ? .modelMissing : .failed
            return
        }
        lastResult = .ok
        if frame.headerOK, !frame.callsign.isEmpty { syncCallsign = frame.callsign }
        history.insert(ReceivedFrame(image: image, modeID: frame.modeID,
                                     callsign: frame.headerOK ? frame.callsign : "", sequence: frame.headerOK ? frame.sequence : -1, time: time,
                                     snrDb: frame.snrDb),
                       at: 0)
        if history.count > AppModel.historyLimit { history.removeLast(history.count - AppModel.historyLimit) }
        // 有新的畫面進來：主畫面一律換成它。先前點選來看的那一張還在「最近收到」裡，要看可以再點
        selectedID = nil
    }

    func appendSpectrum(_ rows: [[Float]], reference: WaterfallStore.Reference) {
        var changed = false
        for row in rows where waterfallStore.append(row, reference: reference) { changed = true }
        if changed { waterfall = waterfallStore.makeImage() }
    }

    /// 主畫面顯示的那一張：使用者點選的，沒有點選就是最新的。點選只維持到下一張畫面進來為止（見 add）。
    var displayedFrame: ReceivedFrame? {
        if let selectedID, let frame = history.first(where: { $0.id == selectedID }) { return frame }
        return history.first
    }

    func select(_ frame: ReceivedFrame) {
        // 點最新的那張等於回到「跟著最新的」
        selectedID = frame.id == history.first?.id ? nil : frame.id
    }

    func isDisplayed(_ frame: ReceivedFrame) -> Bool { displayedFrame?.id == frame.id }

    /// 一張收到的畫面存成檔案時的內容與檔名：96×72 的 PNG，像素和收到的畫面完全相同。
    static func exportFile(for frame: ReceivedFrame) -> (data: Data, name: String)? {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        let call = frame.callsign.isEmpty ? "unknown" : frame.callsign.replacingOccurrences(of: "/", with: "-")
        let base = "NBTV-\(call)-\(formatter.string(from: frame.time))"
        guard let png = NSBitmapImageRep(cgImage: frame.image).representation(using: .png, properties: [:]) else { return nil }
        return (png, base + ".png")
    }

    /// 儲存主畫面顯示的那一張。
    func saveDisplayedFrame() {
        guard let frame = displayedFrame else { return }
        guard let file = AppModel.exportFile(for: frame) else {
            notice = L("無法產生圖檔")
            return
        }
        let panel = NSSavePanel()
        panel.nameFieldStringValue = file.name
        if let type = UTType(filenameExtension: (file.name as NSString).pathExtension) { panel.allowedContentTypes = [type] }
        panel.begin { [weak self] response in
            guard response == .OK, let url = panel.url else { return }
            do {
                try file.data.write(to: url)
            } catch {
                self?.notice = L("存檔失敗：%@", error.localizedDescription)
            }
        }
    }

    // MARK: - 把一段畫面存成影片

    /// 兩張畫面之間的空檔要小於這麼多秒，才算同一段
    static let sequenceGapSeconds = 10.0

    /// 那一張畫面所在的「一段」：同一個呼號、一張接一張，中間的空檔都不到 10 秒。回傳的順序由舊到新。
    ///
    /// 空檔是前一張收完到後一張開始送之間的時間（後一張收完的時刻減掉它那個模式一張的長度，再減前一張收完的時刻），
    /// 所以連續發射時是 0，中間漏掉一張時是一張的長度。
    /// 標頭沒有解出來、不知道呼號的畫面，時間接得上就跟著前面那一段（它是一段的開頭的話，跟著後面的）。
    /// 每一張畫面只屬於一段：不論從一段裡的哪一張問，得到的都是同一段。
    static func sequence(containing id: UUID, in history: [ReceivedFrame]) -> [ReceivedFrame] {
        guard let index = history.firstIndex(where: { $0.id == id }) else { return [] }
        /// older 收完之後到 newer 開始送，中間的空檔夠不夠短
        func contiguous(_ older: ReceivedFrame, _ newer: ReceivedFrame) -> Bool {
            let started = newer.time.addingTimeInterval(-(Modes.find(newer.modeID)?.frameSeconds ?? 0))
            return started.timeIntervalSince(older.time) < sequenceGapSeconds
        }
        // 1. 時間上接在一起的那一串（history 是新的在前：往後是更早的，往前是更晚的）
        var oldest = index, newest = index
        while oldest + 1 < history.count, contiguous(history[oldest + 1], history[oldest]) { oldest += 1 }
        while newest > 0, contiguous(history[newest], history[newest - 1]) { newest -= 1 }
        // 2. 這一串由舊到新照呼號分段：出現和這一段不一樣的呼號就是下一段的開頭
        var groups: [[ReceivedFrame]] = []
        var callsign = ""
        for frame in history[newest...oldest].reversed() {
            if groups.isEmpty || (!frame.callsign.isEmpty && !callsign.isEmpty && frame.callsign != callsign) {
                groups.append([frame])
                callsign = frame.callsign
            } else {
                groups[groups.count - 1].append(frame)
                if callsign.isEmpty { callsign = frame.callsign }
            }
        }
        return groups.first { group in group.contains { $0.id == id } } ?? []
    }

    /// 主畫面顯示的那一張所在的一段（由舊到新）；還沒有畫面時是空的。
    var displayedSequence: [ReceivedFrame] {
        displayedFrame.map { AppModel.sequence(containing: $0.id, in: history) } ?? []
    }

    /// 一段畫面存成影片時的預設檔名：呼號與第一張收到的時間。
    static func videoFileName(for frames: [ReceivedFrame]) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        let known = frames.first { !$0.callsign.isEmpty }?.callsign ?? ""
        let call = known.isEmpty ? "unknown" : known.replacingOccurrences(of: "/", with: "-")
        return "NBTV-\(call)-\(formatter.string(from: frames.first?.time ?? Date())).mp4"
    }

    /// 影片的時間軸：每一張從它收到的時刻開始顯示，到下一張收到為止，所以影片的快慢和當時收到的一樣；
    /// 最後一張停留它那個模式一張的長度（至少一秒）。
    static func videoTimeline(for frames: [ReceivedFrame]) -> (pictures: [SequenceVideo.Picture], lastHold: Double) {
        guard let first = frames.first, let last = frames.last else { return ([], 0) }
        let pictures = frames.map { SequenceVideo.Picture(image: $0.image, start: $0.time.timeIntervalSince(first.time)) }
        return (pictures, max(1, Modes.find(last.modeID)?.frameSeconds ?? 1))
    }

    /// 把主畫面顯示的那一張所在的一段存成影片檔（.mp4、H.264、每秒 24 格）。只有一張的不存。
    func saveDisplayedSequence() {
        let frames = displayedSequence
        guard frames.count >= 2, !exportingVideo else { return }
        let panel = NSSavePanel()
        panel.nameFieldStringValue = AppModel.videoFileName(for: frames)
        panel.allowedContentTypes = [.mpeg4Movie]
        panel.begin { [weak self] response in
            guard response == .OK, let url = panel.url else { return }
            self?.exportSequence(frames, to: url)
        }
    }

    /// 在背景把一段畫面寫成影片檔；結果寫在底列（completion 是給測試用的）。
    func exportSequence(_ frames: [ReceivedFrame], to url: URL, completion: ((Result<SequenceVideo.Summary, Error>) -> Void)? = nil) {
        guard !frames.isEmpty, !exportingVideo else { return }
        exportingVideo = true
        let timeline = AppModel.videoTimeline(for: frames)
        exportQueue.async {
            let result = Result { try SequenceVideo.write(timeline.pictures, lastHold: timeline.lastHold, to: url) }
            DispatchQueue.main.async {
                self.exportingVideo = false
                switch result {
                case .success(let summary):
                    self.notice = L("影片已存成「%@」：%d 張，%@", url.lastPathComponent, summary.pictures, AppModel.clock(summary.seconds))
                case .failure(let error):
                    self.notice = L("影片存檔失敗：%@", error.localizedDescription)
                }
                completion?(result)
            }
        }
    }

    // MARK: - 發射來源

    func setSourceKind(_ kind: SourceKind) {
        guard settings.sourceKind != kind else { return }
        scheduleRestoreSource = nil   // 定時發射中使用者自己換了來源：結束後就不再換回去
        settings.sourceKind = kind
        restartSource()
    }

    func selectCamera(_ id: String) {
        settings.cameraID = id
        restartSource()
    }

    /// 目前選的鏡頭名稱。
    var cameraName: String {
        if let camera = cameras.first(where: { $0.id == settings.cameraID }) { return camera.name }
        return cameras.first?.name ?? L("沒有鏡頭")
    }

    func restartSource() {
        guard live else { return }
        sourceMessage = nil
        clearSourceFrame()
        switch settings.sourceKind {
        case .camera:
            video.pause()
            startCamera()
        case .video:
            camera.stop()
            if video.url == nil {
                sourceMessage = L("還沒選擇影片檔")
            } else if settings.loopVideo {
                video.play()   // 不循環的影片停在原來的位置，按下發射才開始播（見 goOnAir）
            }
        }
    }

    /// 畫布目前的圖層；沒有打開（或沒有字）時是 nil。設定改了就在這裡重算一次（不到一毫秒）。
    private func currentCanvas() -> CanvasLayer? {
        let style = settings.canvasStyle
        if style != canvasBuilt {
            canvasBuilt = style
            canvasLayer = style.flatMap { CanvasLayer(style: $0) }
            canvasVersion += 1
        }
        return canvasLayer
    }

    /// 把畫布的字印到一張 96×72 的來源畫面上（畫布沒開就原樣回傳）。發射前的每一張與右側欄的預覽都經過這裡。
    func applyCanvas(to rgb: Data) -> Data { currentCanvas()?.apply(to: rgb) ?? rgb }

    /// 打開、關掉畫布。第一次打開而還沒有填字的時候，先填上「CQ 呼號」。
    func setCanvasEnabled(_ on: Bool) {
        if on, settings.canvasText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            settings.canvasText = settings.callsign.isEmpty ? "CQ" : "CQ \(settings.callsign)"
        }
        settings.canvasEnabled = on
    }

    /// 換了來源（或換了影片檔）：上一個來源留下的畫面不能再顯示，也不能再拿來發射。
    private func clearSourceFrame() {
        sourceEpoch += 1
        latestFrame.reset(acceptCamera: settings.sourceKind == .camera)
        previewVersion = -1
        lastSourceRGB = nil
        pendingPreview = nil
        sourcePreview = nil
        sourceSize = nil
    }

    private func startCamera() {
        // 開發用：設了 NBTV_NO_DEVICES 就完全不開鏡頭（也就不會跳出相機權限的詢問）
        guard ProcessInfo.processInfo.environment["NBTV_NO_DEVICES"] == nil else {
            sourceMessage = L("沒有開鏡頭（NBTV_NO_DEVICES）")
            return
        }
        let begin = { [weak self] in
            // 等使用者回答權限的這段時間裡可能已經改選影片檔了，那就不要開鏡頭
            guard let self, self.settings.sourceKind == .camera else { return }
            self.camera.start(deviceID: self.settings.cameraID) { error in
                DebugTap.log(error.map { "鏡頭開不起來：\($0)" } ?? "鏡頭開始擷取")
                DispatchQueue.main.async { if self.settings.sourceKind == .camera { self.sourceMessage = error } }
            }
        }
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            begin()
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .video) { [weak self] granted in
                DispatchQueue.main.async {
                    if granted { begin() } else { self?.sourceMessage = L("沒有相機權限（系統設定 → 隱私權與安全性 → 相機）") }
                }
            }
        default:
            sourceMessage = L("沒有相機權限（系統設定 → 隱私權與安全性 → 相機）")
        }
    }

    func chooseVideoFile() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.movie, .video, .mpeg4Movie, .quickTimeMovie]
        panel.allowsMultipleSelection = false
        panel.begin { [weak self] response in
            guard response == .OK, let url = panel.url else { return }
            // 使用者自己選的影片記下來，下次啟動時自動開啟（見 reopenLastVideo）
            self?.openVideo(url) { ok in if ok { self?.settings.videoPath = url.path } }
        }
    }

    /// 上次選的影片檔還在的話，啟動時自動開啟：定時發射播的就是它，不必每次開 app 都重選。
    private func reopenLastVideo() {
        let path = settings.videoPath
        guard !path.isEmpty, video.url == nil, FileManager.default.fileExists(atPath: path) else { return }
        openVideo(URL(fileURLWithPath: path))
    }

    /// autoplay 是 nil 時照「循環播放」的設定：循環的影片開了就播；不循環的停在開頭，按下發射才開始播、播完自動停止發射。
    /// 來源不是影片檔的時候（從定時發射的面板選影片）只開啟，不播放。
    func openVideo(_ url: URL, autoplay: Bool? = nil, completion: ((Bool) -> Void)? = nil) {
        if settings.sourceKind == .video {
            sourceMessage = nil
            clearSourceFrame()
        }
        video.loop = settings.loopVideo && !scheduledRun
        video.open(url) { [weak self] ok in
            guard let self else { return }
            if ok {
                self.videoName = url.lastPathComponent
                self.videoDuration = self.video.duration
                self.videoPosition = 0
                let isSource = self.settings.sourceKind == .video
                if autoplay ?? (self.settings.loopVideo && isSource) {
                    self.video.play()
                } else if autoplay == nil, isSource {
                    self.video.seek(to: 0)   // 停在開頭；移一下位置，開頭那一格才會送出來當預覽
                }
            } else {
                self.videoName = nil
                self.videoDuration = 0
                // 來源是鏡頭時，預覽那一格是鏡頭的地方，訊息改放底列
                if self.settings.sourceKind == .video {
                    self.sourceMessage = L("無法播放這個影片檔")
                } else {
                    self.notice = L("無法播放影片檔「%@」", url.lastPathComponent)
                }
            }
            completion?(ok)
        }
    }

    func toggleVideoPlayback() {
        guard video.url != nil else { return }
        if video.isPlaying { video.pause() } else { video.play() }
        videoPlaying = video.isPlaying
    }

    func scrubVideo(to seconds: Double, editing: Bool) {
        scrubbing = editing
        videoPosition = seconds
        video.seek(to: seconds)
    }

    /// 每 0.1 秒一次：把這段時間累積的結果一起反映到畫面上。
    private func uiTick() {
        // 接收：照順序套用；重新開始接收之前留下來的不算
        if let engine {
            for update in queuedUpdates where update.generation == engine.generation { apply(update) }
        }
        queuedUpdates.removeAll()
        scheduleTick()   // 定時發射：到時間了嗎（排在接收的更新後面，「正在收別人的畫面」才是最新的）
        // 發射：送出去的訊號的頻譜，以及目前這張的進度
        if !queuedMonitorRows.isEmpty {
            if isOnAir { appendSpectrum(queuedMonitorRows, reference: .transmit) }
            queuedMonitorRows.removeAll()
        }
        publishTransmitDisplay()
        // 發射來源
        if video.url != nil {
            // 畫面只在來源是影片檔時才取；播放的位置與狀態隨時照實更新（來源是鏡頭時影片也可能開著，定時發射要用）
            if settings.sourceKind == .video, let buffer = video.copyCurrentFrame() { latestFrame.set(buffer, turns: video.quarterTurns) }
            let position = video.position, playing = video.isPlaying
            if !scrubbing, position != videoPosition { videoPosition = position }
            if playing != videoPlaying { videoPlaying = playing }
        }
        if let preview = pendingPreview {
            pendingPreview = nil
            sourcePreview = preview.image
            sourceSize = preview.size
        }
        refreshPreview()
    }

    private func publishTransmitDisplay() {
        guard isOnAir, let display = txDisplay else { return }
        if txFrameNumber != display.number || txPreview == nil {
            txFrameNumber = display.number
            txPreview = display.preview
        }
        if txFramesSent != display.sent { txFramesSent = display.sent }
        txFrameProgress = display.progress
        txElapsed = display.elapsed
    }

    /// 來源有新的一格（或縮放方式、畫布改了）時，重算 96×72 的預覽。預覽就是會送出去的樣子，畫布的字也印在上面。
    private func refreshPreview() {
        let (buffer, turns, version) = latestFrame.get()
        let mode = settings.scaleMode
        let canvas = currentCanvas()
        let restyled = mode != previewScale || canvasVersion != previewCanvasVersion
        guard let buffer, version != previewVersion || restyled else { return }
        // 把整格來源畫面縮小並不便宜：來源一直在動時每秒更新 5 次就夠了；改縮放方式、改畫布或剛換來源時立刻更新
        let now = Date()
        guard restyled || sourcePreview == nil || now.timeIntervalSince(previewTime) >= 0.19 else { return }
        previewTime = now
        previewVersion = version
        previewScale = mode
        previewCanvasVersion = canvasVersion
        let epoch = sourceEpoch
        previewQueue.async { [weak self] in
            guard let frame = FrameScaler.scale(buffer, mode: mode, quarterTurns: turns) else { return }
            let image = NBTV.image(fromRGB: canvas?.apply(to: frame.rgb) ?? frame.rgb)
            DispatchQueue.main.async {
                guard let self, epoch == self.sourceEpoch else { return }   // 這段時間裡換了來源
                self.lastSourceRGB = frame.rgb
                self.pendingPreview = (image, frame.sourceSize)   // 下一次 uiTick 顯示
            }
        }
    }

    /// 取來源當下那一格（縮成 96×72，還沒有印上畫布的字）。沒有新的畫面時沿用上一格。
    private func captureSourceFrame(_ completion: @escaping (Data?) -> Void) {
        if settings.sourceKind == .video, let buffer = video.copyCurrentFrame() { latestFrame.set(buffer, turns: video.quarterTurns) }
        let (buffer, turns, _) = latestFrame.get()
        let mode = settings.scaleMode
        let fallback = lastSourceRGB
        previewQueue.async {
            let rgb = buffer.flatMap { FrameScaler.scale($0, mode: mode, quarterTurns: turns)?.rgb } ?? fallback
            DispatchQueue.main.async { completion(rgb) }
        }
    }

    /// 示範用：取影片目前位置的那一格當預覽（不經過計時器）。取到了回傳 true。
    func demoGrabVideoFrame() -> Bool {
        guard let buffer = video.copyCurrentFrame(),
              let frame = FrameScaler.scale(buffer, mode: settings.scaleMode, quarterTurns: video.quarterTurns) else { return false }
        lastSourceRGB = frame.rgb
        sourcePreview = NBTV.image(fromRGB: applyCanvas(to: frame.rgb))
        sourceSize = frame.sourceSize
        videoPosition = video.position
        videoPlaying = video.isPlaying
        return true
    }

    /// 有來源畫面可以發射嗎？
    var canTransmit: Bool { live && txPhase == .idle && (lastSourceRGB != nil || latestFrame.get().buffer != nil) }

    // MARK: - 發射
    //
    // 流程：取來源的一格 → 編碼（txQueue）→ 按下 PTT（pttQueue）→ 開輸出裝置、排入 0.3 秒靜音與第一張
    // → 每張快播完時取來源當下那一格編下一張，緊接著排進去 → 按停止後送完目前這張、補上尾巴 → 放開 PTT → 回到接收。
    // 這些步驟分散在幾條佇列上，每一步回到主執行緒時都要先確認這次發射還沒被取消（txSession 沒變）。

    func toggleTransmit() {
        switch txPhase {
        case .idle: startTransmit()
        case .starting, .transmitting: stopTransmit()
        case .stopping:
            // 已經在收尾了又按一次：不等這一張送完，立刻停止
            if ProcessInfo.processInfo.systemUptime - txStoppingAt >= AppModel.abortGuardSeconds { abortTransmit() }
        }
    }

    /// 不等目前這張送完，立刻停止輸出、放開 PTT、回到接收。最後這一張不完整，對方解不出來。
    /// 平常按停止是「送完目前這張」；這個是給等不及的時候用的（頻率不對、蓋到別人）。
    func abortTransmit() {
        guard txPhase == .transmitting || txPhase == .stopping else { return }
        DebugTap.log("立刻停止發射")
        AppModel.trace?("立刻停止發射：最後一張沒有送完")
        endTransmit(failure: L("按了立刻停止，最後一張沒有送完"))
    }

    /// fromStart：來源是影片檔時先回到開頭再開始（定時發射用）；平常是從影片目前的位置接著播。
    func startTransmit(fromStart: Bool = false) {
        guard live, txPhase == .idle, let mode = Modes.find(settings.txModeID) else { return }
        guard !settings.callsign.isEmpty else {
            notice = L("還沒設定呼號，不能發射")
            return
        }
        notice = nil
        showModeMenu = false
        txSession += 1
        let session = txSession
        txPhase = .starting
        txModeID = mode.id
        txElapsed = 0
        txFramesSent = 0
        txFrameNumber = 0
        txFrameProgress = 0
        txPreview = nil
        onAir.removeAll()
        txNextNumber = 1
        txPreparing = true
        txTailState = 0
        txDisplay = nil
        txLastRGB = nil
        let callsign = settings.callsign
        DebugTap.log("按下發射：模式 \(mode.id)、來源 \(settings.sourceKind.rawValue)")
        let encodeFirst = { [weak self] in
            guard let self, session == self.txSession else { return }
            self.captureSourceFrame { [weak self] rgb in
                guard let self, session == self.txSession else { return }
                guard let rgb else {
                    self.failTransmit(L("沒有來源畫面可以發射"))
                    return
                }
                self.txLastRGB = rgb
                let picture = self.applyCanvas(to: rgb)   // 畫布的字在這裡印上去；txLastRGB 留的是沒有印字的
                self.txQueue.async {
                    // 上一次若有編好卻沒送出去的幀（取消、PTT 失敗），先取消它，再把重取樣濾波器清乾淨
                    self.transmitter.discardLast()
                    _ = self.transmitter.finish()
                    self.transmitter.setMode(mode.id)
                    self.transmitter.setCallsign(callsign)
                    let began = Date()
                    let frame = self.transmitter.encode(rgb: picture)
                    let spent = Date().timeIntervalSince(began)
                    DispatchQueue.main.async {
                        if session == self.txSession { self.txPrepareSeconds = spent }
                        self.firstFrameReady(frame, session: session)
                    }
                }
            }
        }
        // 要不要先回到開頭：影片上一次播完停在結尾；或是指定要從頭來，而影片不是停在開頭
        let rewind = video.hasEnded || (fromStart && (video.isPlaying || video.position > 0.01))
        if settings.sourceKind == .video, video.url != nil, rewind {
            // 先回到開頭，等開頭那一格出來，第一張才不會是原來那個位置的畫面
            DebugTap.log(video.hasEnded ? "影片停在結尾：回到開頭再發射" : "影片回到開頭再發射")
            video.rewind { [weak self] in
                guard let self else { return }
                self.waitForVideoFrame(session: session, since: self.latestFrame.get().version, attempts: 25, then: encodeFirst)
            }
        } else {
            encodeFirst()
        }
    }

    /// 影片的位置剛移過（rewind）：新位置的那一格要一點時間才解得出來。每 20 毫秒看一次，拿到了（或等了半秒）就繼續。
    /// since：位置移過去那時候來源畫面的版本。等的這段時間裡，每 0.1 秒一次的畫面更新（uiTick）可能先把新的那一格取走；
    /// 版本變了就表示已經有了，不必再等。
    private func waitForVideoFrame(session: Int, since version: Int, attempts: Int, then: @escaping () -> Void) {
        guard session == txSession else { return }
        if let buffer = video.copyCurrentFrame() {
            latestFrame.set(buffer, turns: video.quarterTurns)
            then()
        } else if latestFrame.get().buffer != nil, latestFrame.get().version != version {
            then()
        } else if attempts <= 0 {
            then()
        } else {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.02) { [weak self] in
                self?.waitForVideoFrame(session: session, since: version, attempts: attempts - 1, then: then)
            }
        }
    }

    /// 來源是只播一次的影片（沒有勾循環播放，或這一次是定時發射），而且已經播完、停在最後一格。
    private var videoFinished: Bool {
        settings.sourceKind == .video && video.url != nil && (!settings.loopVideo || scheduledRun) && video.hasEnded
    }

    /// 測試用：之後的發射不開音訊裝置，改由模擬的輸出照實際的時間把取樣取走（見 AudioOutput.simulated），
    /// 取走的取樣交給 sink（在別的佇列上呼叫）。stallAfter：模擬的輸出取了這麼多秒之後就不再取（見 AudioOutput.simulatedStallAfter）。
    func debugUseSimulatedOutput(sink: (([Float]) -> Void)?, stallAfter: Double? = nil) {
        engine?.output.simulated = true
        engine?.output.simulatedSink = sink
        engine?.output.simulatedStallAfter = stallAfter
    }

    private func firstFrameReady(_ frame: EncodedFrame?, session: Int) {
        guard session == txSession, txPhase == .starting else { return }
        guard let frame else {
            failTransmit(Modes.find(txModeID) == nil ? L("app 裡沒有這個模式的模型，不能發射") : L("畫面編碼失敗"))
            return
        }
        DebugTap.log("第一張編好（\(frame.samples.count) 個取樣），按下 PTT（\(settings.pttMode.rawValue)）")
        // 先按下 PTT，再開始送音訊。driver 先登記起來：之後不論從哪裡取消，
        // 「放開」都會排在這個「按下」的後面（pttQueue 是序列佇列），不會留下按著不放的 PTT。
        let driver = settings.makePTTDriver()
        pttDriver = driver
        pttQueue.async {
            do {
                try driver.key(true)
            } catch {
                // 沒按成功也照樣放開一次。按下的指令可能其實到了電台（見 PTTError.mayBeKeyed），放開也可能失敗：
                // 有一邊說不準，就要提醒使用者去看電台
                var uncertain = (error as? PTTError)?.mayBeKeyed == true
                do { try driver.key(false) } catch { uncertain = true }
                DispatchQueue.main.async {
                    if self.pttDriver === driver { self.pttDriver = nil }   // 上面已經放開了
                    if uncertain { self.warnPTTMayBeKeyed(error.localizedDescription) }
                    guard session == self.txSession, self.txPhase == .starting else { return }
                    self.failTransmit(L("PTT 失敗：%@", error.localizedDescription))
                }
                return
            }
            DebugTap.log("PTT 已按下")
            DispatchQueue.main.async { self.goOnAir(first: frame, session: session) }
        }
    }

    private func goOnAir(first: EncodedFrame, session: Int) {
        // 等 PTT 的這段時間裡已經取消了：PTT 由取消的那一邊放開，這裡什麼都不用做
        guard session == txSession, txPhase == .starting, let engine else { return }
        engine.stopReceiving()
        clearSyncState()
        engine.resetSpectrum()
        let device = engine.output.simulated ? nil : AudioDevices.resolve(uid: settings.outputDeviceUID, input: false)
        guard device != nil || engine.output.simulated else {
            failTransmit(L("找不到音訊輸出裝置"))
            return
        }
        let deviceName = device?.name ?? "模擬的輸出"
        do {
            engine.output.gain = settings.outputGain
            try engine.output.start(deviceID: device?.id ?? 0)
            if let device { outputDeviceName = device.name }
        } catch {
            failTransmit(L("無法開啟輸出裝置「%@」：%@", deviceName, error.localizedDescription))
            return
        }
        txLead = Int64(AppModel.pttLeadSeconds * Double(NBTV.sampleRate))
        txTap?.close()
        txTap = DebugTap.Stream(kind: "tx")
        DebugTap.log(String(format: "開始送音訊：輸出「%@」，裝置取樣率 %d Hz，輸出增益 %.4f", deviceName,
                            Int(device.map { AudioDevices.sampleRate($0.id) } ?? Double(NBTV.sampleRate)), engine.output.gain))
        txTap?.write([Float](repeating: 0, count: Int(txLead)))
        engine.output.enqueue([Float](repeating: 0, count: Int(txLead)))
        txEnd = txLead
        txMonitored = txLead
        txPulledFrames = -1
        txPulledAt = ProcessInfo.processInfo.systemUptime
        txPhase = .transmitting
        guard enqueue(first) else { return }
        // 來源是影片檔：開始送音訊的同時讓影片開始播放（已經在播的不動）。
        // 不循環的影片播完之後，再送出它的最後一格就自動停止（見 prepareNextFrame）。
        if settings.sourceKind == .video, video.url != nil, !video.isPlaying {
            video.play()
            videoPlaying = true
            DebugTap.log("影片開始播放")
            AppModel.trace?("影片開始播放")
        }
        let timer = Timer(timeInterval: 0.05, repeats: true) { [weak self] _ in self?.txTick() }
        RunLoop.main.add(timer, forMode: .common)
        txTimer = timer
        txTick()
        publishTransmitDisplay()   // 第一張的預覽馬上顯示，不等下一次 uiTick
        AppModel.trace?("開始送出音訊")
    }

    /// 把編好的一幀排進輸出佇列，接在目前排到的位置後面。
    @discardableResult
    private func enqueue(_ frame: EncodedFrame) -> Bool {
        guard let engine else { return false }
        guard engine.output.enqueue(frame.samples) else {
            // 不應該發生：佇列的長度是照最長的模式配置的
            failTransmit(L("輸出佇列放不下這一張，已停止發射"))
            return false
        }
        let start = txEnd
        txEnd += Int64(frame.samples.count)
        txTap?.write(frame.samples)
        if DebugTap.isEnabled {
            let peak = frame.samples.reduce(Float(0)) { max($0, abs($1)) }
            let bad = frame.samples.contains { !$0.isFinite }
            let rate = Double(NBTV.sampleRate)
            DebugTap.log(String(format: "第 %d 張排入：模式 %d、%d 個取樣、峰值 %.3f%@，佇列裡 %.2f 秒，已播 %.2f 秒，佇列空掉 %d 次",
                                txNextNumber, frame.modeID, frame.samples.count, peak, bad ? "（含 NaN 或無限大！）" : "",
                                Double(engine.output.queuedSamples) / rate, Double(engine.output.playedSamples) / rate, engine.output.underruns))
        }
        onAir.append(OnAirFrame(start: start, end: txEnd, samples: frame.samples, preview: NBTV.image(fromRGB: frame.preview),
                                number: txNextNumber))
        txNextNumber += 1
        txPreparing = false
        return true
    }

    private func txTick() {
        guard let engine, txPhase == .transmitting || txPhase == .stopping else { return }
        // 輸出裝置還有沒有來取取樣（佇列空了它也照樣來取，取到的是靜音）。USB 音效卡中途斷線、系統的音訊服務卡住時
        // 它就不來了：已經播到哪裡不會再前進，下面每一步（排下一張、收尾、定時發射的上限）都等不到。
        let pulled = engine.output.renderedFrames, now = ProcessInfo.processInfo.systemUptime
        if pulled != txPulledFrames {
            txPulledFrames = pulled
            txPulledAt = now
        } else if now - txPulledAt > AppModel.outputStallSeconds {
            failTransmit(L("音訊輸出裝置沒有回應（可能被拔掉了），已停止發射並放開 PTT"))
            return
        }
        let position = engine.output.playedSamples
        if DebugTap.isEnabled, Date().timeIntervalSince(txTapLogged) >= 1 {
            txTapLogged = Date()
            let rate = Double(NBTV.sampleRate)
            DebugTap.log(String(format: "發射中：已播 %.2f 秒、已排到 %.2f 秒、佇列裡 %.2f 秒、裝置取了 %lld 格（其中靜音 %lld 格）、佇列空掉 %d 次、%@%@",
                                Double(position) / rate, Double(txEnd) / rate, Double(engine.output.queuedSamples) / rate,
                                engine.output.renderedFrames, engine.output.silentFrames, engine.output.underruns,
                                txPhase == .transmitting ? "發射中" : "停止中", txPreparing ? "、正在準備下一張" : ""))
        }

        // 定時發射：超過上限還沒結束（影片卡住、中途被換成別的來源）就自己停下來，和按下停止一樣收尾
        if scheduledRun, txPhase == .transmitting, let limit = scheduleLimit, Double(position - txLead) / Double(NBTV.sampleRate) > limit {
            scheduleLimit = nil
            scheduleCutShort = true
            notice = L("定時發射 %@ 超過影片的長度還沒結束，已自動停止", scheduleLabel)
            AppModel.trace?("定時發射超過上限：自動停止")
            stopTransmit()
        }

        // 把已經送出去的部分交給頻譜分析
        if position > txMonitored {
            var chunk: [Float] = []
            chunk.reserveCapacity(Int(position - txMonitored))
            for frame in onAir where frame.end > txMonitored && frame.start < position {
                let from = Int(max(txMonitored, frame.start) - frame.start)
                let to = Int(min(position, frame.end) - frame.start)
                if to > from { chunk.append(contentsOf: frame.samples[from..<to]) }
            }
            txMonitored = position
            if !chunk.isEmpty { engine.monitor(chunk) }
        }

        // 目前正在播的那一張（還在開頭的靜音時是第一張）。這裡只記下來，畫面上的數字由 uiTick 更新。
        if let current = onAir.last(where: { $0.start <= position }) ?? onAir.first {
            let rate = Double(NBTV.sampleRate)
            txDisplay = TransmitDisplay(number: current.number, preview: current.preview,
                                        progress: Double(max(0, min(position, current.end) - current.start)) / rate,
                                        sent: position >= current.end ? current.number : current.number - 1,
                                        elapsed: max(0, Double(position - txLead) / rate))
        }
        while onAir.count > 1, let first = onAir.first, first.end <= position { onAir.removeFirst() }

        if txPhase == .transmitting {
            // 目前這張快播完時，取來源當下那一格做下一張
            if !txPreparing, Double(txEnd - position) / Double(NBTV.sampleRate) < prepareLead { prepareNextFrame() }
        } else if !txPreparing {
            if txTailState == 0 {
                queueTail()
            } else if txTailState == 2, position >= txEnd {
                finishTransmit()
            }
        }
    }

    private func prepareNextFrame() {
        txPreparing = true
        let session = txSession
        let started = Date()
        // 來源的影片已經播完：這次取到的是它的最後一格，這一張排進去之後就收尾
        let lastPicture = videoFinished
        captureSourceFrame { [weak self] captured in
            guard let self, session == self.txSession else { return }
            // 來源一時沒有畫面（剛切換來源）：重送上一格，不要讓電台空著發射
            guard self.txPhase == .transmitting, let rgb = captured ?? self.txLastRGB else {
                DebugTap.log("準備下一張：沒有畫面可用（或已經不在發射）")
                self.txPreparing = false
                return
            }
            if captured == nil { DebugTap.log("準備下一張：來源沒有新的畫面，重送上一格") }
            if DebugTap.isEnabled {
                let mean = rgb.reduce(0) { $0 + Int($1) } / max(rgb.count, 1)
                DebugTap.log("準備下一張：來源 \(self.settings.sourceKind.rawValue)，畫面 \(rgb.count) 位元組、平均亮度 \(mean)")
            }
            self.txLastRGB = rgb
            let picture = self.applyCanvas(to: rgb)   // 發射中改了畫布的字，下一張就照新的印
            self.txQueue.async {
                let began = Date()
                let frame = self.transmitter.encode(rgb: picture)
                let spent = Date().timeIntervalSince(began)
                DispatchQueue.main.async {
                    guard session == self.txSession else { return }
                    // 下一次要提早多久：以這次從取畫面到編好的時間為準；比上次快的話慢慢降回來
                    self.txPrepareSeconds = max(Date().timeIntervalSince(started), self.txPrepareSeconds * 0.8)
                    if let frame, self.txPhase == .transmitting {
                        self.enqueue(frame)
                        DebugTap.log(String(format: "準備這一張花了 %.0f 毫秒，下一張提早 %.2f 秒開始準備", self.txPrepareSeconds * 1000, self.prepareLead))
                        AppModel.trace?(String(format: "第 %d 張編好排入（編碼 %.0f 毫秒，前一張還剩 %.2f 秒）", self.txNextNumber - 1, spent * 1000,
                                               Double(self.txEnd - Int64(frame.samples.count) - (self.engine?.output.playedSamples ?? 0)) / Double(NBTV.sampleRate)))
                        if lastPicture { self.stopAfterQueuedFrames() }
                    } else {
                        // 編好之後才發現已經按了停止：這張不送，發射機也回到編它之前的狀態（見 queueTail）
                        if frame == nil { DebugTap.log(String(format: "編碼失敗（花了 %.0f 毫秒）：這一張沒有東西可以送", spent * 1000)) }
                        if frame != nil {
                            self.txQueue.async { self.transmitter.discardLast() }
                            AppModel.trace?("停止時下一張剛編好：不送")
                        }
                        self.txPreparing = false
                    }
                }
            }
        }
    }

    /// 停止發射：送完目前這張後放開 PTT、回到接收。
    func stopTransmit() {
        DebugTap.log("按下停止")
        switch txPhase {
        case .transmitting:
            txPhase = .stopping
            txStoppingAt = ProcessInfo.processInfo.systemUptime
            pauseVideoWithTransmit()   // 之後不會再取新的畫面，影片不必等這一張送完才停
            dropQueuedFrame()
            if !txPreparing { queueTail() }
        case .starting:
            // 還沒開始送：直接取消
            failTransmit(nil)
        default:
            break
        }
    }

    /// 來源的影片播完了，它的最後一格也已經排進輸出佇列：和按下停止一樣收尾（送完、補尾巴、放開 PTT、回到接收），
    /// 差別是不拿掉已經排進去的那一張——對方要看得到影片的結尾。
    private func stopAfterQueuedFrames() {
        guard txPhase == .transmitting else { return }
        DebugTap.log("影片播完：最後一格已經排入，送完就停止發射")
        AppModel.trace?("影片播完：送完最後一張就停止發射")
        txPhase = .stopping
        txStoppingAt = ProcessInfo.processInfo.systemUptime
        if !txPreparing { queueTail() }
    }

    /// 發射停下來了（按了停止，或送到一半出錯）：來源的影片跟著暫停。沒有在送的這段時間影片不會自己往前走，
    /// 下一次發射從停下的地方接著播（見 goOnAir）。還沒開始送就取消的不算，影片維持原來的樣子。
    private func pauseVideoWithTransmit() {
        guard settings.sourceKind == .video, video.url != nil else { return }
        let wasPlaying = video.isPlaying
        // 看起來沒在播也照樣交代一次：循環播放的影片剛好播到結尾的那一瞬間，要這樣它才不會自己又從頭播（見 VideoFileSource）
        video.pause()
        videoPlaying = false
        guard wasPlaying else { return }
        if !scrubbing { videoPosition = video.position }
        DebugTap.log(String(format: "發射停止：影片暫停在 %.1f 秒", video.position))
        AppModel.trace?("發射停止：影片跟著暫停")
    }

    /// 「送完目前這張」就停：下一張如果已經排進輸出佇列、但還沒開始播，把它拿掉。
    private func dropQueuedFrame() {
        guard let engine, let next = onAir.last, engine.output.discardQueued(from: next.start) else { return }
        onAir.removeLast()
        txEnd = next.start
        txNextNumber = next.number
        txQueue.async { self.transmitter.discardLast() }   // 發射機也回到編它之前的狀態（見 queueTail）
        AppModel.trace?("停止時下一張已經排進佇列但還沒開始播：拿掉")
    }

    /// 最後補上重取樣濾波器的尾巴與一小段靜音，確定整張都送出去了才放開 PTT。
    ///
    /// 核心的發射機在 48 kHz 時有一個串流式的重取樣濾波器：每一張輸出的最後約 0.5 毫秒留在濾波器裡，
    /// 接在下一張的開頭送出，最後一張的則由 nbtv_tx_finish 取出。如果有一張已經編好卻決定不送，
    /// 要先用 nbtv_tx_discard_last 取消它（見 dropQueuedFrame 與 prepareNextFrame），
    /// 這裡取到的才是最後送出那一張的尾巴；一張都沒送的話，取到的是空的。
    private func queueTail() {
        txTailState = 1
        let session = txSession
        txQueue.async {
            let tail = self.transmitter.finish()   // 同時把發射機清回初始狀態
            DispatchQueue.main.async {
                guard session == self.txSession, self.txPhase == .stopping, let engine = self.engine else { return }
                // 尾巴要緊接在最後一張後面才有意義；佇列已經空了（來不及）就只補靜音
                let late = engine.output.playedSamples >= self.txEnd
                let block = (late ? [] : tail) + [Float](repeating: 0, count: NBTV.sampleRate / 10)
                self.txTap?.write(block)
                DebugTap.log("排入尾巴 \(block.count) 個取樣\(late ? "（佇列已經空了，只補靜音）" : "")")
                engine.output.enqueue(block)
                self.txEnd += Int64(block.count)
                self.txTailState = 2
            }
        }
    }

    private func finishTransmit() {
        publishTransmitDisplay()
        AppModel.trace?("發射結束：共送出 \(txFramesSent) 張，輸出佇列中途空掉 \(max(0, (engine?.output.underruns ?? 0) - 1)) 次")
        endTransmit(failure: nil)
    }

    private func failTransmit(_ message: String?) {
        if let message { notice = message }
        DebugTap.log(message.map { "發射中止：\($0)" } ?? "發射取消")
        AppModel.trace?(message.map { "發射中止：\($0)" } ?? "發射取消")
        endTransmit(failure: message ?? L("還沒開始送就取消了"))
    }

    /// 停止輸出、放開 PTT、回到接收。failure：不是正常送完的話，原因是什麼（定時發射記錄結果用）。
    private func endTransmit(failure: String?) {
        if let engine {
            DebugTap.log(String(format: "發射結束：已播 %.2f 秒、已排到 %.2f 秒、送出 %d 張、裝置取了 %lld 格（其中靜音 %lld 格）、佇列空掉 %d 次",
                                Double(engine.output.playedSamples) / Double(NBTV.sampleRate), Double(txEnd) / Double(NBTV.sampleRate),
                                txFramesSent, engine.output.renderedFrames, engine.output.silentFrames, engine.output.underruns))
        }
        // 送到一半出錯才會在「發射中」直接走到這裡（按停止的話，當下就暫停過了；之後使用者自己又按播放的不再管）
        if txPhase == .transmitting { pauseVideoWithTransmit() }
        txTap?.close()
        txTap = nil
        txSession += 1
        txTimer?.invalidate()
        txTimer = nil
        txPreparing = false
        txLastRGB = nil
        txDisplay = nil
        queuedMonitorRows.removeAll()
        engine?.output.stop()
        releasePTT()
        txPhase = .idle
        txModeID = settings.txModeID
        onAir.removeAll()
        txPreview = nil
        // 接收在開始送音訊時停掉了（還沒走到那一步就取消的話，接收一直沒停）
        if engine?.isReceiving == false { startReceiving() }
        if scheduledRun { finishScheduledRun(failure: failure) }
    }

    private func releasePTT() {
        guard let driver = pttDriver else { return }
        pttDriver = nil
        pttQueue.async {
            do {
                try driver.key(false)
                DebugTap.log("PTT 已放開")
            } catch {
                DebugTap.log("放開 PTT 失敗：\(error.localizedDescription)")
                DispatchQueue.main.async {
                    self.notice = L("放開 PTT 失敗：%@", error.localizedDescription)
                    self.warnPTTMayBeKeyed(error.localizedDescription)
                }
            }
        }
    }

    // MARK: - 定時發射
    //
    // 開著的時候，每到時鐘上對齊整點的時刻（每 15 分鐘就是每小時的 00、15、30、45 分整），自動把影片檔從頭發射一次：
    // 來源不是影片檔的話先換過去 → 影片回到開頭 → 走和按下發射一樣的流程 → 影片播完自動停止（不管有沒有勾循環播放）
    // → 換回原來的來源。到了時間卻不能發射（正在發射、沒有影片、沒有呼號、正在收別人的畫面）就跳過那一次，
    // 原因寫在底列與面板裡。
    //
    // 時刻由每 0.1 秒一次的 uiTick 對照時鐘，不另外設計時器：電腦睡過頭、時鐘被調整的時候，比一下現在的時間
    // 就知道那一次該不該發，醒來之後不會補發。

    func setScheduleArmed(_ on: Bool) {
        guard on != scheduleArmed else { return }
        scheduleArmed = on
        scheduleCountdown = nil
        scheduleNextFire = on ? TransmitSchedule.nextFire(after: clock(), intervalMinutes: settings.scheduleIntervalMinutes) : nil
        let text = scheduleNextFire.map { "打開定時發射：每 \(settings.scheduleIntervalMinutes) 分鐘，下一次 \(TransmitSchedule.clockText($0))" } ?? "關掉定時發射"
        DebugTap.log(text)
        AppModel.trace?(text)
        // 開著的時候不讓電腦因為閒置而睡眠（螢幕照常可以關）：睡著了，到時間就不會發射
        if on, live, scheduleActivity == nil {
            scheduleActivity = ProcessInfo.processInfo.beginActivity(options: .idleSystemSleepDisabled, reason: "定時發射開著，到了時間要自動發射")
        } else if !on, let activity = scheduleActivity {
            ProcessInfo.processInfo.endActivity(activity)
            scheduleActivity = nil
        }
    }

    private func rescheduleNext(interval: Int) {
        guard scheduleArmed else { return }
        scheduleCountdown = nil
        scheduleNextFire = TransmitSchedule.nextFire(after: clock(), intervalMinutes: interval)
    }

    /// 每 0.1 秒一次（uiTick）：到時間了嗎？
    private func scheduleTick() {
        guard scheduleArmed, let due = scheduleNextFire else { return }
        let now = clock()
        let remaining = due.timeIntervalSince(now)
        let interval = settings.scheduleIntervalMinutes
        if remaining > Double(interval * 60) + 1 {
            rescheduleNext(interval: interval)   // 時鐘被往回調了：從現在的時間重新算
            return
        }
        if remaining > 0 {
            let seconds = remaining <= 60 ? Int(remaining.rounded(.up)) : nil
            if seconds != scheduleCountdown { scheduleCountdown = seconds }
            return
        }
        scheduleCountdown = nil
        scheduleNextFire = TransmitSchedule.nextFire(after: now, intervalMinutes: interval)
        let label = TransmitSchedule.clockText(due)
        if -remaining > AppModel.scheduleLateLimit {
            skipSchedule(label, L("錯過了時間（電腦剛從睡眠醒來，或時鐘被調整過）"))
        } else {
            fireSchedule(label)
        }
    }

    /// 到時間了：把影片檔從頭發射一次。不能發射就跳過這一次。
    private func fireSchedule(_ label: String) {
        guard txPhase == .idle else { return skipSchedule(label, L("正在發射")) }
        if let problem = scheduleProblem { return skipSchedule(label, problem) }
        if settings.scheduleSkipWhenBusy, receivingSomeone { return skipSchedule(label, L("正在收別人的畫面")) }
        guard let mode = Modes.find(settings.txModeID) else { return skipSchedule(label, L("app 裡沒有這個發射模式的模型")) }

        DebugTap.log("定時發射 \(label)：開始")
        AppModel.trace?("定時發射 \(label)：開始")
        scheduledRun = true
        scheduleLabel = label
        scheduleCutShort = false
        scheduleRestoreSource = nil
        if settings.sourceKind != .video {
            // 來源是鏡頭：這一次改用影片檔，結束後換回去（finishScheduledRun）
            scheduleRestoreSource = settings.sourceKind
            settings.sourceKind = .video
            sourceMessage = nil
            clearSourceFrame()
            camera.stop()
            // 影片目前位置的那一格。先前取過的話不會再給一次，那就是最近取到的那一格（見 VideoFileSource.lastFrame）
            if let buffer = video.copyCurrentFrame() ?? video.lastFrame { latestFrame.set(buffer, turns: video.quarterTurns) }
        }
        video.loop = false   // 不管有沒有勾循環播放，定時發射只播一次；結束時改回來
        scheduleLimit = video.duration + 2 * mode.frameSeconds + AppModel.scheduleMarginSeconds
        startTransmit(fromStart: true)
        // 不應該發生（能不能發射上面都檢查過了）：真的沒有開始的話，把狀態收乾淨
        if txPhase == .idle { finishScheduledRun(failure: notice ?? L("沒有開始"), sent: 0) }
    }

    /// 頻率上有人正在送這個 app 的訊號：接收機現在同步著，或 3 秒內還同步著
    /// （連續送的時候，兩張之間同步會斷一下；只看這一瞬間的話，剛好落在空檔就會蓋到對方）。
    private var receivingSomeone: Bool {
        let since = clock().timeIntervalSince(lastSignalTime)
        return synced || (since >= 0 && since < 3)
    }

    private func skipSchedule(_ label: String, _ reason: String) {
        scheduleLastEvent = L("%@ 沒有發射：%@", label, reason)
        notice = L("定時發射 %@ 沒有發射：%@", label, reason)
        DebugTap.log("定時發射 \(label) 沒有發射：\(reason)")
        AppModel.trace?("定時發射 \(label) 沒有發射：\(reason)")
    }

    /// 定時發射的那一次結束了（送完、被停止，或沒送成）：記下結果，把借用的東西還回去。
    private func finishScheduledRun(failure: String?, sent: Int? = nil) {
        scheduledRun = false
        scheduleLimit = nil
        video.loop = settings.loopVideo
        let sent = sent ?? txFramesSent
        if let failure {
            scheduleLastEvent = sent > 0 ? L("%@ 送出 %d 張後中止：%@", scheduleLabel, sent, failure) : L("%@ 沒有發射：%@", scheduleLabel, failure)
        } else {
            scheduleLastEvent = scheduleCutShort ? L("%@ 送出 %d 張（超過影片的長度，自動停止）", scheduleLabel, sent) : L("%@ 送出 %d 張", scheduleLabel, sent)
        }
        DebugTap.log("定時發射 " + (scheduleLastEvent ?? ""))
        AppModel.trace?("定時發射 " + (scheduleLastEvent ?? ""))
        if let previous = scheduleRestoreSource {
            scheduleRestoreSource = nil
            settings.sourceKind = previous
            restartSource()
        }
    }

    /// 定時發射到了時間也發射不了的原因；沒有問題時是 nil。
    var scheduleProblem: String? {
        if settings.callsign.isEmpty { return L("還沒設定呼號") }
        if videoName == nil { return L("還沒選擇影片檔") }
        return nil
    }

    /// 頂列「定時發射」鈕上的字。
    var scheduleSummary: String {
        if scheduledRun { return L("定時發射中") }
        guard scheduleArmed, let next = scheduleNextFire else { return L("定時發射") }
        if let problem = scheduleProblem { return L("定時發射：%@", problem) }
        if let seconds = scheduleCountdown { return L("%d 秒後發射", seconds) }
        return L("定時 %@", TransmitSchedule.clockText(next))
    }

    // MARK: - 顯示用的文字

    var txMode: ModeInfo? { Modes.find(txModeID) }
    var isOnAir: Bool { txPhase == .transmitting || txPhase == .stopping }

    static func clock(_ seconds: Double) -> String {
        let total = Int(max(seconds, 0))
        return String(format: "%02d:%02d", total / 60, total % 60)
    }

    static func timeText(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss"
        return formatter.string(from: date)
    }

    /// 音量條的長度：−50 dB 以下是空的，0 dB 是滿的。
    static func levelFraction(_ db: Double, floor: Double = -50) -> Double { min(max((db - floor) / -floor, 0), 1) }

    /// 發射來源是只播一次的影片檔（沒有勾循環播放，或這一次是定時發射）：按下發射才開始播，播完自動停止發射。
    var videoPlaysOnce: Bool { settings.sourceKind == .video && videoName != nil && (!settings.loopVideo || scheduledRun) }

    /// 「12.3 dB」「−1.4 dB」：標在收到的畫面上的訊雜比，取到小數一位。
    static func snrText(_ db: Double) -> String {
        let tenths = Int((abs(db) * 10).rounded())
        return (db < 0 && tenths > 0 ? "−" : "") + "\(tenths / 10).\(tenths % 10) dB"
    }

    /// 「−18 dB」：負號用數學的減號，與版面稿一致。
    static func dbText(_ db: Double) -> String {
        let value = Int(db.rounded())
        return (value < 0 ? "−\(-value)" : "\(value)") + " dB"
    }
}
