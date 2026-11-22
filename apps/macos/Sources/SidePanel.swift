// 右側欄：發射來源、縮放方式、發射模式與發射按鈕。
import SwiftUI

struct SidePanel: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        let settings = model.settings
        VStack(alignment: .leading, spacing: 12) {
            SectionTitle("發射來源")
            SegmentedControl(options: [(SourceKind.camera, "鏡頭"), (SourceKind.video, "影片檔")],
                             selection: Binding(get: { settings.sourceKind }, set: { model.setSourceKind($0) }))
            if settings.sourceKind == .camera { cameraPicker } else { videoPicker }
            preview
            if settings.sourceKind == .video, model.videoName != nil {
                CheckBox(isOn: Binding(get: { settings.loopVideo }, set: { settings.loopVideo = $0 }), title: "循環播放").lineHeight()
            }
            HStack(spacing: 12) {
                Text("縮放方式").font(Theme.ui(12)).foregroundColor(Theme.secondary)
                Spacer(minLength: 0)
                SegmentedControl(options: [(ScaleMode.fill, "填滿"), (ScaleMode.fit, "完整")],
                                 selection: Binding(get: { settings.scaleMode }, set: { settings.scaleMode = $0 }),
                                 horizontalPadding: 14, fillWidth: false)
            }
            Rectangle().fill(Theme.separator).frame(height: 1)
            SectionTitle("發射模式")
            modeButton
            Text(modeNote)
                .font(Theme.ui(12))
                .foregroundColor(Theme.secondary)
                .lineSpacing(3)
                .fixedSize(horizontal: false, vertical: true)
                .lineHeight()
            Spacer(minLength: 12)
            transmitButton
            Text(buttonNote)
                .font(Theme.ui(12))
                .foregroundColor(Theme.secondary)
                .multilineTextAlignment(.center)
                .lineSpacing(3)
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
            }, emptyTitle: "（沒有鏡頭）")
        } label: {
            PopupButtonLabel(title: model.cameraName, icon: .camera)
        }
        .buttonStyle(.plain)
        .accessibilityLabel("選擇鏡頭：\(model.cameraName)")
    }

    private var videoPicker: some View {
        HStack(spacing: 8) {
            HStack(spacing: 8) {
                Icon(kind: .videoFile, lineWidth: 1.4)
                Text(model.videoName ?? "尚未選擇影片檔")
                    .foregroundColor(model.videoName == nil ? Theme.tertiary : Theme.text)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .padding(.horizontal, 10)
            .frame(height: 28)
            .background(RoundedRectangle(cornerRadius: 6).fill(Theme.disabledFill))
            .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(Theme.separator, lineWidth: 1))
            Button("選擇…") { model.chooseVideoFile() }
                .buttonStyle(SmallButtonStyle())
        }
    }

    private var preview: some View {
        VStack(alignment: .leading, spacing: 6) {
            ZStack {
                Color.black
                if let image = model.sourcePreview {
                    PixelImage(image: image)
                        .accessibilityLabel(model.settings.sourceKind == .camera ? "鏡頭畫面縮成 96×72 後的預覽" : "影片目前這一格縮成 96×72 後的預覽")
                } else {
                    Text(model.sourceMessage ?? "等待來源畫面")
                        .font(Theme.ui(12))
                        .foregroundColor(Theme.tertiary)
                        .multilineTextAlignment(.center)
                        .padding(12)
                }
            }
            .aspectRatio(4.0 / 3.0, contentMode: .fit)
            .clipShape(RoundedRectangle(cornerRadius: 4))

            if model.settings.sourceKind == .video, model.videoName != nil { videoControls }

            VStack(alignment: .leading, spacing: 0) {
                Text(sourceCaption).lineHeight()
                if model.settings.sourceKind == .video {
                    Text(model.videoPlaysOnce ? "按下發射才開始播放，每送一張取當下那一格" : "影片照常播放，每送一張就取當下那一格").lineHeight()
                }
            }
            .font(Theme.ui(12))
            .foregroundColor(Theme.secondary)
        }
    }

    private var sourceCaption: String {
        guard let size = model.sourceSize, model.sourcePreview != nil else { return "來源 — → 送出 96×72" }
        return "來源 \(Int(size.width))×\(Int(size.height)) → 送出 96×72"
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
            .accessibilityLabel(model.videoPlaying ? "暫停" : "播放")

            PlainSlider(value: Binding(get: { model.videoPosition }, set: { model.scrubVideo(to: $0, editing: true) }),
                        range: 0...max(model.videoDuration, 0.1),
                        onEditingChanged: { editing in if !editing { model.scrubVideo(to: model.videoPosition, editing: false) } })
                .accessibilityLabel("播放位置")

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
            .accessibilityLabel("選擇發射模式：\(label)")
            .popover(isPresented: Binding(get: { model.showModeMenu }, set: { model.showModeMenu = $0 }), arrowEdge: .leading) {
                ModeMenuView(selectedID: model.settings.txModeID, standalone: false) { id in
                    model.settings.txModeID = id
                    model.showModeMenu = false
                }
            }
        } else {
            PopupButtonLabel(title: label, locked: true)
                .accessibilityLabel("發射模式：\(label)，發射中不能更換")
        }
    }

    private var modeNote: String {
        if model.txPhase != .idle { return "發射中不能更換模式；來源可以隨時切換，下一張生效" }
        return Modes.find(model.settings.txModeID)?.summary ?? ""
    }

    private var ready: Bool { model.canTransmit && !model.settings.callsign.isEmpty }

    @ViewBuilder private var transmitButton: some View {
        if model.txPhase == .idle {
            Button {
                model.toggleTransmit()
            } label: {
                Text("開始發射")
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
                Text(model.txPhase == .stopping ? "這張送完就停止…" : "停止發射")
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
        if model.txPhase == .transmitting, model.videoPlaysOnce { return "影片播完會自動停止，也可以隨時按停止" }
        if model.txPhase != .idle { return "送完目前這張後放開 PTT，自動回到接收" }
        if model.settings.callsign.isEmpty { return "還沒設定呼號，請先到設定裡填寫" }
        if model.live, !model.canTransmit { return "等來源有畫面後才能發射" }
        if model.videoPlaysOnce { return "以 \(model.settings.callsign) 發射，影片播完自動停止" }
        return "以 \(model.settings.callsign) 發射，停止後自動回到接收"
    }
}
