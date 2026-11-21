// 示範圖（--snapshot）與自我測試（--selftest）共用的工具：內附的測試圖、模擬電台收到的音訊、示範影片。
// 影像與瀑布圖都是核心真的算出來的，這裡只負責準備輸入。
import Accelerate
import AVFoundation
import CoreGraphics
import Foundation

enum Demo {
    /// bundle 裡的測試圖（96×72 的 PPM）：coffee、rocket、cat。
    static func testImage(_ name: String) -> Data? {
        guard let url = Bundle.main.url(forResource: name, withExtension: "ppm"), let data = try? Data(contentsOf: url) else { return nil }
        return parsePPM(data)
    }

    /// 二進位 PPM（P6、96×72、最大值 255）→ RGB。
    static func parsePPM(_ data: Data) -> Data? {
        var fields: [String] = []
        var index = 0
        var token = ""
        // 表頭有四個以空白分隔的欄位：P6、寬、高、最大值；之後隔一個空白字元就是像素
        while index < data.count, fields.count < 4 {
            let c = data[index]
            index += 1
            if c == UInt8(ascii: "#") {
                while index < data.count, data[index] != UInt8(ascii: "\n") { index += 1 }
            } else if c == 32 || c == 10 || c == 13 || c == 9 {
                if !token.isEmpty {
                    fields.append(token)
                    token = ""
                }
            } else {
                token.append(Character(UnicodeScalar(c)))
            }
        }
        guard fields == ["P6", String(NBTV.imageWidth), String(NBTV.imageHeight), "255"], data.count - index >= NBTV.rgbBytes else { return nil }
        return data.subdata(in: index..<(index + NBTV.rgbBytes))
    }

    /// 可重現的亂數（示範圖每次算出來都一樣）。
    struct Random {
        private var state: UInt64

        init(seed: UInt64) { state = seed }

        mutating func next() -> UInt64 {
            state &+= 0x9E37_79B9_7F4A_7C15
            var z = state
            z = (z ^ (z >> 30)) &* 0xBF58_476D_1CE4_E5B9
            z = (z ^ (z >> 27)) &* 0x94D0_49BB_1331_11EB
            return z ^ (z >> 31)
        }

        mutating func uniform() -> Double { Double(next() >> 11) / 9007199254740992.0 }

        mutating func gauss() -> Double {
            let u1 = max(uniform(), 1e-300), u2 = uniform()
            return (-2 * log(u1)).squareRoot() * cos(2 * .pi * u2)
        }
    }

    static func power(_ samples: [Float]) -> Double {
        guard !samples.isEmpty else { return 0 }
        var sum = 0.0
        for v in samples { sum += Double(v) * Double(v) }
        return sum / Double(samples.count)
    }

    static func silence(seconds: Double) -> [Float] { [Float](repeating: 0, count: Int(seconds * Double(NBTV.sampleRate))) }

    /// 模擬 SSB 電台收進來的音訊：加白雜訊、整個頻譜平移（電台沒對準頻率）、過電台的通帶（約 250–2750 Hz）。
    /// - signalPower: 訊號在發射期間的平均功率，用來換算雜訊；snrDb 是 3 kHz 雜訊頻寬的訊雜比。snrDb 為 nil 時不加雜訊。
    /// - noiseOnlyLevelDb: 沒有訊號時（signalPower 為 0）雜訊過通帶後的音量。
    /// - outputLevelDb: 不是 nil 時，把訊號期間的音量調到這個值（dBFS）。
    static func radioChannel(_ input: [Float], signalPower: Double, snrDb: Double?, shiftHz: Double, noiseOnlyLevelDb: Double = -32,
                             outputLevelDb: Double? = nil, seed: UInt64 = 2026) -> [Float] {
        let count = input.count
        let rate = Double(NBTV.sampleRate)
        var log2n = vDSP_Length(1)
        while (1 << log2n) < count { log2n += 1 }
        let size = 1 << log2n
        var real = [Float](repeating: 0, count: size)
        var imag = [Float](repeating: 0, count: size)
        real.replaceSubrange(0..<count, with: input)

        // 白雜訊：48 kHz 取樣下功率 σ² 分布在 0–24 kHz，3 kHz 內的雜訊功率是 σ²/8
        var rng = Random(seed: seed)
        var sigma = 0.0
        if signalPower > 0, let snrDb {
            sigma = (8 * signalPower / pow(10, snrDb / 10)).squareRoot()
        } else if signalPower <= 0 {
            // 只有雜訊：通帶寬約 2.5 kHz，佔 24 kHz 的 2.5/24
            sigma = pow(10, noiseOnlyLevelDb / 20) / (2.5 / 24.0).squareRoot()
        }
        if sigma > 0 {
            for i in 0..<count { real[i] += Float(sigma * rng.gauss()) }
        }

        guard let setup = vDSP_create_fftsetup(log2n, FFTRadix(kFFTRadix2)) else { return input }
        defer { vDSP_destroy_fftsetup(setup) }
        real.withUnsafeMutableBufferPointer { re in
            imag.withUnsafeMutableBufferPointer { im in
                var split = DSPSplitComplex(realp: re.baseAddress!, imagp: im.baseAddress!)
                vDSP_fft_zip(setup, &split, 1, log2n, FFTDirection(kFFTDirection_Forward))

                // 正頻率的每一格往上搬 shift 格（等於整個頻譜平移），再乘上通帶的形狀；負頻率取共軛對稱
                let binHz = rate / Double(size)
                let shift = Int((shiftHz / binHz).rounded())
                let half = size / 2
                var outRe = [Float](repeating: 0, count: half + 1)
                var outIm = [Float](repeating: 0, count: half + 1)
                for k in 1..<half {
                    let source = k - shift
                    guard source >= 1, source < half else { continue }
                    let hz = Double(k) * binHz
                    // 通帶 250–2750 Hz，兩邊各 100 Hz 的餘弦過渡帶；通帶外留一點點（−50 dB），像真的濾波器
                    var gain = 0.003
                    if hz >= 300, hz <= 2700 {
                        gain = 1
                    } else if hz > 200, hz < 300 {
                        gain = max(gain, 0.5 - 0.5 * cos(.pi * (hz - 200) / 100))
                    } else if hz > 2700, hz < 2800 {
                        gain = max(gain, 0.5 + 0.5 * cos(.pi * (hz - 2700) / 100))
                    }
                    outRe[k] = re[source] * Float(gain)
                    outIm[k] = im[source] * Float(gain)
                }
                for k in 0..<size {
                    if k == 0 || k == half {
                        re[k] = 0
                        im[k] = 0
                    } else if k < half {
                        re[k] = outRe[k]
                        im[k] = outIm[k]
                    } else {
                        re[k] = outRe[size - k]
                        im[k] = -outIm[size - k]
                    }
                }
                vDSP_fft_zip(setup, &split, 1, log2n, FFTDirection(kFFTDirection_Inverse))
            }
        }
        var gain = 1.0 / Double(size)
        if let outputLevelDb, signalPower > 0 {
            let total = signalPower + (snrDb.map { signalPower / pow(10, $0 / 10) * 2.5 / 3.0 } ?? 0)
            gain *= pow(10, outputLevelDb / 20) / total.squareRoot()
        }
        return (0..<count).map { real[$0] * Float(gain) }
    }

    /// 把一段音訊分塊餵給接收機與頻譜分析（像音訊裝置那樣一次一小段）。
    /// 回傳每一塊的更新（狀態、解出來的幀、頻譜列），順序與實際接收時相同。
    static func receive(_ audio: [Float], receiver: CoreReceiver = CoreReceiver(), spectrum: CoreSpectrum = CoreSpectrum()) -> [RxUpdate] {
        var updates: [RxUpdate] = []
        let chunk = NBTV.sampleRate / 50
        var position = 0
        while position < audio.count {
            let end = min(position + chunk, audio.count)
            let block = Array(audio[position..<end])
            receiver.push(block)
            spectrum.push(block)
            var update = RxUpdate()
            update.take(receiver.status())
            while let frame = receiver.pollFrame() { update.frames.append(frame) }
            while let row = spectrum.poll() { update.spectrumRows.append(row) }
            updates.append(update)
            position = end
        }
        return updates
    }

    /// 把一段送出去的音訊交給頻譜分析（發射監看），回傳頻譜列。
    static func monitorRows(_ audio: [Float]) -> [[Float]] {
        let spectrum = CoreSpectrum()
        var rows: [[Float]] = []
        let chunk = NBTV.sampleRate / 20
        var position = 0
        while position < audio.count {
            let end = min(position + chunk, audio.count)
            spectrum.push(Array(audio[position..<end]))
            while let row = spectrum.poll() { rows.append(row) }
            position = end
        }
        return rows
    }

    /// 用一張圖做一段示範影片（H.264、1920×1080）：畫面慢慢往上移，每一格都不一樣。
    /// transform：寫進影片軌的顯示方向（手機直拍的影片就是靠它記錄要轉 90 度）。
    static func makeVideo(at url: URL, from image: CGImage, seconds: Int = 38, fps: Int32 = 5, transform: CGAffineTransform? = nil) -> Bool {
        let width = 1920, height = 1080
        try? FileManager.default.removeItem(at: url)
        guard let writer = try? AVAssetWriter(outputURL: url, fileType: .mp4) else { return false }
        let input = AVAssetWriterInput(mediaType: .video, outputSettings: [
            AVVideoCodecKey: AVVideoCodecType.h264, AVVideoWidthKey: width, AVVideoHeightKey: height,
        ])
        input.expectsMediaDataInRealTime = false
        if let transform { input.transform = transform }
        let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
            kCVPixelBufferWidthKey as String: width, kCVPixelBufferHeightKey as String: height,
        ])
        guard writer.canAdd(input) else { return false }
        writer.add(input)
        guard writer.startWriting() else { return false }
        writer.startSession(atSourceTime: .zero)
        let total = seconds * Int(fps)
        guard let space = CGColorSpace(name: CGColorSpace.sRGB) else { return false }
        for index in 0..<total {
            var waited = 0
            while !input.isReadyForMoreMediaData, waited < 5000 {
                usleep(1000)
                waited += 1
            }
            guard let pool = adaptor.pixelBufferPool else { return false }
            var buffer: CVPixelBuffer?
            CVPixelBufferPoolCreatePixelBuffer(nil, pool, &buffer)
            guard let buffer else { return false }
            CVPixelBufferLockBaseAddress(buffer, [])
            if let context = CGContext(data: CVPixelBufferGetBaseAddress(buffer), width: width, height: height, bitsPerComponent: 8,
                                       bytesPerRow: CVPixelBufferGetBytesPerRow(buffer), space: space,
                                       bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue) {
                // 4:3 的圖放大到蓋滿 16:9，多出來的高度用來上下移動
                let scaledHeight = CGFloat(width) * CGFloat(image.height) / CGFloat(image.width)
                let travel = scaledHeight - CGFloat(height)
                let offset = -travel * CGFloat(index) / CGFloat(max(total - 1, 1))
                context.interpolationQuality = .none
                context.draw(image, in: CGRect(x: 0, y: offset, width: CGFloat(width), height: scaledHeight))
            }
            CVPixelBufferUnlockBaseAddress(buffer, [])
            if !adaptor.append(buffer, withPresentationTime: CMTime(value: CMTimeValue(index), timescale: fps)) { return false }
        }
        input.markAsFinished()
        let done = DispatchSemaphore(value: 0)
        writer.finishWriting { done.signal() }
        done.wait()
        return writer.status == .completed
    }

    /// 讓主執行緒的事件迴圈跑一下（等非同步的回呼、讓 SwiftUI 排版）。
    static func spin(_ seconds: Double) {
        RunLoop.main.run(until: Date().addingTimeInterval(seconds))
    }

    /// 邊跑事件迴圈邊等條件成立，最多等 timeout 秒。
    static func wait(timeout: Double, until condition: () -> Bool) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while !condition() {
            if Date() > deadline { return false }
            spin(0.02)
        }
        return true
    }
}
