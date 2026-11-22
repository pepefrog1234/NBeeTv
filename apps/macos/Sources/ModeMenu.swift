// 發射模式選單：數位 0–5、類比 8–10，以及（app 裡有權重檔時）學習式 11–13。每一列的資料都取自核心的模式表。
import SwiftUI

struct ModeMenuView: View {
    let selectedID: Int
    /// true：自己畫圓角外框與陰影（示範圖）；false：放在系統的彈出面板裡
    var standalone = true
    let onSelect: (Int) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            // 欄位標題
            HStack(spacing: 8) {
                Color.clear.frame(width: 16, height: 1)
                Text("模式").frame(maxWidth: .infinity, alignment: .leading)
                Text("每張").frame(width: 64, alignment: .trailing)
                Text("內容").frame(width: 84, alignment: .trailing)
            }
            .font(Theme.ui(12))
            .foregroundColor(Theme.tertiary)
            .padding(.horizontal, 8)
            .frame(height: 24)
            .accessibilityHidden(true)

            group("數位 · LDPC＋AVIF", "畫面乾淨；愈往下畫質愈高或速度愈快，需要的訊號也愈好")
            ForEach(Modes.digital) { row($0) }

            Rectangle().fill(Theme.buttonRing).frame(height: 1).padding(.horizontal, 8).padding(.vertical, 6)

            group("相干類比", "訊號愈弱畫面愈糊，但不會整張收不到")
            ForEach(Modes.analog) { row($0) }

            if !Modes.learned.isEmpty {
                Rectangle().fill(Theme.buttonRing).frame(height: 1).padding(.horizontal, 8).padding(.vertical, 6)

                group("學習式 · 類神經網路（實驗中）", "弱訊號時比相干類比清楚；收發兩邊要用同一份權重")
                ForEach(Modes.learned) { row($0) }
            }
        }
        .padding(6)
        .frame(width: 368)
        .font(Theme.ui(13))
        .foregroundColor(Theme.text)
        .background(RoundedRectangle(cornerRadius: standalone ? 10 : 0).fill(Theme.menu))
        .overlay(RoundedRectangle(cornerRadius: 11).strokeBorder(standalone ? Theme.buttonRing : Color.clear, lineWidth: 1).padding(-1))
        .shadow(color: .black.opacity(standalone ? 0.45 : 0), radius: 14, y: 12)
        .environment(\.colorScheme, .dark)
    }

    private func group(_ title: String, _ detail: String) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(title).font(Theme.ui(12, .semibold)).foregroundColor(Theme.secondary).frame(height: 18)
            Text(detail).font(Theme.ui(12)).foregroundColor(Theme.tertiary).frame(minHeight: 18)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(.top, 6)
        .padding(.bottom, 4)
        .padding(.horizontal, 8)
    }

    private func row(_ mode: ModeInfo) -> some View {
        let selected = mode.id == selectedID
        return Button {
            onSelect(mode.id)
        } label: {
            HStack(spacing: 8) {
                ZStack {
                    if selected { Icon(kind: .check, width: 14, height: 14, lineWidth: 2, color: Theme.rx) }
                }
                .frame(width: 16)
                Text(mode.name).font(Theme.ui(13, selected ? .semibold : .regular)).frame(width: 64, alignment: .leading)
                Text(mode.detail).foregroundColor(Theme.secondary).frame(maxWidth: .infinity, alignment: .leading)
                Text(mode.secondsText).font(Theme.mono(13)).frame(width: 64, alignment: .trailing)
                Text(mode.contentText).foregroundColor(Theme.secondary).frame(width: 84, alignment: .trailing)
            }
            .lineLimit(1)
            .padding(.horizontal, 8)
            .frame(height: 30)
            .background(RoundedRectangle(cornerRadius: 6).fill(selected ? Theme.menuSelected : Color.clear))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(selected ? .isSelected : [])
    }
}
