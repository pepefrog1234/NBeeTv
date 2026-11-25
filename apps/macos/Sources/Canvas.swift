// 畫布：疊在送出畫面上的一層字（例如「CQ BX4ACP」），發射前印在每一張畫面上。
// 字帶的樣子只跟文字與位置有關，所以先在 96×72 的透明底上算好一張圖層，之後每一張畫面疊上去就好。
import CoreGraphics
import CoreText
import Foundation

enum CanvasPosition: String {
    case top, bottom
}

/// 畫布的內容：印什麼字、印在畫面的上緣還是下緣。
struct CanvasStyle: Equatable {
    var text: String
    var position: CanvasPosition
}

/// 算好的圖層。建立之後不會再改，可以在任何佇列上使用。
final class CanvasLayer {
    let style: CanvasStyle
    /// 實際排出來的幾行字（太長時拆成兩行；再放不下的那一行結尾是「…」）
    let lines: [String]
    /// 字的大小（點；畫面上一點就是一個像素）
    let fontSize: CGFloat
    /// 字帶佔畫面的哪幾列（從上往下數）
    let rows: Range<Int>
    private let pixels: [UInt8]   // 96×72 的 RGBA，顏色已經乘過透明度；字帶以外是全透明

    /// 一張畫面只有 96×72，還要經過很省頻寬的編碼，字太小對方看不出來：盡量大，放不下才縮，再放不下就拆成兩行。
    static let maximumFontSize: CGFloat = 18
    static let minimumFontSize: CGFloat = 9
    /// 一行放得下、但字得縮到比這個還小的時候，改排成兩行
    static let wrapBelowFontSize: CGFloat = 12
    /// 排成兩行時字最大到這裡：再大的話，字帶會蓋掉快一半的畫面（兩行 12 的字帶約 26 列，是畫面的三分之一多一點）
    static let wrappedMaximumFontSize: CGFloat = 12
    static let sideMargin: CGFloat = 3
    private static let padding: CGFloat = 3       // 字帶上下的留白
    private static let lineGap: CGFloat = 2
    private static let bandOpacity: CGFloat = 0.62
    static let maximumCharacters = 40

    /// 沒有字可以印時回傳 nil。
    init?(style: CanvasStyle) {
        let text = String(style.text.trimmingCharacters(in: .whitespacesAndNewlines).prefix(CanvasLayer.maximumCharacters))
        guard !text.isEmpty else { return nil }
        let width = NBTV.imageWidth, height = NBTV.imageHeight
        let available = CGFloat(width) - 2 * CanvasLayer.sideMargin

        // 1. 決定排成幾行、字多大
        var chosen = [text]
        var size = CanvasLayer.fit([text], width: available)
        if size ?? 0 < CanvasLayer.wrapBelowFontSize, let split = CanvasLayer.splitInTwo(text) {
            let twoLineSize = CanvasLayer.fit(split, width: available, maximum: CanvasLayer.wrappedMaximumFontSize)
            if let twoLineSize, twoLineSize > (size ?? 0) {
                chosen = split
                size = twoLineSize
            }
        }
        let fontSize = size ?? CanvasLayer.minimumFontSize
        let font = CanvasLayer.font(fontSize)
        // 用最小的字還是放不下：截短，結尾補「…」
        var typeset = chosen.map { CanvasLayer.line($0, font: font) }
        if size == nil {
            let ellipsis = CanvasLayer.line("…", font: font)
            typeset = typeset.map { CTLineCreateTruncatedLine($0, Double(available), .end, ellipsis) ?? $0 }
        }

        // 2. 每一行實際有墨的範圍（相對於基線）：字帶只留這麼高，不浪費畫面
        let inks = typeset.map { CTLineGetBoundsWithOptions($0, .useGlyphPathBounds) }
        let inkHeight = inks.reduce(0) { $0 + ceil($1.height) } + CGFloat(inks.count - 1) * CanvasLayer.lineGap
        let band = min(CGFloat(height), ceil(inkHeight + 2 * CanvasLayer.padding))

        // 3. 畫在透明的底上。CoreGraphics 的 y 軸朝上；記憶體裡的第一列是畫面的最上面那一列。
        var pixels = [UInt8](repeating: 0, count: width * height * 4)
        let drawn: Bool = pixels.withUnsafeMutableBytes { raw in
            guard let space = CGColorSpace(name: CGColorSpace.sRGB),
                  let context = CGContext(data: raw.baseAddress, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4,
                                          space: space, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return false }
            let bandBottom = style.position == .top ? CGFloat(height) - band : 0
            context.setFillColor(CGColor(red: 0, green: 0, blue: 0, alpha: CanvasLayer.bandOpacity))
            context.fill(CGRect(x: 0, y: bandBottom, width: CGFloat(width), height: band))
            context.setShouldAntialias(true)
            context.setShouldSmoothFonts(false)   // 不要螢幕用的次像素彩邊
            context.setFillColor(CGColor(red: 1, green: 1, blue: 1, alpha: 1))
            var top = bandBottom + band - (band - inkHeight) / 2   // 第一行有墨範圍的上緣
            for (line, ink) in zip(typeset, inks) {
                // 基線放在整數的像素上，橫的筆畫才不會糊成兩列
                let baseline = (top - ink.maxY).rounded()
                let x = ((CGFloat(width) - ink.width) / 2 - ink.minX).rounded()
                context.textPosition = CGPoint(x: x, y: baseline)
                CTLineDraw(line, context)
                top -= ceil(ink.height) + CanvasLayer.lineGap
            }
            return true
        }
        guard drawn else { return nil }
        self.style = style
        self.lines = chosen
        self.fontSize = fontSize
        self.pixels = pixels
        let bandRows = Int(band)
        rows = style.position == .top ? 0..<bandRows : (height - bandRows)..<height
    }

    /// 疊到一張 96×72 的畫面上（RGB，每個像素三個位元組），回傳疊好的畫面。
    func apply(to rgb: Data) -> Data {
        guard rgb.count == NBTV.rgbBytes else { return rgb }
        var out = rgb
        let width = NBTV.imageWidth
        out.withUnsafeMutableBytes { raw in
            let dst = raw.bindMemory(to: UInt8.self)
            for index in (rows.lowerBound * width)..<(rows.upperBound * width) {
                let alpha = Int(pixels[4 * index + 3])
                if alpha == 0 { continue }
                for channel in 0..<3 {
                    let under = (Int(dst[3 * index + channel]) * (255 - alpha) + 127) / 255
                    dst[3 * index + channel] = UInt8(min(255, under + Int(pixels[4 * index + channel])))
                }
            }
        }
        return out
    }

    // MARK: 排字

    private static func font(_ size: CGFloat) -> CTFont {
        CTFontCreateUIFontForLanguage(.emphasizedSystem, size, nil) ?? CTFontCreateWithName("Helvetica-Bold" as CFString, size, nil)
    }

    private static func line(_ text: String, font: CTFont) -> CTLine {
        let attributes: [CFString: Any] = [kCTFontAttributeName: font, kCTForegroundColorFromContextAttributeName: true]
        return CTLineCreateWithAttributedString(CFAttributedStringCreate(nil, text as CFString, attributes as CFDictionary))
    }

    /// 每一行都放得進 width 的最大字級（從 maximum 起每次縮半點）；用最小的字還放不下時回傳 nil。
    private static func fit(_ lines: [String], width: CGFloat, maximum: CGFloat = maximumFontSize) -> CGFloat? {
        var size = maximum
        while size >= minimumFontSize {
            let font = font(size)
            let widest = lines.map { CTLineGetBoundsWithOptions(line($0, font: font), .useGlyphPathBounds).width }.max() ?? 0
            if widest <= width { return size }
            size -= 0.5
        }
        return nil
    }

    /// 在某個空白處拆成兩行，讓比較長的那一行愈短愈好。沒有空白可以拆時回傳 nil。
    private static func splitInTwo(_ text: String) -> [String]? {
        let words = text.split(separator: " ").map(String.init)
        guard words.count >= 2 else { return nil }
        var best: [String]?
        var bestLength = Int.max
        for cut in 1..<words.count {
            let first = words[..<cut].joined(separator: " "), second = words[cut...].joined(separator: " ")
            let longer = max(first.count, second.count)
            if longer < bestLength {
                bestLength = longer
                best = [first, second]
            }
        }
        return best
    }
}
