// 發射來源：鏡頭與影片檔，以及把來源畫面縮成送出的大小（96×72；模式 4–7 與 15 是 192×144）的工具。
// （第三種來源「圖片序列」在 ImageSequence.swift。）
import Accelerate
import AVFoundation
import CoreGraphics
import CoreVideo
import Foundation
import VideoToolbox

enum SourceKind: String {
    case camera, video
    case images   // 圖片序列：選好的一批圖片，一張一張送
}

enum ScaleMode: String {
    case fill   // 填滿：等比例放大到蓋滿 4:3，裁掉多出來的部分
    case fit    // 完整：整張都留著，空出來的地方補黑
}

/// 縮成送出的大小的一格畫面。
struct ScaledFrame {
    let rgb: Data            // 寬×高×3（96×72，或模式 4–7 與 15 的 192×144）
    let sourceSize: CGSize   // 來源的像素大小
}

enum FrameScaler {
    /// 把任意大小的影像縮成 size 這麼大的 RGB（預設是 96×72）。quarterTurns：顯示前要先順時針轉幾個 90 度（手機直拍的影片）。
    static func scale(_ image: CGImage, mode: ScaleMode, quarterTurns: Int = 0, size: PictureSize = .standard) -> Data? {
        let width = size.width, height = size.height
        guard let space = CGColorSpace(name: CGColorSpace.sRGB),
              let context = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4, space: space,
                                      bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else { return nil }
        context.setFillColor(CGColor(red: 0, green: 0, blue: 0, alpha: 1))
        context.fill(CGRect(x: 0, y: 0, width: width, height: height))
        context.interpolationQuality = .high
        // 轉正之後的寬高：轉 90 或 270 度時寬高對調
        let sideways = quarterTurns % 2 != 0
        let sw = CGFloat(sideways ? image.height : image.width), sh = CGFloat(sideways ? image.width : image.height)
        guard sw > 0, sh > 0 else { return nil }
        let ratio = mode == .fill ? max(CGFloat(width) / sw, CGFloat(height) / sh) : min(CGFloat(width) / sw, CGFloat(height) / sh)
        // 以輸出的中心為原點轉好方向再畫。CoreGraphics 的 y 軸朝上，順時針是負的角度。
        let iw = CGFloat(image.width) * ratio, ih = CGFloat(image.height) * ratio
        context.translateBy(x: CGFloat(width) / 2, y: CGFloat(height) / 2)
        context.rotate(by: -CGFloat(quarterTurns) * .pi / 2)
        context.draw(image, in: CGRect(x: -iw / 2, y: -ih / 2, width: iw, height: ih))
        guard let pixels = context.data else { return nil }
        let src = pixels.assumingMemoryBound(to: UInt8.self)
        var rgb = Data(count: size.rgbBytes)
        rgb.withUnsafeMutableBytes { raw in
            let dst = raw.bindMemory(to: UInt8.self)
            for i in 0..<(width * height) {
                dst[3 * i] = src[4 * i]
                dst[3 * i + 1] = src[4 * i + 1]
                dst[3 * i + 2] = src[4 * i + 2]
            }
        }
        return rgb
    }

    static func scale(_ pixelBuffer: CVPixelBuffer, mode: ScaleMode, quarterTurns: Int = 0, size: PictureSize = .standard) -> ScaledFrame? {
        let width = CVPixelBufferGetWidth(pixelBuffer), height = CVPixelBufferGetHeight(pixelBuffer)
        guard let image = reducedImage(pixelBuffer, for: size) ?? fullImage(pixelBuffer),
              let rgb = scale(image, mode: mode, quarterTurns: quarterTurns, size: size) else { return nil }
        let sideways = quarterTurns % 2 != 0
        return ScaledFrame(rgb: rgb, sourceSize: CGSize(width: sideways ? height : width, height: sideways ? width : height))
    }

    /// 一張已經縮好的畫面換成另一種大小（模式換成畫面大小不同的那一種時，手上只有上一格縮好的畫面）。
    /// 已經是那個大小就原樣回傳；長度不是任何一種畫面時回傳 nil。
    static func resize(_ rgb: Data, to size: PictureSize) -> Data? {
        if rgb.count == size.rgbBytes { return rgb }
        guard let image = NBTV.image(fromRGB: rgb) else { return nil }
        return scale(image, mode: .fill, size: size)
    }

    /// 整格畫面原樣包成 CGImage。
    static func fullImage(_ pixelBuffer: CVPixelBuffer) -> CGImage? {
        var image: CGImage?
        VTCreateCGImageFromCVPixelBuffer(pixelBuffer, options: nil, imageOut: &image)
        return image
    }

    /// 來源比輸出的畫面大很多時，先用 vImage 粗縮一次，再交給 CoreGraphics 做色彩轉換與最後的縮放。
    /// 讓 CoreGraphics 直接處理整格 1080p 的畫面要將近 10 毫秒（大半花在對每個來源像素做色彩轉換），預覽一秒要做好幾次。
    /// 粗縮後短邊還有輸出長邊的 4 倍（96×72 是 384 像素），最後一次縮放的品質不受影響。不適用時回傳 nil。
    static func reducedImage(_ pixelBuffer: CVPixelBuffer, for size: PictureSize = .standard) -> CGImage? {
        guard CVPixelBufferGetPixelFormatType(pixelBuffer) == kCVPixelFormatType_32BGRA else { return nil }
        let width = CVPixelBufferGetWidth(pixelBuffer), height = CVPixelBufferGetHeight(pixelBuffer)
        guard width > 0, height > 0 else { return nil }
        let factor = 4 * Double(max(size.width, size.height)) / Double(min(width, height))
        guard factor <= 0.8 else { return nil }
        let reducedWidth = max(1, Int((Double(width) * factor).rounded())), reducedHeight = max(1, Int((Double(height) * factor).rounded()))

        CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(pixelBuffer) else { return nil }
        var source = vImage_Buffer(data: base, height: vImagePixelCount(height), width: vImagePixelCount(width),
                                   rowBytes: CVPixelBufferGetBytesPerRow(pixelBuffer))
        let rowBytes = reducedWidth * 4
        let storage = UnsafeMutableRawPointer.allocate(byteCount: rowBytes * reducedHeight, alignment: 64)
        var destination = vImage_Buffer(data: storage, height: vImagePixelCount(reducedHeight), width: vImagePixelCount(reducedWidth),
                                        rowBytes: rowBytes)
        guard vImageScale_ARGB8888(&source, &destination, nil, vImage_Flags(kvImageHighQualityResampling)) == kvImageNoError else {
            storage.deallocate()
            return nil
        }
        let data = Data(bytesNoCopy: storage, count: rowBytes * reducedHeight, deallocator: .custom { pointer, _ in pointer.deallocate() })
        // 色彩空間照畫面自己記載的（影片與鏡頭通常是 Rec. 709），之後畫進 sRGB 的畫布時才會轉對
        let attachments = CVBufferCopyAttachments(pixelBuffer, .shouldPropagate)
        guard let space = attachments.flatMap({ CVImageBufferCreateColorSpaceFromAttachments($0)?.takeRetainedValue() }),
              let provider = CGDataProvider(data: data as CFData) else { return nil }
        return CGImage(width: reducedWidth, height: reducedHeight, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: rowBytes, space: space,
                       bitmapInfo: CGBitmapInfo(rawValue: CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.noneSkipFirst.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
    }
}

struct CameraInfo: Identifiable, Hashable {
    let id: String   // AVCaptureDevice.uniqueID
    let name: String
}

/// 鏡頭。每一格畫面都交給 onFrame（在內部的佇列上呼叫）。
final class CameraSource: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    var onFrame: ((CVPixelBuffer) -> Void)?
    /// 擷取中途出了問題（鏡頭被拔掉、系統回報錯誤）時呼叫，參數是給使用者看的說明（在內部的佇列上呼叫）。
    var onInterrupted: ((String) -> Void)?

    private let session = AVCaptureSession()
    private let sessionQueue = DispatchQueue(label: "nbtv.camera.session")
    private let videoQueue = DispatchQueue(label: "nbtv.camera.video")
    private var currentID: String?   // 只在 sessionQueue 上讀寫
    private var observers: [NSObjectProtocol] = []

    override init() {
        super.init()
        let center = NotificationCenter.default
        observers.append(center.addObserver(forName: AVCaptureDevice.wasDisconnectedNotification, object: nil, queue: nil) { [weak self] note in
            guard let self, let device = note.object as? AVCaptureDevice else { return }
            let id = device.uniqueID, name = device.localizedName
            self.sessionQueue.async {
                guard self.currentID == id else { return }
                if self.session.isRunning { self.session.stopRunning() }
                self.currentID = nil
                self.onInterrupted?(L("鏡頭「%@」中斷連線了", name))
            }
        })
        observers.append(center.addObserver(forName: AVCaptureSession.runtimeErrorNotification, object: session, queue: nil) { [weak self] note in
            let reason = (note.userInfo?[AVCaptureSessionErrorKey] as? NSError)?.localizedDescription ?? L("不明的錯誤")
            self?.sessionQueue.async {
                guard let self, self.currentID != nil else { return }
                self.currentID = nil
                self.onInterrupted?(L("鏡頭擷取中斷：%@", reason))
            }
        })
    }

    deinit {
        observers.forEach(NotificationCenter.default.removeObserver)
    }

    static func devices() -> [CameraInfo] {
        let discovery = AVCaptureDevice.DiscoverySession(deviceTypes: [.builtInWideAngleCamera, .external, .continuityCamera],
                                                         mediaType: .video, position: .unspecified)
        return discovery.devices.map { CameraInfo(id: $0.uniqueID, name: $0.localizedName) }
    }

    /// 開始擷取。deviceID 是空字串或找不到時用系統預設的鏡頭。失敗時回傳說明文字。
    func start(deviceID: String, completion: @escaping (String?) -> Void) {
        sessionQueue.async { [self] in
            let device = AVCaptureDevice(uniqueID: deviceID) ?? AVCaptureDevice.default(for: .video)
            guard let device else {
                completion(L("找不到鏡頭"))
                return
            }
            if session.isRunning && currentID == device.uniqueID {
                completion(nil)
                return
            }
            session.beginConfiguration()
            session.inputs.forEach { session.removeInput($0) }
            session.outputs.forEach { session.removeOutput($0) }
            // 先退回每個鏡頭都支援的設定再接上鏡頭（上一個鏡頭的 720p 這個鏡頭不一定有），接上之後能用 720p 就用
            session.sessionPreset = .high
            do {
                let input = try AVCaptureDeviceInput(device: device)
                guard session.canAddInput(input) else { throw AudioIOError(message: L("無法使用這個鏡頭")) }
                session.addInput(input)
            } catch {
                session.commitConfiguration()
                currentID = nil
                completion(L("無法開啟鏡頭：%@", error.localizedDescription))
                return
            }
            if session.canSetSessionPreset(.hd1280x720) { session.sessionPreset = .hd1280x720 }
            let output = AVCaptureVideoDataOutput()
            output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA]
            output.alwaysDiscardsLateVideoFrames = true
            output.setSampleBufferDelegate(self, queue: videoQueue)
            if session.canAddOutput(output) { session.addOutput(output) }
            session.commitConfiguration()
            currentID = device.uniqueID
            session.startRunning()
            completion(nil)
        }
    }

    func stop() {
        sessionQueue.async { [self] in
            if session.isRunning { session.stopRunning() }
            currentID = nil
        }
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        if let buffer = CMSampleBufferGetImageBuffer(sampleBuffer) { onFrame?(buffer) }
    }
}

/// 影片檔。要畫面時取播放位置當下那一格。
/// 循環播放時影片一直播；不循環時播到結尾就停在最後一格（hasEnded），發射流程靠它知道影片播完了。
final class VideoFileSource {
    let player = AVPlayer()
    private var output: AVPlayerItemVideoOutput?
    private var endObserver: NSObjectProtocol?
    private(set) var url: URL?
    private(set) var duration: Double = 0
    /// 影片記載的顯示方向：解出來的畫面要順時針轉幾個 90 度才是正的（手機直拍的影片是 1）
    private(set) var quarterTurns = 0
    var loop = true
    /// 播到結尾停住了（循環播放時不會發生）。重新播放、拖動位置或換檔案之後就不算。
    private var reachedEnd = false
    /// 最近一次要它播（play）還是要它停（pause、rewind）。循環播放的影片剛好在播到結尾的那一瞬間被暫停時，
    /// 「播完了」的通知還在路上；靠這個知道不要再從頭播。
    private var wantsPlay = false
    /// 最近一次取到的那一格（copyCurrentFrame）。同一格只給一次，所以影片停著、那一格已經取過之後再問會拿不到；
    /// 這時候它就是目前位置的畫面（位置要是動過，新位置的那一格會是「新的」，問得到）。
    private(set) var lastFrame: CVPixelBuffer?

    init() {
        player.isMuted = true  // 影片的聲音不能混進發射的音訊
        player.actionAtItemEnd = .pause
    }

    deinit { close() }

    func open(_ url: URL, completion: @escaping (Bool) -> Void) {
        close()
        let asset = AVURLAsset(url: url)
        let item = AVPlayerItem(asset: asset)
        let output = AVPlayerItemVideoOutput(pixelBufferAttributes: [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA])
        item.add(output)
        self.output = output
        self.url = url
        player.replaceCurrentItem(with: item)
        endObserver = NotificationCenter.default.addObserver(forName: .AVPlayerItemDidPlayToEndTime, object: item, queue: .main) { [weak self] _ in
            guard let self else { return }
            guard self.loop else {
                self.reachedEnd = true
                return
            }
            // 通知送到之前已經被暫停了：就停在結尾，下一次播放再從頭來（見 play）
            guard self.wantsPlay else { return }
            self.player.seek(to: .zero)
            self.player.play()
        }
        // 影片的長度與方向要非同步讀取；讀到後回主執行緒通知
        let finish: (Double, Bool, Int) -> Void = { [weak self] seconds, playable, turns in
            guard let self, self.url == url else { return }   // 這段時間裡又開了別的檔案
            self.duration = seconds.isFinite ? seconds : 0
            self.quarterTurns = turns
            completion(playable && seconds > 0)
        }
        Task {
            let seconds = (try? await asset.load(.duration).seconds) ?? 0
            let playable = (try? await asset.load(.isPlayable)) ?? false
            var turns = 0
            if let track = try? await asset.loadTracks(withMediaType: .video).first, let transform = try? await track.load(.preferredTransform) {
                turns = VideoFileSource.quarterTurns(of: transform)
            }
            DispatchQueue.main.async { finish(seconds, playable, turns) }
        }
    }

    /// 影片軌的 preferredTransform 是順時針轉幾個 90 度（影片的座標 y 軸朝下，正的角度是順時針）。
    static func quarterTurns(of transform: CGAffineTransform) -> Int {
        let turns = Int((atan2(transform.b, transform.a) / (.pi / 2)).rounded())
        return ((turns % 4) + 4) % 4
    }

    func close() {
        player.pause()
        if let endObserver { NotificationCenter.default.removeObserver(endObserver) }
        endObserver = nil
        player.replaceCurrentItem(with: nil)
        output = nil
        url = nil
        duration = 0
        quarterTurns = 0
        reachedEnd = false
        wantsPlay = false
        lastFrame = nil
    }

    var isPlaying: Bool { player.rate != 0 }
    var position: Double {
        let t = player.currentTime().seconds
        return t.isFinite ? t : 0
    }

    /// 影片已經播完、停在結尾（或被拖到結尾停著）。
    var hasEnded: Bool { reachedEnd || (duration > 0 && !isPlaying && position >= duration - 0.05) }

    func play() {
        if hasEnded { player.seek(to: .zero) }
        reachedEnd = false
        wantsPlay = true
        player.play()
    }

    func pause() {
        wantsPlay = false
        player.pause()
    }

    func seek(to seconds: Double) {
        reachedEnd = false
        player.seek(to: CMTime(seconds: seconds, preferredTimescale: 600), toleranceBefore: .zero, toleranceAfter: .zero)
    }

    /// 回到開頭並停在那裡；位置移過去之後在主執行緒上呼叫 completion（之後 copyCurrentFrame 拿到的才是開頭那一格）。
    func rewind(completion: @escaping () -> Void) {
        reachedEnd = false
        wantsPlay = false
        player.pause()
        player.seek(to: .zero, toleranceBefore: .zero, toleranceAfter: .zero) { _ in
            DispatchQueue.main.async(execute: completion)
        }
    }

    /// 目前播放位置的那一格。暫停時重複呼叫會拿到同一格；還沒有新的一格時回傳 nil。
    func copyCurrentFrame() -> CVPixelBuffer? {
        guard let output else { return nil }
        let time = player.currentTime()
        guard output.hasNewPixelBuffer(forItemTime: time) else { return nil }
        let buffer = output.copyPixelBuffer(forItemTime: time, itemTimeForDisplay: nil)
        if let buffer { lastFrame = buffer }
        return buffer
    }
}
