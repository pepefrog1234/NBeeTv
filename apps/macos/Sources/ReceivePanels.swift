// 主區的四塊：收到的畫面、訊號（或發射）面板、最近收到、音訊頻譜。
import SwiftUI

/// 96×72（或 192×144）的畫面放大顯示，用最近鄰，不要糊掉。
/// 直接用 CoreGraphics 畫：SwiftUI 的 Image 在離屏算圖時不保證照 interpolation(.none) 做。
struct PixelImage: View {
    let image: CGImage

    var body: some View {
        Canvas(opaque: true, rendersAsynchronously: false) { context, size in
            context.withCGContext { cg in
                cg.setFillColor(CGColor(red: 0, green: 0, blue: 0, alpha: 1))
                cg.fill(CGRect(origin: .zero, size: size))
                cg.interpolationQuality = .none
                // Canvas 的座標原點在左上，CGImage 要上下翻過來畫
                cg.translateBy(x: 0, y: size.height)
                cg.scaleBy(x: 1, y: -1)
                cg.draw(image, in: CGRect(origin: .zero, size: size))
            }
        }
    }
}

/// 疊在收到的畫面左上角的訊雜比：那一張收到當下接收機量到的值。設定裡的「在收到的畫面上標示訊雜比」打開才顯示。
struct SNRBadge: View {
    let snrDb: Double
    /// 縮圖上用的小字（不寫「SNR」）
    var compact = false

    var body: some View {
        Text((compact ? "" : "SNR ") + AppModel.snrText(snrDb))
            .font(Theme.mono(compact ? 10 : 13, .semibold))
            .foregroundColor(.white)
            .lineLimit(1)
            .fixedSize()
            .padding(.horizontal, compact ? 3 : 6)
            .padding(.vertical, compact ? 1 : 3)
            .background(RoundedRectangle(cornerRadius: compact ? 2 : 4).fill(Color.black.opacity(0.62)))
            .padding(compact ? 2 : 8)
            .accessibilityLabel(L("訊雜比 %@", AppModel.snrText(snrDb)))
    }
}

struct PictureSection: View {
    @EnvironmentObject var model: AppModel
    /// 畫面（4:3）的高度；剩下的高度都給下方的資訊列
    let pictureHeight: CGFloat
    /// 畫面下方資訊列的最小高度：上下各 9 的留白加上 28 的內容
    static let infoBarHeight: CGFloat = 46

    var body: some View {
        let onAir = model.txPhase != .idle
        VStack(spacing: 0) {
            picture.frame(maxWidth: .infinity).frame(height: pictureHeight)
            infoBar.frame(maxWidth: .infinity, maxHeight: .infinity).background(Theme.panel)
        }
        .background(Theme.well)
        .clipShape(RoundedRectangle(cornerRadius: 8))
        .outerRing(onAir ? Theme.tx : Theme.separator, width: onAir ? 2 : 1, radius: 8)
    }

    @ViewBuilder private var picture: some View {
        if model.txPhase != .idle {
            if let preview = model.txPreview {
                PixelImage(image: preview)
                    .accessibilityLabel(L("正在送出的畫面：壓縮後對方會看到的 %@ 影像", "\(preview.width)×\(preview.height)"))
            } else {
                placeholder(L("準備發射"), L("正在處理第一張畫面"))
            }
        } else if let frame = model.displayedFrame {
            PixelImage(image: frame.image)
                .accessibilityLabel(L("收到的畫面：%@ 傳來的 %@ 影像", frame.callsign, "\(frame.image.width)×\(frame.image.height)"))
                .overlay(alignment: .topLeading) {
                    if model.settings.showFrameSNR { SNRBadge(snrDb: frame.snrDb) }
                }
        } else {
            placeholder(L("等待訊號"), L("偵測到同步訊號後會自動解碼並顯示畫面"))
        }
    }

    private func placeholder(_ title: String, _ detail: String) -> some View {
        ZStack {
            Theme.placeholder
            VStack(spacing: 6) {
                Text(title).font(Theme.ui(17, .semibold)).foregroundColor(Theme.secondary)
                Text(detail).font(Theme.ui(12)).foregroundColor(Theme.tertiary)
            }
            .multilineTextAlignment(.center)
            .padding(24)
        }
    }

    @ViewBuilder private var infoBar: some View {
        HStack(spacing: 12) {
            if model.txPhase != .idle {
                title(model.settings.callsign, onAirMeta)
                Spacer(minLength: 0)
                Text(onAirNote).font(Theme.ui(12)).foregroundColor(Theme.secondary).lineLimit(1)
            } else if let frame = model.displayedFrame {
                // 視窗窄（或別的語言字比較長）的時候放不下完整的說明與兩顆按鈕：先改用精簡的寫法
                // （按鈕不帶圖示與張數，說明不寫「中等」那一段），再放不下就讓按鈕只剩圖示
                let count = model.displayedSequence.count
                ViewThatFits(in: .horizontal) {
                    received(frame, sequence: count, level: .full)
                    received(frame, sequence: count, level: .compact)
                    received(frame, sequence: count, level: .iconsOnly)
                }
            } else {
                Text(L("尚未收到畫面")).foregroundColor(Theme.secondary).frame(height: 28)
                Spacer(minLength: 0)
                ViewThatFits(in: .horizontal) {
                    ForEach([InfoBarLevel.full, .compact, .iconsOnly], id: \.self) { level in
                        HStack(spacing: 8) {
                            videoLabel(count: 0, enabled: false, level: level).modifier(DisabledSmallButton())
                            saveLabel(level: level).modifier(DisabledSmallButton())
                        }
                        .fixedSize()
                    }
                }
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 9)
    }

    /// 資訊列的三種寫法，由完整到精簡
    private enum InfoBarLevel: Hashable {
        case full        // 按鈕有圖示、文字與張數，說明完整
        case compact     // 按鈕只有文字，說明不寫模式的「中等」那一段
        case iconsOnly   // 按鈕只有圖示（文字在滑鼠提示與輔助使用的說明裡）
    }

    /// 收到畫面時的資訊列：左邊是呼號與說明，右邊是「儲存影片」「儲存圖片」。
    private func received(_ frame: AppModel.ReceivedFrame, sequence count: Int, level: InfoBarLevel) -> some View {
        HStack(spacing: 12) {
            title(frame.callsign.isEmpty ? "—" : frame.callsign, meta(frame, compact: level != .full))
            Spacer(minLength: 0)
            HStack(spacing: 8) {
                saveVideoButton(count: count, level: level)
                Button {
                    model.saveDisplayedFrame()
                } label: {
                    saveLabel(level: level)
                }
                .buttonStyle(SmallButtonStyle())
                .help(level == .iconsOnly ? L("儲存圖片") : "")
                .accessibilityLabel(L("儲存圖片"))
            }
            .fixedSize()   // 還是放不下時，縮短的是左邊的說明，不是按鈕
        }
    }

    private func saveLabel(level: InfoBarLevel) -> some View {
        HStack(spacing: 6) {
            if level != .compact { Icon(kind: .download, width: 14, height: 14) }
            if level != .iconsOnly { Text(L("儲存圖片")) }
        }
    }

    /// 「儲存影片」：把主畫面這一張所在的那一段（同一個呼號、一張接一張，空檔不到 10 秒）串成一個 .mp4。只有一張時不能按。
    @ViewBuilder private func saveVideoButton(count: Int, level: InfoBarLevel) -> some View {
        let enabled = count >= 2 && !model.exportingVideo
        if enabled {
            Button {
                model.saveDisplayedSequence()
            } label: {
                videoLabel(count: count, enabled: true, level: level)
            }
            .buttonStyle(SmallButtonStyle())
            .help(L("把這一段連續收到的 %d 張串成影片檔（.mp4）：同一個呼號，兩張之間的空檔不到 %d 秒", count, Int(AppModel.sequenceGapSeconds)))
            .accessibilityLabel(L("儲存影片，這一段有 %d 張", count))
        } else {
            videoLabel(count: count, enabled: false, level: level)
                .modifier(DisabledSmallButton())
                .help(model.exportingVideo ? L("正在存影片檔") : L("這一張前後沒有連續收到的畫面：同一個呼號、空檔不到 %d 秒的才能串成影片", Int(AppModel.sequenceGapSeconds)))
                .accessibilityLabel(model.exportingVideo ? L("正在儲存影片") : L("儲存影片：這一段只有一張，不能存"))
        }
    }

    private func videoLabel(count: Int, enabled: Bool, level: InfoBarLevel) -> some View {
        HStack(spacing: 6) {
            if level != .compact { Icon(kind: .videoFile, width: 14, height: 14, lineWidth: 1.4, color: enabled ? nil : Theme.tertiary) }
            if level != .iconsOnly { Text(model.exportingVideo ? L("儲存中…") : L("儲存影片")) }
            if enabled, level == .full { Text(L("%d 張", count)).font(Theme.mono(12)).foregroundColor(Theme.secondary) }
        }
    }

    private func title(_ callsign: String, _ meta: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 12) {
            Text(callsign).font(Theme.mono(20, .semibold)).tracking(0.4).lineLimit(1).fixedSize()
            Text(meta).font(Theme.ui(12)).foregroundColor(Theme.secondary).lineLimit(1).truncationMode(.tail)
        }
        .frame(height: 28)
    }

    private func meta(_ frame: AppModel.ReceivedFrame, compact: Bool) -> String {
        let mode = Modes.find(frame.modeID)
        var parts = [(compact ? mode?.name : mode?.label) ?? L("模式 %d", frame.modeID)]
        if frame.sequence >= 0 { parts.append(L("第 %d 張", frame.sequence + 1)) }
        parts.append(AppModel.timeText(frame.time))
        return parts.joined(separator: " · ")
    }

    private var onAirMeta: String {
        var parts = [model.txMode?.label ?? ""]
        if model.txFrameNumber > 0 { parts.append(L("第 %d 張", model.txFrameNumber)) }
        return parts.joined(separator: " · ")
    }

    private var onAirNote: String { model.txPreview == nil ? "" : L("對方收到的樣子（沒有雜訊時）") }
}

/// 停用的小按鈕外觀（尚未收到畫面時的「儲存圖片」）。
private struct DisabledSmallButton: ViewModifier {
    func body(content: Content) -> some View {
        content
            .foregroundColor(Theme.tertiary)
            .padding(.horizontal, 12)
            .frame(height: 28)
            .background(RoundedRectangle(cornerRadius: 6).fill(Theme.disabledFill))
            .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(Theme.separator, lineWidth: 1))
    }
}

/// 訊號面板；發射中換成發射面板。
struct StatusPanel: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(spacing: 0) {
            if model.txPhase != .idle { transmitting } else { receiving }
        }
        .padding(12)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(RoundedRectangle(cornerRadius: 8).fill(Theme.panel))
    }

    private var dash: some View { Text("—").foregroundColor(Theme.tertiary) }

    @ViewBuilder private var receiving: some View {
        header(L("訊號")) {
            if model.synced {
                Circle().fill(Theme.rx).frame(width: 6, height: 6)
                Text(L("已同步")).foregroundColor(Theme.rx)
            } else {
                Circle().strokeBorder(Theme.secondary, lineWidth: 1).frame(width: 6, height: 6)
                Text(L("搜尋同步訊號中")).foregroundColor(Theme.secondary)
            }
        }
        row(L("訊雜比")) {
            if let snr = model.snrDb { Text(AppModel.dbText(snr)).font(Theme.mono(15, .semibold)) } else { dash }
        }
        row(L("頻率偏移")) {
            if let offset = model.offsetHz {
                let value = Int(offset.rounded())
                Text((value < 0 ? "−\(-value)" : "+\(value)") + " Hz").font(Theme.mono(13))
            } else {
                dash
            }
        }
        row(L("模式（自動偵測）")) {
            if let id = model.rxModeID { Text(Modes.find(id)?.label ?? L("模式 %d", id)) } else { dash }
        }
        row(L("上一張")) {
            switch model.lastResult {
            case .ok: Text(L("解碼成功"))
            case .failed: Text(L("解碼失敗")).foregroundColor(Theme.tx)
            case .modelMissing: Text(L("模型不符")).foregroundColor(Theme.tx)
            case .none: dash
            }
        }
        Spacer(minLength: 0)
    }

    @ViewBuilder private var transmitting: some View {
        header(L("發射")) {
            Circle().fill(Theme.tx).frame(width: 6, height: 6)
            Text(pttText).foregroundColor(Theme.tx)
        }
        row(L("已發射")) { Text(AppModel.clock(model.txElapsed)).font(Theme.mono(15, .semibold)) }
        row(L("已送出")) { Text(L("%d 張", model.txFramesSent)) }
        row(L("模式")) { Text(model.txMode?.label ?? "") }
        row(L("接收")) { Text(L("暫停，停止發射後自動恢復")) }
        Spacer(minLength: 0)
    }

    private var pttText: String {
        if model.txPhase == .starting { return L("準備中") }
        return model.settings.pttMode == .none ? L("發射中（VOX）") : L("PTT 已按下")
    }

    private func header<Indicator: View>(_ title: String, @ViewBuilder indicator: () -> Indicator) -> some View {
        HStack(spacing: 12) {
            SectionTitle(title)
            Spacer(minLength: 0)
            HStack(spacing: 6) { indicator() }.font(Theme.ui(12))
        }
        .padding(.bottom, 8)
    }

    /// 面板比四列需要的高時，各列平分多出來的高度（每列最高 rowMaximum），不會全部空在底下。
    private func row<Value: View>(_ label: String, @ViewBuilder value: () -> Value) -> some View {
        HStack(spacing: 12) {
            // 別的語言的名稱比較長：寧可字縮小一點，也不要折成兩行把這一列撐高
            Text(label).font(Theme.ui(12)).foregroundColor(Theme.secondary).lineLimit(1).minimumScaleFactor(0.8)
            Spacer(minLength: 0)
            value().lineLimit(1).layoutPriority(1)
        }
        .frame(minHeight: StatusPanel.rowMinimum, maxHeight: StatusPanel.rowMaximum)
        .overlay(alignment: .top) { Rectangle().fill(Theme.separator).frame(height: 1) }
    }

    static let rowMinimum: CGFloat = 31
    static let rowMaximum: CGFloat = 44
    /// 面板放得下標題與四列的最小高度：上下各 12 的留白、標題 26、四列各 31
    static let minimumHeight: CGFloat = 24 + 26 + 4 * rowMinimum
}

/// 最近收到的縮圖：三張一列、最新的排最前面，點了就換到主畫面。
/// 一次看得到兩列，更早的往下捲；留在記憶體裡的張數見 AppModel.historyLimit。
struct RecentPanel: View {
    @EnvironmentObject var model: AppModel
    /// 每張縮圖的寬度（由外面依欄寬與可用的高度算好）
    let thumbnailWidth: CGFloat
    /// 不捲動時看得到幾張
    static let visibleCount = 6
    /// 選取框的寬度，也是捲動區域往外多佔的距離
    private static let ringInset: CGFloat = 2

    var body: some View {
        // 看得到的列數：三張以內一列，更多就是兩列（外面的 MainColumn.Metrics 用同樣的規則算高度）
        let rows: CGFloat = model.history.count > 3 ? 2 : 1
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 8) {
                SectionTitle(L("最近收到"))
                if model.history.count > RecentPanel.visibleCount {
                    // 捲軸會佔掉縮圖的寬度，所以不顯示；還有更多張這件事寫在標題旁邊
                    Text(L("共 %d 張，往下捲看更早的", model.history.count))
                        .font(Theme.ui(12))
                        .foregroundColor(Theme.tertiary)
                        .lineLimit(1)
                        .frame(height: 18)
                }
            }
            if model.history.isEmpty {
                Text(L("收到的畫面會依時間排在這裡"))
                    .font(Theme.ui(12))
                    .foregroundColor(Theme.tertiary)
                    .multilineTextAlignment(.center)
                    .padding(16)
                    .frame(maxWidth: .infinity, minHeight: 190)
                    .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(Theme.separator, lineWidth: 1))
            } else {
                ScrollView(.vertical) {
                    LazyVGrid(columns: Array(repeating: GridItem(.fixed(thumbnailWidth), spacing: 8, alignment: .topLeading), count: 3),
                              alignment: .leading, spacing: 10) {
                        ForEach(model.history) { frame in thumbnail(frame) }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(RecentPanel.ringInset)
                }
                .scrollIndicators(.never)
                // 每一列是縮圖（4:3）加下面 18 的說明，列與列之間隔 10
                .frame(height: rows * (thumbnailWidth * 0.75 + 18) + (rows - 1) * 10 + 2 * RecentPanel.ringInset)
                // 「目前顯示中」的框畫在縮圖外面 2 點。捲動的區域會裁掉超出去的東西，所以讓它往四邊各多佔 2 點、
                // 裡面的內容再縮回來：縮圖的位置不變，框也不會被裁掉。
                .padding(-RecentPanel.ringInset)
            }
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 8).fill(Theme.panel))
    }

    private func thumbnail(_ frame: AppModel.ReceivedFrame) -> some View {
        let shown = model.txPhase == .idle && model.isDisplayed(frame)
        return Button {
            model.select(frame)
        } label: {
            VStack(alignment: .leading, spacing: 4) {
                PixelImage(image: frame.image)
                    .frame(width: thumbnailWidth, height: thumbnailWidth * 0.75)
                    .overlay(alignment: .topLeading) {
                        if model.settings.showFrameSNR { SNRBadge(snrDb: frame.snrDb, compact: true) }
                    }
                    .clipShape(RoundedRectangle(cornerRadius: 2))
                    .outerRing(shown ? Theme.rx : Color.clear, width: 2, radius: 2)
                HStack(spacing: 0) {
                    Text(AppModel.timeText(frame.time)).font(Theme.mono(11)).layoutPriority(1)
                    Spacer(minLength: 2)
                    // 縮圖很窄的時候（視窗縮到最小）放不下「模式 13」，只寫編號
                    Text(thumbnailWidth < 90 ? "\(frame.modeID)" : (Modes.find(frame.modeID)?.name ?? "")).font(Theme.ui(11))
                }
                .foregroundColor(Theme.secondary)
                .lineLimit(1)
                .minimumScaleFactor(0.75)   // 視窗縮到最小時縮圖只有 72 pt 寬，字要能縮一點
                .frame(width: thumbnailWidth, height: 14)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(thumbnailDescription(frame, shown: shown))
    }

    /// 給螢幕閱讀器的說明：什麼時候收到的、哪個模式、訊雜比，以及是不是主畫面顯示的那一張。
    private func thumbnailDescription(_ frame: AppModel.ReceivedFrame, shown: Bool) -> String {
        let time = AppModel.timeText(frame.time), mode = Modes.find(frame.modeID)?.name ?? "", snr = AppModel.snrText(frame.snrDb)
        return shown ? L("%@ 收到的畫面，%@，訊雜比 %@，目前顯示中", time, mode, snr) : L("%@ 收到的畫面，%@，訊雜比 %@", time, mode, snr)
    }
}

/// 音訊頻譜的瀑布圖，0–3 kHz，最新的在上面。
struct SpectrumSection: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        let onAir = model.txPhase != .idle
        VStack(spacing: 8) {
            HStack(spacing: 16) {
                HStack(alignment: .firstTextBaseline, spacing: 12) {
                    SectionTitle(L("音訊頻譜 0–3 kHz"))
                    Text(onAir ? L("發射中顯示送出的訊號") : L("白線之間是訊號頻帶 352–2648 Hz"))
                        .font(Theme.ui(12))
                        .foregroundColor(Theme.tertiary)
                        .lineLimit(1)
                }
                Spacer(minLength: 0)
                HStack(spacing: 8) {
                    Text(onAir ? L("輸出音量") : L("輸入音量"))
                    if onAir {
                        LevelBar(fraction: AppModel.levelFraction(model.settings.outputLevelDb, floor: -40), color: Theme.tx).frame(width: 120)
                        Text(AppModel.dbText(model.settings.outputLevelDb)).font(Theme.mono(12)).foregroundColor(Theme.text)
                    } else {
                        LevelBar(fraction: AppModel.levelFraction(model.inputLevelDb), color: Theme.rx).frame(width: 120)
                        Text(model.inputLevelDb > -119 ? AppModel.dbText(model.inputLevelDb) : "—").font(Theme.mono(12)).foregroundColor(Theme.text)
                    }
                }
                .font(Theme.ui(12))
                .foregroundColor(Theme.secondary)
            }
            .frame(minHeight: 20)

            WaterfallView(image: model.waterfall)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .frame(minHeight: 140)

            FrequencyAxis().frame(height: 16)
        }
    }
}

struct WaterfallView: View {
    let image: CGImage?

    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .topLeading) {
                Theme.waterfall
                if let image {
                    // 一列一個點高，貼齊上緣；超出的部分裁掉
                    Image(decorative: image, scale: 1)
                        .resizable()
                        .interpolation(.low)
                        .frame(width: geo.size.width, height: CGFloat(image.height))
                        .accessibilityLabel(L("音訊的頻譜瀑布圖，最新的在上方"))
                }
                // 訊號頻帶的兩條邊界
                ForEach([NBTV.bandLowHz, NBTV.bandHighHz], id: \.self) { hz in
                    Rectangle().fill(Color.white.opacity(0.6))
                        .frame(width: 1, height: geo.size.height)
                        .offset(x: geo.size.width * CGFloat(hz / NBTV.spectrumMaxHz))
                        .accessibilityHidden(true)
                }
            }
            .frame(width: geo.size.width, height: geo.size.height, alignment: .topLeading)
            .clipped()
        }
        .clipShape(RoundedRectangle(cornerRadius: 6))
    }
}

struct FrequencyAxis: View {
    var body: some View {
        GeometryReader { geo in
            ZStack {
                Text("0").frame(maxWidth: .infinity, alignment: .leading)
                ForEach(1..<6, id: \.self) { index in
                    Text(String(index * 500)).position(x: geo.size.width * CGFloat(index) / 6, y: 8)
                }
                Text("3000 Hz").frame(maxWidth: .infinity, alignment: .trailing)
            }
            .font(Theme.mono(11))
            .foregroundColor(Theme.tertiary)
        }
        .accessibilityHidden(true)
    }
}
