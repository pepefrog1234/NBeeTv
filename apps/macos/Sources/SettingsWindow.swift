// 設定視窗：呼號、音訊裝置、PTT 控制、顯示的選項。改了立刻生效並存進 UserDefaults。
// （輸出音量在主視窗的發射鈕上面：要一邊發射一邊看著電台調。）
import SwiftUI

struct SettingsView: View {
    @EnvironmentObject var model: AppModel
    var placeholderControls = false
    /// 整個設定視窗的高度：版面稿是 700，後來多了「顯示」一組，PTT 多了用 CAT 指令的那一種（電台型號、序列速率兩列）。
    static let windowHeight: CGFloat = 844
    /// 排版區的高度。真的視窗裡內容會往上延伸到標題列底下，所以要比 windowHeight 少一個標題列的高度。
    var height: CGFloat = SettingsView.windowHeight

    var body: some View {
        VStack(spacing: 0) {
            header
            Rectangle().fill(Theme.barBorder).frame(height: 1)
            VStack(alignment: .leading, spacing: 18) {
                section(L("電台")) { station }
                section(L("音訊")) { audio }
                section(L("PTT 控制")) { ptt }
                section(L("顯示")) { display }
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
        return row(L("我的呼號"), L("寫進每張畫面的標頭，最多 9 個字元"), first: true) {
            field(Binding(get: { settings.callsign }, set: { settings.callsign = NBTV.normalizeCallsign($0) }),
                  width: 150, font: Theme.mono(13, .semibold), tracking: 0.52, label: L("我的呼號"))
        }
    }

    // MARK: 顯示

    private var display: some View {
        let settings = model.settings
        return row(L("在收到的畫面上標示訊雜比"), L("畫面與縮圖的左上角，標出收到那一張當下的訊雜比"), first: true) {
            CheckBox(isOn: Binding(get: { settings.showFrameSNR }, set: { settings.showFrameSNR = $0 }), title: "")
                .accessibilityLabel(L("在收到的畫面上標示訊雜比"))
        }
    }

    // MARK: 音訊

    @ViewBuilder private var audio: some View {
        let settings = model.settings
        row(L("輸入裝置"), L("從電台收進來的聲音"), first: true) {
            devicePicker(devices: model.inputDevices, uid: settings.inputDeviceUID, current: model.inputDeviceName) { settings.inputDeviceUID = $0 }
        }
        row(L("輸出裝置"), L("送去電台發射的聲音")) {
            devicePicker(devices: model.outputDevices, uid: settings.outputDeviceUID, current: model.outputDeviceName) { settings.outputDeviceUID = $0 }
        }
    }

    private func devicePicker(devices: [AudioDevice], uid: String, current: String, select: @escaping (String) -> Void) -> some View {
        Button {
            model.refreshDevices()
            var items = [PopupItem(title: L("系統預設"), selected: uid.isEmpty) { select("") }]
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
            RadioRow(title: L("不控制，使用電台的 VOX"), selected: settings.pttMode == .none) { settings.pttMode = .none }
            RadioRow(title: L("序列埠的 RTS 或 DTR"), selected: settings.pttMode == .serial) { settings.pttMode = .serial }
            RadioRow(title: L("電台的 CAT 指令（經序列埠，由 Hamlib 翻譯）"), selected: settings.pttMode == .cat) { settings.pttMode = .cat }
            RadioRow(title: L("Hamlib rigctld（TCP）"), selected: settings.pttMode == .rigctld) { settings.pttMode = .rigctld }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 6)

        row(L("序列埠"), L("RTS／DTR、CAT 共用"), minHeight: 48) {
            HStack(spacing: 8) {
                Button {
                    model.refreshDevices()
                    showPopupMenu(model.serialPorts.map { port in
                        PopupItem(title: port, selected: port == settings.serialPort) { settings.serialPort = port }
                    }, emptyTitle: L("（沒有序列埠）"))
                } label: {
                    PopupButtonLabel(title: settings.serialPort.isEmpty ? L("尚未選擇") : settings.serialPort, monospaced: !settings.serialPort.isEmpty)
                        .frame(width: 200)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(L("序列埠"))
                SegmentedControl(options: [(SerialLine.rts, "RTS"), (SerialLine.dtr, "DTR")],
                                 selection: Binding(get: { settings.serialLine }, set: { settings.serialLine = $0 }),
                                 horizontalPadding: 12, fillWidth: false)
            }
        }
        row(L("電台型號"), rigModelDetail) {
            Button {
                showPopupMenu(SettingsView.rigModelItems(selected: settings.rigModel) { rig in
                    settings.rigModel = rig.id
                    settings.rigModelName = rig.title
                }, emptyTitle: L("（這台電腦沒有裝 Hamlib）"))
            } label: {
                PopupButtonLabel(title: settings.rigModel > 0 ? settings.rigModelTitle : L("尚未選擇")).frame(width: 220)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(L("電台型號：%@", settings.rigModel > 0 ? settings.rigModelTitle : L("尚未選擇")))
        }
        row(L("序列速率"), L("要和電台裡設定的 CAT 速率相同")) {
            Button {
                var items = [PopupItem(title: L("依型號的預設"), selected: settings.serialBaud == 0) { settings.serialBaud = 0 }]
                items += Hamlib.baudRates.map { rate in
                    PopupItem(title: "\(rate) bps", selected: settings.serialBaud == rate) { settings.serialBaud = rate }
                }
                showPopupMenu(items)
            } label: {
                PopupButtonLabel(title: settings.serialBaud == 0 ? L("依型號的預設") : "\(settings.serialBaud) bps").frame(width: 220)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(L("序列速率：%@", settings.serialBaud == 0 ? L("依型號的預設") : "\(settings.serialBaud) bps"))
        }
        row(L("rigctld 位址"), L("TCP 連線，本機或遠端都可以")) {
            HStack(spacing: 6) {
                field(Binding(get: { settings.rigHost }, set: { settings.rigHost = $0.trimmingCharacters(in: .whitespaces) }),
                      width: 150, font: Theme.mono(12), label: L("rigctld 位址"))
                Text(verbatim: ":").foregroundColor(Theme.secondary).accessibilityHidden(true)
                field(Binding(get: { settings.rigPort }, set: { settings.rigPort = $0.filter(\.isNumber) }),
                      width: 64, font: Theme.mono(12), label: L("rigctld 連接埠"))
            }
        }
    }

    /// 「電台型號」那一列的說明。選了 CAT 卻沒有裝 Hamlib 的時候直接寫出來，不要等到發射才知道。
    private var rigModelDetail: String {
        if model.settings.pttMode == .cat, !Hamlib.isInstalled { return L("找不到 Hamlib，請先安裝") }
        return L("用 CAT 指令時要選")
    }

    /// 電台型號的選單：依廠牌分組，每個廠牌一個子選單（Hamlib 支援三百多個型號，排成一長串找不到）。
    /// selected 是目前選的型號（它和它的廠牌前面打勾）；選了哪一個由 select 回報。沒有裝 Hamlib 時是空的。
    static func rigModelItems(selected: Int, select: @escaping (Hamlib.Model) -> Void) -> [PopupItem] {
        Hamlib.modelsByManufacturer().map { group in
            PopupItem(title: group.manufacturer.isEmpty ? L("其他") : group.manufacturer,
                      selected: group.models.contains { $0.id == selected },
                      children: group.models.map { rig in
                          PopupItem(title: rig.name, selected: rig.id == selected) { select(rig) }
                      })
        }
    }
}
