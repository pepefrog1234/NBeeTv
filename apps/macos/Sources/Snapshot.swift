// 開發用：--snapshot <目錄>。不開音訊裝置、不要求權限，把五張版面各算成一張 PNG，用來對照版面稿；
// 第六張是版面稿裡沒有的定時發射面板。
// 畫面、縮圖、狀態與瀑布圖都是把內附的測試圖真的經過核心收發算出來的。
import AppKit
import SwiftUI

enum SnapshotRunner {
    private static var failures = 0

    static func run(outputDirectory: String) -> Int32 {
        let app = NSApplication.shared
        app.setActivationPolicy(.prohibited)
        // 示範圖固定用繁體中文算（不跟著開發機的系統語言變）；NBTV_LANGUAGE=<語言> 改算別的語言的版面
        // （zh-Hans、ja、es、en；要連系統的字型挑選也一樣，另外加上參數 -AppleLanguages "(ja)"）
        L10n.use(ProcessInfo.processInfo.environment["NBTV_LANGUAGE"] ?? L10n.source)
        let directory = URL(fileURLWithPath: outputDirectory, isDirectory: true)
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)

        guard let coffee = Demo.testImage("coffee"), let rocket = Demo.testImage("rocket"), let cat = Demo.testImage("cat") else {
            print("[snapshot] 找不到內附的測試圖")
            return 1
        }
        guard [11, 12, 13].allSatisfy({ Modes.find($0) != nil }) else {
            print("[snapshot] app 裡少了模式 11、12 或 13 的權重檔，無法產生示範資料")
            return 1
        }

        // 1. 接收中
        let receiving = makeReceivingModel(coffee: coffee, rocket: rocket, cat: cat)
        render(MainWindowView(placeholderControls: true).environmentObject(receiving), width: 1280, height: 840,
               to: directory.appendingPathComponent("1-receiving.png"))

        // 開發用：設了 NBTV_SNAPSHOT_EXTRA_SIZE=寬x高 時，多算一張其他視窗大小的「接收中」，用來檢查版面縮放
        if let extra = ProcessInfo.processInfo.environment["NBTV_SNAPSHOT_EXTRA_SIZE"] {
            let parts = extra.split(separator: "x").compactMap { Double($0) }
            if parts.count == 2 {
                render(MainWindowView(placeholderControls: true).environmentObject(receiving), width: parts[0], height: parts[1],
                       to: directory.appendingPathComponent("extra-\(Int(parts[0]))x\(Int(parts[1])).png"))
            }
        }

        // 開發用：NBTV_SNAPSHOT_HISTORY=<張數> 把收到的畫面重複到這麼多張，多算兩張「接收中」：
        // 很多張時的樣子（many-received.png），以及「最近收到」往下捲 NBTV_SNAPSHOT_SCROLL 點（預設 300）之後的樣子（many-received-scrolled.png）。
        // 配合 NBTV_SNAPSHOT_SNR=1（打開「在收到的畫面上標示訊雜比」）可以一起看標示的樣子。
        let environment = ProcessInfo.processInfo.environment
        if let count = environment["NBTV_SNAPSHOT_HISTORY"].flatMap({ Int($0) }), !receiving.history.isEmpty {
            let original = receiving.history
            var many = original
            while many.count < count {
                let source = original[many.count % original.count]
                many.append(AppModel.ReceivedFrame(image: source.image, modeID: source.modeID,
                                                   callsign: source.callsign, sequence: source.sequence,
                                                   time: (many.last?.time ?? Date()).addingTimeInterval(-3),
                                                   snrDb: source.snrDb - Double(many.count % 9) * 1.7))
            }
            receiving.history = many
            render(MainWindowView(placeholderControls: true).environmentObject(receiving), width: 1280, height: 840,
                   to: directory.appendingPathComponent("many-received.png"))
            render(MainWindowView(placeholderControls: true).environmentObject(receiving), width: 1280, height: 840,
                   to: directory.appendingPathComponent("many-received-scrolled.png"),
                   scrollBy: environment["NBTV_SNAPSHOT_SCROLL"].flatMap { Double($0) } ?? 300)
            receiving.history = original
        }

        // 2. 發射中
        let transmitting = makeTransmittingModel(coffee: coffee, rocket: rocket, cat: cat, history: receiving.history)
        render(MainWindowView(placeholderControls: true).environmentObject(transmitting), width: 1280, height: 840,
               to: directory.appendingPathComponent("2-transmitting.png"))

        // 3. 待機接收＋影片檔來源。開發用：NBTV_SNAPSHOT_SCHEDULE=1 時定時發射是開著的（頂列的按鈕變成橘色、寫下一次的時刻）
        let idle = makeIdleModel(rocket: rocket, workDirectory: directory)
        idle.clock = { time(14, 32, 7) }   // 示範的鐘點：下一次是 14:45
        if ProcessInfo.processInfo.environment["NBTV_SNAPSHOT_SCHEDULE"] != nil { idle.setScheduleArmed(true) }
        render(MainWindowView(placeholderControls: true).environmentObject(idle), width: 1280, height: 840,
               to: directory.appendingPathComponent("3-idle-video.png"))

        // 開發用：畫布的字經過各個模式的編碼之後的樣子（NBTV_SNAPSHOT_CANVAS 有設才算）
        if let style = receiving.settings.canvasStyle { renderCanvasSheet(style, pictures: [coffee, rocket, cat], to: directory) }

        // 4. 設定
        render(SettingsView(placeholderControls: true).environmentObject(receiving), width: 560, height: SettingsView.windowHeight,
               to: directory.appendingPathComponent("4-settings.png"))

        // 5. 發射模式選單：欄位標題、一組說明，每個模式一列 30 點，外加四周的留白
        // 別的語言的說明折成比較多行時，照內容實際的高度
        let menuContent = ModeMenuView(selectedID: Modes.defaultID, standalone: true) { _ in }.padding(16).frame(width: 400)
        let menuHeight = max(112 + 30 * CGFloat(Modes.menuOrder.count), ceil(NSHostingView(rootView: menuContent).fittingSize.height))
        let menu = menuContent
            .frame(width: 400, height: menuHeight, alignment: .top)
            .background(Theme.background)
        render(menu, width: 400, height: menuHeight, to: directory.appendingPathComponent("5-mode-menu.png"))

        // 6. 定時發射的面板（開著、已經選好影片的樣子）
        idle.setScheduleArmed(true)
        let scheduleContent = ScheduleMenuView(standalone: true).environmentObject(idle).padding(16).frame(width: 372)
        let scheduleHeight = max(430, ceil(NSHostingView(rootView: scheduleContent).fittingSize.height))
        let schedule = scheduleContent
            .frame(width: 372, height: scheduleHeight, alignment: .top)
            .background(Theme.background)
        render(schedule, width: 372, height: scheduleHeight, to: directory.appendingPathComponent("6-schedule.png"))

        print(failures == 0 ? "[snapshot] 完成：\(directory.path)" : "[snapshot] 有 \(failures) 張沒有算出來")
        return failures == 0 ? 0 : 1
    }

    // MARK: 示範資料

    private static func demoSettings(source: SourceKind) -> AppSettings {
        let settings = AppSettings(store: nil)
        settings.callsign = "BX4ACP"
        settings.outputLevelDb = -6
        settings.pttMode = .serial
        settings.serialPort = "/dev/cu.usbserial-1420"
        settings.serialLine = .rts
        settings.rigModel = 3073
        settings.rigModelName = "Icom IC-7300"
        settings.serialBaud = 19200
        // 開發用：NBTV_SNAPSHOT_CANVAS=<文字> 打開畫布、印這些字（NBTV_SNAPSHOT_CANVAS_POSITION=bottom 印在下緣），
        // 右側欄的預覽與「發射中」的畫面上看得到；另外多算一張 canvas-modes.png（各個模式編碼之後字還看不看得清楚）
        if let text = ProcessInfo.processInfo.environment["NBTV_SNAPSHOT_CANVAS"], !text.isEmpty {
            settings.canvasText = text
            settings.canvasEnabled = true
            settings.canvasPosition = CanvasPosition(rawValue: ProcessInfo.processInfo.environment["NBTV_SNAPSHOT_CANVAS_POSITION"] ?? "") ?? .top
        }
        settings.txModeID = Modes.defaultID
        settings.sourceKind = source
        // 開發用：NBTV_SNAPSHOT_SNR=1 打開「在收到的畫面上標示訊雜比」，NBTV_SNAPSHOT_NO_LOOP=1 取消「循環播放」
        settings.showFrameSNR = ProcessInfo.processInfo.environment["NBTV_SNAPSHOT_SNR"] != nil
        settings.loopVideo = ProcessInfo.processInfo.environment["NBTV_SNAPSHOT_NO_LOOP"] == nil
        return settings
    }

    private static func time(_ hour: Int, _ minute: Int, _ second: Int) -> Date {
        Calendar.current.date(bySettingHour: hour, minute: minute, second: second, of: Date()) ?? Date()
    }

    /// 對方（N0CALL）先後用模式 12、模式 11、模式 13 傳了幾張；我們從最後那一段的第 10 張開始收，目前第 13 張收到一半。
    private static func makeReceivingModel(coffee: Data, rocket: Data, cat: Data) -> AppModel {
        let model = AppModel(settings: demoSettings(source: .camera), live: false)
        model.refreshDevices()

        var audio = Demo.silence(seconds: 0.6)
        var signal: [Float] = []
        func session(mode: Int, images: [Data], keepFrom: Int = 0, partial: Int? = nil) -> [Float] {
            let tx = CoreTransmitter()
            tx.setMode(mode)
            tx.setCallsign("N0CALL")
            var out: [Float] = []
            for (index, image) in images.enumerated() {
                guard let frame = tx.encode(rgb: image) else { continue }
                if index < keepFrom { continue }
                if index == images.count - 1, let partial {
                    out += frame.samples[0..<min(partial, frame.samples.count)]
                } else {
                    out += frame.samples
                }
            }
            if partial == nil { out += tx.finish() }
            return out
        }
        let fast = session(mode: 12, images: [coffee])
        let medium = session(mode: 11, images: [rocket, cat])
        // 模式 13：共 13 張，只收第 10–12 張，以及第 13 張的前 61 個符號
        let cycle = [coffee, rocket, cat]
        let symbolSamples = Modes.find(13).map { Int(($0.frameSeconds * Double(NBTV.sampleRate) / Double($0.frameSymbols)).rounded()) } ?? 1280
        let last = session(mode: 13, images: (0..<13).map { cycle[$0 % 3] }, keepFrom: 9, partial: 61 * symbolSamples)
        signal = fast + medium + last
        // 兩次發射之間的空檔。開發用：NBTV_SNAPSHOT_GAP=秒數 可以換成別的長度——不論多長，接收機都應該解出 6 張
        // （核心曾經在 0.75–0.81 秒這幾個長度漏掉下一張或多交出一張，回歸測試在 core/tests/test_link.cpp）
        let gap = ProcessInfo.processInfo.environment["NBTV_SNAPSHOT_GAP"].flatMap(Double.init) ?? 0.8
        audio += fast + Demo.silence(seconds: gap) + medium + Demo.silence(seconds: 6.5) + last

        // 訊雜比 14 dB、電台偏了 +12 Hz、輸入音量約 −18 dB
        let received = Demo.radioChannel(audio, signalPower: Demo.power(signal), snrDb: 14, shiftHz: 12, outputLevelDb: -18)
        // 照實際接收的順序把每一塊的更新交給介面的狀態物件；時間用示範的鐘點
        let times = [time(14, 19, 58), time(14, 20, 12), time(14, 20, 15), time(14, 32, 1), time(14, 32, 4), time(14, 32, 7)]
        var decoded = 0
        var final = RxUpdate()
        var log: [String] = []
        for update in Demo.receive(received) {
            var status = update
            status.frames = []
            model.apply(status)
            for frame in update.frames {
                model.add(frame, time: times[min(decoded, times.count - 1)])
                if frame.decoded { decoded += 1 }
                log.append("\(Modes.find(frame.modeID)?.name ?? "模式 \(frame.modeID)")\(frame.decoded ? "" : "（沒有畫面）")"
                    + (frame.headerOK ? " \(frame.callsign) 第 \(frame.sequence + 1) 張" : " 標頭錯誤"))
            }
            final = update
        }
        if decoded != times.count {
            print("[snapshot] 注意：預期解出 \(times.count) 張，實際 \(decoded) 張。接收機依序交出：" + log.joined(separator: "、"))
        }
        print(String(format: "[snapshot] 接收：解出 %d 張，同步 %@，訊雜比 %.1f dB，頻偏 %+.1f Hz，進度 %d/%d，輸入 %.1f dB", decoded,
                     final.synced ? "是" : "否", final.snrDb, final.offsetHz, final.symbolsDone, final.symbolsTotal, final.inputLevelDb))
        if let source = NBTV.image(fromRGB: coffee), let scaled = FrameScaler.scale(source, mode: .fill) {
            model.sourcePreview = NBTV.image(fromRGB: model.applyCanvas(to: scaled))
            model.sourceSize = CGSize(width: source.width, height: source.height)
        }
        return model
    }

    /// 我們用模式 11 發射：已送出 8 張，第 9 張送到 1.6 秒。
    private static func makeTransmittingModel(coffee: Data, rocket: Data, cat: Data, history: [AppModel.ReceivedFrame]) -> AppModel {
        let model = AppModel(settings: demoSettings(source: .camera), live: false)
        model.refreshDevices()
        model.history = history
        let tx = CoreTransmitter()
        tx.setMode(11)
        tx.setCallsign(model.settings.callsign)
        let cycle = [rocket, cat, coffee]
        let frames = (0..<9).compactMap { tx.encode(rgb: model.applyCanvas(to: cycle[$0 % 3])) }
        guard let current = frames.last, let mode = Modes.find(11) else { return model }
        let progress = 1.6
        model.txPhase = .transmitting
        model.txModeID = 11
        model.txFrameNumber = frames.count
        model.txFramesSent = frames.count - 1
        model.txFrameProgress = progress
        model.txElapsed = Double(frames.count - 1) * mode.frameSeconds + progress
        model.txPreview = NBTV.image(fromRGB: current.preview)
        // 瀑布圖：送出去的訊號（前面幾張加上目前這張的前 1.6 秒）
        var sent: [Float] = []
        for frame in frames.dropLast().suffix(3) { sent += frame.samples }
        sent += current.samples[0..<Int(progress * Double(NBTV.sampleRate))]
        let gain = Float(pow(10.0, model.settings.outputLevelDb / 20.0))
        let rows = Demo.monitorRows(sent.map { $0 * gain })
        model.appendSpectrum(rows, reference: .transmit)
        print("[snapshot] 發射：第 \(frames.count) 張（\(mode.label)），瀑布圖 \(rows.count) 列")
        if let source = NBTV.image(fromRGB: coffee), let scaled = FrameScaler.scale(source, mode: .fill) {
            model.sourcePreview = NBTV.image(fromRGB: model.applyCanvas(to: scaled))
            model.sourceSize = CGSize(width: source.width, height: source.height)
        }
        return model
    }

    /// 沒有訊號，只有電台通帶內的雜訊；發射來源選影片檔。
    private static func makeIdleModel(rocket: Data, workDirectory: URL) -> AppModel {
        let model = AppModel(settings: demoSettings(source: .video), live: false)
        model.refreshDevices()
        let noise = Demo.radioChannel(Demo.silence(seconds: 22), signalPower: 0, snrDb: nil, shiftHz: 0, noiseOnlyLevelDb: -32, seed: 7)
        var falseFrames = 0
        var last = RxUpdate()
        for update in Demo.receive(noise) {
            falseFrames += update.frames.count
            model.apply(update)
            last = update
        }
        print(String(format: "[snapshot] 待機：同步 %@，誤解出 %d 張，輸入 %.1f dB", last.synced ? "是" : "否", falseFrames, last.inputLevelDb))

        // 影片檔來源：用測試圖做一段 38 秒的影片，照正常流程開啟、跳到 12 秒、取那一格
        let url = workDirectory.appendingPathComponent("rocket_launch.mp4")
        if let image = NBTV.image(fromRGB: rocket), Demo.makeVideo(at: url, from: image) {
            var opened: Bool?
            model.openVideo(url, autoplay: false) { opened = $0 }
            _ = Demo.wait(timeout: 10) { opened != nil }
            if opened == true {
                model.scrubVideo(to: 12, editing: false)
                let got = Demo.wait(timeout: 10) { model.demoGrabVideoFrame() }
                print("[snapshot] 影片檔：\(url.lastPathComponent)，長度 \(AppModel.clock(model.videoDuration))，取到畫面：\(got ? "是" : "否")")
            } else {
                print("[snapshot] 注意：示範影片無法開啟")
            }
        } else {
            print("[snapshot] 注意：示範影片做不出來")
        }
        return model
    }

    /// 畫布的字經過編碼之後還看不看得清楚：每一列是一張測試圖，最左邊是印好字的原圖，往右是各個模式（由快到慢）
    /// 編碼之後、沒有雜訊時對方看到的樣子。每一格放大四倍。
    private static func renderCanvasSheet(_ style: CanvasStyle, pictures: [Data], to directory: URL) {
        guard let layer = CanvasLayer(style: style) else { return }
        let zoom = 4, gap = 8
        let cellWidth = NBTV.imageWidth * zoom, cellHeight = NBTV.imageHeight * zoom
        let columns = Modes.menuOrder.count + 1
        let width = columns * cellWidth + (columns + 1) * gap, height = pictures.count * cellHeight + (pictures.count + 1) * gap
        guard let space = CGColorSpace(name: CGColorSpace.sRGB),
              let context = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4, space: space,
                                      bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else { return }
        context.setFillColor(CGColor(red: 0.07, green: 0.08, blue: 0.1, alpha: 1))
        context.fill(CGRect(x: 0, y: 0, width: width, height: height))
        context.interpolationQuality = .none
        for (row, picture) in pictures.enumerated() {
            let stamped = layer.apply(to: picture)
            var cells = [stamped]
            for mode in Modes.menuOrder {
                let tx = CoreTransmitter()
                tx.setMode(mode.id)
                tx.setCallsign("N0CALL")
                cells.append(tx.encode(rgb: stamped)?.preview ?? Data(count: NBTV.rgbBytes))
            }
            for (column, cell) in cells.enumerated() {
                guard let image = NBTV.image(fromRGB: cell) else { continue }
                context.draw(image, in: CGRect(x: gap + column * (cellWidth + gap), y: height - (row + 1) * (cellHeight + gap),
                                               width: cellWidth, height: cellHeight))
            }
        }
        guard let image = context.makeImage(), let png = NSBitmapImageRep(cgImage: image).representation(using: .png, properties: [:]) else { return }
        let url = directory.appendingPathComponent("canvas-modes.png")
        if (try? png.write(to: url)) != nil {
            print("[snapshot] canvas-modes.png：「\(layer.lines.joined(separator: "／"))」字級 \(layer.fontSize)，字帶第 \(layer.rows.lowerBound)–\(layer.rows.upperBound - 1) 列；"
                + "各欄依序是原圖、" + Modes.menuOrder.map(\.name).joined(separator: "、"))
        }
    }

    // MARK: 算圖

    /// scrollBy：算圖之前，把畫面裡第一個可以捲動的區域（主視窗裡只有「最近收到」）往下捲這麼多點。
    private static func render<V: View>(_ view: V, width: CGFloat, height: CGFloat, to url: URL, scrollBy: Double? = nil) {
        let size = CGSize(width: width, height: height)
        // 文字的語言也交代給 SwiftUI：漢字要用哪一種字形（日文、簡體、繁體的寫法不同）是照這個挑的
        let host = NSHostingView(rootView: view.frame(width: width, height: height).environment(\.locale, Locale(identifier: L10n.language)))
        host.frame = CGRect(origin: .zero, size: size)
        let window = NSWindow(contentRect: host.frame, styleMask: [.borderless], backing: .buffered, defer: false)
        window.appearance = NSAppearance(named: .darkAqua)
        window.contentView = host
        host.layoutSubtreeIfNeeded()
        Demo.spin(0.3)
        host.layoutSubtreeIfNeeded()
        if let scrollBy {
            func find(_ view: NSView) -> NSScrollView? {
                if let scroll = view as? NSScrollView { return scroll }
                for child in view.subviews {
                    if let found = find(child) { return found }
                }
                return nil
            }
            if let scroll = find(host) {
                scroll.contentView.scroll(to: NSPoint(x: 0, y: scrollBy))
                scroll.reflectScrolledClipView(scroll.contentView)
                Demo.spin(0.4)   // 捲進來的縮圖這時候才建立
                host.layoutSubtreeIfNeeded()
                print(String(format: "[snapshot] 捲動：內容高 %.0f 點，看得到 %.0f 點，捲到 %.0f 點", scroll.documentView?.frame.height ?? 0,
                             scroll.contentView.bounds.height, scroll.contentView.bounds.origin.y))
            } else {
                print("[snapshot] 注意：找不到可以捲動的區域")
            }
        }
        let scale: CGFloat = 2
        guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(width * scale), pixelsHigh: Int(height * scale),
                                         bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                                         colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0) else {
            failures += 1
            return
        }
        rep.size = size
        host.cacheDisplay(in: host.bounds, to: rep)
        guard let png = rep.representation(using: .png, properties: [:]) else {
            failures += 1
            return
        }
        do {
            try png.write(to: url)
            print("[snapshot] \(url.lastPathComponent)（\(Int(width))×\(Int(height))）")
        } catch {
            print("[snapshot] 無法寫入 \(url.path)：\(error.localizedDescription)")
            failures += 1
        }
        window.contentView = nil
    }
}
