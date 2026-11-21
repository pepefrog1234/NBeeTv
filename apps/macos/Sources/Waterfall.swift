// 瀑布圖：把核心算出來的頻譜列（dBFS，257 點）畫成一張點陣圖，最新的在最上面。
import CoreGraphics
import Foundation

final class WaterfallStore {
    /// 這一列的亮度要以什麼為基準。
    enum Reference {
        case noise      // 沒有同步：把目前的頻譜當成雜訊，慢慢更新雜訊基準
        case hold       // 已同步：基準固定，訊號比雜訊強多少就亮多少
        case transmit   // 發射監看：沒有雜訊可比，直接讓訊號落在偏亮的位置
    }

    static let width = NBTV.spectrumBins
    static let height = 512           // 保留的列數
    static let rowsPerLine = 2        // 兩列頻譜平均成圖上的一列：約每秒 12 列，176 pt 高的圖可看 15 秒
    static let linesPerSecond = Double(NBTV.sampleRate) / 4.0 / 512.0 / Double(rowsPerLine)

    private static let range: Float = 38          // 色階從最暗到最亮相當於幾 dB
    private static let noiseLevel: Float = 0.27   // 雜訊的中位數畫在色階的這個位置（暗的青色紋理）
    private static let signalLevel: Float = 0.72  // 發射監看時訊號畫在這個位置
    private static let maxSignalLevel: Float = 0.88  // 接收時訊號（第 80 百分位）最亮畫到這裡
    private static let silenceLevel: Float = -95   // 通帶的中位數低於這個 dBFS 就當作沒有輸入
    private static let referenceDelay = 16        // 雜訊基準只採用這麼多列之前的資料（約 1.4 秒）

    private var pixels = [UInt32](repeating: WaterfallStore.background, count: WaterfallStore.width * WaterfallStore.height)
    private var pending = [Float](repeating: 0, count: WaterfallStore.width)
    private var pendingCount = 0
    private var noiseReference: Float?   // 雜訊在通帶內的中位數（dB）
    private var recentMedians: [Float] = []
    private var transmitReference: Float?

    /// 色階：深藍黑 → 藍綠 → 青 → 淡黃
    private static let palette: [UInt32] = {
        let stops: [(Float, Float, Float, Float)] = [
            (0.00, 5, 7, 10), (0.15, 8, 38, 54), (0.35, 14, 84, 100), (0.55, 36, 150, 150),
            (0.75, 100, 214, 200), (0.90, 200, 236, 180), (1.00, 250, 244, 170),
        ]
        return (0..<256).map { index in
            let t = Float(index) / 255
            var a = stops[0], b = stops[stops.count - 1]
            for i in 0..<(stops.count - 1) where t >= stops[i].0 && t <= stops[i + 1].0 {
                a = stops[i]
                b = stops[i + 1]
                break
            }
            let f = b.0 > a.0 ? (t - a.0) / (b.0 - a.0) : 0
            let r = UInt32(a.1 + (b.1 - a.1) * f), g = UInt32(a.2 + (b.2 - a.2) * f), bl = UInt32(a.3 + (b.3 - a.3) * f)
            return 0xFF00_0000 | (bl << 16) | (g << 8) | r   // 記憶體裡的順序是 R、G、B、A
        }
    }()
    private static let background: UInt32 = palette[0]

    func clear() {
        pixels = [UInt32](repeating: WaterfallStore.background, count: WaterfallStore.width * WaterfallStore.height)
        pendingCount = 0
        noiseReference = nil
        recentMedians.removeAll()
        transmitReference = nil
    }

    /// 加入一列頻譜（dBFS）。湊滿 rowsPerLine 列後畫出一列，回傳 true 表示圖有更新。
    @discardableResult
    func append(_ row: [Float], reference: Reference) -> Bool {
        guard row.count == WaterfallStore.width else { return false }
        if pendingCount == 0 { pending = [Float](repeating: 0, count: WaterfallStore.width) }
        for i in 0..<row.count { pending[i] += pow(10, row[i] / 10) }
        pendingCount += 1
        guard pendingCount >= WaterfallStore.rowsPerLine else { return false }
        let line = pending.map { 10 * log10(max($0 / Float(pendingCount), 1e-14)) }
        pendingCount = 0
        draw(line, reference: reference)
        return true
    }

    /// 這一列以哪個 dB 值當作雜訊的位置。
    private func level(for line: [Float], reference: Reference) -> Float {
        // 只看電台通帶內（約 300–2700 Hz）的部分
        let first = Int(300 / NBTV.spectrumMaxHz * Double(WaterfallStore.width - 1))
        let last = Int(2700 / NBTV.spectrumMaxHz * Double(WaterfallStore.width - 1))
        let sorted = line[first...last].sorted()
        let median = sorted[sorted.count / 2]
        let p80 = sorted[sorted.count * 4 / 5]
        // 完全沒有輸入（數位靜音）：畫成全黑，也不拿來更新基準
        if median < WaterfallStore.silenceLevel { return median + WaterfallStore.noiseLevel * WaterfallStore.range }
        // 訊號比基準強太多時（例如沒有雜訊的虛擬音效卡）壓住上限，免得整片飽和
        let ceiling = p80 - (WaterfallStore.maxSignalLevel - WaterfallStore.noiseLevel) * WaterfallStore.range
        switch reference {
        case .noise:
            transmitReference = nil
            // 接收機要花一秒左右才會宣告同步；這段時間的頻譜其實已經是訊號。
            // 所以雜訊基準只採用夠舊的資料，一旦同步就把還沒採用的丟掉（見 .hold）。
            recentMedians.append(median)
            if recentMedians.count > WaterfallStore.referenceDelay {
                let old = recentMedians.removeFirst()
                noiseReference = noiseReference.map { $0 + 0.15 * (old - $0) } ?? old
            }
            return max(noiseReference ?? (recentMedians.min() ?? median), ceiling)
        case .hold:
            transmitReference = nil
            recentMedians.removeAll()
            // 還沒看過雜訊就同步了（一開機就有訊號）：假設訊號比雜訊強 15 dB
            if noiseReference == nil { noiseReference = median - 15 }
            return max(noiseReference ?? median, ceiling)
        case .transmit:
            recentMedians.removeAll()
            let target = p80 - (WaterfallStore.signalLevel - WaterfallStore.noiseLevel) * WaterfallStore.range
            let smoothed = transmitReference.map { $0 + 0.2 * (target - $0) } ?? target
            transmitReference = smoothed
            return smoothed
        }
    }

    private func draw(_ line: [Float], reference: Reference) {
        let base = level(for: line, reference: reference)
        let width = WaterfallStore.width, height = WaterfallStore.height
        // 往下捲一列，新的一列畫在最上面
        pixels.withUnsafeMutableBufferPointer { buffer in
            let start = buffer.baseAddress!
            memmove(start + width, start, (height - 1) * width * MemoryLayout<UInt32>.size)
            for x in 0..<width {
                let value = WaterfallStore.noiseLevel + (line[x] - base) / WaterfallStore.range
                start[x] = WaterfallStore.palette[Int(min(max(value, 0), 1) * 255)]
            }
        }
    }

    /// 目前的瀑布圖（寬 257、高 512 像素，最新的在第 0 列）。
    func makeImage() -> CGImage? {
        let width = WaterfallStore.width, height = WaterfallStore.height
        let data = pixels.withUnsafeBufferPointer { Data(buffer: $0) }
        guard let provider = CGDataProvider(data: data as CFData), let space = CGColorSpace(name: CGColorSpace.sRGB) else { return nil }
        return CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: width * 4, space: space,
                       bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue), provider: provider, decode: nil,
                       shouldInterpolate: true, intent: .defaultIntent)
    }
}
