// 發射來源「圖片序列」：使用者選的一批圖片檔，照檔名排好，發射時一張一張送出去。
// 這裡只管檔案：哪些是圖片、怎麼排、把一張圖讀進來（照相片記載的方向轉正、縮到夠用的大小）。
// 送到第幾張、什麼時候換下一張由 AppModel 決定（見那邊「發射來源：圖片序列」那一段）。
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

final class ImageSequenceSource {
    /// 讀進來的一張圖：已經照檔案裡記載的方向轉正、縮到夠用的大小。size 是原圖轉正之後的像素大小。
    struct Picture {
        let image: CGImage
        let size: CGSize
    }

    // MARK: 哪些檔案、怎麼排

    /// 系統讀得進來的圖片類型（JPEG、PNG、HEIC、TIFF、GIF、WebP、各家相機的 RAW…）。
    private static let readableTypes = Set((CGImageSourceCopyTypeIdentifiers() as? [String]) ?? [])

    /// 這個檔案是不是讀得進來的圖片。只看副檔名對應到的類型，不開檔
    /// （SVG 這種系統認得是圖片、但這裡讀不進來的不算）。
    static func isImage(_ url: URL) -> Bool {
        guard let type = UTType(filenameExtension: url.pathExtension), type.conforms(to: .image) else { return false }
        return readableTypes.contains(type.identifier) || type.supertypes.contains { readableTypes.contains($0.identifier) }
    }

    /// 使用者選的東西 → 要送的圖片檔，照發射的順序排好。
    /// 選到的資料夾換成它裡面的圖片（只看那一層，不往子資料夾裡找；隱藏的檔案不算）；不是圖片的檔案略過；同一個檔案只算一次。
    /// 順序照檔名，和 Finder 用名稱排序時一樣：數字照大小排（2.jpg 在 10.jpg 前面），不分大小寫。
    /// 在開檔的對話框裡先點哪一張、後點哪一張不影響順序（對話框本來就不保證照點的順序給）。
    static func files(from selection: [URL]) -> [URL] {
        let manager = FileManager.default
        var found: [URL] = []
        for url in selection {
            var isDirectory: ObjCBool = false
            guard manager.fileExists(atPath: url.path, isDirectory: &isDirectory) else { continue }
            if isDirectory.boolValue {
                let children = (try? manager.contentsOfDirectory(at: url, includingPropertiesForKeys: [.isDirectoryKey],
                                                                 options: [.skipsHiddenFiles])) ?? []
                found += children.filter { isImage($0) && (try? $0.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory != true }
            } else if isImage(url) {
                found.append(url)
            }
        }
        var seen = Set<String>()
        return found.map(\.standardizedFileURL)
            .filter { seen.insert($0.path).inserted }
            .sorted { $0.path.localizedStandardCompare($1.path) == .orderedAscending }
    }

    // MARK: 讀一張圖

    /// 讀進來的圖，短邊至少留這麼多像素（384）：96×72 的畫面長邊的四倍，最後縮成送出的大小時品質不受影響。
    /// 畫面是 192×144 的模式（4–7 與 15）也夠用：短邊是畫面高度的 2.7 倍，還是往小的縮。
    private static let shortSidePixels = 4 * max(NBTV.imageWidth, NBTV.imageHeight)

    /// 把一張圖讀進來。讀不到（檔案不見了、壞了、其實不是圖片）回傳 nil。
    /// 手機直拍的相片存的是橫的像素加上「要轉 90 度」的記號，這裡照記號轉正；
    /// 幾千萬像素的相片不整張解開，直接縮到夠用的大小（JPEG 在解碼時就縮，快得多，也不佔記憶體）。
    /// 有很多格的檔案（GIF 動畫、多頁的 TIFF）只取第一格。
    static func load(_ url: URL) -> Picture? {
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil), CGImageSourceGetCount(source) > 0,
              let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
              let width = (properties[kCGImagePropertyPixelWidth] as? NSNumber)?.intValue,
              let height = (properties[kCGImagePropertyPixelHeight] as? NSNumber)?.intValue, width > 0, height > 0 else { return nil }
        // 很扁或很長的圖（全景）：照短邊算，長邊跟著放大；不超過原圖，也不超過 8192
        let long = max(width, height), short = min(width, height)
        let wanted = min(long, 8192, Int((Double(shortSidePixels) * Double(long) / Double(short)).rounded(.up)))
        let options: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,   // 不用檔案裡內嵌的小縮圖（太小，也壓得太兇）
            kCGImageSourceCreateThumbnailWithTransform: true,     // 照記載的方向轉正
            kCGImageSourceThumbnailMaxPixelSize: wanted,
            kCGImageSourceShouldCacheImmediately: true,           // 現在就解開（這裡是背景的佇列），不要等到畫的時候才解
        ]
        guard let image = CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary) else { return nil }
        // 記載的方向 5–8 是轉了 90 度的：轉正之後寬高對調
        let orientation = (properties[kCGImagePropertyOrientation] as? NSNumber)?.intValue ?? 1
        let sideways = (5...8).contains(orientation)
        return Picture(image: image, size: CGSize(width: sideways ? height : width, height: sideways ? width : height))
    }

    // MARK: 最近讀過的幾張

    /// 最近讀進來的幾張（最新的在最後）：預覽的那一張、它的下一張，再多留一張。
    /// 只在一條佇列上使用（AppModel 的 previewQueue），所以不必上鎖。
    private var recent: [(url: URL, picture: Picture)] = []
    private static let keep = 3

    /// 同 load，但最近讀過的直接給。只能在那一條佇列上呼叫。
    func picture(at url: URL) -> Picture? {
        if let index = recent.firstIndex(where: { $0.url == url }) {
            let hit = recent.remove(at: index)
            recent.append(hit)
            return hit.picture
        }
        guard let picture = ImageSequenceSource.load(url) else { return nil }
        recent.append((url, picture))
        if recent.count > ImageSequenceSource.keep { recent.removeFirst(recent.count - ImageSequenceSource.keep) }
        return picture
    }

    /// 換了一批圖片：之前讀進來的都丟掉（同一個檔名可能已經是不同的內容）。只能在那一條佇列上呼叫。
    func forget() { recent.removeAll() }
}
