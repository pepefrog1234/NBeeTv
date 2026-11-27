// 語音與畫面共用 FM 語音通道（實驗性）：一邊送畫面，一邊送即時的語音。
// 通道的音訊頻帶切成兩段，語音在低的一段（360–1220 Hz），畫面放慢之後搬到高的一段（1335 Hz 起）。
// 語音的編解碼器是 FreeDV 的 RADE V2（核心之外的 voice 模組）；建置時沒有把它編進來的話，Voice.available 是 false，
// 介面上不會出現這個功能。合成與分離由核心做（nbtv.h 的 nbtv_duplex_*）。
import CNBTV
import CoreAudio
import Foundation

/// 版面：畫面放慢幾倍、佔到多高的頻率。
enum VoiceLayout: String, CaseIterable {
    case standard   // 畫面放慢 1.5 倍，佔到 2866 Hz
    case narrow     // 畫面放慢 1.8 倍，只佔到 2611 Hz：給音訊上緣比較窄的電台

    var code: Int32 { self == .narrow ? NBTV_DUPLEX_NARROW : NBTV_DUPLEX_STANDARD }
    /// 一幀在空中的時間是原來的幾倍
    var slowFactor: Double { nbtv_duplex_slow_factor(code) }
    /// 畫面佔到多高的頻率（Hz）
    var pictureHighHz: Double { nbtv_duplex_picture_high_hz(code) }
    /// 放慢最多的那一種是幾倍（輸出佇列照它配置）
    static var slowest: Double { allCases.map(\.slowFactor).max() ?? 1 }
}

enum Voice {
    /// 這份 app 有沒有把語音的編解碼器編進來
    static let available = nbtv_voice_available() != 0
    /// 編解碼器的名稱與版本，例如「FreeDV RADE V2 (rade_c 23c3746)」
    static var codecName: String { String(cString: nbtv_voice_description()) }
    /// 這個模式能不能和語音一起送：只有給 FM 用的模式（沒有導頻的，或大畫面的）
    static func supports(_ mode: ModeInfo?) -> Bool {
        guard let mode else { return false }
        return mode.pilotFree || mode.largePicture
    }
    /// 語音排進輸出佇列之後，墊到這麼多才開始送（語音是一段一段編出來的，每段 40 毫秒）
    static let startSeconds = 0.12
    /// 語音的訊號一個符號的長度（48 kHz 的取樣數；RADE V2 是 960 個，20 毫秒）。語音的佇列中途空掉之後，輸出要等到
    /// 停的長度是它的整數倍才接回去，對方的接收端才不必重新同步（見 AudioOutput.voiceGapStep）。
    static let symbolSamples = Int(nbtv_voice_symbol_samples()) * NBTV.sampleRate / Int(NBTV_VOICE_SIGNAL_RATE)
    /// 喇叭那一頭：解出來的語音墊到這麼多才開始播，積到上限就先不排新的（兩邊音效卡的快慢不會剛好一樣）
    static let speakerStartSeconds = 0.15
    static let speakerLimitSeconds = 0.8
}

/// 發射端：麥克風的語音（48 kHz）→「語音那一半」的音訊（48 kHz，已經搬到語音的頻段、照比例調好音量），
/// 播放時一個取樣一個取樣加到畫面那一半上（AudioOutput.enqueueVoice）。只能在同一條佇列上用。
final class VoiceSender {
    private let encoder: OpaquePointer
    private let mixer: OpaquePointer
    private var iq = [Float](repeating: 0, count: 2 * 2048)

    init?(layout: VoiceLayout) {
        guard let encoder = nbtv_voice_tx_create() else { return nil }
        guard let mixer = nbtv_duplex_tx_create(layout.code) else {
            nbtv_voice_tx_destroy(encoder)
            return nil
        }
        self.encoder = encoder
        self.mixer = mixer
    }

    deinit {
        nbtv_voice_tx_destroy(encoder)
        nbtv_duplex_tx_destroy(mixer)
    }

    /// 推入語音，回傳多出來的那一段音訊（還沒湊滿一段時是空的）。
    func process(_ speech: [Float]) -> [Float] {
        speech.withUnsafeBufferPointer { nbtv_voice_tx_push(encoder, $0.baseAddress, Int32($0.count)) }
        return drain()
    }

    /// 這一次發射的語音結束：送出剩下的，加上「結束」的標記（對方的接收端看到它就知道講完了），並把濾波器裡的尾巴擠出來。
    func finish() -> [Float] {
        nbtv_voice_tx_finish(encoder)
        var out = drain()
        let quiet = [Float](repeating: 0, count: 2 * 400)   // 50 毫秒的靜音
        nbtv_duplex_tx_push_voice(mixer, quiet, 400)
        out += take()
        return out
    }

    private func drain() -> [Float] {
        while true {
            let n = nbtv_voice_tx_read(encoder, &iq, 2048)
            guard n > 0 else { break }
            nbtv_duplex_tx_push_voice(mixer, iq, n)
        }
        return take()
    }

    private func take() -> [Float] {
        let pending = Int(nbtv_duplex_tx_pending_voice(mixer))
        guard pending > 0 else { return [] }
        var out = [Float](repeating: 0, count: pending)
        nbtv_duplex_tx_read(mixer, &out, Int32(pending))
        return out
    }
}

/// 接收端：電台收到的音訊（48 kHz）→ 分出語音那一段 → 解出語音（48 kHz）。只能在同一條佇列上用。
final class VoiceReceiver {
    private let splitter: OpaquePointer
    private let decoder: OpaquePointer
    private var iq = [Float](repeating: 0, count: 2 * 2048)
    private var speech = [Float](repeating: 0, count: 9600)

    init?(layout: VoiceLayout) {
        guard let decoder = nbtv_voice_rx_create() else { return nil }
        guard let splitter = nbtv_duplex_rx_create(layout.code) else {
            nbtv_voice_rx_destroy(decoder)
            return nil
        }
        nbtv_duplex_rx_enable(splitter, 0, 1)   // 畫面那一路由另一個接收機自己分
        self.splitter = splitter
        self.decoder = decoder
    }

    deinit {
        nbtv_duplex_rx_destroy(splitter)
        nbtv_voice_rx_destroy(decoder)
    }

    /// 回到剛建立的狀態（重新開始接收時）。
    func reset() {
        nbtv_duplex_rx_reset(splitter)
        nbtv_voice_rx_reset(decoder)
    }

    /// 推入收到的音訊，回傳解出來的語音（沒有同步到語音的訊號時是空的）。
    func process(_ samples: [Float]) -> [Float] {
        samples.withUnsafeBufferPointer { nbtv_duplex_rx_push(splitter, $0.baseAddress, Int32($0.count)) }
        while true {
            let n = nbtv_duplex_rx_read_voice(splitter, &iq, 2048)
            guard n > 0 else { break }
            nbtv_voice_rx_push(decoder, iq, n)
        }
        var out: [Float] = []
        while true {
            let n = Int(nbtv_voice_rx_read(decoder, &speech, Int32(speech.count)))
            guard n > 0 else { break }
            out.append(contentsOf: speech[0..<n])
        }
        return out
    }

    /// 目前是不是同步到語音的訊號、訊雜比的估計（dB），以及到目前為止收到幾次「結束」的標記。
    var status: (synced: Bool, snrDb: Double, oversEnded: Int) {
        var s = nbtv_voice_rx_status()
        nbtv_voice_rx_get_status(decoder, &s)
        return (s.synced != 0, Double(s.snr_db), Int(s.overs_ended))
    }
}

/// 發射時語音走的那一條路：麥克風 → 編碼 → 排進輸出的語音佇列。編碼在自己的佇列上做。
final class VoiceTransmitPath {
    private let microphone = AudioInput()
    private let queue = DispatchQueue(label: "nbtv.voice.tx", qos: .userInitiated)
    // 以下只在 queue 上讀寫
    private var sender: VoiceSender?
    private weak var output: AudioOutput?
    private var active = false
    private var level = -120.0
    private let levelLock = NSLock()

    init() {
        microphone.onSamples = { [weak self] samples in
            guard let self else { return }
            self.queue.async { self.feed(samples) }
        }
    }

    /// 最近一小段麥克風的音量（均方根，dBFS）；還沒有聲音時是 −120。
    var levelDb: Double {
        levelLock.lock()
        defer { levelLock.unlock() }
        return level
    }

    /// 開始。microphoneID 是 nil 時不開任何裝置（測試用：語音由 speak 給）。開不了麥克風時丟出錯誤，這條路不會啟動。
    func start(layout: VoiceLayout, microphoneID: AudioDeviceID?, output: AudioOutput) throws {
        var ready = false
        queue.sync {
            sender = VoiceSender(layout: layout)
            self.output = output
            active = sender != nil
            ready = active
            setLevel(-120)
        }
        guard ready else { throw AudioIOError(message: L("語音的編解碼器開不起來")) }
        guard let microphoneID else { return }
        do {
            try microphone.start(deviceID: microphoneID)
        } catch {
            queue.sync { active = false }
            throw error
        }
    }

    /// 測試用：當成麥克風收到了這一段語音（48 kHz）。
    func speak(_ samples: [Float]) {
        queue.async { self.feed(samples) }
    }

    /// 這一次發射快結束了：不再收麥克風，把剩下的語音與「結束」的標記排進去。
    func finish() {
        microphone.stop()
        queue.sync {
            guard active, let sender else { return }
            active = false
            let tail = sender.finish()
            if !tail.isEmpty { output?.enqueueVoice(tail) }
        }
    }

    /// 立刻停下來（發射結束或中止）。
    func stop() {
        microphone.stop()
        queue.sync {
            active = false
            sender = nil
            output = nil
            setLevel(-120)
        }
    }

    private func setLevel(_ value: Double) {
        levelLock.lock()
        level = value
        levelLock.unlock()
    }

    private func feed(_ samples: [Float]) {
        guard active, let sender, !samples.isEmpty else { return }
        var energy = 0.0
        for v in samples { energy += Double(v) * Double(v) }
        setLevel(10 * log10(max(energy / Double(samples.count), 1e-12)))
        let voice = sender.process(samples)
        if !voice.isEmpty { output?.enqueueVoice(voice) }
    }
}
