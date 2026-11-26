// 把一段連續收到的畫面串成影片檔：.mp4、H.264、每秒 24 格。
// 「一段」怎麼算在 AppModel.sequence(containing:in:)；這裡只管把畫面照時間寫成影片，
// 以及（使用者勾了的話）在每一張的左上角印上一小塊字——那一張的訊雜比。
import AppKit
import AVFoundation
import CoreGraphics
import CoreText
import CoreVideo
import Foundation

enum SequenceVideo {
    static let frameRate: Int32 = 24
    /// 96×72 的畫面放大幾倍存：10 倍是 960×720（192×144 的畫面同樣存成 960×720，放大 5 倍）。
    /// 用最近鄰放大，和主畫面上看到的一樣是一格一格的，壓縮之後邊緣也不會糊掉。
    static let scale = 10
    static var width: Int { NBTV.imageWidth * scale }
    static var height: Int { NBTV.imageHeight * scale }

    /// 影片裡的一張畫面：從影片的第幾秒開始顯示，顯示到下一張開始為止。
    /// label 不是 nil 的話，印在這一張的左上角（「SNR 12.3 dB」）。
    struct Picture {
        let image: CGImage
        let start: Double
        var label: String? = nil
    }

    struct Summary {
        let pictures: Int
        let videoFrames: Int
        var seconds: Double { Double(videoFrames) / Double(SequenceVideo.frameRate) }
    }

    struct ExportError: LocalizedError {
        let message: String
        var errorDescription: String? { message }
    }

    /// 每一張畫面從影片的第幾格開始，以及影片總共幾格。照開始的時間換成格數；每一張至少佔一格
    /// （兩張幾乎同時收到時，後面那張往後挪一格）。最後一張停留 lastHold 秒。
    static func layout(starts: [Double], lastHold: Double) -> (first: [Int], total: Int) {
        var first: [Int] = []
        for start in starts {
            let frame = Int((max(start, 0) * Double(frameRate)).rounded())
            first.append(max(frame, (first.last ?? -1) + 1))
        }
        let hold = max(1, Int((lastHold * Double(frameRate)).rounded()))
        return (first, (first.last ?? 0) + hold)
    }

    /// 寫成影片檔（已經有同名的檔案會被換掉）。要花一點時間，請在背景佇列上呼叫。
    @discardableResult
    static func write(_ pictures: [Picture], lastHold: Double, to url: URL) throws -> Summary {
        guard !pictures.isEmpty else { throw ExportError(message: L("沒有畫面可以存")) }
        let plan = layout(starts: pictures.map(\.start), lastHold: lastHold)
        try? FileManager.default.removeItem(at: url)
        let writer: AVAssetWriter
        do {
            writer = try AVAssetWriter(outputURL: url, fileType: .mp4)
        } catch {
            throw ExportError(message: L("無法建立影片檔：%@", error.localizedDescription))
        }
        writer.shouldOptimizeForNetworkUse = true   // 索引放在檔案開頭，傳到網路上可以邊下載邊播
        let input = AVAssetWriterInput(mediaType: .video, outputSettings: [
            AVVideoCodecKey: AVVideoCodecType.h264, AVVideoWidthKey: width, AVVideoHeightKey: height,
            AVVideoColorPropertiesKey: [
                AVVideoColorPrimariesKey: AVVideoColorPrimaries_ITU_R_709_2,
                AVVideoTransferFunctionKey: AVVideoTransferFunction_ITU_R_709_2,
                AVVideoYCbCrMatrixKey: AVVideoYCbCrMatrix_ITU_R_709_2,
            ],
            AVVideoCompressionPropertiesKey: [
                AVVideoAverageBitRateKey: 2_000_000,
                AVVideoExpectedSourceFrameRateKey: Int(frameRate),
                AVVideoMaxKeyFrameIntervalKey: Int(frameRate) * 2,
                AVVideoProfileLevelKey: AVVideoProfileLevelH264HighAutoLevel,
            ] as [String: Any],
        ])
        input.expectsMediaDataInRealTime = false
        let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
            kCVPixelBufferWidthKey as String: width, kCVPixelBufferHeightKey as String: height,
        ])
        guard writer.canAdd(input) else { throw ExportError(message: L("這台電腦不能編 H.264 的影片")) }
        writer.add(input)
        guard writer.startWriting() else { throw failure(writer, L("無法開始寫入影片檔")) }
        writer.startSession(atSourceTime: .zero)

        var frame = 0
        for (index, picture) in pictures.enumerated() {
            guard let buffer = pixelBuffer(for: picture, pool: adaptor.pixelBufferPool) else {
                writer.cancelWriting()
                throw ExportError(message: L("無法準備影片的畫面"))
            }
            // 這一張顯示到下一張開始為止：同一格畫面重複寫進去，影片的格率才是固定的每秒 24 格
            let end = index + 1 < pictures.count ? plan.first[index + 1] : plan.total
            while frame < end {
                var waited = 0
                while !input.isReadyForMoreMediaData, waited < 10_000 {
                    usleep(1000)
                    waited += 1
                }
                guard input.isReadyForMoreMediaData,
                      adaptor.append(buffer, withPresentationTime: CMTime(value: CMTimeValue(frame), timescale: frameRate)) else {
                    let error = failure(writer, L("寫入影片檔時失敗"))
                    writer.cancelWriting()
                    throw error
                }
                frame += 1
            }
        }
        input.markAsFinished()
        writer.endSession(atSourceTime: CMTime(value: CMTimeValue(plan.total), timescale: frameRate))
        let done = DispatchSemaphore(value: 0)
        writer.finishWriting { done.signal() }
        done.wait()
        guard writer.status == .completed else { throw failure(writer, L("影片檔沒有寫完")) }
        return Summary(pictures: pictures.count, videoFrames: plan.total)
    }

    private static func failure(_ writer: AVAssetWriter, _ what: String) -> ExportError {
        ExportError(message: writer.error.map { L("%@：%@", what, $0.localizedDescription) } ?? what)
    }

    /// 把一張畫面（96×72 或 192×144）畫成影片的一格，畫進影片用的緩衝。
    private static func pixelBuffer(for picture: Picture, pool: CVPixelBufferPool?) -> CVPixelBuffer? {
        guard let pool, let space = CGColorSpace(name: CGColorSpace.sRGB) else { return nil }
        var created: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, pool, &created)
        guard let buffer = created else { return nil }
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        guard let context = CGContext(data: CVPixelBufferGetBaseAddress(buffer), width: width, height: height, bitsPerComponent: 8,
                                      bytesPerRow: CVPixelBufferGetBytesPerRow(buffer), space: space,
                                      bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue) else {
            return nil
        }
        draw(picture, in: context)
        return buffer
    }

    /// 影片的一格：96×72 的畫面放大十倍、192×144 的放大五倍（最近鄰，不內插），有 label 的話印在左上角。
    /// context 的大小是 width×height，y 軸朝上。
    private static func draw(_ picture: Picture, in context: CGContext) {
        context.interpolationQuality = .none
        context.draw(picture.image, in: CGRect(x: 0, y: 0, width: width, height: height))
        if let label = picture.label, !label.isEmpty { stamp(label, in: context) }
    }

    /// 影片裡那一格的樣子（960×720）：和寫進影片檔的是同一份內容。給測試與示範圖用。
    static func frameImage(_ picture: Picture) -> CGImage? {
        guard let space = CGColorSpace(name: CGColorSpace.sRGB),
              let context = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: 0, space: space,
                                      bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue) else {
            return nil
        }
        draw(picture, in: context)
        return context.makeImage()
    }

    // MARK: 印在左上角的字

    // 樣子照主畫面上疊的訊雜比標示（SNRBadge）：半透明的黑底圓角方塊、白色的等寬字。大小照影片的像素另外定：
    // 影片常常被縮小了看（傳到手機上、貼在聊天室裡），字要比主畫面上的大一點才認得出來；
    // 整塊約 230×50，只蓋住 960×720 的畫面左上角一小塊。
    private static let stampFont = NSFont.monospacedSystemFont(ofSize: 30, weight: .semibold) as CTFont
    private static let stampMargin: CGFloat = 14
    private static let stampPadding = CGSize(width: 12, height: 7)
    private static let stampOpacity: CGFloat = 0.62

    private static func stampLine(_ text: String) -> CTLine {
        let attributes: [CFString: Any] = [kCTFontAttributeName: stampFont, kCTForegroundColorFromContextAttributeName: true]
        return CTLineCreateWithAttributedString(CFAttributedStringCreate(nil, text as CFString, attributes as CFDictionary))
    }

    /// 那一塊字在影片畫面裡佔的位置：原點在畫面的左上角、y 往下，單位是影片的像素。（測試用它知道要看哪裡。）
    static func stampRect(for text: String) -> CGRect {
        var ascent: CGFloat = 0, descent: CGFloat = 0, leading: CGFloat = 0
        let textWidth = CGFloat(CTLineGetTypographicBounds(stampLine(text), &ascent, &descent, &leading))
        return CGRect(x: stampMargin, y: stampMargin, width: ceil(textWidth) + 2 * stampPadding.width,
                      height: ceil(ascent + descent) + 2 * stampPadding.height)
    }

    private static func stamp(_ text: String, in context: CGContext) {
        let line = stampLine(text)
        var ascent: CGFloat = 0, descent: CGFloat = 0, leading: CGFloat = 0
        _ = CTLineGetTypographicBounds(line, &ascent, &descent, &leading)
        let box = stampRect(for: text)
        let rect = CGRect(x: box.minX, y: CGFloat(height) - box.maxY, width: box.width, height: box.height)   // 換成 y 軸朝上
        context.saveGState()
        context.setFillColor(CGColor(red: 0, green: 0, blue: 0, alpha: stampOpacity))
        context.addPath(CGPath(roundedRect: rect, cornerWidth: 8, cornerHeight: 8, transform: nil))
        context.fillPath()
        context.setShouldAntialias(true)
        context.setShouldSmoothFonts(false)   // 不要螢幕用的次像素彩邊
        context.setFillColor(CGColor(red: 1, green: 1, blue: 1, alpha: 1))
        // 基線放在整數的像素上，橫的筆畫才不會糊成兩列
        context.textPosition = CGPoint(x: rect.minX + stampPadding.width, y: (rect.minY + stampPadding.height + descent).rounded())
        CTLineDraw(line, context)
        context.restoreGState()
    }
}
