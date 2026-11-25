// 版面稿用到的控制元件：分段選擇、下拉選單鈕、滑桿、核取方塊、單選鈕、進度條。
// 都是自己畫的，外觀才對得上深色的版面稿。
import AppKit
import SwiftUI

/// 分段選擇（鏡頭／影片檔、填滿／完整、RTS／DTR）。
struct SegmentedControl<Value: Hashable>: View {
    let options: [(value: Value, title: String)]
    @Binding var selection: Value
    var horizontalPadding: CGFloat = 10
    var fillWidth = true

    var body: some View {
        HStack(spacing: 2) {
            ForEach(Array(options.enumerated()), id: \.offset) { _, option in
                let selected = option.value == selection
                Button {
                    selection = option.value
                } label: {
                    Text(option.title)
                        .font(Theme.ui(13, selected ? .semibold : .regular))
                        .foregroundColor(selected ? Theme.text : Theme.secondary)
                        .padding(.horizontal, horizontalPadding)
                        .frame(maxWidth: fillWidth ? .infinity : nil)
                        .frame(height: 24)
                        .background(RoundedRectangle(cornerRadius: 5).fill(selected ? Theme.buttonRing : Color.clear))
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityAddTraits(selected ? .isSelected : [])
            }
        }
        .padding(2)
        .background(RoundedRectangle(cornerRadius: 7).fill(Theme.well))
    }
}

/// 下拉選單的一個項目。children 不是空的就是子選單（電台型號依廠牌分組用），這時候 action 不會被呼叫。
struct PopupItem {
    let title: String
    var selected = false
    var children: [PopupItem] = []
    var action: () -> Void = {}

    /// 子選單攤平之後的每一個可以選的項目；子選單裡的項目標題前面加上它那一組的名稱（「Icom IC-7300」）
    var leaves: [PopupItem] {
        children.isEmpty ? [self] : children.flatMap(\.leaves).map { PopupItem(title: "\(title) \($0.title)", selected: $0.selected, action: $0.action) }
    }
}

private final class ClosureMenuItem: NSMenuItem {
    private let handler: () -> Void

    init(title: String, handler: @escaping () -> Void) {
        self.handler = handler
        super.init(title: title, action: #selector(fire), keyEquivalent: "")
        target = self
    }

    @available(*, unavailable)
    required init(coder: NSCoder) { fatalError("不支援") }

    @objc private func fire() { handler() }
}

/// 開發用：設了之後 showPopupMenu 不跳出選單，改把項目交給它（沒有人操作的測試用它代替人去點選單）。
var popupMenuInterceptor: (([PopupItem]) -> Void)?

/// 在滑鼠位置跳出系統選單。
func showPopupMenu(_ items: [PopupItem], emptyTitle: String = "（沒有可選的項目）") {
    if let interceptor = popupMenuInterceptor {
        interceptor(items.flatMap(\.leaves))
        return
    }
    makePopupMenu(items, emptyTitle: emptyTitle).popUp(positioning: nil, at: NSEvent.mouseLocation, in: nil)
}

/// 把項目做成系統選單（有 children 的項目是子選單）。沒有項目時放一列不能選的 emptyTitle。
func makePopupMenu(_ items: [PopupItem], emptyTitle: String = "（沒有可選的項目）") -> NSMenu {
    let menu = NSMenu()
    menu.autoenablesItems = false
    for item in items {
        let menuItem = ClosureMenuItem(title: item.title, handler: item.action)
        menuItem.state = item.selected ? .on : .off
        if !item.children.isEmpty { menuItem.submenu = makePopupMenu(item.children) }
        menu.addItem(menuItem)
    }
    if items.isEmpty {
        let item = NSMenuItem(title: emptyTitle, action: nil, keyEquivalent: "")
        item.isEnabled = false
        menu.addItem(item)
    }
    return menu
}

/// 下拉選單鈕的外觀：左邊可放圖示，中間是目前的選項，右邊是上下箭頭（停用時是鎖）。
struct PopupButtonLabel: View {
    let title: String
    var icon: IconKind? = nil
    var monospaced = false
    var locked = false

    var body: some View {
        HStack(spacing: monospaced ? 4 : 8) {
            if let icon { Icon(kind: icon, lineWidth: 1.4, color: locked ? Theme.tertiary : Theme.text) }
            Text(title)
                .font(monospaced ? Theme.mono(12) : Theme.ui(13))
                .lineLimit(1)
                .truncationMode(monospaced ? .middle : .tail)
                .frame(maxWidth: .infinity, alignment: .leading)
            if locked {
                Icon(kind: .lock, width: 14, height: 14, color: Theme.tertiary)
            } else {
                Icon(kind: .chevrons, width: 12, height: 14)
            }
        }
        .foregroundColor(locked ? Theme.tertiary : Theme.text)
        .padding(.leading, 10)
        .padding(.trailing, locked ? 10 : 8)
        .frame(height: 28)
        .background(RoundedRectangle(cornerRadius: 6).fill(locked ? Theme.disabledFill : Theme.button))
        .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(locked ? Theme.separator : Theme.buttonRing, lineWidth: 1))
        .contentShape(Rectangle())
    }
}

/// 一般的小按鈕（儲存圖片、選擇…）。
struct SmallButtonStyle: ButtonStyle {
    var enabled = true

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .foregroundColor(enabled ? Theme.text : Theme.tertiary)
            .padding(.horizontal, 12)
            .frame(height: 28)
            .background(RoundedRectangle(cornerRadius: 6).fill(enabled ? Theme.button : Theme.disabledFill))
            .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(enabled ? Theme.buttonRing : Theme.separator, lineWidth: 1))
            .opacity(configuration.isPressed && enabled ? 0.75 : 1)
            .contentShape(Rectangle())
    }
}

/// 滑桿：5 pt 高的軌道、14 pt 的圓鈕，外觀照版面稿。按下就跳到游標的位置，按住拖動時跟著走。
/// 滑鼠事件由 AppKit 的視圖直接處理（見 SliderView）。
struct PlainSlider: View {
    @Binding var value: Double
    let range: ClosedRange<Double>
    var step: Double? = nil
    var tint: Color = Theme.rx
    var onEditingChanged: (Bool) -> Void = { _ in }

    var body: some View {
        SliderRepresentable(value: $value, range: range, step: step, tint: tint, onEditingChanged: onEditingChanged)
            .frame(height: SliderView.height)
    }
}

private struct SliderRepresentable: NSViewRepresentable {
    @Binding var value: Double
    let range: ClosedRange<Double>
    let step: Double?
    let tint: Color
    let onEditingChanged: (Bool) -> Void

    func makeNSView(context: Context) -> SliderView { SliderView() }

    func updateNSView(_ view: SliderView, context: Context) {
        let span = range.upperBound - range.lowerBound
        view.fraction = span > 0 ? CGFloat(min(max((value - range.lowerBound) / span, 0), 1)) : 0
        view.tint = NSColor(tint)
        view.onEditingChanged = onEditingChanged
        view.onChange = { fraction in
            var newValue = range.lowerBound + Double(fraction) * span
            if let step, step > 0 { newValue = (newValue / step).rounded() * step }
            value = min(max(newValue, range.lowerBound), range.upperBound)
        }
    }
}

/// 滑桿的 AppKit 視圖。fraction 是圓鈕的位置（0…1）；使用者拖動時經由 onChange 回報新的位置。
final class SliderView: NSView {
    static let height: CGFloat = 16
    private static let knob: CGFloat = 14
    private static let trackHeight: CGFloat = 5

    var fraction: CGFloat = 0 { didSet { if fraction != oldValue { needsDisplay = true } } }
    var tint = NSColor(Theme.rx) { didSet { needsDisplay = true } }
    var onChange: (CGFloat) -> Void = { _ in }
    var onEditingChanged: (Bool) -> Void = { _ in }

    override var isFlipped: Bool { true }
    override var mouseDownCanMoveWindow: Bool { false }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override func draw(_ dirtyRect: NSRect) {
        let usable = max(bounds.width - SliderView.knob, 1)
        let middle = bounds.midY
        let track = NSRect(x: 0, y: middle - SliderView.trackHeight / 2, width: bounds.width, height: SliderView.trackHeight)
        NSColor(Theme.track).setFill()
        NSBezierPath(roundedRect: track, xRadius: track.height / 2, yRadius: track.height / 2).fill()
        tint.setFill()
        var filled = track
        filled.size.width = fraction * usable + SliderView.knob / 2
        NSBezierPath(roundedRect: filled, xRadius: track.height / 2, yRadius: track.height / 2).fill()
        // 圓鈕，底下一點點陰影
        NSGraphicsContext.saveGraphicsState()
        let shadow = NSShadow()
        shadow.shadowColor = NSColor.black.withAlphaComponent(0.35)
        shadow.shadowBlurRadius = 1
        shadow.shadowOffset = NSSize(width: 0, height: -0.5)
        shadow.set()
        NSBezierPath(ovalIn: NSRect(x: fraction * usable, y: middle - SliderView.knob / 2, width: SliderView.knob, height: SliderView.knob)).fill()
        NSGraphicsContext.restoreGraphicsState()
    }

    override func mouseDown(with event: NSEvent) {
        onEditingChanged(true)
        report(event)
    }

    override func mouseDragged(with event: NSEvent) { report(event) }

    override func mouseUp(with event: NSEvent) {
        report(event)
        onEditingChanged(false)
    }

    private func report(_ event: NSEvent) {
        let x = convert(event.locationInWindow, from: nil).x
        let usable = max(bounds.width - SliderView.knob, 1)
        onChange(min(max((x - SliderView.knob / 2) / usable, 0), 1))
    }
}

/// 核取方塊。
struct CheckBox: View {
    @Binding var isOn: Bool
    let title: String

    var body: some View {
        Button {
            isOn.toggle()
        } label: {
            HStack(spacing: 8) {
                ZStack {
                    RoundedRectangle(cornerRadius: 3).fill(isOn ? Theme.rx : Theme.well)
                    if isOn {
                        Icon(kind: .check, width: 12, height: 12, lineWidth: 2.2, color: Theme.background)
                    } else {
                        RoundedRectangle(cornerRadius: 3).strokeBorder(Theme.buttonRing, lineWidth: 1)
                    }
                }
                .frame(width: 14, height: 14)
                if !title.isEmpty { Text(title) }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(isOn ? .isSelected : [])
    }
}

/// 單選鈕。
struct RadioRow: View {
    let title: String
    let selected: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 10) {
                // 系統深色外觀的單選鈕：選中的是強調色的實心圓加中間的小圓點
                ZStack {
                    if selected {
                        Circle().fill(Theme.rx)
                        Circle().fill(Theme.background).frame(width: 5, height: 5)
                    } else {
                        Circle().fill(Color.white.opacity(0.12))
                        Circle().strokeBorder(Color.white.opacity(0.28), lineWidth: 1)
                    }
                }
                .frame(width: 14, height: 14)
                Text(title)
                Spacer(minLength: 0)
            }
            .frame(minHeight: 34)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(selected ? .isSelected : [])
    }
}

/// 進度條與音量條。
struct LevelBar: View {
    let fraction: Double
    var color: Color = Theme.rx
    var height: CGFloat = 6

    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                Capsule().fill(Theme.track)
                Rectangle().fill(color).frame(width: geo.size.width * CGFloat(min(max(fraction, 0), 1)))
            }
            .clipShape(Capsule())
        }
        .frame(height: height)
        .accessibilityHidden(true)
    }
}

/// 面板的小標題（12 px、半粗、次要色）。
struct SectionTitle: View {
    let text: String
    init(_ text: String) { self.text = text }

    var body: some View {
        Text(text)
            .font(Theme.ui(12, .semibold))
            .foregroundColor(Theme.secondary)
            .frame(height: 18)
    }
}

/// 圓角外框：畫在形狀外側，等同 CSS 的 box-shadow: 0 0 0 Npx。
struct OuterRing: ViewModifier {
    let color: Color
    let width: CGFloat
    let radius: CGFloat

    func body(content: Content) -> some View {
        content.overlay(
            RoundedRectangle(cornerRadius: radius + width).strokeBorder(color, lineWidth: width).padding(-width)
        )
    }
}

extension View {
    /// 版面稿的行高是 18 px（字級 12–13 px）。多行文字用 lines 指定行數的上限以外的情形請自行處理。
    func lineHeight(_ height: CGFloat = 18) -> some View {
        frame(minHeight: height)
    }

    func outerRing(_ color: Color, width: CGFloat = 1, radius: CGFloat) -> some View {
        modifier(OuterRing(color: color, width: width, radius: radius))
    }
}
