// 設定視窗：呼號、音訊裝置、輸出音量、PTT 控制、接收與顯示的選項。改了立刻生效並存進 UserDefaults。
import SwiftUI

struct SettingsView: View {
    @EnvironmentObject var model: AppModel
    var placeholderControls = false
    /// 整個設定視窗的高度：版面稿是 700，後來多了「接收」與「顯示」兩組，加高到 900。
    static let windowHeight: CGFloat = 900
    /// 排版區的高度。真的視窗裡內容會往上延伸到標題列底下，所以要比 windowHeight 少一個標題列的高度。
    var height: CGFloat = SettingsView.windowHeight

    var body: some View {
        VStack(spacing: 0) {
            header
            Rectangle().fill(Theme.barBorder).frame(height: 1)
            VStack(alignment: .leading, spacing: 18) {
                section("電台") { station }
                section("音訊") { audio }
                section("PTT 控制") { ptt }
                section("接收") { receive }
                section("顯示") { display }
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 24)
            .padding(.vertical, 20)
        }
        .frame(width: 560)
        .frame(minHeight: height, maxHeight: .infinity, alignment: .top)
        .font(Theme.ui(13))
        .foregroundColor(Theme.text)
        .background(Theme.background)
        .environment(\.colorScheme, .dark)
        .onAppear { model.refreshDevices() }
    }

    /// 頂列的高度
    static let headerHeight: CGFloat = 44

    private var header: some View {
        // 左邊是視窗的控制鈕（真的視窗由 TitlebarButtonLayout 排到這裡；算示範圖時畫三個灰點），右邊留一樣寬，標題才會在正中央
        let side = placeholderControls ? 52 : max(52, TitlebarMetrics.buttonsWidth)
        return HStack(spacing: 12) {
            HStack(spacing: 8) {
                ForEach(0..<3, id: \.self) { _ in
                    Circle().fill(placeholderControls ? Theme.trafficLight : Color.clear).frame(width: 12, height: 12)
                }
            }
            .frame(width: side, alignment: .leading)
            .accessibilityHidden(true)
            Text(AppIdentity.settingsTitle).font(Theme.ui(13, .semibold)).frame(maxWidth: .infinity)
            Color.clear.frame(width: side, height: 1)
        }
        .padding(.horizontal, 16)
        .frame(height: SettingsView.headerHeight)
        .background(Theme.bar)
    }

    private func section<Content: View>(_ title: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            SectionTitle(title)
            VStack(spacing: 0) { content() }
                .background(RoundedRectangle(cornerRadius: 8).fill(Theme.panel))
        }
    }

    /// 卡片裡的一列：左邊是名稱與說明，右邊是控制元件。
    private func row<Control: View>(_ label: String, _ detail: String? = nil, first: Bool = false, minHeight: CGFloat? = nil,
                                    @ViewBuilder control: () -> Control) -> some View {
        HStack(spacing: 16) {
            VStack(alignment: .leading, spacing: 0) {
                Text(label).frame(height: 18)
                if let detail { Text(detail).font(Theme.ui(12)).foregroundColor(Theme.secondary).frame(height: 18) }
            }
            Spacer(minLength: 0)
            // 控制元件一律用自己的大小；不這樣做的話，HStack 分配寬度時會把裡面的文字擠成 0
            control().fixedSize()
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .padding(.top, first || minHeight != nil ? 0 : 1)
        .frame(minHeight: minHeight)
        .overlay(alignment: .top) { if !first { Rectangle().fill(Theme.separator).frame(height: 1) } }
    }

    private func field(_ text: Binding<String>, width: CGFloat, font: Font, tracking: CGFloat = 0, label: String) -> some View {
        TextField("", text: text)
            .textFieldStyle(.plain)
            .font(font)
            .tracking(tracking)
            .foregroundColor(Theme.text)
            .padding(.horizontal, 10)
            .frame(width: width, height: 28)
            .background(RoundedRectangle(cornerRadius: 6).fill(Theme.well))
            .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(Theme.buttonRing, lineWidth: 1))
            .accessibilityLabel(label)
    }

    // MARK: 電台

    private var station: some View {
        let settings = model.settings
        return row("我的呼號", "寫進每張畫面的標頭，最多 9 個字元", first: true) {
            field(Binding(get: { settings.callsign }, set: { settings.callsign = NBTV.normalizeCallsign($0) }),
                  width: 150, font: Theme.mono(13, .semibold), tracking: 0.52, label: "我的呼號")
        }
    }

    // MARK: 顯示

    private var display: some View {
        let settings = model.settings
        return row("在收到的畫面上標示訊雜比", "畫面與縮圖的左上角，標出收到那一張當下的訊雜比", first: true) {
            CheckBox(isOn: Binding(get: { settings.showFrameSNR }, set: { settings.showFrameSNR = $0 }), title: "")
                .accessibilityLabel("在收到的畫面上標示訊雜比")
        }
    }

    // MARK: 接收

    private var receive: some View {
        let settings = model.settings
        return row("合併最近幾幀", "畫面沒變的地方合併前面幾張，訊號弱時比較清楚；對方不必設定",
                   first: true) {
            CheckBox(isOn: Binding(get: { settings.mergeFrames }, set: { settings.mergeFrames = $0 }), title: "")
                .accessibilityLabel("合併最近幾幀")
        }
    }

    // MARK: 音訊

    @ViewBuilder private var audio: some View {
        let settings = model.settings
        row("輸入裝置", "從電台收進來的聲音", first: true) {
            devicePicker(devices: model.inputDevices, uid: settings.inputDeviceUID, current: model.inputDeviceName) { settings.inputDeviceUID = $0 }
        }
        row("輸出裝置", "送去電台發射的聲音") {
            devicePicker(devices: model.outputDevices, uid: settings.outputDeviceUID, current: model.outputDeviceName) { settings.outputDeviceUID = $0 }
        }
        row("輸出音量", minHeight: 48) {
            HStack(spacing: 10) {
                PlainSlider(value: Binding(get: { settings.outputLevelDb }, set: { settings.outputLevelDb = $0 }), range: -40...0, step: 1)
                    .frame(width: 170)
                    .accessibilityLabel("輸出音量")
                Text(AppModel.dbText(settings.outputLevelDb)).font(Theme.mono(13)).fixedSize().frame(width: 40, alignment: .trailing)
            }
        }
    }

    private func devicePicker(devices: [AudioDevice], uid: String, current: String, select: @escaping (String) -> Void) -> some View {
        Button {
            model.refreshDevices()
            var items = [PopupItem(title: "系統預設", selected: uid.isEmpty) { select("") }]
            items += devices.map { device in PopupItem(title: device.name, selected: device.uid == uid) { select(device.uid) } }
            showPopupMenu(items)
        } label: {
            PopupButtonLabel(title: current).frame(width: 220)
        }
        .buttonStyle(.plain)
    }

    // MARK: PTT

    @ViewBuilder private var ptt: some View {
        let settings = model.settings
        VStack(spacing: 0) {
            RadioRow(title: "不控制，使用電台的 VOX", selected: settings.pttMode == .none) { settings.pttMode = .none }
            RadioRow(title: "序列埠的 RTS 或 DTR", selected: settings.pttMode == .serial) { settings.pttMode = .serial }
            RadioRow(title: "Hamlib rigctld（TCP）", selected: settings.pttMode == .rigctld) { settings.pttMode = .rigctld }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 6)

        row("序列埠", minHeight: 48) {
            HStack(spacing: 8) {
                Button {
                    model.refreshDevices()
                    showPopupMenu(model.serialPorts.map { port in
                        PopupItem(title: port, selected: port == settings.serialPort) { settings.serialPort = port }
                    }, emptyTitle: "（沒有序列埠）")
                } label: {
                    PopupButtonLabel(title: settings.serialPort.isEmpty ? "尚未選擇" : settings.serialPort, monospaced: !settings.serialPort.isEmpty)
                        .frame(width: 200)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("序列埠")
                SegmentedControl(options: [(SerialLine.rts, "RTS"), (SerialLine.dtr, "DTR")],
                                 selection: Binding(get: { settings.serialLine }, set: { settings.serialLine = $0 }),
                                 horizontalPadding: 12, fillWidth: false)
            }
        }
        row("rigctld 位址", "TCP 連線，本機或遠端都可以") {
            HStack(spacing: 6) {
                field(Binding(get: { settings.rigHost }, set: { settings.rigHost = $0.trimmingCharacters(in: .whitespaces) }),
                      width: 150, font: Theme.mono(12), label: "rigctld 位址")
                Text(verbatim: ":").foregroundColor(Theme.secondary).accessibilityHidden(true)
                field(Binding(get: { settings.rigPort }, set: { settings.rigPort = $0.filter(\.isNumber) }),
                      width: 64, font: Theme.mono(12), label: "rigctld 連接埠")
            }
        }
    }
}
