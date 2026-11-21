// 進入點。一般啟動是 SwiftUI app；另外有兩個開發用的模式，都不開視窗：
//   NBTV --snapshot <目錄>            把五張版面算成 PNG（對照版面稿用）
//   NBTV --selftest [--rig 主機:埠]   檢查外殼自己的零件
//   NBTV --transmit-test …            把測試圖送到指定的輸出裝置
//   NBTV --receive-test …             從指定的輸入裝置接收並印出結果
import AppKit
import SwiftUI

// 寫到已經關閉的管線或連線時回報錯誤就好，不要整個結束
signal(SIGPIPE, SIG_IGN)

let arguments = CommandLine.arguments
if let index = arguments.firstIndex(of: "--snapshot"), index + 1 < arguments.count {
    exit(SnapshotRunner.run(outputDirectory: arguments[index + 1]))
} else if arguments.contains("--selftest") {
    exit(SelfTest.run(arguments: arguments))
} else if arguments.contains("--transmit-test") {
    exit(SelfTest.transmitTest(arguments: arguments))
} else if arguments.contains("--receive-test") {
    exit(SelfTest.receiveTest(arguments: arguments))
} else {
    NBTVApp.main()
}
