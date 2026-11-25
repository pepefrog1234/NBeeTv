// 視窗的標題列是隱藏的，內容一路畫到視窗的上緣；紅黃綠三個控制鈕要照版面稿放在頂列的垂直中央。
// 系統把它們固定在左上角（中心離上緣 16 pt），這裡把它們所在的標題列容器拉到和頂列一樣高，再重新排好位置。
// AppKit 在視窗改變大小、進出全螢幕時會把它們排回原位，所以這些時候都要再排一次。
// 找不到預期的視圖階層時（將來的系統版本改了）什麼都不做，控制鈕留在系統原本的位置。
import AppKit
import SwiftUI

enum TitlebarMetrics {
    /// 版面稿：頂列左邊留 16 pt
    static let leading: CGFloat = 16

    /// 系統標題列的高度（隱藏標題列的視窗，內容上方被它佔去的高度）。各版本的 macOS 不一樣。
    static let titleBarHeight: CGFloat = NSWindow.frameRect(forContentRect: .zero, styleMask: [.titled, .closable]).height

    /// 三個控制鈕合起來的寬度（第一個的左緣到第三個的右緣）。拿一個不顯示的視窗量出來；量不到時用版面稿的 52。
    static let buttonsWidth: CGFloat = {
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 200, height: 100),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: true)
        guard let close = window.standardWindowButton(.closeButton), let zoom = window.standardWindowButton(.zoomButton),
              zoom.frame.maxX > close.frame.minX else { return 52 }
        return zoom.frame.maxX - close.frame.minX
    }()
}

/// 把一個視窗的控制鈕排到頂列（高 barHeight）的垂直中央、左邊留 16 pt，並在需要時重排。
final class TitlebarButtonLayout {
    private weak var window: NSWindow?
    private let barHeight: CGFloat
    private var observers: [NSObjectProtocol] = []

    init(window: NSWindow, barHeight: CGFloat) {
        self.window = window
        self.barHeight = barHeight
        let names: [Notification.Name] = [
            NSWindow.didResizeNotification, NSWindow.didEndLiveResizeNotification, NSWindow.didExitFullScreenNotification,
            NSWindow.didBecomeKeyNotification, NSWindow.didResignKeyNotification, NSWindow.didChangeScreenNotification,
        ]
        for name in names {
            observers.append(NotificationCenter.default.addObserver(forName: name, object: window, queue: .main) { [weak self] _ in
                self?.apply()
            })
        }
        apply()
        // SwiftUI 把視窗設定好（隱藏標題列等）之後，AppKit 會再排一次標題列；等它排完再排一次
        for delay in [0.0, 0.1, 0.5] {
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in self?.apply() }
        }
    }

    deinit {
        observers.forEach(NotificationCenter.default.removeObserver)
    }

    func apply() {
        guard let window, !window.styleMask.contains(.fullScreen),
              let close = window.standardWindowButton(.closeButton),
              let miniaturize = window.standardWindowButton(.miniaturizeButton),
              let zoom = window.standardWindowButton(.zoomButton),
              let container = close.superview?.superview, let frameView = container.superview else { return }
        // 標題列的容器：和頂列一樣高，貼齊視窗上緣
        var frame = container.frame
        frame.size.height = barHeight
        frame.origin.y = frameView.isFlipped ? 0 : frameView.bounds.height - barHeight
        if container.frame != frame { container.frame = frame }
        // 三個鈕：垂直置中，間距照系統原本的
        let pitch = miniaturize.frame.minX - close.frame.minX
        guard pitch > 0 else { return }
        let y = ((barHeight - close.frame.height) / 2).rounded()
        for (index, button) in [close, miniaturize, zoom].enumerated() {
            let origin = NSPoint(x: TitlebarMetrics.leading + CGFloat(index) * pitch, y: y)
            if button.frame.origin != origin { button.setFrameOrigin(origin) }
        }
    }
}

/// 放在視窗內容的背景裡：取得 SwiftUI 視窗底下的 NSWindow，把控制鈕排到頂列的垂直中央。
struct TitlebarButtons: NSViewRepresentable {
    /// 頂列的高度
    let barHeight: CGFloat
    /// true：視窗打開時鍵盤的輸入焦點先放在這個看不見的視圖上，而不是視窗裡的第一個輸入框。
    /// 主視窗用：裡面唯一的輸入框是畫布要印的字，視窗一開就讓它接鍵盤的話，隨手按到的鍵會改掉要送出去的字。
    var holdsInitialFocus = false

    func makeNSView(context: Context) -> NSView { Probe(barHeight: barHeight, holdsInitialFocus: holdsInitialFocus) }
    func updateNSView(_ nsView: NSView, context: Context) {}

    private final class Probe: NSView {
        private let barHeight: CGFloat
        private let holdsInitialFocus: Bool
        private var layout: TitlebarButtonLayout?

        init(barHeight: CGFloat, holdsInitialFocus: Bool) {
            self.barHeight = barHeight
            self.holdsInitialFocus = holdsInitialFocus
            super.init(frame: .zero)
        }

        required init?(coder: NSCoder) { fatalError("不支援從 nib 載入") }

        override var acceptsFirstResponder: Bool { holdsInitialFocus }

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            layout = window.map { TitlebarButtonLayout(window: $0, barHeight: barHeight) }
            guard holdsInitialFocus, let window else { return }
            window.initialFirstResponder = self
            // 視窗已經排好第一個接鍵盤的視圖的話（SwiftUI 先建好內容才把這個視圖放進來），這時候再換過來
            DispatchQueue.main.async { [weak self, weak window] in
                guard let self, let window, self.window === window else { return }
                if window.firstResponder !== self { window.makeFirstResponder(self) }
            }
        }
    }
}
