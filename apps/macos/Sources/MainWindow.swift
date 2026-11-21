// 主視窗：頂列、左側主區（畫面、訊號、最近收到、頻譜）、右側欄、底列。
import SwiftUI

struct MainWindowView: View {
    @EnvironmentObject var model: AppModel
    /// 算示範圖時沒有真的視窗控制鈕，照版面稿畫三個灰點
    var placeholderControls = false

    var body: some View {
        VStack(spacing: 0) {
            HeaderBar(placeholderControls: placeholderControls)
            Rectangle().fill(Theme.barBorder).frame(height: 1)
            HStack(spacing: 0) {
                MainColumn()
                Rectangle().fill(Theme.barBorder).frame(width: 1)
                SidePanel().frame(width: 319)
            }
            Rectangle().fill(Theme.barBorder).frame(height: 1)
            FooterBar()
        }
        .font(Theme.ui(13))
        .foregroundColor(Theme.text)
        .background(Theme.background)
        .environment(\.colorScheme, .dark)
    }
}

struct HeaderBar: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.openWindow) private var openWindow
    let placeholderControls: Bool
    /// 頂列的高度：上下各 10 的留白加上 32 的內容
    static let height: CGFloat = 52

    var body: some View {
        HStack(spacing: 16) {
            HStack(spacing: 14) {
                // 真的視窗在這個位置是紅黃綠三個控制鈕（TitlebarButtonLayout 把它們排到這裡），留出它們的寬度；
                // 算示範圖時照版面稿畫三個灰點
                if placeholderControls {
                    HStack(spacing: 8) {
                        ForEach(0..<3, id: \.self) { _ in Circle().fill(Theme.trafficLight).frame(width: 12, height: 12) }
                    }
                    .accessibilityHidden(true)
                } else {
                    Color.clear.frame(width: TitlebarMetrics.buttonsWidth, height: 12).accessibilityHidden(true)
                }
                Text("短波 NBTV").font(Theme.ui(13, .semibold))
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            StatusPill()

            HStack(spacing: 10) {
                Text("我的呼號").font(Theme.ui(12)).foregroundColor(Theme.secondary)
                Text(model.settings.callsign.isEmpty ? "未設定" : model.settings.callsign)
                    .font(Theme.mono(13, .semibold))
                    .foregroundColor(model.settings.callsign.isEmpty ? Theme.tertiary : Theme.text)
                Button {
                    openWindow(id: "settings")
                } label: {
                    Icon(kind: .settings)
                        .frame(width: 32, height: 32)
                        .background(RoundedRectangle(cornerRadius: 6).fill(Theme.button))
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .padding(.leading, 6)
                .accessibilityLabel("設定")
            }
            .frame(maxWidth: .infinity, alignment: .trailing)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
        .background(Theme.bar)
    }
}

/// 頂列中間的狀態膠囊：接收中（青）或發射中（橘）。
struct StatusPill: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        if model.txPhase != .idle {
            HStack(spacing: 8) {
                Circle().fill(Theme.txInk).frame(width: 8, height: 8)
                Text(model.txPhase == .starting ? "準備發射" : "發射中").font(Theme.ui(13, .bold))
                if model.isOnAir { Text(AppModel.clock(model.txElapsed)).font(Theme.mono(13, .semibold)) }
            }
            .foregroundColor(Theme.txInk)
            .padding(.horizontal, 14)
            .frame(height: 32)
            .background(RoundedRectangle(cornerRadius: 16).fill(Theme.tx))
        } else {
            HStack(spacing: 8) {
                Circle().fill(Theme.rx).frame(width: 8, height: 8)
                Text("接收中").font(Theme.ui(13, .semibold)).foregroundColor(Theme.rx)
                Text(detail).foregroundColor(Theme.secondary)
            }
            .padding(.horizontal, 14)
            .frame(height: 32)
            // 外框用兩層實心的圓角矩形疊出來（半圓的兩端用描邊畫會多出一小段直線）
            .background(RoundedRectangle(cornerRadius: 15).fill(Theme.rxPill).padding(1))
            .background(RoundedRectangle(cornerRadius: 16).fill(Theme.rxPillRing))
        }
    }

    private var detail: String {
        guard model.synced else { return "等待訊號" }
        return model.syncCallsign.isEmpty ? "已同步" : "已同步 \(model.syncCallsign)"
    }
}

struct FooterBar: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        HStack(spacing: 24) {
            item("音訊輸入", model.inputDeviceName)
            item("音訊輸出", model.outputDeviceName)
            item("PTT", model.settings.pttSummary)
            Spacer(minLength: 12)
            if let notice = model.notice {
                Button {
                    model.notice = nil
                } label: {
                    Text(notice).foregroundColor(Theme.tx).lineLimit(1).truncationMode(.tail)
                }
                .buttonStyle(.plain)
                .help("按一下關閉這則訊息")
            }
        }
        .font(Theme.ui(12))
        .foregroundColor(Theme.secondary)
        .padding(.horizontal, 16)
        .frame(height: 28)
        .background(Theme.bar)
    }

    private func item(_ label: String, _ value: String) -> some View {
        HStack(spacing: 12) {
            Text(label)
            Text(value).foregroundColor(Theme.text).lineLimit(1)
        }
    }
}

/// 左側主區。畫面最寬 576 pt（96×72 的六倍）；視窗變大時多出來的寬度給右邊的面板、高度給頻譜。
/// 版面稿是用 CSS 的彈性盒排的，這裡照同樣的規則把各塊的大小算出來：
///   * 右邊一欄至少 280 pt 寬，縮圖三張一列、隨欄寬放大（最多 128 pt 寬）
///   * 上半部的高度取「畫面加資訊列」與「訊號面板加最近收到」兩者較高的，但要留至少 192 pt 給頻譜
struct MainColumn: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        GeometryReader { geo in
            let metrics = Metrics(width: geo.size.width - 40, height: geo.size.height - 36, frames: min(model.history.count, 6))
            VStack(spacing: 16) {
                HStack(alignment: .top, spacing: 16) {
                    PictureSection(pictureHeight: metrics.pictureHeight)
                        .frame(width: metrics.pictureWidth, height: metrics.topHeight)
                    VStack(spacing: 12) {
                        StatusPanel().frame(maxHeight: .infinity)
                        RecentPanel(thumbnailWidth: metrics.thumbnailWidth)
                    }
                    .frame(maxWidth: .infinity)
                    .frame(height: metrics.topHeight)
                }
                SpectrumSection().frame(maxHeight: .infinity)
            }
            .padding(.top, 20)
            .padding(.horizontal, 20)
            .padding(.bottom, 16)
        }
    }

    struct Metrics {
        let pictureWidth: CGFloat
        let pictureHeight: CGFloat
        let topHeight: CGFloat
        let thumbnailWidth: CGFloat

        init(width: CGFloat, height: CGFloat, frames: Int) {
            let infoBar = PictureSection.infoBarHeight
            let statusMinimum: CGFloat = 214        // 訊號面板放得下所有列的最小高度
            let spectrumMinimum: CGFloat = 192
            let tallest = max(300, height - 16 - spectrumMinimum)

            var picture = min(576, max(288, width - 16 - 280))
            let column = width - picture - 16
            let rows: CGFloat = frames == 0 ? 0 : (frames > 3 ? 2 : 1)
            var thumbnail = min(128, max(40, (column - 24 - 16) / 3))
            // 最近收到的高度：留白與標題 52，加上每一列縮圖（4:3）與下面 18 的說明；沒有畫面時是固定的提示框
            func recentHeight(_ thumbnail: CGFloat) -> CGFloat {
                rows == 0 ? 242 : 52 + rows * (thumbnail * 0.75 + 18) + (rows - 1) * 10
            }
            var top = max(picture * 0.75 + infoBar, statusMinimum + 12 + recentHeight(thumbnail))
            if top > tallest {
                // 視窗不夠高：先縮縮圖，再不夠就縮畫面
                top = tallest
                if rows > 0 {
                    let available = top - statusMinimum - 12
                    thumbnail = min(thumbnail, max(40, ((available - 52 - (rows - 1) * 10) / rows - 18) * 4 / 3))
                }
                picture = min(picture, (top - infoBar) * 4 / 3)
            }
            pictureWidth = picture
            pictureHeight = picture * 0.75
            topHeight = top
            thumbnailWidth = thumbnail
        }
    }
}
