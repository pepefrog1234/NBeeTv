// 介面的顏色、字型與圖示。數值照「短波 NBTV macOS 介面版面」的版面稿。
import SwiftUI

extension Color {
    init(hex: UInt32) {
        self.init(.sRGB, red: Double((hex >> 16) & 0xFF) / 255.0, green: Double((hex >> 8) & 0xFF) / 255.0,
                  blue: Double(hex & 0xFF) / 255.0, opacity: 1.0)
    }
}

enum Theme {
    static let background = Color(hex: 0x12151A)    // 視窗底色
    static let text = Color(hex: 0xE8ECF0)          // 主要文字
    static let secondary = Color(hex: 0xADB6BF)     // 次要文字、標籤
    static let tertiary = Color(hex: 0x909AA5)      // 說明、停用
    static let bar = Color(hex: 0x1A1E24)           // 頂列與底列
    static let barBorder = Color(hex: 0x0A0C0F)
    static let panel = Color(hex: 0x1B2026)         // 面板
    static let well = Color(hex: 0x0C0E11)          // 畫面區底色、分段選擇的軌道、輸入框
    static let separator = Color(hex: 0x2A3038)
    static let button = Color(hex: 0x252B33)
    static let buttonRing = Color(hex: 0x3A424C)
    static let disabledFill = Color(hex: 0x1E2329)
    static let track = Color(hex: 0x2B323B)         // 進度條與音量條的軌道
    static let aside = Color(hex: 0x161A1F)         // 右側欄
    static let rx = Color(hex: 0x44D6D6)            // 接收：青色
    static let rxPill = Color(hex: 0x14282B)
    static let rxPillRing = Color(hex: 0x1F6B6E)
    static let tx = Color(hex: 0xF9A870)            // 發射：橘色
    static let txInk = Color(hex: 0x1D1206)         // 橘色底上的深色字
    static let waterfall = Color(hex: 0x05070A)
    static let placeholder = Color(hex: 0x07090C)
    static let menu = Color(hex: 0x20262D)
    static let menuSelected = Color(hex: 0x2B323B)
    static let trafficLight = Color(hex: 0x454D57)

    static func ui(_ size: CGFloat, _ weight: Font.Weight = .regular) -> Font { .system(size: size, weight: weight) }
    static func mono(_ size: CGFloat, _ weight: Font.Weight = .regular) -> Font {
        .system(size: size, weight: weight, design: .monospaced)
    }
}

/// 版面稿裡的線條圖示，照 SVG 的路徑重畫。
enum IconKind {
    case settings, download, camera, chevrons, lock, videoFile, pause, play, check, clock
}

struct IconShape: Shape {
    let kind: IconKind

    /// 圖示原本的座標範圍
    var viewBox: CGSize {
        switch kind {
        case .chevrons: return CGSize(width: 12, height: 14)
        case .pause, .play: return CGSize(width: 12, height: 12)
        default: return CGSize(width: 16, height: 16)
        }
    }

    func path(in rect: CGRect) -> Path {
        var p = Path()
        switch kind {
        case .settings:
            p.move(to: CGPoint(x: 2, y: 4.5)); p.addLine(to: CGPoint(x: 8, y: 4.5))
            p.move(to: CGPoint(x: 12, y: 4.5)); p.addLine(to: CGPoint(x: 14, y: 4.5))
            p.move(to: CGPoint(x: 2, y: 11.5)); p.addLine(to: CGPoint(x: 4, y: 11.5))
            p.move(to: CGPoint(x: 8, y: 11.5)); p.addLine(to: CGPoint(x: 14, y: 11.5))
            p.addEllipse(in: CGRect(x: 8, y: 2.5, width: 4, height: 4))
            p.addEllipse(in: CGRect(x: 4, y: 9.5, width: 4, height: 4))
        case .download:
            p.move(to: CGPoint(x: 8, y: 2)); p.addLine(to: CGPoint(x: 8, y: 10))
            p.move(to: CGPoint(x: 4.5, y: 7)); p.addLine(to: CGPoint(x: 8, y: 10.5)); p.addLine(to: CGPoint(x: 11.5, y: 7))
            p.move(to: CGPoint(x: 2.5, y: 13.5)); p.addLine(to: CGPoint(x: 13.5, y: 13.5))
        case .camera:
            p.addRoundedRect(in: CGRect(x: 1.5, y: 4, width: 9, height: 8), cornerSize: CGSize(width: 1.5, height: 1.5))
            p.move(to: CGPoint(x: 10.5, y: 7)); p.addLine(to: CGPoint(x: 14.5, y: 5)); p.addLine(to: CGPoint(x: 14.5, y: 11))
            p.addLine(to: CGPoint(x: 10.5, y: 9)); p.closeSubpath()
        case .chevrons:
            p.move(to: CGPoint(x: 3, y: 5.5)); p.addLine(to: CGPoint(x: 6, y: 2.5)); p.addLine(to: CGPoint(x: 9, y: 5.5))
            p.move(to: CGPoint(x: 3, y: 8.5)); p.addLine(to: CGPoint(x: 6, y: 11.5)); p.addLine(to: CGPoint(x: 9, y: 8.5))
        case .lock:
            p.addRoundedRect(in: CGRect(x: 3.5, y: 7, width: 9, height: 6.5), cornerSize: CGSize(width: 1.5, height: 1.5))
            p.move(to: CGPoint(x: 5.5, y: 7)); p.addLine(to: CGPoint(x: 5.5, y: 5))
            p.addArc(center: CGPoint(x: 8, y: 5), radius: 2.5, startAngle: .degrees(180), endAngle: .degrees(360), clockwise: false)
            p.addLine(to: CGPoint(x: 10.5, y: 7))
        case .videoFile:
            p.addRoundedRect(in: CGRect(x: 2, y: 2.5, width: 12, height: 11), cornerSize: CGSize(width: 1.5, height: 1.5))
            p.move(to: CGPoint(x: 6.6, y: 5.8)); p.addLine(to: CGPoint(x: 6.6, y: 10.2)); p.addLine(to: CGPoint(x: 10.4, y: 8)); p.closeSubpath()
        case .pause:
            p.addRoundedRect(in: CGRect(x: 2, y: 1.5, width: 3, height: 9), cornerSize: CGSize(width: 0.8, height: 0.8))
            p.addRoundedRect(in: CGRect(x: 7, y: 1.5, width: 3, height: 9), cornerSize: CGSize(width: 0.8, height: 0.8))
        case .play:
            p.move(to: CGPoint(x: 3, y: 1.5)); p.addLine(to: CGPoint(x: 10.5, y: 6)); p.addLine(to: CGPoint(x: 3, y: 10.5)); p.closeSubpath()
        case .check:
            p.move(to: CGPoint(x: 3, y: 8.5)); p.addLine(to: CGPoint(x: 6.5, y: 12)); p.addLine(to: CGPoint(x: 13, y: 4.5))
        case .clock:   // 版面稿裡沒有，照同樣的線條畫的：圓框加上指著四點的兩根針
            p.addEllipse(in: CGRect(x: 2, y: 2, width: 12, height: 12))
            p.move(to: CGPoint(x: 8, y: 4.8)); p.addLine(to: CGPoint(x: 8, y: 8)); p.addLine(to: CGPoint(x: 10.3, y: 9.5))
        }
        let box = viewBox
        let scale = min(rect.width / box.width, rect.height / box.height)
        return p.applying(CGAffineTransform(scaleX: scale, y: scale))
            .offsetBy(dx: rect.minX + (rect.width - box.width * scale) / 2, dy: rect.minY + (rect.height - box.height * scale) / 2)
    }
}

/// 線條圖示（pause、play 是實心的）。width、height 是顯示大小，lineWidth 以原座標計。
struct Icon: View {
    let kind: IconKind
    var width: CGFloat = 16
    var height: CGFloat = 16
    var lineWidth: CGFloat = 1.5
    var color: Color? = nil

    var body: some View {
        let shape = IconShape(kind: kind)
        let scale = min(width / shape.viewBox.width, height / shape.viewBox.height)
        Group {
            if kind == .pause || kind == .play {
                shape.fill(color ?? Theme.text)
            } else {
                shape.stroke(color ?? Theme.text, style: StrokeStyle(lineWidth: lineWidth * scale, lineCap: .round, lineJoin: .round))
            }
        }
        .frame(width: width, height: height)
        .accessibilityHidden(true)
    }
}
