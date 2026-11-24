// 定時發射：頂列的按鈕，以及按下去打開的面板（開關、間隔、要播的影片、下一次的時刻與上一次的結果）。
// 時刻的算法在 Schedule.swift，到了時間做什麼在 AppModel 的「定時發射」那一段。
import SwiftUI

/// 頂列右邊的「定時發射」鈕。關著是灰的；開著是發射的橘色，寫下一次的時刻（最後一分鐘倒數）。
struct ScheduleButton: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        let active = model.scheduleArmed || model.scheduledRun
        let tint = active ? Theme.tx : Theme.secondary
        Button {
            model.showScheduleMenu.toggle()
        } label: {
            HStack(spacing: 6) {
                Icon(kind: .clock, color: tint)
                Text(model.scheduleSummary)
                    .font(Theme.ui(12, active ? .semibold : .regular))
                    .foregroundColor(tint)
                    .lineLimit(1)
            }
            .padding(.horizontal, 10)
            .frame(height: 32)
            .background(RoundedRectangle(cornerRadius: 6).fill(Theme.button))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(active ? model.scheduleSummary : "定時發射：關閉")
        .popover(isPresented: Binding(get: { model.showScheduleMenu }, set: { model.showScheduleMenu = $0 }), arrowEdge: .bottom) {
            ScheduleMenuView(standalone: false).environmentObject(model)
        }
    }
}

struct ScheduleMenuView: View {
    @EnvironmentObject var model: AppModel
    /// true：自己畫圓角外框與陰影（示範圖）；false：放在系統的彈出面板裡
    var standalone = true

    var body: some View {
        let settings = model.settings
        VStack(alignment: .leading, spacing: 12) {
            VStack(alignment: .leading, spacing: 0) {
                Text("定時發射").font(Theme.ui(13, .semibold)).frame(height: 18)
                Text("到了時間自動把影片檔從頭發射一次，播完就停止")
                    .font(Theme.ui(12)).foregroundColor(Theme.secondary).frame(height: 18)
            }

            CheckBox(isOn: Binding(get: { model.scheduleArmed }, set: { model.setScheduleArmed($0) }), title: "開啟定時發射")
                .frame(height: 18)

            VStack(alignment: .leading, spacing: 6) {
                label("每隔幾分鐘")
                SegmentedControl(options: TransmitSchedule.intervals.map { (value: $0, title: "\($0)") },
                                 selection: Binding(get: { settings.scheduleIntervalMinutes }, set: { settings.scheduleIntervalMinutes = $0 }))
                    .accessibilityLabel("每隔幾分鐘")
                Text("每小時的 \(TransmitSchedule.minutesText(settings.scheduleIntervalMinutes)) 分整")
                    .font(Theme.ui(12)).foregroundColor(Theme.secondary).frame(height: 18)
            }

            VStack(alignment: .leading, spacing: 6) {
                label("要播的影片")
                HStack(spacing: 8) {
                    HStack(spacing: 8) {
                        Icon(kind: .videoFile, lineWidth: 1.4)
                        Text(model.videoName ?? "尚未選擇影片檔")
                            .foregroundColor(model.videoName == nil ? Theme.tertiary : Theme.text)
                            .lineLimit(1)
                            .truncationMode(.middle)
                            .frame(maxWidth: .infinity, alignment: .leading)
                        if model.videoName != nil {
                            Text(AppModel.clock(model.videoDuration)).font(Theme.mono(12)).foregroundColor(Theme.secondary)
                        }
                    }
                    .padding(.horizontal, 10)
                    .frame(height: 28)
                    .background(RoundedRectangle(cornerRadius: 6).fill(Theme.disabledFill))
                    .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(Theme.separator, lineWidth: 1))
                    Button("選擇…") {
                        model.showScheduleMenu = false   // 開檔的對話框一出來，這個面板本來就會收起來
                        model.chooseVideoFile()
                    }
                    .buttonStyle(SmallButtonStyle())
                }
            }

            CheckBox(isOn: Binding(get: { settings.scheduleSkipWhenBusy }, set: { settings.scheduleSkipWhenBusy = $0 }),
                     title: "正在收別人的畫面時，那一次不發射")
                .frame(height: 18)

            Rectangle().fill(Theme.separator).frame(height: 1)

            VStack(alignment: .leading, spacing: 0) {
                ForEach(Array(statusLines.enumerated()), id: \.offset) { _, line in
                    Text(line.text).foregroundColor(line.warning ? Theme.tx : Theme.text).frame(minHeight: 18)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            .font(Theme.ui(12))

            Text("用當時選的發射模式；來源是鏡頭的話，播完會換回鏡頭。正在發射時到了時間，那一次就跳過。"
                 + "開著的時候電腦不會因為閒置而睡眠；重新開啟 app 之後要再打開一次。")
                .font(Theme.ui(12))
                .foregroundColor(Theme.tertiary)
                .lineSpacing(3)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(14)
        .frame(width: 340)
        .font(Theme.ui(13))
        .foregroundColor(Theme.text)
        .background(RoundedRectangle(cornerRadius: standalone ? 10 : 0).fill(Theme.menu))
        .overlay(RoundedRectangle(cornerRadius: 11).strokeBorder(standalone ? Theme.buttonRing : Color.clear, lineWidth: 1).padding(-1))
        .shadow(color: .black.opacity(standalone ? 0.45 : 0), radius: 14, y: 12)
        .environment(\.colorScheme, .dark)
    }

    private func label(_ text: String) -> some View {
        Text(text).font(Theme.ui(12)).foregroundColor(Theme.secondary).frame(height: 18)
    }

    /// 面板下半的狀態：下一次什麼時候、為什麼發射不了、上一次的結果。
    private var statusLines: [(text: String, warning: Bool)] {
        var lines: [(text: String, warning: Bool)] = []
        if let problem = model.scheduleProblem {
            lines.append(("\(problem)，到了時間不會發射", true))
        }
        if model.scheduledRun {
            lines.append(("正在發射這一次的影片", false))
        }
        if let next = model.scheduleNextFire {
            lines.append(("下一次 \(TransmitSchedule.clockText(next))", false))
        } else {
            lines.append(("目前關著，不會自己發射", false))
        }
        if let last = model.scheduleLastEvent {
            lines.append(("上一次：\(last)", false))
        }
        return lines
    }
}
