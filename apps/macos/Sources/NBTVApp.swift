// app 的進入點（一般啟動）。--snapshot 與 --selftest 在 main.swift 裡分流。
import AppKit
import SwiftUI

/// 這個 app 叫什麼。同一台電腦要同時開兩個（兩部電台互相收發）時，第二個 app 是另一個 bundle：
/// 名稱與識別碼不同，設定與系統的權限也就各存各的（見 CMakeLists.txt 的 NBTV_MACOS_SECOND_APP）。
/// 視窗上的名稱取自 bundle，才看得出眼前這個視窗是哪一個 app 的。
enum AppIdentity {
    /// 顯示在主視窗頂列的名稱：「短波 NBTV」或「短波 NBTV 2」
    static let name = Bundle.main.object(forInfoDictionaryKey: "CFBundleDisplayName") as? String ?? "短波 NBTV"
    /// 第二個 app 才有的編號（「2」）；一般的 app 沒有
    static let instance = Bundle.main.object(forInfoDictionaryKey: "NBTVInstance") as? String
    /// 設定視窗的標題：第二個 app 要標明是誰的設定
    static let settingsTitle = instance == nil ? "設定" : "設定 — \(name)"
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    /// 結束前要放開 PTT，所以需要找得到目前的狀態物件
    static weak var model: AppModel?
    private var signalSources: [DispatchSourceSignal] = []
    /// 整個執行期間都握著，系統才不會把 app 當成閒置
    private var activity: NSObjectProtocol?

    func applicationDidFinishLaunching(_ notification: Notification) {
        // 視窗被蓋住、隱藏或縮進 Dock 時，系統會讓沒在出聲的 app 打盹（App Nap），計時器跟著被拖慢；
        // 收音是靠計時器把緩衝裡的取樣取走的，拖太久緩衝會滿出來，收到一半的幀就斷了。
        // 同時開兩個 app 對傳時一定有一個不在最前面，所以整個執行期間都宣告正在做即時的工作。
        // 系統閒置時要不要睡眠不受影響。
        activity = ProcessInfo.processInfo.beginActivity(options: [.userInitiatedAllowingIdleSystemSleep, .latencyCritical],
                                                         reason: "即時收發音訊")

        // 被要求結束（kill、終端機的 Ctrl-C）時也走正常的結束流程，才會放開 PTT；預設的處理是當場結束行程
        for number in [SIGTERM, SIGINT] {
            signal(number, SIG_IGN)
            let source = DispatchSource.makeSignalSource(signal: number, queue: .main)
            source.setEventHandler { NSApp.terminate(nil) }
            source.resume()
            signalSources.append(source)
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    func applicationWillTerminate(_ notification: Notification) {
        AppDelegate.model?.shutdown()
    }
}

struct NBTVApp: App {
    /// 隱藏標題列的視窗，內容上方被標題列佔去的高度
    static let titleBar = TitlebarMetrics.titleBarHeight

    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var model: AppModel

    init() {
        let model = AppModel(settings: DebugOptions.settings() ?? AppSettings(), live: true)
        AppDelegate.model = model
        _model = StateObject(wrappedValue: model)
    }

    var body: some Scene {
        Window(AppIdentity.name, id: "main") {
            // 內容會延伸到標題列底下（標題列高 titleBar），所以排版區的高度要扣掉它，
            // 整個視窗才是版面稿的 1280×840；最小可以縮到 1100×820。
            MainWindowView()
                .environmentObject(model)
                .frame(minWidth: 1100, idealWidth: 1280, minHeight: 820 - NBTVApp.titleBar, idealHeight: 840 - NBTVApp.titleBar)
                .ignoresSafeArea()   // 頂列畫到標題列的位置，視窗控制鈕疊在上面
                .background(TitlebarButtons(barHeight: HeaderBar.height))
                .preferredColorScheme(.dark)
                .onAppear {
                    model.start()
                    DebugOptions.run(model)
                }
        }
        .windowStyle(.hiddenTitleBar)
        .defaultSize(width: 1280, height: 840)
        .commands {
            CommandGroup(replacing: .appSettings) { OpenSettingsCommand() }
        }

        Window(AppIdentity.settingsTitle, id: "settings") {
            SettingsView(height: SettingsView.windowHeight - NBTVApp.titleBar)
                .environmentObject(model)
                .ignoresSafeArea()
                .background(TitlebarButtons(barHeight: SettingsView.headerHeight))
                .preferredColorScheme(.dark)
        }
        .windowStyle(.hiddenTitleBar)
        .windowResizability(.contentSize)
    }
}

/// 功能表「設定…」（⌘,）
private struct OpenSettingsCommand: View {
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        Button("設定…") { openWindow(id: "settings") }
            .keyboardShortcut(",", modifiers: .command)
    }
}

/// 開發用的環境變數，用來在沒有人操作的情況下驗證真的 app（不是離屏算圖）：
///   NBTV_DEBUG_SETTINGS="鍵=值;…"   用暫時的設定啟動，不讀也不寫 UserDefaults。可用的鍵：
///       callsign、input、output（音訊裝置名稱的一部分）、ptt（none／serial／rigctld）、rig（主機:連接埠）、
///       mode（發射模式編號）、source（camera／video）、loop（0／1：影片循環播放）、snr（0／1：在收到的畫面上標示訊雜比）
///   NBTV_DEBUG_VIDEO=<影片檔>        啟動後開啟這個影片檔當發射來源
///   NBTV_DEBUG_DUMP=<目錄>           dump 動作把視窗的 PNG 存在這裡（沒設就只印狀態）
///   NBTV_DEBUG_SCRIPT="秒:動作;…"    主視窗出現後，在指定的秒數執行動作，並把收發流程的轉折印出來。動作：
///       tx、stop、toggle             開始發射、停止發射、按一下發射鈕
///       input=<名稱>、output=<名稱>   換音訊裝置（名稱的一部分；找不到就是系統預設）
///       source=camera|video、mode=<編號>、scale=fill|fit
///       settings                     執行功能表的「設定…」
///       resize=<寬>x<高>             把主視窗改成這個大小
///       click=<x>,<y>[,<視窗>]       在視窗裡按一下滑鼠（座標從視窗左上角算；視窗是 main、settings 或 popover，預設 main）
///       drag=<x1>,<y1>,<x2>,<y2>[,<視窗>]  按住滑鼠從第一點拖到第二點再放開（拉滑桿用），約 0.3 秒
///       menu=<文字>                  之後跳出的下拉選單（音訊裝置、序列埠、鏡頭）不真的跳出來，直接選標題含這段文字的那一項
///       savepanel                    存檔或開檔的對話框開著時：印出預設的檔名，然後按取消
///                                    （對話框是系統另一個行程畫的，沒辦法從這裡替使用者按下「儲存」）
///       modes                        打開發射模式選單
///       select=<n>                   點「最近收到」的第 n 張（1 起算）
///       dump                         印出狀態，並把每個視窗的內容存成 PNG
///       quit                         結束 app（走正常的結束流程，會放開 PTT）
///   例：NBTV_DEBUG_SCRIPT="2:tx;9.5:stop;12:dump;13:quit"
enum DebugOptions {
    private static var environment: [String: String] { ProcessInfo.processInfo.environment }
    private static let launch = Date()
    private static var dumps = 0

    private static func log(_ text: String) {
        print(String(format: "[debug %7.3f] %@", Date().timeIntervalSince(launch), text))
    }

    private static func inputUID(_ name: String) -> String { AudioDevices.inputs().first { $0.name.contains(name) }?.uid ?? "" }
    private static func outputUID(_ name: String) -> String { AudioDevices.outputs().first { $0.name.contains(name) }?.uid ?? "" }

    static func settings() -> AppSettings? {
        guard let text = environment["NBTV_DEBUG_SETTINGS"] else { return nil }
        let settings = AppSettings(store: nil)
        for pair in text.split(separator: ";") {
            let parts = pair.split(separator: "=", maxSplits: 1).map(String.init)
            guard parts.count == 2 else { continue }
            let value = parts[1]
            switch parts[0] {
            case "callsign": settings.callsign = NBTV.normalizeCallsign(value)
            case "input": settings.inputDeviceUID = inputUID(value)
            case "output": settings.outputDeviceUID = outputUID(value)
            case "ptt": settings.pttMode = PTTMode(rawValue: value) ?? .none
            case "rig":
                let address = value.split(separator: ":").map(String.init)
                if address.count == 2 {
                    settings.rigHost = address[0]
                    settings.rigPort = address[1]
                }
            case "mode": settings.txModeID = Int(value).flatMap { Modes.find($0)?.id } ?? Modes.defaultID
            case "source": settings.sourceKind = SourceKind(rawValue: value) ?? .camera
            case "loop": settings.loopVideo = value != "0"
            case "snr": settings.showFrameSNR = value != "0"
            case "merge": settings.mergeFrames = value != "0"
            default: print("[debug] 不認得的設定：\(parts[0])")
            }
        }
        return settings
    }

    static func run(_ model: AppModel) {
        setvbuf(stdout, nil, _IOLBF, 0)
        _ = launch   // 時間從這裡起算
        if let path = environment["NBTV_DEBUG_VIDEO"] {
            model.openVideo(URL(fileURLWithPath: path)) { ok in log("開啟影片檔：\(ok ? "成功" : "失敗")") }
        }
        guard let script = environment["NBTV_DEBUG_SCRIPT"] else { return }
        AppModel.trace = { log($0) }
        for step in script.split(separator: ";") {
            let parts = step.split(separator: ":", maxSplits: 1).map(String.init)
            guard parts.count == 2, let seconds = Double(parts[0]) else {
                print("[debug] 看不懂的步驟：\(step)")
                continue
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + seconds) { perform(parts[1], model) }
        }
    }

    private static func perform(_ action: String, _ model: AppModel) {
        let parts = action.split(separator: "=", maxSplits: 1).map(String.init)
        let value = parts.count > 1 ? parts[1] : ""
        switch parts[0] {
        case "tx":
            log("開始發射（canTransmit = \(model.canTransmit)）")
            model.startTransmit()
        case "stop":
            log("停止發射：已送出 \(model.txFramesSent) 張，目前第 \(model.txFrameNumber) 張，這張已送 \(String(format: "%.2f", model.txFrameProgress)) 秒")
            model.stopTransmit()
        case "toggle":
            log("按發射鈕")
            model.toggleTransmit()
        case "input":
            log("輸入裝置改成「\(value)」")
            model.settings.inputDeviceUID = inputUID(value)
        case "output":
            log("輸出裝置改成「\(value)」")
            model.settings.outputDeviceUID = outputUID(value)
        case "source":
            log("發射來源改成 \(value)")
            model.setSourceKind(SourceKind(rawValue: value) ?? .camera)
        case "mode":
            log("發射模式改成 \(value)")
            if let id = Int(value), Modes.find(id) != nil { model.settings.txModeID = id }
        case "scale":
            log("縮放方式改成 \(value)")
            model.settings.scaleMode = ScaleMode(rawValue: value) ?? .fill
        case "settings":
            let item = NSApp.mainMenu?.items.first?.submenu?.items.first { $0.title.hasPrefix("設定") }
            if let item, let action = item.action {
                log("執行功能表「\(item.title)」：\(NSApp.sendAction(action, to: item.target, from: item) ? "成功" : "失敗")")
            } else {
                log("功能表裡找不到「設定…」")
            }
        case "resize":
            let size = value.split(separator: "x").compactMap { Double($0) }
            if size.count == 2, let window = NSApp.windows.first(where: { $0.identifier?.rawValue.hasPrefix("main") == true }) {
                log("主視窗改成 \(Int(size[0]))×\(Int(size[1]))")
                var frame = window.frame
                frame.origin.y += frame.height - size[1]   // 左上角不動
                frame.size = NSSize(width: size[0], height: size[1])
                window.setFrame(frame, display: true)
            }
        case "click":
            let fields = value.split(separator: ",").map(String.init)
            let prefix = fields.count > 2 ? fields[2] : "main"
            guard fields.count >= 2, let x = Double(fields[0]), let y = Double(fields[1]),
                  let window = NSApp.windows.first(where: {
                      prefix == "popover" ? $0.isVisible && $0.className.contains("Popover") : $0.identifier?.rawValue.hasPrefix(prefix) == true
                  }) else {
                log("click：找不到視窗或座標不對")
                return
            }
            // 把滑鼠事件放進 app 的事件佇列，走和使用者按滑鼠一樣的路徑（事件迴圈取出來、找出游標底下的視圖、交給它）
            if prefix != "popover" { window.makeKeyAndOrderFront(nil) }
            let point = NSPoint(x: x, y: window.frame.height - y)
            for (type, clicks) in [(NSEvent.EventType.leftMouseDown, 1), (.leftMouseUp, 1)] {
                if let event = NSEvent.mouseEvent(with: type, location: point, modifierFlags: [], timestamp: ProcessInfo.processInfo.systemUptime,
                                                  windowNumber: window.windowNumber, context: nil, eventNumber: 0, clickCount: clicks, pressure: 1) {
                    NSApp.postEvent(event, atStart: false)
                }
            }
            log("在「\(window.title)」的 (\(Int(x)), \(Int(y))) 按了一下")
        case "drag":
            let fields = value.split(separator: ",").map(String.init)
            let prefix = fields.count > 4 ? fields[4] : "main"
            let numbers = fields.prefix(4).compactMap { Double($0) }
            guard numbers.count == 4, let window = NSApp.windows.first(where: { $0.identifier?.rawValue.hasPrefix(prefix) == true }) else {
                log("drag：找不到視窗或座標不對")
                return
            }
            window.makeKeyAndOrderFront(nil)
            let steps = 6
            for step in 0...(steps + 1) {
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.05 * Double(step)) {
                    let t = min(Double(step), Double(steps)) / Double(steps)
                    let point = NSPoint(x: numbers[0] + (numbers[2] - numbers[0]) * t,
                                        y: window.frame.height - (numbers[1] + (numbers[3] - numbers[1]) * t))
                    let type: NSEvent.EventType = step == 0 ? .leftMouseDown : (step > steps ? .leftMouseUp : .leftMouseDragged)
                    if let event = NSEvent.mouseEvent(with: type, location: point, modifierFlags: [], timestamp: ProcessInfo.processInfo.systemUptime,
                                                      windowNumber: window.windowNumber, context: nil, eventNumber: 0, clickCount: 1, pressure: 1) {
                        NSApp.postEvent(event, atStart: false)
                    }
                }
            }
            log("在「\(window.title)」從 (\(Int(numbers[0])), \(Int(numbers[1]))) 拖到 (\(Int(numbers[2])), \(Int(numbers[3])))")
        case "menu":
            popupMenuInterceptor = { items in
                log("下拉選單的項目：" + items.map { ($0.selected ? "✓" : "") + $0.title }.joined(separator: "、"))
                if let item = items.first(where: { $0.title.contains(value) }) {
                    log("選「\(item.title)」")
                    item.action()
                } else {
                    log("沒有標題含「\(value)」的項目")
                }
            }
        case "savepanel":
            guard let panel = NSApp.windows.compactMap({ $0 as? NSSavePanel }).first(where: { $0.isVisible }) else {
                log("沒有開著的存檔或開檔對話框")
                return
            }
            log("\(panel is NSOpenPanel ? "開檔" : "存檔")對話框開著，預設檔名「\(panel.nameFieldStringValue)」；按取消")
            panel.cancel(nil)
        case "modes":
            log("打開發射模式選單")
            model.showModeMenu = true
        case "select":
            if let index = Int(value), index >= 1, index <= model.history.count {
                log("點最近收到的第 \(index) 張")
                model.select(model.history[index - 1])
            }
        case "dump":
            dump(model)
        case "quit":
            log("結束 app")
            NSApp.terminate(nil)
        default:
            print("[debug] 不認得的動作：\(action)")
        }
    }

    private static func dump(_ model: AppModel) {
        dumps += 1
        for (index, window) in NSApp.windows.enumerated() {
            guard window.isVisible, let content = window.contentView, content.bounds.width > 100 else { continue }
            // 連標題列（紅黃綠三個控制鈕）一起算：畫內容視圖的上一層，也就是整個視窗的外框視圖
            let view = content.superview ?? content
            var saved = ""
            if let directory = environment["NBTV_DEBUG_DUMP"], let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) {
                view.cacheDisplay(in: view.bounds, to: rep)
                let url = URL(fileURLWithPath: directory).appendingPathComponent("dump\(dumps)-window\(index).png")
                if (try? rep.representation(using: .png, properties: [:])?.write(to: url)) != nil { saved = "，存成 \(url.lastPathComponent)" }
            }
            log("視窗 \(index)「\(window.title)」：外框 \(Int(window.frame.width))×\(Int(window.frame.height))，位置 (\(Int(window.frame.minX)), \(Int(window.frame.minY)))，"
                + "內容 \(Int(content.bounds.width))×\(Int(content.bounds.height))，可拖背景移動 \(window.isMovableByWindowBackground)\(saved)")
            // 三個控制鈕在視窗裡的位置（從左上角算）
            let buttons: [NSWindow.ButtonType] = [.closeButton, .miniaturizeButton, .zoomButton]
            let frames = buttons.compactMap { window.standardWindowButton($0) }.filter { $0.frame.width > 0 }.map { button -> String in
                let frame = button.convert(button.bounds, to: nil)
                return String(format: "x %.0f–%.0f、y %.0f–%.0f", frame.minX, frame.maxX, window.frame.height - frame.maxY, window.frame.height - frame.minY)
            }
            if !frames.isEmpty { log("  控制鈕：" + frames.joined(separator: "；")) }
        }
        let phase: String
        switch model.txPhase {
        case .idle: phase = "接收"
        case .starting: phase = "準備發射"
        case .transmitting: phase = "發射中"
        case .stopping: phase = "結束發射中"
        }
        log("設定：發射模式 \(model.settings.txModeID)，縮放 \(model.settings.scaleMode.rawValue)，來源 \(model.settings.sourceKind.rawValue)，"
            + "PTT \(model.settings.pttSummary)，輸出音量 \(Int(model.settings.outputLevelDb)) dB，循環播放 \(model.settings.loopVideo)，"
            + "模式選單\(model.showModeMenu ? "開著" : "關著")")
        log("狀態：\(phase)，同步 \(model.synced)，收到 \(model.history.count) 張，輸入音量 \(String(format: "%.1f", model.inputLevelDb)) dB，"
            + "發射第 \(model.txFrameNumber) 張／已送出 \(model.txFramesSent) 張，輸入「\(model.inputDeviceName)」，輸出「\(model.outputDeviceName)」，"
            + "影片 \(model.videoPlaying ? "播放中" : "暫停") \(String(format: "%.1f", model.videoPosition))／\(String(format: "%.1f", model.videoDuration)) 秒，"
            + "訊息：\(model.notice ?? "無")，來源訊息：\(model.sourceMessage ?? "無")")
        for frame in model.history.prefix(6) {
            log("  收到：\(Modes.find(frame.modeID)?.label ?? "?")，呼號 \(frame.callsign)，第 \(frame.sequence + 1) 張\(model.isDisplayed(frame) ? "（主畫面顯示中）" : "")")
        }
    }
}
