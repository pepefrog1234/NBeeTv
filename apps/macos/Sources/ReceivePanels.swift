// 主區的四塊：收到的畫面、訊號（或發射）面板、最近收到、音訊頻譜。
import SwiftUI

/// 96×72 的畫面放大顯示，用最近鄰，不要糊掉。
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
            .accessibilityLabel("訊雜比 \(AppModel.snrText(snrDb))")
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
                    .accessibilityLabel("正在送出的畫面：壓縮後對方會看到的 96×72 影像")
            } else {
                placeholder("準備發射", "正在處理第一張畫面")
            }
        } else if let frame = model.displayedFrame {
            PixelImage(image: frame.image)
                .accessibilityLabel("收到的畫面：\(frame.callsign) 傳來的 96×72 影像")
                .overlay(alignment: .topLeading) {
                    if model.settings.showFrameSNR { SNRBadge(snrDb: frame.snrDb) }
                }
        } else {
            placeholder("等待訊號", "偵測到同步訊號後會自動解碼並顯示畫面")
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
                title(frame.callsign.isEmpty ? "—" : frame.callsign, meta(frame))
                Spacer(minLength: 0)
                Button {
                    model.saveDisplayedFrame()
                } label: {
                    saveLabel
                }
                .buttonStyle(SmallButtonStyle())
            } else {
                Text("尚未收到畫面").foregroundColor(Theme.secondary).frame(height: 28)
                Spacer(minLength: 0)
                saveLabel.modifier(DisabledSmallButton())
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 9)
    }

    private var saveLabel: some View {
        HStack(spacing: 6) {
            Icon(kind: .download, width: 14, height: 14)
            Text("儲存圖片")
        }
    }

    private func title(_ callsign: String, _ meta: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 12) {
            Text(callsign).font(Theme.mono(20, .semibold)).tracking(0.4).lineLimit(1).fixedSize()
            Text(meta).font(Theme.ui(12)).foregroundColor(Theme.secondary).lineLimit(1)
        }
        .frame(height: 28)
    }

    private func meta(_ frame: AppModel.ReceivedFrame) -> String {
        var parts = [Modes.find(frame.modeID)?.label ?? "模式 \(frame.modeID)"]
        if frame.sequence >= 0 { parts.append("第 \(frame.sequence + 1) 張") }
        parts.append(AppModel.timeText(frame.time))
        return parts.joined(separator: " · ")
    }

    private var onAirMeta: String {
        var parts = [model.txMode?.label ?? ""]
        if model.txFrameNumber > 0 { parts.append("第 \(model.txFrameNumber) 張") }
        return parts.joined(separator: " · ")
    }

    private var onAirNote: String { model.txPreview == nil ? "" : "對方收到的樣子（沒有雜訊時）" }
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
        header("訊號") {
            if model.synced {
                Circle().fill(Theme.rx).frame(width: 6, height: 6)
                Text("已同步").foregroundColor(Theme.rx)
            } else {
                Circle().strokeBorder(Theme.secondary, lineWidth: 1).frame(width: 6, height: 6)
                Text("搜尋同步訊號中").foregroundColor(Theme.secondary)
            }
        }
        row("訊雜比") {
            if let snr = model.snrDb { Text(AppModel.dbText(snr)).font(Theme.mono(15, .semibold)) } else { dash }
        }
        row("頻率偏移") {
            if let offset = model.offsetHz {
                let value = Int(offset.rounded())
                Text((value < 0 ? "−\(-value)" : "+\(value)") + " Hz").font(Theme.mono(13))
            } else {
                dash
            }
        }
        row("模式（自動偵測）") {
            if let id = model.rxModeID { Text(Modes.find(id)?.label ?? "模式 \(id)") } else { dash }
        }
        row("上一張") {
            switch model.lastResult {
            case .ok: Text("解碼成功")
            case .failed: Text("解碼失敗").foregroundColor(Theme.tx)
            case .modelMissing: Text("模型不符").foregroundColor(Theme.tx)
            case .none: dash
            }
        }
        Spacer(minLength: 0)
        progress(color: Theme.rx, fraction: model.symbolsTotal > 0 ? Double(model.symbolsDone) / Double(model.symbolsTotal) : 0) {
            if model.synced, model.symbolsTotal > 0 {
                Text("\(model.symbolsDone) / \(model.symbolsTotal) 符號").font(Theme.mono(13))
            } else {
                dash
            }
        }
    }

    @ViewBuilder private var transmitting: some View {
        let duration = model.txMode?.frameSeconds ?? 0
        header("發射") {
            Circle().fill(Theme.tx).frame(width: 6, height: 6)
            Text(pttText).foregroundColor(Theme.tx)
        }
        row("已發射") { Text(AppModel.clock(model.txElapsed)).font(Theme.mono(15, .semibold)) }
        row("已送出") { Text("\(model.txFramesSent) 張") }
        row("模式") { Text(model.txMode?.label ?? "") }
        row("接收") { Text("暫停，停止發射後自動恢復") }
        Spacer(minLength: 0)
        progress(color: Theme.tx, fraction: duration > 0 ? model.txFrameProgress / duration : 0) {
            if model.isOnAir {
                Text(String(format: "%.1f / %.2f 秒", model.txFrameProgress, duration)).font(Theme.mono(13))
            } else {
                dash
            }
        }
    }

    private var pttText: String {
        if model.txPhase == .starting { return "準備中" }
        return model.settings.pttMode == .none ? "發射中（VOX）" : "PTT 已按下"
    }

    private func header<Indicator: View>(_ title: String, @ViewBuilder indicator: () -> Indicator) -> some View {
        HStack(spacing: 12) {
            SectionTitle(title)
            Spacer(minLength: 0)
            HStack(spacing: 6) { indicator() }.font(Theme.ui(12))
        }
        .padding(.bottom, 8)
    }

    private func row<Value: View>(_ label: String, @ViewBuilder value: () -> Value) -> some View {
        HStack(spacing: 12) {
            Text(label).font(Theme.ui(12)).foregroundColor(Theme.secondary)
            Spacer(minLength: 0)
            value().lineLimit(1)
        }
        .frame(minHeight: 31)
        .overlay(alignment: .top) { Rectangle().fill(Theme.separator).frame(height: 1) }
    }

    private func progress<Value: View>(color: Color, fraction: Double, @ViewBuilder value: () -> Value) -> some View {
        VStack(spacing: 6) {
            HStack(spacing: 12) {
                Text("本張進度").font(Theme.ui(12)).foregroundColor(Theme.secondary)
                Spacer(minLength: 0)
                value()
            }
            .frame(height: 18)
            LevelBar(fraction: fraction, color: color)
        }
        .padding(.top, 8)
        .overlay(alignment: .top) { Rectangle().fill(Theme.separator).frame(height: 1) }
    }
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
                SectionTitle("最近收到")
                if model.history.count > RecentPanel.visibleCount {
                    // 捲軸會佔掉縮圖的寬度，所以不顯示；還有更多張這件事寫在標題旁邊
                    Text("共 \(model.history.count) 張，往下捲看更早的")
                        .font(Theme.ui(12))
                        .foregroundColor(Theme.tertiary)
                        .lineLimit(1)
                        .frame(height: 18)
                }
            }
            if model.history.isEmpty {
                Text("收到的畫面會依時間排在這裡")
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
                    Text(Modes.find(frame.modeID)?.name ?? "").font(Theme.ui(11))
                }
                .foregroundColor(Theme.secondary)
                .lineLimit(1)
                .minimumScaleFactor(0.75)   // 視窗縮到最小時縮圖只有 72 pt 寬，字要能縮一點
                .frame(width: thumbnailWidth, height: 14)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel("\(AppModel.timeText(frame.time)) 收到的畫面，\(Modes.find(frame.modeID)?.name ?? "")，訊雜比 \(AppModel.snrText(frame.snrDb))\(shown ? "，目前顯示中" : "")")
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
                    SectionTitle("音訊頻譜 0–3 kHz")
                    Text(onAir ? "發射中顯示送出的訊號" : "白線之間是訊號頻帶 352–2648 Hz")
                        .font(Theme.ui(12))
                        .foregroundColor(Theme.tertiary)
                        .lineLimit(1)
                }
                Spacer(minLength: 0)
                HStack(spacing: 8) {
                    Text(onAir ? "輸出音量" : "輸入音量")
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
                        .accessibilityLabel("音訊的頻譜瀑布圖，最新的在上方")
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
