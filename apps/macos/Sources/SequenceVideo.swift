// 把一段連續收到的畫面串成影片檔：.mp4、H.264、每秒 24 格。
// 「一段」怎麼算在 AppModel.sequence(containing:in:)；這裡只管把畫面照時間寫成影片。
import AVFoundation
import CoreGraphics
import CoreVideo
import Foundation

enum SequenceVideo {
    static let frameRate: Int32 = 24
    /// 96×72 的畫面放大幾倍存：10 倍是 960×720。用最近鄰放大，和主畫面上看到的一樣是一格一格的，壓縮之後邊緣也不會糊掉。
    static let scale = 10
    static var width: Int { NBTV.imageWidth * scale }
    static var height: Int { NBTV.imageHeight * scale }

    /// 影片裡的一張畫面：從影片的第幾秒開始顯示，顯示到下一張開始為止。
    struct Picture {
        let image: CGImage
        let start: Double
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
            guard let buffer = pixelBuffer(for: picture.image, pool: adaptor.pixelBufferPool) else {
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

    /// 把一張 96×72 的畫面放大畫進影片用的緩衝（最近鄰，不內插）。
    private static func pixelBuffer(for image: CGImage, pool: CVPixelBufferPool?) -> CVPixelBuffer? {
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
        context.interpolationQuality = .none
        context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
        return buffer
    }
}
