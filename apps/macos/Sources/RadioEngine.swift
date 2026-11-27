// 音訊裝置與核心之間的橋樑。核心的接收機與頻譜分析都只在 coreQueue 這條序列佇列上使用，
// 不在即時音訊執行緒、也不在主執行緒上做解調（一幀結束時 nbtv_rx_push 可能花幾十毫秒）。
import CNBTV
import CoreAudio
import Foundation

/// 接收端的一次更新，在主佇列上交給介面。
struct RxUpdate {
    /// 第幾次開始接收的結果。重新開始接收（換裝置、發射結束）之後，先前那一次晚到的更新要丟掉。
    var generation = 0
    var synced = false
    var modeID = -1
    var snrValid = false
    var snrDb = 0.0
    var offsetHz = 0.0
    var symbolsDone = 0
    var symbolsTotal = 0
    var inputLevelDb = -120.0
    var frames: [DecodedFrame] = []
    var spectrumRows: [[Float]] = []
    /// 「同時送語音」開著時：語音那一路目前有沒有同步、訊雜比的估計。沒開著時是 nil
    var voiceSynced: Bool?
    var voiceSnrDb = 0.0

    /// 把核心的狀態抄進來。
    mutating func take(_ status: nbtv_rx_status) {
        synced = status.synced != 0
        modeID = Int(status.mode_id)
        snrValid = status.snr_valid != 0
        snrDb = Double(status.snr_db)
        offsetHz = Double(status.freq_offset_hz)
        symbolsDone = Int(status.frame_symbols_done)
        symbolsTotal = Int(status.frame_symbols_total)
        inputLevelDb = Double(status.input_level_db)
    }
}

final class RadioEngine {
    /// 接收的更新（主佇列）。
    var onRxUpdate: ((RxUpdate) -> Void)?
    /// 發射監看的頻譜列（主佇列）。
    var onMonitorRows: (([[Float]]) -> Void)?

    /// 輸出佇列要放得下開頭的靜音、一整張，再加上提早排進去的下一張；照最長的模式算，至少 14 秒。
    /// （和語音一起送的時候畫面是放慢的，每一張更長。）
    let output = AudioOutput(queueSeconds: max(14, 2 * (Modes.all.map(\.frameSeconds).max() ?? 6) * (Voice.available ? VoiceLayout.slowest : 1) + 2))
    /// 「同時送語音」開著時，解出來的語音從這裡播。語音是一小段一小段解出來的，先墊一點才開始播。
    let speaker: AudioOutput = {
        let speaker = AudioOutput(queueSeconds: 2)
        speaker.startLevel = Int(Voice.speakerStartSeconds * Double(NBTV.sampleRate))
        return speaker
    }()
    private let input = AudioInput()
    private let coreQueue = DispatchQueue(label: "nbtv.core", qos: .userInitiated)
    private let receiver = RadioEngine.makeReceiver()
    private let spectrum = CoreSpectrum()
    /// 開始接收了幾次（主執行緒上讀寫）。
    private(set) var generation = 0
    // 以下只在 coreQueue 上讀寫
    private var receiving = false
    private var activeGeneration = 0
    private var pending = RxUpdate()          // 還沒交給介面的更新
    private var lastDelivery: UInt64 = 0      // 上次交給介面的時間（DispatchTime 的奈秒）
    // 「同時送語音」開著時（voiceLayout 不是 nil）：另一個接收機收合成訊號裡的畫面，語音那一路解出來排到喇叭
    private lazy var duplexReceiver = RadioEngine.makeReceiver()
    private var voiceReceiver: VoiceReceiver?
    private var voiceReceiverLayout: VoiceLayout?   // voiceReceiver 是照哪一種版面建立的
    private var voiceLayout: VoiceLayout?
    private var rxTap: DebugTap.Stream?       // 除錯用：交給接收機的取樣原樣記到檔案（見 DebugTap）
    private var tapSynced = false             // 上一次記到紀錄檔的同步狀態
    /// 音訊每 20 毫秒進來一段，但介面不需要更新得那麼勤（整個視窗重排一次並不便宜）：
    /// 攢到這麼久才交一次；解出新的一幀時立刻交。
    private static let deliveryInterval: UInt64 = 80_000_000

    init() {
        input.onSamples = { [weak self] samples in
            guard let self else { return }
            self.coreQueue.async { self.process(samples) }
        }
    }

    var isReceiving: Bool { input.isRunning }

    /// app 接收用的接收機：一律把最近幾幀合起來解（畫面沒變的地方合併前面幾張，訊號弱時比較清楚也比較穩定；
    /// 對方不必配合，畫面有變的地方不受影響）。這原本是設定裡的一個選項，後來固定開著。
    static func makeReceiver() -> CoreReceiver {
        let receiver = CoreReceiver()
        receiver.setMerging(true)
        return receiver
    }

    /// 從指定的輸入裝置開始接收；接收機會先清成初始狀態。
    /// voice：「同時送語音」開著時是它的版面——除了平常的訊號，也收和語音一起送的那一種，語音排到喇叭（startSpeaker）。
    /// deviceID 是 nil 時不開任何裝置（測試用：收到的音訊由 debugReceive 給）。
    func startReceiving(deviceID: AudioDeviceID?, voice: VoiceLayout? = nil) throws {
        generation += 1
        let current = generation
        coreQueue.sync {
            receiver.reset()
            voiceLayout = voice
            if let voice {
                duplexReceiver.setDuplex(voice)   // 順便清成初始狀態
                if voiceReceiverLayout == voice, let voiceReceiver {
                    voiceReceiver.reset()
                } else {
                    voiceReceiver = VoiceReceiver(layout: voice)
                    voiceReceiverLayout = voice
                }
            }
            spectrum.reset()
            pending = RxUpdate()
            activeGeneration = current
            receiving = true
            rxTap?.close()
            rxTap = DebugTap.Stream(kind: "rx")
            tapSynced = false
        }
        guard let deviceID else { return }
        do {
            try input.start(deviceID: deviceID)
        } catch {
            coreQueue.sync { receiving = false }
            throw error
        }
    }

    /// 解出來的語音從這個裝置播。開不了時丟出錯誤：接收照常，只是聽不到語音。
    func startSpeaker(deviceID: AudioDeviceID) throws { try speaker.start(deviceID: deviceID) }

    /// 測試用：當成輸入裝置收到了這一段音訊（處理完才回來）。
    func debugReceive(_ samples: [Float]) {
        coreQueue.sync { process(samples) }
    }

    func stopReceiving() {
        input.stop()
        speaker.stop()
        coreQueue.sync {
            receiving = false
            rxTap?.close()
            rxTap = nil
        }
    }

    /// 發射時把送出的音訊交給頻譜分析，瀑布圖顯示送出的訊號。
    func monitor(_ samples: [Float]) {
        coreQueue.async { [self] in
            spectrum.push(samples)
            var rows: [[Float]] = []
            while let row = spectrum.poll() { rows.append(row) }
            if !rows.isEmpty { DispatchQueue.main.async { self.onMonitorRows?(rows) } }
        }
    }

    func resetSpectrum() {
        coreQueue.sync { spectrum.reset() }
    }

    /// 「同時送語音」開著時：同一段音訊也交給收合成訊號的那個接收機，語音那一路解出來排到喇叭。
    private func processVoice(_ samples: [Float]) {
        duplexReceiver.push(samples)
        let status = duplexReceiver.status()
        if status.synced != 0 {
            // 對方是和語音一起送的：同步、模式、進度看這個接收機的；輸入音量還是看整段音訊的
            let level = pending.inputLevelDb
            pending.take(status)
            pending.inputLevelDb = level
        }
        while let frame = duplexReceiver.pollFrame() {
            DebugTap.log("接收（和語音一起送的）：收到一幀，模式 \(frame.modeID)，\(frame.decoded ? "解碼成功" : "沒有畫面")，序號 \(frame.sequence)")
            pending.frames.append(frame)
        }
        guard let voiceReceiver else { return }
        let speech = voiceReceiver.process(samples)
        // 喇叭那邊積太多（兩邊音效卡的快慢不同）就先不排，聲音才不會愈來愈晚
        if !speech.isEmpty, speaker.isRunning, speaker.queuedSamples < Int(Voice.speakerLimitSeconds * Double(NBTV.sampleRate)) {
            speaker.enqueue(speech)
        }
        let voice = voiceReceiver.status
        pending.voiceSynced = voice.synced
        pending.voiceSnrDb = voice.snrDb
    }

    private func process(_ samples: [Float]) {
        guard receiving else { return }
        rxTap?.write(samples)
        receiver.push(samples)
        spectrum.push(samples)
        pending.take(receiver.status())
        while let frame = receiver.pollFrame() {
            DebugTap.log("接收：收到一幀，模式 \(frame.modeID)，\(frame.decoded ? "解碼成功" : (frame.modelMissing ? "模型不符，沒有畫面" : "沒解出來"))，標頭\(frame.headerOK ? "正確" : "錯誤")，序號 \(frame.sequence)")
            pending.frames.append(frame)
        }
        if voiceLayout != nil { processVoice(samples) }
        if pending.synced != tapSynced {
            tapSynced = pending.synced
            DebugTap.log(tapSynced ? String(format: "接收：同步到模式 %d，頻偏 %+.1f Hz，輸入 %.1f dB", pending.modeID, pending.offsetHz, pending.inputLevelDb)
                                   : String(format: "接收：失去同步，輸入 %.1f dB", pending.inputLevelDb))
        }
        while let row = spectrum.poll() { pending.spectrumRows.append(row) }
        let now = DispatchTime.now().uptimeNanoseconds
        guard !pending.frames.isEmpty || now - lastDelivery >= RadioEngine.deliveryInterval else { return }
        lastDelivery = now
        var update = pending
        update.generation = activeGeneration
        pending = RxUpdate()
        DispatchQueue.main.async { self.onRxUpdate?(update) }
    }
}
