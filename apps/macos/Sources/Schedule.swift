// 定時發射的時刻怎麼算：照時鐘、對齊整點的固定間隔（每 15 分鐘就是每小時的 00、15、30、45 分整）。
// 到了時間要做什麼在 AppModel 的「定時發射」那一段；設定的面板在 ScheduleMenu.swift。
import Foundation

enum TransmitSchedule {
    /// 可以選的間隔（分鐘）。都除得盡 60，所以每個小時的時刻都一樣。
    static let intervals = [5, 10, 15, 20, 30, 60]
    static let defaultInterval = 15

    /// date 之後最近的一個時刻（不含 date 本身）：分是 intervalMinutes 的倍數、秒是 0。
    static func nextFire(after date: Date, intervalMinutes: Int, calendar: Calendar = .current) -> Date {
        let step = Double(max(intervalMinutes, 1) * 60)
        // 從「這個小時的開頭」往後數，不是從某個固定的起點數：時區和世界協調時間差的不是整點的地方（例如印度的 +5:30）、
        // 或日光節約時間前後，整點的位置才對
        guard let hour = calendar.dateInterval(of: .hour, for: date) else {
            return Date(timeIntervalSinceReferenceDate: (floor(date.timeIntervalSinceReferenceDate / step) + 1) * step)
        }
        let slot = floor(date.timeIntervalSince(hour.start) / step) + 1
        return min(hour.start.addingTimeInterval(slot * step), hour.end)
    }

    /// 一小時裡是哪幾分：「00、15、30、45」。太多個時只列頭三個與最後一個：「00、05、10 … 55」。
    static func minutesText(_ intervalMinutes: Int) -> String {
        let minutes = stride(from: 0, to: 60, by: max(intervalMinutes, 1)).map { String(format: "%02d", $0) }
        guard minutes.count > 6, let last = minutes.last else { return minutes.joined(separator: "、") }
        return minutes.prefix(3).joined(separator: "、") + " … " + last
    }

    /// 「12:15」：本地時間，24 小時制。
    static func clockText(_ date: Date, calendar: Calendar = .current) -> String {
        let parts = calendar.dateComponents([.hour, .minute], from: date)
        return String(format: "%02d:%02d", parts.hour ?? 0, parts.minute ?? 0)
    }
}
