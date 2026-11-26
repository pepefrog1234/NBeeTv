// 發射模式選單：app 裡有權重檔的模式（模式 11 到 14），由快到慢；給 FM 用的大畫面另外排在後面，
// 分成兩組：幀裡有導頻的（模式 15）與沒有導頻的（模式 5、6、7）。
// 每一列的資料都取自核心的模式表。
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
                Text(L("模式")).frame(maxWidth: .infinity, alignment: .leading)
                Text(L("每張")).frame(width: 64, alignment: .trailing)
                Text(L("內容")).frame(width: 84, alignment: .trailing)
            }
            .font(Theme.ui(12))
            .foregroundColor(Theme.tertiary)
            .padding(.horizontal, 8)
            .frame(height: 24)
            .accessibilityHidden(true)

            if Modes.menuOrder.isEmpty {
                group(L("沒有可以用的模式"), L("app 裡找不到模型的權重檔"))
            } else {
                let standard = Modes.menuOrder.filter { !$0.largePicture }
                let large = Modes.menuOrder.filter { $0.largePicture && !$0.pilotFree }, free = Modes.menuOrder.filter(\.pilotFree)
                if !standard.isEmpty {
                    group(L("類神經網路編碼（實驗中）"), L("訊號愈弱畫面愈糊，不會整張收不到；兩邊要用同一份權重"))
                    ForEach(standard) { row($0) }
                }
                if let picture = large.first?.pictureSize {
                    group(L("FM 語音通道用的大畫面（實驗中）"),
                          L("畫面 %@，比較清楚。給 VHF／UHF 的 FM 用：訊號弱時比上面的模式差，不適合短波", picture.text))
                    ForEach(large) { row($0) }
                }
                if let picture = free.first?.pictureSize {
                    group(L("FM 大畫面，幀裡沒有導頻（實驗中）"),
                          L("畫面 %@。同樣的時間多送 2 成的資料；只給訊號穩定的 FM 用，不適合短波", picture.text))
                    ForEach(free) { row($0) }
                }
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
                Text(mode.menuDetail).foregroundColor(Theme.secondary).frame(maxWidth: .infinity, alignment: .leading)
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
