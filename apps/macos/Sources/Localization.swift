// 介面的語言。原始碼裡的介面文字是繁體中文（台灣的用語），同時也是每一句的索引；
// 其他語言的翻譯在 Resources/<語言>.lproj/Localizable.strings，啟動時照系統的語言載入一份。
// 給使用者看的每一句都經過 L()；開發用的輸出（--selftest、--snapshot、除錯紀錄）不翻譯，一律是繁體中文。
import Foundation

enum L10n {
    /// 有哪些語言（.lproj 的名稱）：繁體中文、簡體中文、日文、西班牙文、美式英文。其他語言一律顯示英文。
    static let languages = ["zh-Hant", "zh-Hans", "ja", "es", "en"]
    /// 原始碼用的語言：不需要翻譯表
    static let source = "zh-Hant"
    static let fallback = "en"

    /// 目前用的語言
    private(set) static var language = systemLanguage()
    private static var table = loadTable(language)
    private static var info = loadInfo(language)

    /// 系統替這個 app 挑的語言：使用者的語言清單裡排最前面、而且這個 app 有翻譯的那一個；都沒有就是英文。
    /// bundle 的開發語言設成 en，系統自己畫的部分（選單、開檔與存檔的對話框、權限的詢問）也照同樣的規則挑，
    /// 所以整個 app 會是同一種語言。在「系統設定 → 語言與地區 → 應用程式」裡替這個 app 另外指定語言也有效。
    /// 開發用：環境變數 NBTV_LANGUAGE=<語言> 直接指定（只影響這個 app 自己的文字）。
    static func systemLanguage() -> String {
        if let forced = ProcessInfo.processInfo.environment["NBTV_LANGUAGE"], languages.contains(forced) { return forced }
        return Bundle.main.preferredLocalizations.first { languages.contains($0) } ?? fallback
    }

    /// 給一份語言偏好的清單（「zh-Hant-TW」「es-419」這種代碼，愈前面愈優先），系統會挑哪一種語言。
    /// 和 systemLanguage 用的是同一套系統的比對規則，差別只在清單是自己給的（測試用）。
    static func language(forPreferences preferences: [String]) -> String {
        // 沒有一個合的時候系統回傳清單裡的某一個；把英文排最前面，那時候拿到的就是英文
        let candidates = [fallback] + languages.filter { $0 != fallback }
        return Bundle.preferredLocalizations(from: candidates, forPreferences: preferences).first { languages.contains($0) } ?? fallback
    }

    /// 開發用：改用另一種語言。自我測試與示範圖預設固定用繁體中文，才不會跟著開發機的系統語言變。
    static func use(_ newLanguage: String) {
        language = languages.contains(newLanguage) ? newLanguage : fallback
        table = loadTable(language)
        info = loadInfo(language)
    }

    /// 這一句在目前的語言裡怎麼說；沒有翻譯（或目前就是繁體中文）時是 nil
    static func translation(_ key: String) -> String? { table[key] }

    /// app 的名稱（InfoPlist.strings 的 CFBundleDisplayName）：照目前的語言
    static var appName: String? { info["CFBundleDisplayName"] }

    /// 某個語言的翻譯表。那一份檔案不在 app 裡（或讀不懂）時是空的，介面就顯示原始碼裡的繁體中文。
    static func loadTable(_ language: String) -> [String: String] {
        language == source ? [:] : strings(named: "Localizable", language: language)
    }

    private static func loadInfo(_ language: String) -> [String: String] { strings(named: "InfoPlist", language: language) }

    private static func strings(named name: String, language: String) -> [String: String] {
        guard let url = Bundle.main.url(forResource: name, withExtension: "strings", subdirectory: nil, localization: language),
              let dictionary = NSDictionary(contentsOf: url) as? [String: String] else { return [:] }
        return dictionary
    }
}

/// 介面上的一句話，照目前的語言。key 是繁體中文的原文；有要填進去的值時，原文裡用 %@（字串）、%d（整數）、%.1f 這類記號，
/// 值依序接在後面。翻譯可以用 %1$@、%2$d 調換順序。
///
/// 值裡面的第一個整數是 1、而且翻譯表裡另外有「原文#1」這一項時，用那一項（英文、西班牙文的單數：1 frame／3 frames）。
/// 沒有給值的時候原樣回傳翻譯好的句子（裡面的 % 記號不處理），可以留著晚一點再填。
func L(_ key: String, _ arguments: CVarArg...) -> String {
    var format = L10n.translation(key) ?? key
    guard !arguments.isEmpty else { return format }
    if let count = arguments.first(where: { $0 is Int }) as? Int, count == 1, let singular = L10n.translation(key + "#1") { format = singular }
    return String(format: format, arguments: arguments)
}
