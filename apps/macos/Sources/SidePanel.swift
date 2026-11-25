// 右側欄：發射來源、縮放方式、畫布（印在畫面上的字）、發射模式、輸出音量與發射按鈕。
import SwiftUI

struct SidePanel: View {
    @EnvironmentObject var model: AppModel
    /// 鍵盤的輸入焦點是不是在畫布的輸入框裡
    @FocusState private var canvasFocused: Bool

    var body: some View {
        let settings = model.settings
        VStack(alignment: .leading, spacing: 12) {
            SectionTitle(L("發射來源"))
            SegmentedControl(options: [(SourceKind.camera, L("鏡頭")), (SourceKind.video, L("影片檔"))],
                             selection: Binding(get: { settings.sourceKind }, set: { model.setSourceKind($0) }))
            if settings.sourceKind == .camera { cameraPicker } else { videoPicker }
            // 欄位不夠高時（視窗縮到最小、下面的說明又折成兩行）縮的是預覽，其他的控制元件都留著
            preview.layoutPriority(1)
            HStack(spacing: 12) {
                if settings.sourceKind == .video, model.videoName != nil {
                    CheckBox(isOn: Binding(get: { settings.loopVideo }, set: { settings.loopVideo = $0 }), title: L("循環播放"))
                        .help(L("沒有勾：按下發射才開始播放，播完自動停止發射。勾了：影片循環播放，要自己按停止。兩種都是每送一張取當下那一格"))
                } else {
                    Text(L("縮放方式")).font(Theme.ui(12)).foregroundColor(Theme.secondary)
                }
                Spacer(minLength: 0)
                SegmentedControl(options: [(ScaleMode.fill, L("填滿")), (ScaleMode.fit, L("完整"))],
                                 selection: Binding(get: { settings.scaleMode }, set: { settings.scaleMode = $0 }),
                                 horizontalPadding: 14, fillWidth: false)
                    .accessibilityLabel(L("縮放方式"))
                    .help(L("縮放方式。填滿：把畫面放大到蓋滿，裁掉多出來的邊。完整：整個畫面都留著，上下或左右補黑邊"))
            }
            canvasRow
            Rectangle().fill(Theme.separator).frame(height: 1)
            SectionTitle(L("發射模式"))
            modeButton
            Text(modeNote)
                .font(Theme.ui(12))
                .foregroundColor(Theme.secondary)
                .lineSpacing(3)
                .fixedSize(horizontal: false, vertical: true)
                .lineHeight()
            Spacer(minLength: 4)
            volumeRow
            transmitButton
            Text(buttonNote)
                .font(Theme.ui(12))
                .foregroundColor(Theme.secondary)
                .multilineTextAlignment(.center)
                .lineSpacing(3)
                .fixedSize(horizontal: false, vertical: true)   // 別的語言這一句比較長：折成兩行，不要截掉
                .frame(maxWidth: .infinity)
                .lineHeight()
        }
        .padding(16)
        .frame(maxHeight: .infinity, alignment: .top)
        .background(Theme.aside)
    }

    // MARK: 來源

    private var cameraPicker: some View {
        Button {
            model.refreshDevices()
            showPopupMenu(model.cameras.map { camera in
                PopupItem(title: camera.name, selected: camera.name == model.cameraName) { model.selectCamera(camera.id) }
            }, emptyTitle: L("（沒有鏡頭）"))
        } label: {
            PopupButtonLabel(title: model.cameraName, icon: .camera)
        }
        .buttonStyle(.plain)
        .accessibilityLabel(L("選擇鏡頭：%@", model.cameraName))
    }

    private var videoPicker: some View {
        HStack(spacing: 8) {
            HStack(spacing: 8) {
                Icon(kind: .videoFile, lineWidth: 1.4)
                Text(model.videoName ?? L("尚未選擇影片檔"))
                    .foregroundColor(model.videoName == nil ? Theme.tertiary : Theme.text)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .padding(.horizontal, 10)
            .frame(height: 28)
            .background(RoundedRectangle(cornerRadius: 6).fill(Theme.disabledFill))
            .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(Theme.separator, lineWidth: 1))
            Button(L("選擇…")) { model.chooseVideoFile() }
                .buttonStyle(SmallButtonStyle())
        }
    }

    private var preview: some View {
        VStack(alignment: .leading, spacing: 6) {
            ZStack {
                Color.black
                if let image = model.sourcePreview {
                    PixelImage(image: image)
                        .accessibilityLabel(model.settings.sourceKind == .camera ? L("鏡頭畫面縮成 96×72 後的預覽") : L("影片目前這一格縮成 96×72 後的預覽"))
                } else {
                    Text(model.sourceMessage ?? L("等待來源畫面"))
                        .font(Theme.ui(12))
                        .foregroundColor(Theme.tertiary)
                        .multilineTextAlignment(.center)
                        .padding(12)
                }
            }
            .aspectRatio(4.0 / 3.0, contentMode: .fit)
            .clipShape(RoundedRectangle(cornerRadius: 4))
            .frame(minHeight: 120)
            .frame(maxWidth: .infinity)   // 被壓矮的時候跟著變窄，留在欄位的中間

            if model.settings.sourceKind == .video, model.videoName != nil { videoControls }

            Text(sourceCaption)
                .font(Theme.ui(12))
                .foregroundColor(Theme.secondary)
                .lineLimit(1)
                .lineHeight()
        }
    }

    // MARK: 畫布

    /// 畫布：發射前印在每一張畫面上的字。打開之後上面的預覽就是印好字的樣子。
    private var canvasRow: some View {
        let settings = model.settings
        return HStack(spacing: 8) {
            CheckBox(isOn: Binding(get: { settings.canvasEnabled }, set: { model.setCanvasEnabled($0) }), title: L("畫布"))
                .help(L("在送出去的每一張畫面上印字，例如「CQ %@」。上面的預覽就是送出去的樣子", settings.callsign.isEmpty ? L("呼號") : settings.callsign))
            TextField(L("要印的字"), text: Binding(get: { settings.canvasText },
                                              set: { settings.canvasText = String($0.prefix(CanvasLayer.maximumCharacters)) }))
                .textFieldStyle(.plain)
                .font(Theme.ui(13))
                .foregroundColor(settings.canvasEnabled ? Theme.text : Theme.tertiary)
                .padding(.horizontal, 8)
                .frame(height: 28)
                .frame(maxWidth: .infinity)
                .background(RoundedRectangle(cornerRadius: 6).fill(Theme.well))
                .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(canvasFocused ? Theme.rx : Theme.buttonRing, lineWidth: 1))
                .focused($canvasFocused)
                .onSubmit { canvasFocused = false }   // 按 Return 就離開輸入框，之後按到的鍵不會再改到這些字
                .accessibilityLabel(L("畫布要印的字"))
            SegmentedControl(options: [(CanvasPosition.top, L("上")), (CanvasPosition.bottom, L("下"))],
                             selection: Binding(get: { settings.canvasPosition }, set: { settings.canvasPosition = $0 }),
                             horizontalPadding: 9, fillWidth: false)
                .accessibilityLabel(L("字印在畫面的上緣或下緣"))
        }
    }

    private var sourceCaption: String {
        guard let size = model.sourceSize, model.sourcePreview != nil else { return L("來源 — → 送出 96×72") }
        return L("來源 %d×%d → 送出 96×72", Int(size.width), Int(size.height))
    }

    private var videoControls: some View {
        HStack(spacing: 8) {
            Button {
                model.toggleVideoPlayback()
            } label: {
                Icon(kind: model.videoPlaying ? .pause : .play, width: 12, height: 12)
                    .frame(width: 28, height: 28)
                    .background(RoundedRectangle(cornerRadius: 6).fill(Theme.button))
                    .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(Theme.buttonRing, lineWidth: 1))
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(model.videoPlaying ? L("暫停") : L("播放"))

            PlainSlider(value: Binding(get: { model.videoPosition }, set: { model.scrubVideo(to: $0, editing: true) }),
                        range: 0...max(model.videoDuration, 0.1),
                        onEditingChanged: { editing in if !editing { model.scrubVideo(to: model.videoPosition, editing: false) } })
                .accessibilityLabel(L("播放位置"))

            Text("\(AppModel.clock(model.videoPosition)) / \(AppModel.clock(model.videoDuration))")
                .font(Theme.mono(12))
                .foregroundColor(Theme.secondary)
        }
    }

    // MARK: 模式與發射

    @ViewBuilder private var modeButton: some View {
        let label = (model.txPhase == .idle ? Modes.find(model.settings.txModeID) : model.txMode)?.label ?? ""
        if model.txPhase == .idle {
            Button {
                model.showModeMenu = true
            } label: {
                PopupButtonLabel(title: label)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(L("選擇發射模式：%@", label))
            .popover(isPresented: Binding(get: { model.showModeMenu }, set: { model.showModeMenu = $0 }), arrowEdge: .leading) {
                ModeMenuView(selectedID: model.settings.txModeID, standalone: false) { id in
                    model.settings.txModeID = id
                    model.showModeMenu = false
                }
            }
        } else {
            PopupButtonLabel(title: label, locked: true)
                .accessibilityLabel(L("發射模式：%@，發射中不能更換", label))
        }
    }

    private var modeNote: String {
        if model.txPhase != .idle { return L("發射中不能更換模式；來源可以隨時切換，下一張生效") }
        return Modes.find(model.settings.txModeID)?.summary ?? ""
    }

    /// 輸出音量：送去電台的聲音大小。放在發射鈕上面，發射中可以一邊看電台的 ALC 一邊調，改了立刻生效。
    private var volumeRow: some View {
        let settings = model.settings
        return HStack(spacing: 8) {
            Text(L("輸出音量")).font(Theme.ui(12)).foregroundColor(Theme.secondary)
            PlainSlider(value: Binding(get: { settings.outputLevelDb }, set: { settings.outputLevelDb = $0 }), range: -40...0, step: 1,
                        tint: Theme.tx)
                .accessibilityLabel(L("輸出音量"))
                .accessibilityValue(AppModel.dbText(settings.outputLevelDb))
            Text(AppModel.dbText(settings.outputLevelDb)).font(Theme.mono(12)).fixedSize().frame(width: 44, alignment: .trailing)
        }
        .frame(height: 20)
        .help(L("送去電台的聲音大小。發射中看著電台的 ALC 調：ALC 剛好不動或只動一點點"))
    }

    private var ready: Bool { model.canTransmit && !model.settings.callsign.isEmpty }

    @ViewBuilder private var transmitButton: some View {
        if model.txPhase == .idle {
            Button {
                model.toggleTransmit()
            } label: {
                Text(L("開始發射"))
                    .font(Theme.ui(15, .bold))
                    .foregroundColor(Theme.txInk)
                    .frame(maxWidth: .infinity)
                    .frame(height: 44)
                    .background(RoundedRectangle(cornerRadius: 8).fill(Theme.tx))
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .disabled(!ready && model.live)
            .opacity(ready || !model.live ? 1 : 0.45)
        } else {
            Button {
                model.toggleTransmit()
            } label: {
                Text(model.txPhase == .stopping ? L("這張送完就停止…") : L("停止發射"))
                    .font(Theme.ui(15, .bold))
                    .foregroundColor(Theme.tx)
                    .frame(maxWidth: .infinity)
                    .frame(height: 44)
                    .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(Theme.tx, lineWidth: 2))
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .disabled(model.txPhase == .stopping)
        }
    }

    private var buttonNote: String {
        if model.txPhase != .idle, model.scheduledRun { return L("定時發射：影片播完會自動停止，也可以隨時按停止") }
        if model.txPhase == .transmitting, model.videoPlaysOnce { return L("影片播完會自動停止，也可以隨時按停止") }
        if model.txPhase != .idle { return L("送完目前這張後放開 PTT，自動回到接收") }
        if model.settings.callsign.isEmpty { return L("還沒設定呼號，請先到設定裡填寫") }
        if model.live, !model.canTransmit { return L("等來源有畫面後才能發射") }
        if model.videoPlaysOnce { return L("以 %@ 發射，影片播完自動停止", model.settings.callsign) }
        return L("以 %@ 發射，停止後自動回到接收", model.settings.callsign)
    }
}
