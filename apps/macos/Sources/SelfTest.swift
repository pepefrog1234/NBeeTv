// 開發用，都不開視窗：
//   --transmit-test／--receive-test：把測試圖送到某個輸出裝置／從某個輸入裝置接收（見各函式的說明）
//   --selftest [--rig 主機:連接埠] [--no-camera] [--no-audio] [--no-devices]：逐項檢查外殼自己的零件
//   （--no-camera 不開鏡頭，--no-audio 不經過音訊裝置，--no-devices 兩者都不要；
//     --ask-camera 還沒有相機權限時向系統要求，--save-camera-frame <檔名> 把鏡頭取到的那一格存成 PPM）：
//   1. Swift 與核心之間的收發（不經音訊裝置），包含送全黑、全白的畫面
//   2. 影片檔來源（做一段影片、開啟、跳到指定位置、取一格），以及用影片當來源的整個發射流程
//      （音訊輸出換成模擬的、PTT 換成記錄用的：按下發射自動播放，播完自動停止、放開 PTT），
//      以及定時發射（另外把時鐘換成假的：到了時間自動發射、該跳過的時候跳過、超過上限自動停止）
//      把一段收到的畫面存成影片（讀回來檢查格式與每一格）、畫布（印在送出畫面上的字，含經過整個發射流程）
//   3. rigctld 的 PTT（要給 --rig，對著真的 rigctld 測）；用 CAT 指令的 PTT 用 Hamlib 不接電台的示範機測
//      （自己開的 rigctld，只聽本機的臨時連接埠，不碰任何序列埠；這台電腦沒有裝 Hamlib 就略過）
//   4. 音訊輸出（需要 BlackHole 虛擬音效卡）
//   5. 經 BlackHole 自發自收（另外需要麥克風權限）
import AppKit
import AVFoundation
import Foundation

enum SelfTest {
    private static var failures = 0

    private static func report(_ name: String, _ ok: Bool?, _ detail: String = "") {
        let mark = ok == nil ? "略過" : (ok == true ? "通過" : "失敗")
        print("[selftest] \(name)：\(mark)\(detail.isEmpty ? "" : "　\(detail)")")
        if ok == false { failures += 1 }
    }

    private static func option(_ name: String, in arguments: [String]) -> String? {
        guard let index = arguments.firstIndex(of: name), index + 1 < arguments.count else { return nil }
        return arguments[index + 1]
    }

    /// --transmit-test --device <名稱> [--mode <編號>] [--frames <張數>] [--callsign <呼號>] [--flat <0–255>]
    /// 不開視窗，把內附的測試圖用指定的模式送到指定的輸出裝置（配合另一個行程或真的 app 的接收測試）。
    /// --flat 改送整張同一個亮度的畫面（很暗、過曝或被蓋住的鏡頭拍到的就是這種畫面）。
    static func transmitTest(arguments: [String]) -> Int32 {
        NSApplication.shared.setActivationPolicy(.prohibited)
        setvbuf(stdout, nil, _IOLBF, 0)
        L10n.use(L10n.source)
        let name = option("--device", in: arguments) ?? "BlackHole"
        guard let device = AudioDevices.outputs().first(where: { $0.name.contains(name) }) else {
            print("[transmit-test] 找不到輸出裝置「\(name)」")
            return 1
        }
        let modeID = option("--mode", in: arguments).flatMap(Int.init) ?? Modes.defaultID
        let count = option("--frames", in: arguments).flatMap(Int.init) ?? 3
        var images = ["coffee", "rocket", "cat"].compactMap(Demo.testImage)
        if let level = option("--flat", in: arguments).flatMap({ UInt8($0) }) {
            images = [Data(repeating: level, count: NBTV.rgbBytes)]
        }
        guard let mode = Modes.find(modeID), !images.isEmpty else { return 1 }
        let tx = CoreTransmitter()
        tx.setMode(modeID)
        tx.setCallsign(option("--callsign", in: arguments) ?? "N0CALL")
        let output = AudioOutput()
        do {
            try output.start(deviceID: device.id)
        } catch {
            print("[transmit-test] \(error.localizedDescription)")
            return 1
        }
        var total = output.enqueue(Demo.silence(seconds: 0.5)) ? Int(0.5 * Double(NBTV.sampleRate)) : 0
        for index in 0..<count {
            guard let frame = tx.encode(rgb: images[index % images.count]) else { return 1 }
            // 佇列快空了才排下一張，和 app 的做法一樣
            _ = Demo.wait(timeout: 30) { output.queuedSamples < NBTV.sampleRate }
            output.enqueue(frame.samples)
            total += frame.samples.count
            print("[transmit-test] 送出第 \(index + 1) 張（\(mode.label)）")
        }
        let tail = tx.finish() + Demo.silence(seconds: 0.5)
        output.enqueue(tail)
        total += tail.count
        _ = Demo.wait(timeout: 60) { output.playedSamples >= Int64(total) }
        output.stop()
        print("[transmit-test] 完成，輸出佇列中途空掉 \(max(0, output.underruns - 1)) 次")
        return 0
    }

    /// --receive-test --device <名稱> [--seconds <秒數>]
    /// 不開視窗，從指定的輸入裝置接收，印出解出來的每一幀。
    static func receiveTest(arguments: [String]) -> Int32 {
        NSApplication.shared.setActivationPolicy(.prohibited)
        setvbuf(stdout, nil, _IOLBF, 0)
        L10n.use(L10n.source)
        let name = option("--device", in: arguments) ?? "BlackHole"
        let seconds = option("--seconds", in: arguments).flatMap(Double.init) ?? 15
        guard let device = AudioDevices.inputs().first(where: { $0.name.contains(name) }) else {
            print("[receive-test] 找不到輸入裝置「\(name)」")
            return 1
        }
        guard AVCaptureDevice.authorizationStatus(for: .audio) == .authorized else {
            print("[receive-test] 沒有麥克風權限")
            return 1
        }
        let receiver = CoreReceiver()
        let queue = DispatchQueue(label: "nbtv.receive-test")
        var decoded = 0
        var received = 0
        var peak: Float = 0
        let input = AudioInput()
        input.onSamples = { samples in
            queue.async {
                receiver.push(samples)
                received += samples.count
                peak = max(peak, samples.map { abs($0) }.max() ?? 0)
                while let frame = receiver.pollFrame() {
                    if frame.decoded { decoded += 1 }
                    print("[receive-test] \(Modes.find(frame.modeID)?.label ?? "?")：\(frame.decoded ? "有畫面" : "沒有畫面")，呼號 \(frame.callsign)，"
                        + "第 \(frame.sequence + 1) 張，訊雜比 \(String(format: "%.1f", frame.snrDb)) dB")
                }
            }
        }
        do {
            try input.start(deviceID: device.id)
        } catch {
            print("[receive-test] \(error.localizedDescription)")
            return 1
        }
        let began = Date()
        Demo.spin(seconds)
        input.stop()
        let elapsed = Date().timeIntervalSince(began)
        let (count, total, level) = queue.sync { (decoded, received, peak) }
        // 裝置不是 48 kHz 時要轉換：交給接收機的取樣數應該是每秒 48000 個左右
        print(String(format: "[receive-test] 「%@」（裝置取樣率 %.0f Hz）：%.1f 秒收到 %d 個取樣（每秒 %.0f 個），峰值 %.3f", device.name,
                     AudioDevices.sampleRate(device.id), elapsed, total, Double(total) / elapsed, level))
        print("[receive-test] \(Int(seconds)) 秒內收到 \(count) 張")
        return 0
    }

    static func run(arguments: [String]) -> Int32 {
        NSApplication.shared.setActivationPolicy(.prohibited)
        setvbuf(stdout, nil, _IOLBF, 0)
        // 測試裡會比對 app 產生的文字，所以固定用原始碼的語言（繁體中文），不跟著這台電腦的系統語言變；
        // 各語言的翻譯另外有一項專門檢查（testLocalization）
        L10n.use(L10n.source)
        print("[selftest] 核心 \(NBTV.version)，模式 \(Modes.all.count) 個")
        guard let coffee = Demo.testImage("coffee"), let rocket = Demo.testImage("rocket") else {
            report("內附的測試圖", false)
            return 1
        }
        // --no-camera：不開鏡頭。--no-audio：不經過音訊裝置（BlackHole 上有別的程式在收發時，自發自收的結果沒有意義，
        // 送出去的測試訊號也會混進別人的聲音裡）。--no-devices：兩者都不要。
        let devices = !arguments.contains("--no-devices")
        let useCamera = devices && !arguments.contains("--no-camera")
        let useAudio = devices && !arguments.contains("--no-audio")
        // 每個模式各有一份權重檔；少了哪一份，那個模式就不會列在 Modes 裡
        let models = Modes.all.map {
            "\($0.name)：模型 \($0.modelName)、識別碼 \(LearnedModel.idText(values: $0.learnedValues, width: $0.learnedWidth) ?? "?")、"
                + "\($0.learnedValues) 個實數\($0.largeModel ? "、大型網路" : "")"
        }
        report("各模式的模型", models.count == LearnedModel.resourceNames.count,
               models.isEmpty ? "app 裡沒有權重檔，沒有模式可以用"
                   : models.joined(separator: "；") + (models.count < LearnedModel.resourceNames.count ? "；其餘的權重檔不在 app 裡" : ""))
        guard !Modes.all.isEmpty else {
            print("[selftest] 1 項失敗")
            return 1
        }
        // 合併最近幾幀用的權重：每一份都載入得了，而且配的是 app 裡有的模型
        let merges = zip(MergeWeights.loaded, MergeWeights.modelIDs).map { String(format: "%@（配模型 %08X）", $0.name, $1) }
        report("合併最近幾幀的權重", MergeWeights.loaded.count == MergeWeights.resourceNames.count
                   && MergeWeights.modelIDs.allSatisfy { LearnedModel.ids.contains($0) },
               merges.isEmpty ? "app 裡沒有" : merges.joined(separator: "；"))
        // 開發用：--only <關鍵字> 只跑下面這一串裡關鍵字相符的幾項（改了某一塊，不必每次等全部跑完）
        let only = option("--only", in: arguments)
        func run(_ key: String, _ test: () -> Void) {
            if only == nil || key.contains(only ?? "") { test() }
        }
        run("core") { testCoreLoopback(coffee) }
        run("merge") { testMergeFrames(coffee) }
        run("flat") { testFlatPictures() }
        run("export") { testExport(coffee) }
        run("tail") { testDroppedFrameTail(coffee, rocket) }
        run("resampler") { testResampler(coffee) }
        run("videofile") { testVideoFile(rocket) }
        run("history") { testReceivedHistory(coffee) }
        run("sequence") { testSequenceVideo(coffee, rocket) }
        run("videotransmit") { testVideoTransmit(rocket) }
        run("canvas") { testCanvas(coffee) }
        run("canvas-onair") { testCanvasOnAir(rocket) }
        run("schedule") { testScheduleTimes() }
        run("schedule-transmit") { testScheduledTransmit(rocket) }
        run("safety") { testTransmitSafety(rocket) }
        run("l10n") { testLocalization() }
        if only != nil {
            run("serial") { testSerialErrors() }
            run("hamlib") { testHamlib() }
            print("[selftest] （只跑了「\(only ?? "")」相符的項目）\(failures == 0 ? "沒有失敗的項目" : "\(failures) 項失敗")")
            return failures == 0 ? 0 : 1
        }
        if useCamera {
            testCamera(ask: arguments.contains("--ask-camera"), savePath: option("--save-camera-frame", in: arguments))
        } else {
            report("鏡頭", nil, "指定了不開鏡頭")
        }
        testSerialErrors()
        testHamlib()
        if let index = arguments.firstIndex(of: "--rig"), index + 1 < arguments.count {
            testRigctld(arguments[index + 1])
        } else {
            report("rigctld PTT", nil, "沒有給 --rig 主機:連接埠")
        }
        if useAudio {
            testAudio(coffee)
        } else {
            report("音訊輸出、BlackHole 自發自收", nil, "指定了不經過音訊裝置")
        }
        if DebugTap.isEnabled, devices { testDebugTap() }
        print("[selftest] \(failures == 0 ? "沒有失敗的項目" : "\(failures) 項失敗")")
        return failures == 0 ? 0 : 1
    }

    // MARK: 1. 核心

    private static func testCoreLoopback(_ image: Data) {
        for mode in Modes.all {  // app 裡有權重檔的模式才會列在 Modes 裡
            let tx = CoreTransmitter()
            tx.setMode(mode.id)
            tx.setCallsign("n0call")
            var audio = Demo.silence(seconds: 0.3)
            var previews: [Data] = []
            for _ in 0..<2 {
                guard let frame = tx.encode(rgb: image) else {
                    report("核心收發（\(mode.name)）", false, "編碼失敗")
                    return
                }
                audio += frame.samples
                previews.append(frame.preview)
            }
            audio += tx.finish() + Demo.silence(seconds: 0.5)
            let frames = Demo.receive(audio).flatMap(\.frames)
            let good = frames.filter { $0.decoded && $0.headerOK && $0.callsign == "N0CALL" && $0.modeID == mode.id && $0.rgb != nil }
            // 收到的畫面和發射端的預覽只差削波失真；用錯模型的話解出來的只是雜訊，差得很遠
            let worst = zip(good, previews).map { difference($0.rgb ?? Data(), $1) }.max() ?? 255
            report("核心收發（\(mode.label)）", good.count == 2 && worst < 3,
                   String(format: "收到 %d/2 張，和預覽平均差 %.1f／255", good.count, worst))
        }
    }

    /// 合併最近幾幀：同一張畫面在很弱的訊號下連續送 6 張。打開合併之後，後面幾張要比不合併的接近原圖；
    /// 第一張前面沒有東西可以合，兩者一模一樣。雜訊是固定種子的亂數，每次的結果相同。
    private static func testMergeFrames(_ image: Data) {
        var checkedAppReceiver = false
        for mode in Modes.all {
            guard let modelID = LearnedModel.id(values: mode.learnedValues, width: mode.learnedWidth),
                  MergeWeights.covers(modelID: modelID) else {
                report("合併最近幾幀（\(mode.label)）", nil, "這個模式的模型沒有配合併用的權重")
                continue
            }
            let tx = CoreTransmitter()
            tx.setMode(mode.id)
            tx.setCallsign("n0call")
            var signal: [Float] = []
            for _ in 0..<6 {
                guard let frame = tx.encode(rgb: image) else {
                    report("合併最近幾幀（\(mode.label)）", false, "編碼失敗")
                    return
                }
                signal += frame.samples
            }
            signal += tx.finish()
            // 白雜訊：訊雜比 3 dB（訊號的平均功率 ÷ 3 kHz 頻寬內的雜訊功率）
            let power = signal.reduce(0.0) { $0 + Double($1) * Double($1) } / Double(max(signal.count, 1))
            let sigma = (power / pow(10.0, 0.3) * (Double(NBTV.sampleRate) / 2) / 3000).squareRoot()
            var audio = Demo.silence(seconds: 0.3) + signal + Demo.silence(seconds: 0.5)
            var state: UInt64 = 0x9E37_79B9_7F4A_7C15
            func uniform() -> Double {
                state ^= state << 13
                state ^= state >> 7
                state ^= state << 17
                return (Double(state >> 11) + 0.5) / 9_007_199_254_740_992.0
            }
            for i in audio.indices {
                audio[i] += Float(sigma * (-2 * log(uniform())).squareRoot() * cos(2 * Double.pi * uniform()))
            }
            func pictures(merging: Bool) -> [Data] {
                let receiver = CoreReceiver()
                receiver.setMerging(merging)
                return Demo.receive(audio, receiver: receiver).flatMap(\.frames).filter { $0.decoded && $0.modeID == mode.id }.compactMap(\.rgb)
            }
            let plain = pictures(merging: false), merged = pictures(merging: true)
            guard plain.count == 6, merged.count == 6 else {
                report("合併最近幾幀（\(mode.label)）", false, "收到 \(plain.count)／\(merged.count) 張，應該各 6 張")
                continue
            }
            let before = plain[3...].map { difference($0, image) }.reduce(0, +) / 3
            let after = merged[3...].map { difference($0, image) }.reduce(0, +) / 3
            report("合併最近幾幀（\(mode.label)）", plain[0] == merged[0] && after < before,
                   String(format: "訊雜比 3 dB、同一張畫面：第 4–6 張和原圖平均差 %.1f／255，不合併是 %.1f", after, before))
            // app 接收用的接收機固定開著合併（設定裡已經沒有這個選項）：解出來的要和打開合併的一模一樣，而且和沒有合併的不同
            if !checkedAppReceiver {
                checkedAppReceiver = true
                let app = Demo.receive(audio, receiver: RadioEngine.makeReceiver()).flatMap(\.frames)
                    .filter { $0.decoded && $0.modeID == mode.id }.compactMap(\.rgb)
                report("合併最近幾幀：app 的接收機固定開著", app == merged && app != plain,
                       app == merged ? "\(mode.name) 的 6 張和打開合併的逐位元相同" : "和打開合併的結果不同")
            }
        }
    }

    /// 送沒有細節的畫面（全黑、全白）：很暗、過曝或被蓋住的鏡頭拍到的就是這種畫面。
    /// 不論畫面內容是什麼，每一張都要收到，而且是原本的亮度。
    private static func testFlatPictures() {
        var details: [String] = []
        var ok = true
        for mode in Modes.all {
            for level: UInt8 in [0, 255] {
                let picture = Data(repeating: level, count: NBTV.rgbBytes)
                let tx = CoreTransmitter()
                tx.setMode(mode.id)
                tx.setCallsign("N0CALL")
                var audio = Demo.silence(seconds: 0.3)
                for _ in 0..<2 {
                    guard let frame = tx.encode(rgb: picture) else {
                        report("送全黑、全白的畫面", false, "\(mode.name) 編碼失敗")
                        return
                    }
                    audio += frame.samples
                }
                audio += tx.finish() + Demo.silence(seconds: 0.5)
                let good = Demo.receive(audio).flatMap(\.frames).filter { $0.decoded && $0.headerOK && $0.rgb != nil }
                // 收到的畫面要是原本的亮度：平均每個數值差不到 12／255
                let error = good.compactMap(\.rgb).map { rgb in
                    rgb.reduce(0.0) { $0 + abs(Double($1) - Double(level)) } / Double(max(rgb.count, 1))
                }.max() ?? 255
                if good.count != 2 || error >= 12 { ok = false }
                details.append("\(mode.name) \(level == 0 ? "全黑" : "全白") \(good.count)/2" + (good.isEmpty ? "" : String(format: "（差 %.1f）", error)))
            }
        }
        report("送全黑、全白的畫面", ok, details.joined(separator: "、"))
    }

    /// 除錯紀錄（設定了 debugTapDirectory 才測）：從 BlackHole 收兩秒（只收不放），檢查音訊檔與紀錄檔有沒有寫出來。
    private static func testDebugTap() {
        guard let directory = DebugTap.directory,
              let device = AudioDevices.inputs().first(where: { $0.name.contains("BlackHole") }) else {
            report("除錯紀錄", nil, "找不到 BlackHole 虛擬音效卡")
            return
        }
        let engine = RadioEngine()
        do {
            try engine.startReceiving(deviceID: device.id)
        } catch {
            report("除錯紀錄", false, error.localizedDescription)
            return
        }
        _ = Demo.wait(timeout: 2) { false }
        engine.stopReceiving()
        _ = Demo.wait(timeout: 0.5) { false }
        let names = (try? FileManager.default.contentsOfDirectory(atPath: directory.path)) ?? []
        func size(_ name: String) -> Int {
            ((try? FileManager.default.attributesOfItem(atPath: directory.appendingPathComponent(name).path))?[.size] as? NSNumber)?.intValue ?? 0
        }
        let audio = names.filter { $0.contains("-rx-") }.map(size).max() ?? 0
        let log = names.filter { $0.hasSuffix("-log.txt") }.map(size).max() ?? 0
        // 兩秒的 48 kHz 浮點取樣是 384000 位元組；裝置啟動要一點時間，收到一半以上就算有在寫
        report("除錯紀錄", audio > 190_000 && log > 0, "音訊檔 \(audio) 位元組、紀錄檔 \(log) 位元組，在 \(directory.path)")
    }

    // MARK: 存檔

    /// 「儲存圖片」寫出去的檔案內容：96×72 的 PNG，系統的影像解碼器讀回來要和收到的畫面完全相同。
    private static func testExport(_ image: Data) {
        var details: [String] = []
        var ok = true
        for mode in Modes.all.filter({ [11, 14].contains($0.id) }) {
            let tx = CoreTransmitter()
            tx.setMode(mode.id)
            tx.setCallsign("N0CALL/P")
            guard let encoded = tx.encode(rgb: image) else {
                report("存檔的內容", false, "編碼失敗")
                return
            }
            let audio = Demo.silence(seconds: 0.3) + encoded.samples + tx.finish() + Demo.silence(seconds: 0.5)
            guard let frame = Demo.receive(audio).flatMap(\.frames).first(where: { $0.decoded }), let rgb = frame.rgb,
                  let picture = NBTV.image(fromRGB: rgb) else {
                report("存檔的內容", false, "\(mode.name) 沒有收到畫面")
                return
            }
            let received = AppModel.ReceivedFrame(image: picture, modeID: frame.modeID, callsign: frame.callsign, sequence: frame.sequence,
                                                  time: Date(timeIntervalSince1970: 1_800_000_000), snrDb: frame.snrDb)
            guard let file = AppModel.exportFile(for: received) else {
                report("存檔的內容", false, "\(mode.name) 產生不出檔案")
                return
            }
            // 用系統的影像解碼器（ImageIO）讀回來，縮放成同樣的 96×72 RGB 後和收到的畫面比
            var difference = -1.0
            var size = ""
            if let source = CGImageSourceCreateWithData(file.data as CFData, nil), let decoded = CGImageSourceCreateImageAtIndex(source, 0, nil),
               let back = FrameScaler.scale(decoded, mode: .fill) {
                size = "\(decoded.width)×\(decoded.height)"
                var total = 0
                for (a, b) in zip(back, rgb) { total += abs(Int(a) - Int(b)) }
                difference = Double(total) / Double(rgb.count)
            }
            let nameOK = file.name.hasPrefix("NBTV-N0CALL-P-") && file.name.hasSuffix(".png")   // 呼號裡的斜線不能出現在檔名裡
            ok = ok && nameOK && size == "96×72" && difference == 0
            details.append(String(format: "%@ → %@（%d 位元組，系統解出 %@，與收到的畫面平均差 %.2f／255）", mode.name, file.name, file.data.count,
                                  size.isEmpty ? "失敗" : size, difference))
        }
        report("存檔的內容", ok && !details.isEmpty, details.joined(separator: "；"))
    }

    // MARK: 停止時的尾巴

    /// 按停止時如果下一張已經編好（或已經排進佇列又拿掉），發射機的重取樣濾波器已經走到那一張的結尾，
    /// 直接 nbtv_tx_finish 拿到的是那一張的尾巴。AppModel 會先用 nbtv_tx_discard_last 取消那一張。
    /// 這裡對著「只編一張就 finish」的結果驗證：取消之後的尾巴逐取樣相同、幀序號接得上，接起來的音訊解得出來。
    private static func testDroppedFrameTail(_ image: Data, _ other: Data) {
        let name = "停止時不送下一張的尾巴"
        func transmitter(_ modeID: Int) -> CoreTransmitter {
            let tx = CoreTransmitter()
            tx.setMode(modeID)
            tx.setCallsign("N0CALL")
            return tx
        }
        var details: [String] = []
        var ok = true
        for modeID in [12, 14] where Modes.find(modeID) != nil {  // 一個小的模型、一個大的
            let reference = transmitter(modeID)
            let tx = transmitter(modeID)
            guard let alone = reference.encode(rgb: image), let sent = tx.encode(rgb: image), let dropped = tx.encode(rgb: other) else {
                report(name, false, "編碼失敗")
                return
            }
            let trueTail = reference.finish()   // 只送這一張時的尾巴
            let discarded = tx.discardLast()    // 取消已經編好的下一張
            let again = tx.discardLast()        // 只能取消一次
            let tail = tx.finish()
            // 取消之後再編一張：幀序號接在送出去的那一張後面
            let next = tx.encode(rgb: other)
            let frames = Demo.receive(Demo.silence(seconds: 0.3) + sent.samples + tail + Demo.silence(seconds: 0.5)).flatMap(\.frames)
            let decoded = frames.count == 1 && frames[0].decoded && frames[0].callsign == "N0CALL"
            ok = ok && alone.samples == sent.samples && discarded && !again && !trueTail.isEmpty && tail == trueTail
                && dropped.sequence == sent.sequence + 1 && next?.sequence == dropped.sequence && decoded
            details.append(String(format: "%@：尾巴 %d 個取樣，取消下一張之後%@，收到 %d 張",
                                  Modes.find(modeID)?.name ?? "", trueTail.count, tail == trueTail ? "與只送一張時完全相同" : "不同", frames.count))
        }
        report(name, ok && !details.isEmpty, details.joined(separator: "；"))
    }

    // MARK: 取樣率轉換

    /// 模擬 44.1 kHz 的音訊裝置：48 kHz 的訊號先轉成 44.1 kHz，再像輸入裝置那樣每 20 ms 一段轉回 48 kHz 給接收機。
    private static func testResampler(_ image: Data) {
        guard let mode = Modes.find(12), let down = StreamResampler(from: 48000, to: 44100),
              let up = StreamResampler(from: 44100, to: 48000) else {
            report("取樣率轉換", false, "無法建立轉換器")
            return
        }
        let tx = CoreTransmitter()
        tx.setMode(mode.id)
        tx.setCallsign("N0CALL")
        var audio = Demo.silence(seconds: 0.3)
        for _ in 0..<2 { audio += tx.encode(rgb: image)?.samples ?? [] }
        audio += tx.finish() + Demo.silence(seconds: 0.6)
        let device = down.process(audio)
        let receiver = CoreReceiver()
        var frames: [DecodedFrame] = []
        var produced = 0
        let chunk = 882  // 44.1 kHz 的 20 ms
        var position = 0
        while position < device.count {
            let end = min(position + chunk, device.count)
            let block = up.process(Array(device[position..<end]))
            produced += block.count
            receiver.push(block)
            while let frame = receiver.pollFrame() { frames.append(frame) }
            position = end
        }
        let good = frames.filter { $0.decoded && $0.callsign == "N0CALL" }
        report("取樣率轉換（48 k → 44.1 k → 48 k，分段）", good.count == 2,
               String(format: "收到 %d/2 張，%d 個取樣轉成 %d 個再轉回 %d 個", good.count, audio.count, device.count, produced))
    }

    // MARK: 鏡頭

    /// 平常只有在已經取得相機權限時才測，不主動跳出權限對話框；ask 為 true（--ask-camera）時，
    /// 還沒決定過權限的話會向系統要求，等使用者回答（最多一分鐘）。
    /// savePath（--save-camera-frame）：把取到的那一格（縮成 96×72）存成 PPM，方便拿同一格餵給命令列工具。
    private static func testCamera(ask: Bool, savePath: String?) {
        var status = AVCaptureDevice.authorizationStatus(for: .video)
        let cameras = CameraSource.devices()
        if status == .notDetermined, ask {
            var answered = false
            AVCaptureDevice.requestAccess(for: .video) { _ in answered = true }
            print("[selftest] 等使用者回答相機權限的詢問……")
            _ = Demo.wait(timeout: 60) { answered }
            status = AVCaptureDevice.authorizationStatus(for: .video)
        }
        guard status == .authorized else {
            report("鏡頭", nil, "找到 \(cameras.count) 個鏡頭；相機權限狀態 \(status.rawValue)（尚未授權），沒有實際擷取")
            return
        }
        let box = LatestFrameBox()
        let camera = CameraSource()
        camera.onFrame = { box.set($0) }
        var failure: String?
        var started = false
        camera.start(deviceID: "") { error in
            failure = error
            started = true
        }
        _ = Demo.wait(timeout: 5) { started }
        var ok = failure == nil && Demo.wait(timeout: 6) { box.get().buffer != nil }
        // 鏡頭剛開的頭幾格通常偏暗（自動曝光還沒穩定），等一下再取
        if ok { _ = Demo.wait(timeout: 1.5) { false } }
        var detail = failure ?? "沒有收到畫面"
        let buffer = box.get().buffer
        camera.stop()
        if ok, let buffer, let frame = FrameScaler.scale(buffer, mode: .fill) {
            // 亮度的平均與標準差（0–255）。標準差只有個位數的畫面（很暗、過曝、對著白牆）幾乎沒有細節。
            var sum = 0.0, squares = 0.0
            let pixels = frame.rgb.count / 3
            frame.rgb.withUnsafeBytes { raw in
                let p = raw.bindMemory(to: UInt8.self)
                for i in 0..<pixels {
                    let y = 0.299 * Double(p[3 * i]) + 0.587 * Double(p[3 * i + 1]) + 0.114 * Double(p[3 * i + 2])
                    sum += y
                    squares += y * y
                }
            }
            let mean = sum / Double(max(pixels, 1))
            let deviation = (squares / Double(max(pixels, 1)) - mean * mean).squareRoot()
            detail = String(format: "來源 %d×%d → 96×72，亮度平均 %.0f、標準差 %.1f", Int(frame.sourceSize.width), Int(frame.sourceSize.height),
                            mean, deviation)
            if let savePath {
                var file = Data("P6\n\(NBTV.imageWidth) \(NBTV.imageHeight)\n255\n".utf8)
                file.append(frame.rgb)
                let saved = (try? file.write(to: URL(fileURLWithPath: savePath))) != nil
                detail += saved ? "，已存成 \(savePath)" : "，存不進 \(savePath)"
            }
            // 拿鏡頭拍到的這一格實際收發一次（不經音訊裝置）：每個模式各送兩張
            var results: [String] = []
            for mode in Modes.all {
                let tx = CoreTransmitter()
                tx.setMode(mode.id)
                tx.setCallsign("N0CALL")
                var audio = Demo.silence(seconds: 0.3)
                var encoded = 0
                for _ in 0..<2 {
                    if let sent = tx.encode(rgb: frame.rgb) {
                        audio += sent.samples
                        encoded += 1
                    }
                }
                audio += tx.finish() + Demo.silence(seconds: 0.5)
                let good = Demo.receive(audio).flatMap(\.frames).filter { $0.decoded && $0.headerOK && $0.rgb != nil }
                if encoded != 2 || good.count != 2 { ok = false }
                results.append("\(mode.name) \(good.count)/2")
            }
            detail += "；用這一格收發：" + results.joined(separator: "、")
        }
        report("鏡頭（\(AVCaptureDevice.default(for: .video)?.localizedName ?? "預設")）", ok, detail)
    }

    // MARK: 2. 影片檔

    private static func testVideoFile(_ image: Data) {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("nbtv-selftest-\(getpid()).mp4")
        defer { try? FileManager.default.removeItem(at: url) }
        guard let cg = NBTV.image(fromRGB: image), Demo.makeVideo(at: url, from: cg, seconds: 10) else {
            report("影片檔來源", false, "示範影片做不出來")
            return
        }
        let source = VideoFileSource()
        var opened: Bool?
        source.open(url) { opened = $0 }
        guard Demo.wait(timeout: 10, until: { opened != nil }), opened == true else {
            report("影片檔來源", false, "影片無法開啟")
            return
        }
        source.seek(to: 3)
        var frame: ScaledFrame?
        var buffer: CVPixelBuffer?
        let got = Demo.wait(timeout: 10) {
            if let current = source.copyCurrentFrame() {
                buffer = current
                frame = FrameScaler.scale(current, mode: .fill)
            }
            return frame != nil
        }
        if let buffer { testScaler(buffer) }
        var varied = false
        if let frame {
            let bytes = [UInt8](frame.rgb)
            varied = (bytes.max() ?? 0) - (bytes.min() ?? 0) > 40
        }
        report("影片檔來源", got && varied && frame?.sourceSize == CGSize(width: 1920, height: 1080),
               String(format: "長度 %.1f 秒，位置 %.1f 秒，來源 %d×%d", source.duration, source.position,
                      Int(frame?.sourceSize.width ?? 0), Int(frame?.sourceSize.height ?? 0)))
        source.close()
        testVideoOrientation(cg)
    }

    /// 大的來源畫面會先粗縮再做最後的縮放（FrameScaler.reducedImage）。結果要和整格直接交給 CoreGraphics 的幾乎一樣，而且比較快。
    private static func testScaler(_ buffer: CVPixelBuffer) {
        guard FrameScaler.reducedImage(buffer) != nil else {
            report("來源畫面的縮放", false, "粗縮的路徑沒有啟用")
            return
        }
        var details: [String] = []
        var ok = true
        for (mode, name) in [(ScaleMode.fill, "填滿"), (ScaleMode.fit, "完整")] {
            let rounds = 5
            var began = Date()
            var fast: Data?
            for _ in 0..<rounds { fast = FrameScaler.scale(buffer, mode: mode)?.rgb }
            let fastTime = Date().timeIntervalSince(began) / Double(rounds)
            began = Date()
            var reference: Data?
            // 每次都重新包一張 CGImage，和實際使用時一樣（同一張重複畫的話 CoreGraphics 會用快取，量不到真正的時間）
            for _ in 0..<rounds { reference = FrameScaler.fullImage(buffer).flatMap { FrameScaler.scale($0, mode: mode) } }
            let referenceTime = Date().timeIntervalSince(began) / Double(rounds)
            guard let fast, let reference else {
                report("來源畫面的縮放", false, "縮放失敗")
                return
            }
            var total = 0, worst = 0
            for (a, b) in zip(fast, reference) {
                let difference = abs(Int(a) - Int(b))
                total += difference
                worst = max(worst, difference)
            }
            let mean = Double(total) / Double(fast.count)
            ok = ok && mean < 1.5
            details.append(String(format: "%@：平均差 %.2f／255（最大 %d），%.1f 毫秒（整格直接縮 %.1f 毫秒）", name, mean, worst,
                                  fastTime * 1000, referenceTime * 1000))
        }
        report("來源畫面的縮放（\(CVPixelBufferGetWidth(buffer))×\(CVPixelBufferGetHeight(buffer)) → 96×72）", ok, details.joined(separator: "；"))
    }

    /// 手機直拍的影片：畫面是橫的存進去，另外記載顯示時要轉幾度。做三段各轉 90、180、270 度的影片，
    /// 我們取到的那一格要和 AVFoundation 自己轉正的結果（AVAssetImageGenerator）一樣。
    private static func testVideoOrientation(_ image: CGImage) {
        var details: [String] = []
        var ok = true
        for turns in 1...3 {
            let url = FileManager.default.temporaryDirectory.appendingPathComponent("nbtv-selftest-\(getpid())-turn\(turns).mp4")
            defer { try? FileManager.default.removeItem(at: url) }
            guard Demo.makeVideo(at: url, from: image, seconds: 2, transform: CGAffineTransform(rotationAngle: CGFloat(turns) * .pi / 2)) else {
                report("影片檔的方向", false, "示範影片做不出來")
                return
            }
            // 標準答案
            let generator = AVAssetImageGenerator(asset: AVURLAsset(url: url))
            generator.appliesPreferredTrackTransform = true
            generator.requestedTimeToleranceBefore = .zero
            generator.requestedTimeToleranceAfter = .zero
            var truth: CGImage?
            var answered = false
            generator.generateCGImagesAsynchronously(forTimes: [NSValue(time: .zero)]) { _, cgImage, _, _, _ in
                truth = cgImage
                answered = true
            }
            _ = Demo.wait(timeout: 10) { answered }
            // 我們的做法
            let source = VideoFileSource()
            var opened: Bool?
            source.open(url) { opened = $0 }
            _ = Demo.wait(timeout: 10) { opened != nil }
            var frame: ScaledFrame?
            var unturned: ScaledFrame?   // 對照：不管方向直接縮
            _ = Demo.wait(timeout: 10) {
                if let buffer = source.copyCurrentFrame() {
                    frame = FrameScaler.scale(buffer, mode: .fit, quarterTurns: source.quarterTurns)
                    unturned = FrameScaler.scale(buffer, mode: .fit)
                }
                return frame != nil
            }
            let detected = source.quarterTurns
            source.close()
            guard let truth, let expected = FrameScaler.scale(truth, mode: .fit), let frame, let unturned else {
                report("影片檔的方向", false, "轉 \(turns * 90) 度的影片取不到畫面")
                return
            }
            func meanDifference(_ rgb: Data) -> Double {
                var total = 0
                for (a, b) in zip(expected, rgb) { total += abs(Int(a) - Int(b)) }
                return Double(total) / Double(expected.count)
            }
            let mean = meanDifference(frame.rgb), ignored = meanDifference(unturned.rgb)
            let sizeOK = frame.sourceSize == CGSize(width: truth.width, height: truth.height)
            ok = ok && detected == turns && source.quarterTurns == 0 && sizeOK && mean < 6 && ignored > 20
            details.append(String(format: "%d 度：%d×%d，與標準答案平均差 %.1f／255（不管方向的話是 %.0f）", turns * 90,
                                  Int(frame.sourceSize.width), Int(frame.sourceSize.height), mean, ignored))
        }
        report("影片檔的方向", ok, details.joined(separator: "；"))
    }

    // MARK: 收到的畫面

    /// 收到的每一張都記著自己的訊雜比（設定打開時標在畫面左上角）；「最近收到」留很多張，滿了丟最舊的。
    private static func testReceivedHistory(_ image: Data) {
        let name = "收到的畫面：各自的訊雜比、保留的張數"
        let tx = CoreTransmitter()
        tx.setMode(12)
        tx.setCallsign("N0CALL")
        var signal: [Float] = []
        for _ in 0..<2 { signal += tx.encode(rgb: image)?.samples ?? [] }
        signal += tx.finish()
        // 兩段不同的訊雜比：收到的兩張要各自記著自己的值
        var frames: [DecodedFrame] = []
        for snr in [18.0, 6.0] {
            let audio = Demo.silence(seconds: 0.4) + signal + Demo.silence(seconds: 0.5)
            let received = Demo.radioChannel(audio, signalPower: Demo.power(signal), snrDb: snr, shiftHz: 0, seed: UInt64(snr))
            frames += Demo.receive(received).flatMap(\.frames).filter { $0.decoded }.prefix(1)
        }
        guard frames.count == 2 else {
            report(name, false, "示範的訊號收到 \(frames.count)/2 張")
            return
        }
        let model = AppModel(settings: AppSettings(store: nil), live: false)
        for frame in frames { model.add(frame, time: Date()) }
        // 最新的排最前面；各自的訊雜比和接收機回報的相同，而且和通道的訊雜比差不多
        var ok = model.history.count == 2 && model.history[0].snrDb == frames[1].snrDb && model.history[1].snrDb == frames[0].snrDb
        ok = ok && abs(frames[0].snrDb - 18) < 2.5 && abs(frames[1].snrDb - 6) < 2.5
        let texts = [12.34, -1.44, -0.04, 9.96, 0].map(AppModel.snrText)
        ok = ok && texts == ["12.3 dB", "−1.4 dB", "0.0 dB", "10.0 dB", "0.0 dB"]
        // 超過上限：留下最新的 historyLimit 張
        let extra = 25
        for _ in 0..<(AppModel.historyLimit + extra - 2) { model.add(frames[0], time: Date()) }
        model.add(frames[1], time: Date())
        ok = ok && model.history.count == AppModel.historyLimit && model.history[0].snrDb == frames[1].snrDb
        report(name, ok, String(format: "兩張的訊雜比 %@、%@（通道是 18、6 dB）；最多留 %d 張", AppModel.snrText(frames[0].snrDb),
                                AppModel.snrText(frames[1].snrDb), AppModel.historyLimit))

        // 點選了不是最新的那一張：主畫面顯示它。之後有新的畫面進來，主畫面要換成新的那一張，不能停在原來點選的
        // （沒有解出畫面的幀不算，主畫面不動）。點最新的那一張等於沒有點選。
        var problems: [String] = []
        let older = model.history[3]
        model.select(older)
        if model.displayedFrame?.id != older.id { problems.append("點選之後主畫面不是點的那一張") }
        model.add(DecodedFrame(modeID: 12, decoded: false, modelMissing: false, headerOK: false, callsign: "", sequence: -1,
                               snrDb: 0, offsetHz: 0, rgb: nil), time: Date())
        if model.displayedFrame?.id != older.id { problems.append("沒有解出畫面的幀也讓主畫面換掉了") }
        model.add(frames[0], time: Date())
        if model.displayedFrame?.id != model.history.first?.id || model.selectedID != nil {
            problems.append("有新的畫面進來，主畫面還停在原來點選的那一張")
        }
        if !model.history.contains(where: { $0.id == older.id }) { problems.append("原來點選的那一張不見了") }
        if let newest = model.history.first {
            model.select(newest)
            if model.selectedID != nil { problems.append("點最新的那一張之後沒有回到「跟著最新的」") }
        }
        report("收到的畫面：有新的進來就換成最新的那一張", problems.isEmpty,
               problems.isEmpty ? "點選第 4 張 → 收到新的一張 → 主畫面是新的那一張" : problems.joined(separator: "；"))
    }

    // MARK: 把一段畫面存成影片

    /// 讀回一個影片檔：編碼格式（四個字元的代碼）、畫面大小、格率、長度，以及每一格解出來的畫面（每 10×10 取中間那一點，縮回 96×72 的 RGB）。
    private static func readVideo(_ url: URL) -> (codec: String, size: CGSize, frameRate: Float, seconds: Double, frames: [Data])? {
        let asset = AVURLAsset(url: url)
        var loaded: (track: AVAssetTrack, codec: String, size: CGSize, rate: Float, seconds: Double)?
        var finished = false
        Task {
            var result: (track: AVAssetTrack, codec: String, size: CGSize, rate: Float, seconds: Double)?
            if let track = try? await asset.loadTracks(withMediaType: .video).first,
               let formats = try? await track.load(.formatDescriptions), let format = formats.first,
               let size = try? await track.load(.naturalSize), let rate = try? await track.load(.nominalFrameRate),
               let duration = try? await asset.load(.duration) {
                let code = CMFormatDescriptionGetMediaSubType(format)
                let text = String(bytes: [24, 16, 8, 0].map { UInt8((code >> UInt32($0)) & 0xFF) }, encoding: .ascii) ?? "?"
                result = (track, text, size, rate, duration.seconds)
            }
            let value = result
            DispatchQueue.main.async {
                loaded = value
                finished = true
            }
        }
        guard Demo.wait(timeout: 15, until: { finished }), let loaded, let reader = try? AVAssetReader(asset: asset) else { return nil }
        let output = AVAssetReaderTrackOutput(track: loaded.track, outputSettings: [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA])
        guard reader.canAdd(output) else { return nil }
        reader.add(output)
        guard reader.startReading() else { return nil }
        var frames: [Data] = []
        while let sample = output.copyNextSampleBuffer() {
            guard let buffer = CMSampleBufferGetImageBuffer(sample) else { continue }
            CVPixelBufferLockBaseAddress(buffer, .readOnly)
            defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
            guard let base = CVPixelBufferGetBaseAddress(buffer) else { continue }
            let stride = CVPixelBufferGetBytesPerRow(buffer), scale = SequenceVideo.scale
            guard CVPixelBufferGetWidth(buffer) == SequenceVideo.width, CVPixelBufferGetHeight(buffer) == SequenceVideo.height else { continue }
            let pixels = base.assumingMemoryBound(to: UInt8.self)
            var rgb = Data(count: NBTV.rgbBytes)
            rgb.withUnsafeMutableBytes { raw in
                let dst = raw.bindMemory(to: UInt8.self)
                for y in 0..<NBTV.imageHeight {
                    for x in 0..<NBTV.imageWidth {
                        let source = pixels + (y * scale + scale / 2) * stride + (x * scale + scale / 2) * 4   // BGRA
                        let index = 3 * (y * NBTV.imageWidth + x)
                        dst[index] = source[2]
                        dst[index + 1] = source[1]
                        dst[index + 2] = source[0]
                    }
                }
            }
            frames.append(rgb)
        }
        return (loaded.codec, loaded.size, loaded.rate, loaded.seconds, frames)
    }

    /// 「儲存影片」：哪些畫面算同一段（同一個呼號、空檔不到 10 秒），以及存出來的影片檔。
    /// 影片實際寫出來再讀回：要是 H.264 的 .mp4、每秒 24 格、960×720，每一張顯示的時間和當時收到的間隔一樣，畫面的內容也對。
    private static func testSequenceVideo(_ coffee: Data, _ rocket: Data) {
        let name = "把一段收到的畫面存成影片"
        guard let cat = Demo.testImage("cat"), let mode = Modes.find(12) ?? Modes.all.first else {
            report(name, false, "少了測試圖或模式")
            return
        }
        let pictures = [coffee, rocket, cat]
        let images = pictures.compactMap { NBTV.image(fromRGB: $0) }
        guard images.count == 3 else {
            report(name, false, "測試圖轉不成影像")
            return
        }
        // 七張畫面：同一個呼號接連三張（中間有一次 9.5 秒的空檔）；隔了 10.5 秒再兩張（第二張的標頭沒解出來，不知道呼號）；
        // 緊接著換另一個呼號的兩張。空檔是前一張收完到後一張開始送之間的時間。
        let length = mode.frameSeconds
        let base = Date(timeIntervalSince1970: 1_800_000_000)
        let plan: [(call: String, gap: Double)] = [("N0CALL", 0), ("N0CALL", 0), ("N0CALL", 9.5), ("N0CALL", 10.5), ("", 0), ("W1AW", 0), ("W1AW", 0)]
        var time = base
        var frames: [AppModel.ReceivedFrame] = []
        for (index, item) in plan.enumerated() {
            if index > 0 { time = time.addingTimeInterval(item.gap + length) }
            frames.append(AppModel.ReceivedFrame(image: images[index % 3], modeID: mode.id, callsign: item.call, sequence: index, time: time, snrDb: 10))
        }
        let history = Array(frames.reversed())   // app 裡是新的在前
        var problems: [String] = []
        let expected = [[0, 1, 2], [0, 1, 2], [0, 1, 2], [3, 4], [3, 4], [5, 6], [5, 6]]
        for (index, frame) in frames.enumerated() {
            let got = AppModel.sequence(containing: frame.id, in: history).map { member in frames.firstIndex { $0.id == member.id } ?? -1 }
            if got != expected[index] { problems.append("第 \(index + 1) 張所在的一段是 \(got.map { $0 + 1 })，應該是 \(expected[index].map { $0 + 1 })") }
        }
        if !AppModel.sequence(containing: UUID(), in: history).isEmpty { problems.append("不在清單裡的畫面也找到了一段") }
        let fileName = AppModel.videoFileName(for: Array(frames[3...4]))
        if !fileName.hasPrefix("NBTV-N0CALL-") || !fileName.hasSuffix(".mp4") { problems.append("預設檔名是 \(fileName)") }

        // 把第一段（三張）經過 app 的狀態物件存成影片：主畫面顯示其中一張時，「儲存影片」存的就是這一段
        let model = AppModel(settings: AppSettings(store: nil), live: false)
        model.history = history
        model.select(frames[1])
        let sequence = model.displayedSequence
        if sequence.map(\.id) != frames[0...2].map(\.id) { problems.append("主畫面那一張所在的一段不是前三張") }
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("nbtv-selftest-sequence-\(getpid()).mp4")
        defer { try? FileManager.default.removeItem(at: url) }
        var result: Result<SequenceVideo.Summary, Error>?
        let began = Date()
        model.exportSequence(sequence, to: url) { result = $0 }
        let busy = model.exportingVideo
        guard Demo.wait(timeout: 60, until: { result != nil }), case .success(let summary)? = result else {
            if case .failure(let error)? = result { problems.append("存檔失敗：\(error.localizedDescription)") } else { problems.append("存檔一直沒有結束") }
            report(name, false, problems.joined(separator: "；"))
            return
        }
        let spent = Date().timeIntervalSince(began)
        if !busy || model.exportingVideo { problems.append("「儲存中」的狀態不對") }
        if model.notice?.contains(url.lastPathComponent) != true { problems.append("底列沒有寫存成了哪個檔案") }

        // 時間軸：第二張在 length 秒、第三張在 2×length＋9.5 秒開始，最後一張停留 length 秒
        let starts = [0, length, 2 * length + 9.5]
        let first = starts.map { Int(($0 * 24).rounded()) }
        let total = first[2] + max(1, Int((max(1, length) * 24).rounded()))
        if summary.pictures != 3 || summary.videoFrames != total { problems.append("寫了 \(summary.pictures) 張、\(summary.videoFrames) 格，應該是 3 張、\(total) 格") }
        guard let video = readVideo(url) else {
            report(name, false, (problems + ["存出來的影片讀不回來"]).joined(separator: "；"))
            return
        }
        if video.codec != "avc1" { problems.append("影片的編碼是 \(video.codec)，不是 H.264") }
        if video.size != CGSize(width: 960, height: 720) { problems.append("畫面大小是 \(Int(video.size.width))×\(Int(video.size.height))") }
        if abs(video.frameRate - 24) > 0.05 { problems.append(String(format: "格率是每秒 %.2f 格", video.frameRate)) }
        if abs(video.seconds - Double(total) / 24) > 0.05 { problems.append(String(format: "長度是 %.2f 秒，應該是 %.2f 秒", video.seconds, Double(total) / 24)) }
        if video.frames.count != total {
            problems.append("讀回來 \(video.frames.count) 格，應該是 \(total) 格")
        } else {
            // 每一張的第一格、最後一格都要是那一張的畫面（換畫面的那一格不能早也不能晚）
            var worst = 0.0
            for index in 0..<3 {
                let last = (index < 2 ? first[index + 1] : total) - 1
                for frame in [first[index], (first[index] + last) / 2, last] {
                    let error = difference(video.frames[frame], pictures[index])
                    worst = max(worst, error)
                    if error > 8 { problems.append(String(format: "第 %d 格不是第 %d 張的畫面（平均差 %.1f）", frame, index + 1, error)) }
                }
            }
            if problems.isEmpty {
                let size = ((try? FileManager.default.attributesOfItem(atPath: url.path))?[.size] as? NSNumber)?.intValue ?? 0
                report(name, true, String(format: "七張分成 3、2、2 張的三段；第一段存成 H.264 的 .mp4：960×720、每秒 %.0f 格、%d 格（%.2f 秒）、%d KB，"
                                              + "花了 %.1f 秒；每一張的頭、中、尾三格和原圖平均最多差 %.1f／255", video.frameRate, total, video.seconds, size / 1024, spent, worst))
                return
            }
        }
        report(name, false, problems.joined(separator: "；"))
    }

    // MARK: 影片來源的發射流程

    /// 記錄用的 PTT：把每一次按下、放開記下來，不碰任何硬體。
    private final class RecordingPTT: PTTDriver {
        private let lock = NSLock()
        private var states: [Bool] = []
        /// 放開要花幾秒（像真的 PTT 要等電台回覆那樣）；用來分辨「有等放開做完」和「只是把放開排進佇列」
        var releaseDelay = 0.0
        func key(_ on: Bool) throws {
            if !on, releaseDelay > 0 { usleep(UInt32(releaseDelay * 1_000_000)) }
            lock.lock()
            states.append(on)
            lock.unlock()
        }
        /// 到目前為止的紀錄，取走之後清空
        func take() -> [Bool] {
            lock.lock()
            defer { lock.unlock() }
            let result = states
            states.removeAll()
            return result
        }
    }

    /// 收下模擬的輸出裝置取走的取樣（在別的佇列上被呼叫）。
    private final class SampleCollector {
        private let lock = NSLock()
        private var samples: [Float] = []
        func append(_ block: [Float]) {
            lock.lock()
            samples += block
            lock.unlock()
        }
        func take() -> [Float] {
            lock.lock()
            defer { lock.unlock() }
            let result = samples
            samples.removeAll()
            return result
        }
    }

    /// 兩張 96×72 的畫面平均每個值差多少。
    private static func difference(_ a: Data, _ b: Data) -> Double {
        guard a.count == b.count, !a.isEmpty else { return 255 }
        var total = 0
        for (x, y) in zip(a, b) { total += abs(Int(x) - Int(y)) }
        return Double(total) / Double(a.count)
    }

    /// 用影片檔當來源的整個發射流程。用的是 app 真正的狀態物件（AppModel），只把音訊輸出換成模擬的裝置、
    /// PTT 換成記錄用的，所以不需要任何裝置，也不會真的發出聲音：
    ///   * 不循環的影片開了之後停在開頭；按下發射才開始播
    ///   * 影片播完，再送出它的最後一格，然後自動停止、放開 PTT、回到待機
    ///   * 送出去的每一張都解得回來：第一張是影片開頭的畫面，最後一張是結尾的畫面
    ///   * 影片停在結尾時再按一次發射，會回到開頭重新來過
    ///   * 播到一半按停止，影片當下就跟著暫停；再按一次發射，從停下的地方接著播到結尾
    ///   * 勾了循環播放就不會自動停止，要按停止；按了停止影片一樣跟著暫停
    ///   * 還沒開始送音訊就取消的，影片維持原來的樣子
    private static func testVideoTransmit(_ image: Data) {
        let name = "影片來源：發射時自動播放、停止時暫停、播完自動停止"
        let seconds = 3
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("nbtv-selftest-tx-\(getpid()).mp4")
        defer { try? FileManager.default.removeItem(at: url) }
        guard let cg = NBTV.image(fromRGB: image), Demo.makeVideo(at: url, from: cg, seconds: seconds, fps: 10) else {
            report(name, false, "示範影片做不出來")
            return
        }
        // 另外開一份同樣的影片，用來取指定位置那一格縮成 96×72 的樣子，之後拿來認收到的畫面
        let reference = VideoFileSource()
        defer { reference.close() }
        var referenceOpened: Bool?
        reference.open(url) { referenceOpened = $0 }
        guard Demo.wait(timeout: 10, until: { referenceOpened != nil }), referenceOpened == true else {
            report(name, false, "影片無法開啟")
            return
        }
        func picture(at time: Double) -> Data? {
            // 等位置確實移過去了再取，才不會拿到上一個位置留下來的那一格
            var arrived = false
            reference.player.seek(to: CMTime(seconds: time, preferredTimescale: 600), toleranceBefore: .zero, toleranceAfter: .zero) { _ in
                DispatchQueue.main.async { arrived = true }
            }
            guard Demo.wait(timeout: 5, until: { arrived }) else { return nil }
            var rgb: Data?
            _ = Demo.wait(timeout: 5) {
                if let buffer = reference.copyCurrentFrame() { rgb = FrameScaler.scale(buffer, mode: .fill)?.rgb }
                return rgb != nil
            }
            return rgb
        }
        let first = picture(at: 0), last = picture(at: Double(seconds))
        guard let first, let last, difference(first, last) > 12 else {
            report(name, false, "取不到影片開頭與結尾的畫面，或兩者太像（開頭\(first == nil ? "沒取到" : "有")、結尾\(last == nil ? "沒取到" : "有")"
                   + (first != nil && last != nil ? String(format: "，平均差 %.1f", difference(first!, last!)) : "") + "）")
            return
        }

        setenv("NBTV_NO_DEVICES", "1", 1)   // 狀態物件啟動時不開輸入裝置與鏡頭
        let settings = AppSettings(store: nil)
        settings.callsign = "N0CALL"
        settings.sourceKind = .video
        settings.loopVideo = false
        settings.txModeID = 12   // 模式 12：每張 1.04 秒
        let ptt = RecordingPTT()
        settings.debugPTTDriver = { ptt }
        let model = AppModel(settings: settings, live: true)
        let sent = SampleCollector()
        model.debugUseSimulatedOutput { sent.append($0) }
        model.start()
        defer { model.shutdown() }

        var opened: Bool?
        model.openVideo(url) { opened = $0 }
        guard Demo.wait(timeout: 10, until: { opened != nil }), opened == true, Demo.wait(timeout: 10, until: { model.canTransmit }) else {
            report(name, false, "影片開不起來，或一直沒有來源畫面")
            return
        }
        Demo.spin(0.6)
        var problems: [String] = []
        if model.videoPlaying || model.videoPosition > 0.2 { problems.append("不循環的影片開了之後沒有停在開頭") }

        /// 按下發射，等它自己結束；回傳（影片有沒有自己開始播、送出幾張、解回來的畫面）。
        func transmitOnce(_ label: String) -> (frames: [DecodedFrame], count: Int)? {
            _ = sent.take()
            _ = ptt.take()
            model.startTransmit()
            var played = false
            let finished = Demo.wait(timeout: 25) {
                if model.txPhase != .idle, model.videoPlaying { played = true }
                return model.txPhase == .idle
            }
            guard finished else {
                problems.append("\(label)：影片播完之後沒有自動停止")
                model.stopTransmit()
                _ = Demo.wait(timeout: 10) { model.txPhase == .idle }
                return nil
            }
            if !played { problems.append("\(label)：按下發射之後影片沒有開始播放") }
            Demo.spin(0.3)   // PTT 的放開在背景佇列上做
            if ptt.take() != [true, false] { problems.append("\(label)：PTT 不是「按下一次、放開一次」") }
            if model.videoPlaying || model.videoPosition < Double(seconds) - 0.3 { problems.append("\(label)：結束時影片不在結尾") }
            if let notice = model.notice { problems.append("\(label)：\(notice)") }
            let frames = Demo.receive(sent.take() + Demo.silence(seconds: 0.5)).flatMap(\.frames)
            return (frames, model.txFramesSent)
        }

        var details: [String] = []
        for label in ["第一次", "影片停在結尾時再按一次"] {
            guard let result = transmitOnce(label) else { continue }
            let good = result.frames.filter { $0.decoded && $0.headerOK && $0.callsign == "N0CALL" }
            // 影片 3 秒、每張 1.04 秒：開頭一張、中間幾張、結尾一張
            if result.count < 4 || result.count > 6 { problems.append("\(label)：送出 \(result.count) 張，預期 4–6 張") }
            if good.count != result.count || result.frames.count != result.count {
                problems.append("\(label)：送出 \(result.count) 張，解回來 \(good.count) 張")
            }
            if let head = good.first?.rgb, let tail = good.last?.rgb {
                if difference(head, first) >= difference(head, last) { problems.append("\(label)：第一張不是影片開頭的畫面") }
                if difference(tail, last) >= difference(tail, first) { problems.append("\(label)：最後一張不是影片結尾的畫面") }
            }
            details.append("\(label)送出 \(result.count) 張、解回 \(good.count) 張")
        }

        // 播到一半按停止：影片當下就跟著暫停，之後不會自己往前走
        var pausedAt: Double?
        _ = sent.take()
        _ = ptt.take()
        model.startTransmit()   // 影片停在結尾，所以這一次從頭來
        // 先等它真的從頭播起來（畫面上的播放位置每 0.1 秒才更新一次，剛開始還留著上一次結尾的數字），再等播到一半
        if Demo.wait(timeout: 10, until: { model.txPhase == .transmitting && model.videoPlaying && model.videoPosition < 1 }),
           Demo.wait(timeout: 10, until: { model.txPhase == .transmitting && model.videoPlaying && model.videoPosition >= 1.4 }) {
            model.stopTransmit()
            let pausedAtOnce = !model.videoPlaying
            let stoppedAt = model.videoPosition
            if !Demo.wait(timeout: 10, until: { model.txPhase == .idle }) { problems.append("播到一半按停止：發射沒有結束") }
            Demo.spin(0.6)   // 影片要是還在播，這段時間會繼續往前走，甚至播完
            if !pausedAtOnce || model.videoPlaying || abs(model.videoPosition - stoppedAt) > 0.15 {
                problems.append(String(format: "播到一半按停止：影片沒有跟著暫停（按停止時在 %.2f 秒，後來在 %.2f 秒%@）", stoppedAt, model.videoPosition,
                                       model.videoPlaying ? "，而且還在播" : ""))
            }
            if stoppedAt > Double(seconds) - 1 { problems.append(String(format: "播到一半按停止：按停止時影片已經到 %.2f 秒，不算一半", stoppedAt)) }
            if ptt.take() != [true, false] { problems.append("播到一半按停止：PTT 不是「按下一次、放開一次」") }
            let frames = Demo.receive(sent.take() + Demo.silence(seconds: 0.5)).flatMap(\.frames)
            let good = frames.filter { $0.decoded && $0.headerOK && $0.callsign == "N0CALL" }
            if good.isEmpty || good.count != model.txFramesSent || frames.count != good.count {
                problems.append("播到一半按停止：送出 \(model.txFramesSent) 張，解回來 \(good.count) 張")
            }
            if let head = good.first?.rgb, difference(head, first) >= difference(head, last) { problems.append("播到一半按停止：第一張不是影片開頭的畫面") }
            details.append(String(format: "播到 %.1f 秒按停止，影片停在 %.1f 秒（送出 %d 張）", stoppedAt, model.videoPosition, model.txFramesSent))
            pausedAt = model.videoPosition
        } else {
            problems.append("播到一半按停止：按下發射之後影片一直沒有播到一半")
            model.stopTransmit()
            _ = Demo.wait(timeout: 10) { model.txPhase == .idle }
        }

        // 再按一次發射：從停下的地方接著播（第一張是停下的那一格，不是影片開頭），播完照樣自動停止
        if let pausedAt {
            let label = "停下之後再按一次"
            if let here = picture(at: pausedAt), min(difference(here, first), difference(here, last)) > 4 {
                if let result = transmitOnce(label) {
                    let good = result.frames.filter { $0.decoded && $0.headerOK && $0.callsign == "N0CALL" }
                    // 剩下一秒半左右：停下的那一格、中間一張、結尾一張
                    if result.count < 2 || result.count > 4 { problems.append("\(label)：送出 \(result.count) 張，預期 2–4 張") }
                    if good.count != result.count || result.frames.count != result.count {
                        problems.append("\(label)：送出 \(result.count) 張，解回來 \(good.count) 張")
                    }
                    if let head = good.first?.rgb, let tail = good.last?.rgb {
                        if difference(head, here) >= difference(head, first) { problems.append("\(label)：沒有從停下的地方接著播（第一張是影片開頭的畫面）") }
                        if difference(tail, last) >= difference(tail, first) { problems.append("\(label)：最後一張不是影片結尾的畫面") }
                    }
                    details.append("\(label)從停下的地方接著送出 \(result.count) 張")
                }
            } else {
                problems.append("\(label)：取不到影片停下那個位置的畫面，或它和開頭、結尾太像")
            }
        }

        // 循環播放：影片播完會從頭再播，發射不會自己停；按了停止，影片一樣跟著暫停
        settings.loopVideo = true
        _ = sent.take()
        _ = ptt.take()
        model.startTransmit()
        let stoppedByItself = Demo.wait(timeout: Double(seconds) + 3.5) { model.txPhase == .idle }
        if stoppedByItself {
            problems.append("循環播放時發射自己停了")
        } else {
            model.stopTransmit()
            let pausedAtOnce = !model.videoPlaying
            if !Demo.wait(timeout: 10, until: { model.txPhase == .idle }) { problems.append("循環播放時按停止沒有結束發射") }
            Demo.spin(0.3)
            // 按停止的時候影片可能剛好在從結尾跳回開頭，所以不比按下那一刻的位置，只看停下來之後還有沒有在動
            let rested = model.videoPosition
            Demo.spin(0.5)
            if !pausedAtOnce || model.videoPlaying || abs(model.videoPosition - rested) > 0.05 {
                problems.append(String(format: "循環播放：按停止之後影片沒有跟著暫停（%.2f 秒 → %.2f 秒）", rested, model.videoPosition))
            }
            if ptt.take() != [true, false] { problems.append("循環播放：PTT 不是「按下一次、放開一次」") }
            details.append("循環播放時送了 \(model.txFramesSent) 張、按停止才結束，影片跟著暫停")
        }

        // 還沒開始送音訊就取消（按下發射馬上又按停止）：影片維持原來的樣子。這裡先自己按播放，取消之後它應該還在播
        if !model.videoPlaying { model.toggleVideoPlayback() }
        Demo.spin(0.3)
        _ = ptt.take()
        model.startTransmit()
        model.stopTransmit()
        if model.txPhase != .idle { problems.append("還沒開始送就取消：沒有馬上回到待機") }
        Demo.spin(0.5)
        if !model.videoPlaying { problems.append("還沒開始送就取消：原本在播的影片被暫停了") }
        if ptt.take().last == true { problems.append("還沒開始送就取消：PTT 還按著") }
        _ = Demo.wait(timeout: 10) { model.txPhase == .idle }
        report(name, problems.isEmpty, problems.isEmpty ? details.joined(separator: "；") : problems.joined(separator: "；"))
        testPauseAtLoopPoint(url)
    }

    /// 循環播放的影片剛好播到結尾的那一瞬間被暫停（例如這時按下停止發射）：「播完了」的通知是排到主執行緒上、
    /// 晚一點才送到的，送到的時候不能又把影片從頭播起來。這裡佔住主執行緒等影片走到結尾，讓通知確實排在暫停的後面。
    private static func testPauseAtLoopPoint(_ url: URL) {
        let name = "循環播放的影片剛好在結尾被暫停"
        let source = VideoFileSource()
        defer { source.close() }
        source.loop = true
        var opened: Bool?
        source.open(url) { opened = $0 }
        guard Demo.wait(timeout: 10, until: { opened != nil }), opened == true, source.duration > 1 else {
            report(name, false, "影片無法開啟")
            return
        }
        source.seek(to: source.duration - 0.6)
        Demo.spin(0.3)
        source.play()
        // 不讓主執行緒的事件迴圈跑：位置不再往前走、而且已經在結尾附近，就是播完了
        var position = source.position, still = 0
        let deadline = Date().addingTimeInterval(5)
        while Date() < deadline, still < 15 {
            usleep(10_000)
            let now = source.position
            still = (now == position && now >= source.duration - 0.2) ? still + 1 : 0
            position = now
        }
        let reached = still >= 15
        source.pause()
        Demo.spin(0.8)   // 通知這時才送到
        let stayed = !source.isPlaying && source.position >= source.duration - 0.2
        let rested = source.position
        // 之後再播放，要從頭來
        source.play()
        let restarted = Demo.wait(timeout: 3) { source.isPlaying && source.position > 0.1 && source.position < source.duration - 1 }
        source.pause()
        report(name, reached && stayed && restarted,
               String(format: "%@；暫停之後停在 %.2f 秒%@；再播放%@從頭來", reached ? "播到了結尾" : "一直沒有播到結尾", rested,
                      stayed ? "" : "（沒有停住）", restarted ? "" : "沒有"))
    }

    // MARK: 畫布

    /// 96×72 的 CGImage 原本的 RGB 位元組（用 NBTV.image(fromRGB:) 做出來的影像）。
    private static func bytes(of image: CGImage?) -> Data? {
        guard let data = image?.dataProvider?.data as Data?, data.count == NBTV.rgbBytes else { return nil }
        return data
    }

    /// 兩張畫面在指定的那幾列平均每個值差多少。
    private static func difference(_ a: Data, _ b: Data, rows: Range<Int>) -> Double {
        let range = (rows.lowerBound * NBTV.imageWidth * 3)..<(rows.upperBound * NBTV.imageWidth * 3)
        guard a.count == NBTV.rgbBytes, b.count == NBTV.rgbBytes, !range.isEmpty else { return 255 }
        return difference(a.subdata(in: range), b.subdata(in: range))
    }

    /// 畫布：印在送出畫面上的字。字帶只蓋住上緣或下緣的幾列、其他地方一個位元組都不動；字盡量大，放不下就縮、再拆成兩行；
    /// 沒有字就沒有畫布；印好字的畫面經過編碼、接收之後字帶還在。
    private static func testCanvas(_ image: Data) {
        let name = "畫布：印在送出畫面上的字"
        var problems: [String] = []
        let gray = Data(repeating: 128, count: NBTV.rgbBytes)
        let width = NBTV.imageWidth, height = NBTV.imageHeight
        /// 那幾列裡很亮（字）與很暗（字帶）的像素各有幾個
        func count(_ rgb: Data, rows: Range<Int>) -> (bright: Int, dark: Int) {
            var bright = 0, dark = 0
            for index in (rows.lowerBound * width)..<(rows.upperBound * width) {
                let value = Int(rgb[3 * index]) + Int(rgb[3 * index + 1]) + Int(rgb[3 * index + 2])
                if value > 3 * 225 { bright += 1 } else if value < 3 * 70 { dark += 1 }
            }
            return (bright, dark)
        }
        var details: [String] = []
        for position in [CanvasPosition.top, .bottom] {
            let label = position == .top ? "上緣" : "下緣"
            guard let layer = CanvasLayer(style: CanvasStyle(text: "CQ N0CALL", position: position)) else {
                problems.append("\(label)：做不出圖層")
                continue
            }
            let stamped = layer.apply(to: gray)
            let band = layer.rows
            if position == .top ? band.lowerBound != 0 : band.upperBound != height { problems.append("\(label)：字帶不在\(label)（第 \(band.lowerBound)–\(band.upperBound - 1) 列）") }
            if band.count < 12 || band.count > 24 { problems.append("\(label)：字帶高 \(band.count) 列") }
            if layer.lines != ["CQ N0CALL"] || layer.fontSize < 12 { problems.append("\(label)：排成 \(layer.lines)、字級 \(layer.fontSize)") }
            // 字帶以外一個位元組都不能動
            let outside = position == .top ? band.upperBound..<height : 0..<band.lowerBound
            if difference(stamped, gray, rows: outside) != 0 { problems.append("\(label)：字帶以外的畫面被改了") }
            let inside = count(stamped, rows: band)
            if inside.bright < 150 || inside.dark < band.count * width / 2 { problems.append("\(label)：字帶裡亮的 \(inside.bright) 點、暗的 \(inside.dark) 點") }
            if position == .top { details.append("「CQ N0CALL」字級 \(layer.fontSize)、字帶 \(band.count) 列（亮 \(inside.bright) 點）") }
        }
        // 沒有字：沒有畫布
        if CanvasLayer(style: CanvasStyle(text: "   ", position: .top)) != nil { problems.append("只有空白也做出了圖層") }
        // 一行放不下：拆成兩行；再長就截短
        if let long = CanvasLayer(style: CanvasStyle(text: "CQ CQ CQ DE N0CALL", position: .top)) {
            // 兩行的字不能大到把畫面蓋掉一半：字帶最多佔畫面高度的四成
            if long.lines.count != 2 || long.fontSize < CanvasLayer.minimumFontSize || long.fontSize > CanvasLayer.wrappedMaximumFontSize
                || long.rows.count > height * 2 / 5 {
                problems.append("長的字排成 \(long.lines)、字級 \(long.fontSize)、字帶 \(long.rows.count) 列")
            }
            details.append("「CQ CQ CQ DE N0CALL」拆成\(long.lines.map { "「\($0)」" }.joined(separator: "與"))、字級 \(long.fontSize)、字帶 \(long.rows.count) 列")
        } else {
            problems.append("長的字做不出圖層")
        }
        if let endless = CanvasLayer(style: CanvasStyle(text: "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", position: .bottom)) {
            if endless.fontSize != CanvasLayer.minimumFontSize || endless.rows.count > 20 { problems.append("放不下的字：字級 \(endless.fontSize)、字帶 \(endless.rows.count) 列") }
        } else {
            problems.append("放不下的字做不出圖層")
        }
        // 中文也可以
        if let chinese = CanvasLayer(style: CanvasStyle(text: "CQ 台灣", position: .top)) {
            if count(chinese.apply(to: gray), rows: chinese.rows).bright < 100 { problems.append("中文字沒有印出來") }
        } else {
            problems.append("中文字做不出圖層")
        }
        // 設定：關著、或沒有字的時候沒有畫布；第一次打開而還沒有字，先填「CQ 呼號」
        let settings = AppSettings(store: nil)
        settings.callsign = "N0CALL"
        let model = AppModel(settings: settings, live: false)
        if settings.canvasStyle != nil || model.applyCanvas(to: image) != image { problems.append("畫布關著的時候畫面也被改了") }
        model.setCanvasEnabled(true)
        if settings.canvasText != "CQ N0CALL" || settings.canvasStyle == nil { problems.append("打開畫布之後的字是「\(settings.canvasText)」") }
        let stamped = model.applyCanvas(to: image)
        if stamped == image { problems.append("打開畫布之後畫面沒有變") }
        settings.canvasText = "  "
        if settings.canvasStyle != nil || model.applyCanvas(to: image) != image { problems.append("只有空白的時候畫面也被改了") }
        settings.canvasText = "CQ N0CALL"

        // 印好字的畫面經過每個模式的編碼與接收：字帶那幾列要比較像印了字的，而不是沒印字的原圖
        if let layer = CanvasLayer(style: CanvasStyle(text: "CQ N0CALL", position: .top)) {
            var worst = ""
            for mode in Modes.menuOrder {
                let tx = CoreTransmitter()
                tx.setMode(mode.id)
                tx.setCallsign("N0CALL")
                guard let frame = tx.encode(rgb: stamped) else { continue }
                let audio = Demo.silence(seconds: 0.3) + frame.samples + tx.finish() + Demo.silence(seconds: 0.5)
                guard let received = Demo.receive(audio).flatMap(\.frames).first(where: { $0.decoded })?.rgb else {
                    problems.append("\(mode.name)：印了字的畫面沒有收到")
                    continue
                }
                let toStamped = difference(received, stamped, rows: layer.rows), toPlain = difference(received, image, rows: layer.rows)
                if toStamped >= toPlain / 2 { problems.append(String(format: "%@：字帶和印了字的差 %.1f、和原圖差 %.1f", mode.name, toStamped, toPlain)) }
                worst += String(format: "%@%@ %.1f／%.1f", worst.isEmpty ? "" : "、", mode.name, toStamped, toPlain)
            }
            details.append("收到的字帶和「印了字的／沒印字的」平均差：\(worst)")
        }
        report(name, problems.isEmpty, problems.isEmpty ? details.joined(separator: "；") : problems.joined(separator: "；"))
    }

    /// 畫布與輸出音量在真的發射流程裡（app 的狀態物件、模擬的音訊輸出、記錄用的 PTT）：
    ///   * 右側欄的預覽就是印好字的樣子，和「原來的畫面疊上圖層」逐位元相同
    ///   * 送出去的第一張解回來，字帶那幾列像印了字的
    ///   * 發射中把輸出音量調低 20 dB，送出去的訊號立刻跟著小 20 dB（要能一邊發射一邊看著電台的 ALC 調）
    private static func testCanvasOnAir(_ image: Data) {
        let name = "畫布與輸出音量：經過整個發射流程"
        let seconds = 2
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("nbtv-selftest-canvas-\(getpid()).mp4")
        defer { try? FileManager.default.removeItem(at: url) }
        guard let cg = NBTV.image(fromRGB: image), Demo.makeVideo(at: url, from: cg, seconds: seconds, fps: 10) else {
            report(name, false, "示範影片做不出來")
            return
        }
        setenv("NBTV_NO_DEVICES", "1", 1)   // 狀態物件啟動時不開輸入裝置與鏡頭
        let settings = AppSettings(store: nil)
        settings.callsign = "N0CALL"
        settings.sourceKind = .video
        settings.loopVideo = false
        settings.txModeID = 12
        settings.outputLevelDb = -6
        let ptt = RecordingPTT()
        settings.debugPTTDriver = { ptt }
        let model = AppModel(settings: settings, live: true)
        let sent = SampleCollector()
        model.debugUseSimulatedOutput { sent.append($0) }
        model.start()
        defer { model.shutdown() }
        var opened: Bool?
        model.openVideo(url) { opened = $0 }
        guard Demo.wait(timeout: 10, until: { opened != nil }), opened == true, Demo.wait(timeout: 10, until: { model.canTransmit }),
              Demo.wait(timeout: 5, until: { model.sourcePreview != nil }), let plain = bytes(of: model.sourcePreview) else {
            report(name, false, "影片開不起來，或一直沒有來源畫面")
            return
        }
        var problems: [String] = []
        // 打開畫布：預覽換成印好字的（影片停在開頭，來源的畫面沒有變）
        model.setCanvasEnabled(true)
        guard let layer = CanvasLayer(style: CanvasStyle(text: "CQ N0CALL", position: .top)),
              Demo.wait(timeout: 5, until: { bytes(of: model.sourcePreview) != plain }), let preview = bytes(of: model.sourcePreview) else {
            report(name, false, "打開畫布之後預覽沒有變")
            return
        }
        let stamped = layer.apply(to: plain)
        if preview != stamped { problems.append(String(format: "預覽不是「原來的畫面疊上字」（平均差 %.2f）", difference(preview, stamped))) }
        // 換到下緣：預覽跟著換
        settings.canvasPosition = .bottom
        if !Demo.wait(timeout: 5, until: { bytes(of: model.sourcePreview) != preview }) { problems.append("改成印在下緣之後預覽沒有變") }
        settings.canvasPosition = .top
        _ = Demo.wait(timeout: 5) { bytes(of: model.sourcePreview) == stamped }

        // 發射：送完第一張就把輸出音量調低 20 dB
        _ = sent.take()
        model.startTransmit()
        let firstSent = Demo.wait(timeout: 15) { model.txFramesSent >= 1 || model.txPhase == .idle }
        if !firstSent || model.txPhase == .idle { problems.append("發射沒有送出第一張") }
        settings.outputLevelDb = -26
        if !Demo.wait(timeout: 25, until: { model.txPhase == .idle }) {
            problems.append("影片播完之後沒有自動停止")
            model.stopTransmit()
            _ = Demo.wait(timeout: 10) { model.txPhase == .idle }
        }
        let samples = sent.take()
        let frames = Demo.receive(samples + Demo.silence(seconds: 0.5)).flatMap(\.frames).filter { $0.decoded }
        var detail = ""
        if let first = frames.first?.rgb {
            let toStamped = difference(first, stamped, rows: layer.rows), toPlain = difference(first, plain, rows: layer.rows)
            if toStamped >= toPlain / 2 { problems.append(String(format: "送出去的第一張：字帶和印了字的差 %.1f、和沒印字的差 %.1f", toStamped, toPlain)) }
            detail = String(format: "送出 %d 張，第一張的字帶和「印了字的／沒印字的」平均差 %.1f／%.1f", model.txFramesSent, toStamped, toPlain)
        } else {
            problems.append("送出去的畫面一張都解不回來")
        }
        // 音量：第一張（開頭 0.3 秒靜音之後）是 −6 dB 時送的，最後一張是 −26 dB 時送的
        func level(_ range: Range<Int>) -> Double {
            guard range.lowerBound >= 0, range.upperBound <= samples.count, !range.isEmpty else { return -200 }
            return 10 * log10(max(Demo.power(Array(samples[range])), 1e-20))
        }
        let rate = NBTV.sampleRate
        let early = level(Int(0.45 * Double(rate))..<Int(1.15 * Double(rate)))
        let late = level((samples.count - Int(0.9 * Double(rate)))..<(samples.count - Int(0.4 * Double(rate))))
        if abs((early - late) - 20) > 3 { problems.append(String(format: "發射中把音量調低 20 dB，送出去的訊號只差 %.1f dB（%.1f → %.1f dBFS）", early - late, early, late)) }
        detail += String(format: "；輸出音量 −6 → −26 dB，送出去的訊號 %.1f → %.1f dBFS", early, late)
        report(name, problems.isEmpty, problems.isEmpty ? detail : problems.joined(separator: "；"))
    }

    // MARK: 發射中的保險

    /// 測試用的 PTT：按下或放開時可以丟出指定的錯誤；每一次呼叫都記下來。
    private final class FailingPTT: PTTDriver {
        var keyDownError: Error?
        var keyUpError: Error?
        private let lock = NSLock()
        private var states: [Bool] = []
        func key(_ on: Bool) throws {
            lock.lock()
            states.append(on)
            lock.unlock()
            if let error = on ? keyDownError : keyUpError { throw error }
        }
        func take() -> [Bool] {
            lock.lock()
            defer { lock.unlock() }
            let result = states
            states.removeAll()
            return result
        }
    }

    /// 發射中出狀況時不可以讓 PTT 一直按著。用的是 app 真正的狀態物件，音訊輸出換成模擬的裝置、PTT 換成記錄用的：
    ///   * 輸出裝置中途不再來取取樣（USB 音效卡斷線）：超過時限自動停止、放開 PTT、底列寫出原因；
    ///     已經按了停止、正在等這一張送完的時候停住也一樣
    ///   * 「結束發射中」再按一次發射鈕：立刻停止（半秒內連按的不算）；之後照常可以發射，送出的幀解得回來
    ///   * 電腦即將睡眠：通知一處理完，發射就已經停了、PTT 也已經放開了（不必等任何背景的工作）
    ///   * PTT 的指令說不準有沒有放開（放開失敗、按下沒有得到確認）時跳出警告；確定沒按著的不警告
    private static func testTransmitSafety(_ image: Data) {
        let name = "發射中的保險：輸出停住、立刻停止、電腦睡眠、PTT 指令失敗"
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("nbtv-selftest-safety-\(getpid()).mp4")
        defer { try? FileManager.default.removeItem(at: url) }
        guard let cg = NBTV.image(fromRGB: image), Demo.makeVideo(at: url, from: cg, seconds: 3, fps: 10) else {
            report(name, false, "示範影片做不出來")
            return
        }
        setenv("NBTV_NO_DEVICES", "1", 1)   // 狀態物件啟動時不開輸入裝置與鏡頭
        let settings = AppSettings(store: nil)
        settings.callsign = "N0CALL"
        settings.sourceKind = .video
        settings.loopVideo = true    // 不會自己停，要按停止
        settings.txModeID = 11       // 模式 11：每張 2.72 秒，「結束發射中」夠久，來得及再按一次
        let ptt = RecordingPTT()
        settings.debugPTTDriver = { ptt }
        let model = AppModel(settings: settings, live: true)
        let sent = SampleCollector()
        model.start()
        defer { model.shutdown() }
        var opened: Bool?
        model.openVideo(url) { opened = $0 }
        guard Demo.wait(timeout: 10, until: { opened != nil }), opened == true, Demo.wait(timeout: 10, until: { model.canTransmit }) else {
            report(name, false, "影片開不起來，或一直沒有來源畫面")
            return
        }
        let normalStall = AppModel.outputStallSeconds
        AppModel.outputStallSeconds = 1.0   // 測試時不必真的等三秒
        defer { AppModel.outputStallSeconds = normalStall }
        let stallNotice = L("音訊輸出裝置沒有回應（可能被拔掉了），已停止發射並放開 PTT")
        var problems: [String] = [], details: [String] = []

        /// 開始發射，等到真的在送、而且送了至少 seconds 秒
        func transmit(for seconds: Double) -> Bool {
            model.startTransmit()
            return Demo.wait(timeout: 8) { model.txPhase == .transmitting && model.txElapsed >= seconds }
        }

        // 1. 發射中輸出裝置停住，沒有人按停止
        model.debugUseSimulatedOutput(sink: { sent.append($0) }, stallAfter: 1.2)
        _ = ptt.take()
        var began = Date()
        model.startTransmit()
        if Demo.wait(timeout: 8, until: { model.txPhase == .idle }) {
            let took = Date().timeIntervalSince(began)
            Demo.spin(0.3)   // PTT 的放開在背景佇列上做
            if ptt.take() != [true, false] { problems.append("輸出停住：PTT 不是「按下一次、放開一次」") }
            if model.notice != stallNotice { problems.append("輸出停住：底列的訊息是「\(model.notice ?? "無")」") }
            // 輸出取了 1.2 秒之後停住，再過 1 秒的時限才認定；加上開始前的準備
            if took < 2.0 || took > 4.5 { problems.append(String(format: "輸出停住：%.1f 秒後才停止，預期 2–4.5 秒", took)) }
            details.append(String(format: "輸出停住 %.1f 秒後自動停止並放開 PTT", max(0, took - 1.2)))
        } else {
            problems.append("輸出停住之後發射沒有自動停止（PTT 一直按著）")
            model.abortTransmit()
        }

        // 2. 已經按了停止、正在等這一張送完，輸出才停住
        model.debugUseSimulatedOutput(sink: { sent.append($0) }, stallAfter: 1.6)
        _ = ptt.take()
        if transmit(for: 0.5) {
            model.stopTransmit()
            began = Date()
            if model.txPhase != .stopping { problems.append("按停止之後不是「結束發射中」") }
            if Demo.wait(timeout: 8, until: { model.txPhase == .idle }) {
                Demo.spin(0.3)
                if ptt.take() != [true, false] { problems.append("結束發射中輸出停住：PTT 不是「按下一次、放開一次」") }
                if model.notice != stallNotice { problems.append("結束發射中輸出停住：底列的訊息是「\(model.notice ?? "無")」") }
                // 這一張要 2.72 秒才送完；輸出在 1.6 秒停住，所以是保險讓它結束的（約按停止後 2.1 秒），不是正常送完
                let took = Date().timeIntervalSince(began)
                if took > 3.5 { problems.append(String(format: "結束發射中輸出停住：按停止後 %.1f 秒才結束", took)) }
            } else {
                problems.append("結束發射中輸出停住之後一直沒有結束（PTT 一直按著）")
                model.abortTransmit()
            }
        } else {
            problems.append("第二次發射沒有開始")
            model.abortTransmit()
        }

        // 3. 「結束發射中」再按一次：立刻停止
        model.debugUseSimulatedOutput(sink: { sent.append($0) }, stallAfter: nil)
        _ = ptt.take()
        _ = sent.take()
        if transmit(for: 0.5) {
            model.toggleTransmit()   // 第一次：送完這一張就停
            if model.txPhase != .stopping { problems.append("按發射鈕停止之後不是「結束發射中」") }
            model.toggleTransmit()   // 馬上又按：當成連按，不算
            if model.txPhase != .stopping { problems.append("連按兩下就立刻停止了（最後一張會被切掉）") }
            Demo.spin(AppModel.abortGuardSeconds + 0.1)
            if model.txPhase != .stopping { problems.append("還沒再按一次，這一張就結束了（測試的時間算錯了）") }
            model.toggleTransmit()   // 隔了半秒再按：立刻停止
            if model.txPhase != .idle { problems.append("「結束發射中」再按一次沒有立刻停止") }
            Demo.spin(0.3)
            if ptt.take() != [true, false] { problems.append("立刻停止：PTT 不是「按下一次、放開一次」") }
            if model.pttWarning != nil { problems.append("立刻停止：不該有 PTT 的警告") }
            let complete = Demo.receive(sent.take() + Demo.silence(seconds: 0.5)).flatMap(\.frames).filter(\.decoded)
            if !complete.isEmpty { problems.append("立刻停止：最後一張居然是完整的（\(complete.count) 張解得出來），沒有真的馬上停") }
            details.append("「結束發射中」再按一次立刻停止（半秒內連按的不算）")
        } else {
            problems.append("第三次發射沒有開始")
            model.abortTransmit()
        }

        // 4. 之後照常發射、正常停止：送出的幀都解得回來（立刻停止沒有把發射機的狀態弄亂）
        _ = ptt.take()
        _ = sent.take()
        model.startTransmit()
        if Demo.wait(timeout: 10, until: { model.txFramesSent >= 1 }) {
            model.stopTransmit()
            if Demo.wait(timeout: 10, until: { model.txPhase == .idle }) {
                Demo.spin(0.3)
                if ptt.take() != [true, false] { problems.append("之後的發射：PTT 不是「按下一次、放開一次」") }
                if let notice = model.notice { problems.append("之後的發射：\(notice)") }
                let good = Demo.receive(sent.take() + Demo.silence(seconds: 0.5)).flatMap(\.frames).filter { $0.decoded && $0.headerOK && $0.callsign == "N0CALL" }
                if good.count != model.txFramesSent || good.isEmpty { problems.append("之後的發射：送出 \(model.txFramesSent) 張，解回來 \(good.count) 張") }
                details.append("之後照常發射，\(good.count) 張都解得回來")
            } else {
                problems.append("之後的發射：按停止後沒有結束")
                model.abortTransmit()
            }
        } else {
            problems.append("之後的發射：一張都沒送完")
            model.abortTransmit()
        }

        // 5. 電腦即將睡眠：和系統真的要睡眠時送的是同一個通知。處理完就要已經停了、PTT 已經放開了——
        //    這裡不轉動 run loop、也不等背景佇列，直接看結果
        _ = ptt.take()
        if transmit(for: 0.4) {
            ptt.releaseDelay = 0.3   // 放開要花 0.3 秒：沒有等它做完的話，下面看到的就只有「按下」
            NSWorkspace.shared.notificationCenter.post(name: NSWorkspace.willSleepNotification, object: NSWorkspace.shared)
            if model.txPhase != .idle { problems.append("電腦要睡眠時發射沒有停") }
            if ptt.take() != [true, false] { problems.append("電腦要睡眠時，通知處理完了 PTT 還沒放開") }
            ptt.releaseDelay = 0
            if model.notice != L("電腦進入睡眠，已停止發射並放開 PTT") { problems.append("電腦要睡眠時底列的訊息是「\(model.notice ?? "無")」") }
            details.append("電腦要睡眠時立刻停止，通知處理完 PTT 已經放開")
        } else {
            problems.append("第五次發射沒有開始")
            model.abortTransmit()
        }
        // 剛按下發射、還在準備第一張的時候要睡眠：直接取消，PTT 不會按下去
        model.startTransmit()
        NSWorkspace.shared.notificationCenter.post(name: NSWorkspace.willSleepNotification, object: NSWorkspace.shared)
        if model.txPhase != .idle { problems.append("準備第一張時電腦要睡眠：發射沒有取消") }
        Demo.spin(1.0)
        if ptt.take().last == true { problems.append("準備第一張時電腦要睡眠：之後 PTT 還是按下去了") }
        // 沒有在發射的時候：什麼都不做
        NSWorkspace.shared.notificationCenter.post(name: NSWorkspace.willSleepNotification, object: NSWorkspace.shared)
        if model.txPhase != .idle || !ptt.take().isEmpty { problems.append("沒有在發射時電腦要睡眠：不該動到 PTT") }

        // 6. PTT 的指令說不準有沒有放開：要跳出警告
        let failing = FailingPTT()
        settings.debugPTTDriver = { failing }
        func attempt(_ label: String, down: Error?, up: Error?, expectWarning: Bool) {
            failing.keyDownError = down
            failing.keyUpError = up
            model.pttWarning = nil
            _ = failing.take()
            model.startTransmit()
            if down == nil {
                // 按得下去：送一點點之後立刻停止，放開的時候出錯
                if Demo.wait(timeout: 8, until: { model.txPhase == .transmitting && model.txElapsed >= 0.3 }) { model.abortTransmit() }
            }
            guard Demo.wait(timeout: 8, until: { model.txPhase == .idle }) else {
                problems.append("\(label)：發射沒有結束")
                model.abortTransmit()
                return
            }
            Demo.spin(0.4)
            if failing.take() != [true, false] { problems.append("\(label)：PTT 不是「按下一次、放開一次」") }
            if (model.pttWarning != nil) != expectWarning { problems.append("\(label)：\(expectWarning ? "沒有跳出警告" : "不該跳出警告")") }
            if expectWarning, let warning = model.pttWarning, !warning.contains("測試用") { problems.append("\(label)：警告裡沒有寫原因") }
        }
        attempt("放開失敗", down: nil, up: PTTError(message: "測試用：放開失敗"), expectWarning: true)
        attempt("按下沒有得到確認", down: PTTError(message: "測試用：按下沒有確認", mayBeKeyed: true), up: nil, expectWarning: true)
        attempt("按下失敗、之後的放開也失敗", down: PTTError(message: "測試用：按下失敗"), up: PTTError(message: "測試用：放開失敗"), expectWarning: true)
        attempt("電台根本沒開起來", down: PTTError(message: "測試用：開不起來"), up: nil, expectWarning: false)
        model.pttWarning = nil
        details.append("PTT 的指令說不準有沒有放開時跳出警告，確定沒按著的不警告")

        report(name, problems.isEmpty, problems.isEmpty ? details.joined(separator: "；") : problems.joined(separator: "；"))
    }

    // MARK: 定時發射

    /// 定時發射的時刻怎麼算：對齊整點的固定間隔。
    private static func testScheduleTimes() {
        let name = "定時發射：下一次的時刻"
        var problems: [String] = []
        func calendar(_ zone: String) -> Calendar {
            var calendar = Calendar(identifier: .gregorian)
            calendar.timeZone = TimeZone(identifier: zone) ?? .current
            return calendar
        }
        func at(_ calendar: Calendar, _ month: Int, _ day: Int, _ hour: Int, _ minute: Int, _ second: Double = 0) -> Date {
            let whole = calendar.date(from: DateComponents(year: 2026, month: month, day: day, hour: hour, minute: minute)) ?? Date()
            return whole.addingTimeInterval(second)
        }
        let taipei = calendar("Asia/Taipei")
        let cases: [(from: Date, interval: Int, expected: Date)] = [
            (at(taipei, 11, 24, 12, 0), 15, at(taipei, 11, 24, 12, 15)),          // 剛好在時刻上：算下一個
            (at(taipei, 11, 24, 12, 14, 59.9), 15, at(taipei, 11, 24, 12, 15)),
            (at(taipei, 11, 24, 12, 15, 0.1), 15, at(taipei, 11, 24, 12, 30)),
            (at(taipei, 11, 24, 12, 59, 30), 15, at(taipei, 11, 24, 13, 0)),
            (at(taipei, 11, 24, 23, 50), 20, at(taipei, 11, 25, 0, 0)),           // 跨過午夜
            (at(taipei, 11, 24, 12, 7, 3), 5, at(taipei, 11, 24, 12, 10)),
            (at(taipei, 11, 24, 12, 7, 3), 10, at(taipei, 11, 24, 12, 10)),
            (at(taipei, 11, 24, 12, 7, 3), 20, at(taipei, 11, 24, 12, 20)),
            (at(taipei, 11, 24, 12, 7, 3), 30, at(taipei, 11, 24, 12, 30)),
            (at(taipei, 11, 24, 12, 7, 3), 60, at(taipei, 11, 24, 13, 0)),
        ]
        for item in cases {
            let got = TransmitSchedule.nextFire(after: item.from, intervalMinutes: item.interval, calendar: taipei)
            if abs(got.timeIntervalSince(item.expected)) > 0.001 {
                problems.append("每 \(item.interval) 分鐘：算出 \(TransmitSchedule.clockText(got, calendar: taipei))，應該是 \(TransmitSchedule.clockText(item.expected, calendar: taipei))")
            }
        }
        // 時區和世界協調時間差的不是整點的地方（印度是 +5:30）：照當地的時鐘對齊
        let kolkata = calendar("Asia/Kolkata")
        let india = TransmitSchedule.nextFire(after: at(kolkata, 11, 24, 12, 7), intervalMinutes: 15, calendar: kolkata)
        if TransmitSchedule.clockText(india, calendar: kolkata) != "12:15" {
            problems.append("印度的時區算出 \(TransmitSchedule.clockText(india, calendar: kolkata))，應該是 12:15")
        }
        // 連續走 30 小時，跨過日光節約時間結束的那一夜（紐約，2026-11-01 凌晨 2 點撥回 1 點）：
        // 每一個時刻都剛好比前一個晚一個間隔，當地時鐘上的分是間隔的倍數、秒是 0
        let newYork = calendar("America/New_York")
        var walked = 0
        for interval in TransmitSchedule.intervals {
            var time = at(newYork, 10, 31, 20, 3)
            let end = time.addingTimeInterval(30 * 3600)
            var previous: Date?
            var bad = 0
            while time < end {
                let next = TransmitSchedule.nextFire(after: time, intervalMinutes: interval, calendar: newYork)
                let parts = newYork.dateComponents([.minute, .second], from: next)
                let whole = abs(next.timeIntervalSinceReferenceDate - next.timeIntervalSinceReferenceDate.rounded()) < 1e-6
                if next <= time || (parts.minute ?? 1) % interval != 0 || parts.second != 0 || !whole { bad += 1 }
                if let previous, abs(next.timeIntervalSince(previous) - Double(interval * 60)) > 0.001 { bad += 1 }
                previous = next
                time = next
                walked += 1
            }
            if bad > 0 { problems.append("每 \(interval) 分鐘、跨過日光節約時間結束的那一夜：有 \(bad) 個時刻不對") }
        }
        if TransmitSchedule.minutesText(15) != "00、15、30、45" || TransmitSchedule.minutesText(5) != "00、05、10 … 55"
            || TransmitSchedule.minutesText(60) != "00" {
            problems.append("列出一小時裡哪幾分的文字不對：\(TransmitSchedule.minutesText(15))／\(TransmitSchedule.minutesText(5))／\(TransmitSchedule.minutesText(60))")
        }
        report(name, problems.isEmpty, problems.isEmpty
               ? "\(cases.count) 個例子、印度的時區、紐約跨過日光節約時間結束的 30 小時（\(walked) 個時刻）都對"
               : problems.joined(separator: "；"))
    }

    /// 這個行程現在有沒有向系統宣告「不要因為閒置而睡眠」（查不到時回傳 nil）。
    private static func holdsIdleSleepAssertion() -> Bool? {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/pmset")
        process.arguments = ["-g", "assertions"]
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = Pipe()
        guard (try? process.run()) != nil else { return nil }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        guard process.terminationStatus == 0, let text = String(data: data, encoding: .utf8) else { return nil }
        let mine = "pid \(getpid())("
        return text.split(separator: "\n").contains { $0.contains(mine) && $0.contains("PreventUserIdleSystemSleep") }
    }

    /// 定時發射的整個流程。和影片來源那一項一樣，用的是 app 真正的狀態物件、模擬的音訊輸出與記錄用的 PTT，
    /// 另外把時鐘換成假的（從某個時刻的前一秒開始，照實際經過的時間往前走），不必真的等到整點：
    ///   * 還沒選影片、沒有呼號時到了時間不發射，原因記下來
    ///   * 到了時間自動開始：來源是鏡頭就先換成影片檔，影片從頭播（不是從原來停的地方），
    ///     勾了循環播放也只播一次，播完自動停止、放開 PTT，來源換回鏡頭；送出去的每一張都解得回來
    ///   * 到了時間正在發射、或正在收別人的畫面：那一次跳過（後者可以關掉）
    ///   * 錯過了時間（時鐘一下子跳過去，就像電腦剛睡醒）不補發；時鐘被往回調、間隔改了，下一次的時刻跟著重算
    ///   * 影片一直沒有播完（中途被換成別的來源）：超過上限自動停止
    ///   * 開著的時候向系統宣告不要因為閒置而睡眠，關掉就收回
    ///   * 上次選的影片檔，啟動時自動開啟
    private static func testScheduledTransmit(_ image: Data) {
        let name = "定時發射：到了時間自動發射影片，播完自動停止"
        let seconds = 3
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("nbtv-selftest-schedule-\(getpid()).mp4")
        defer { try? FileManager.default.removeItem(at: url) }
        guard let cg = NBTV.image(fromRGB: image), Demo.makeVideo(at: url, from: cg, seconds: seconds, fps: 10) else {
            report(name, false, "示範影片做不出來")
            return
        }
        // 影片開頭與結尾那一格縮成 96×72 的樣子，之後拿來認收到的畫面
        let reference = VideoFileSource()
        defer { reference.close() }
        var referenceOpened: Bool?
        reference.open(url) { referenceOpened = $0 }
        guard Demo.wait(timeout: 10, until: { referenceOpened != nil }), referenceOpened == true else {
            report(name, false, "影片無法開啟")
            return
        }
        func picture(at time: Double) -> Data? {
            var arrived = false
            reference.player.seek(to: CMTime(seconds: time, preferredTimescale: 600), toleranceBefore: .zero, toleranceAfter: .zero) { _ in
                DispatchQueue.main.async { arrived = true }
            }
            guard Demo.wait(timeout: 5, until: { arrived }) else { return nil }
            var rgb: Data?
            _ = Demo.wait(timeout: 5) {
                if let buffer = reference.copyCurrentFrame() { rgb = FrameScaler.scale(buffer, mode: .fill)?.rgb }
                return rgb != nil
            }
            return rgb
        }
        guard let first = picture(at: 0), let last = picture(at: Double(seconds)), difference(first, last) > 12 else {
            report(name, false, "取不到影片開頭與結尾的畫面，或兩者太像")
            return
        }

        setenv("NBTV_NO_DEVICES", "1", 1)   // 不開輸入裝置，也不開鏡頭
        let settings = AppSettings(store: nil)
        settings.callsign = "N0CALL"
        settings.sourceKind = .video
        settings.loopVideo = true    // 定時發射不管這個，照樣只播一次
        settings.txModeID = 12       // 模式 12：每張 1.04 秒
        let ptt = RecordingPTT()
        settings.debugPTTDriver = { ptt }
        let model = AppModel(settings: settings, live: true)
        let sent = SampleCollector()
        model.debugUseSimulatedOutput { sent.append($0) }
        let margin = AppModel.scheduleMarginSeconds
        AppModel.scheduleMarginSeconds = 1   // 「超過上限自動停止」那一段不必等那麼久
        model.start()
        defer {
            model.shutdown()
            AppModel.scheduleMarginSeconds = margin
        }

        // 假的時鐘：從 setClock 給的時刻開始，照實際經過的時間往前走
        let calendar = Calendar.current
        func moment(_ hour: Int, _ minute: Int, _ second: Double = 0) -> Date {
            (calendar.date(bySettingHour: hour, minute: minute, second: 0, of: Date()) ?? Date()).addingTimeInterval(second)
        }
        var base = Date(), began = Date()
        func setClock(_ hour: Int, _ minute: Int, _ second: Double) {
            base = moment(hour, minute, second)
            began = Date()
        }
        model.clock = { base.addingTimeInterval(Date().timeIntervalSince(began)) }
        /// 關掉、把時鐘撥到某個時刻、再打開：下一次的時刻從新的時間算起
        func rearm(_ hour: Int, _ minute: Int, _ second: Double) {
            model.setScheduleArmed(false)
            setClock(hour, minute, second)
            model.setScheduleArmed(true)
        }
        func next() -> String { model.scheduleNextFire.map { TransmitSchedule.clockText($0) } ?? "沒有" }
        /// 等到「上一次」的結果是某個時刻的
        func waitForEvent(_ label: String, timeout: Double = 4) -> String? {
            _ = Demo.wait(timeout: timeout) { model.scheduleLastEvent?.hasPrefix(label) == true }
            return model.scheduleLastEvent?.hasPrefix(label) == true ? model.scheduleLastEvent : nil
        }
        var problems: [String] = []
        var details: [String] = []

        // 1. 還沒選影片：打開之後到了時間不發射。打開的時候順便看睡眠的宣告
        rearm(12, 14, 59.2)
        if next() != "12:15" { problems.append("12:14:59 打開，下一次是 \(next())，應該是 12:15") }
        if !model.scheduleSummary.contains("還沒選擇影片檔") { problems.append("還沒選影片時頂列寫的是「\(model.scheduleSummary)」") }
        let heldWhileOn = holdsIdleSleepAssertion()
        if let event = waitForEvent("12:15") {
            if !event.contains("還沒選擇影片檔") { problems.append("還沒選影片：記下來的是「\(event)」") }
        } else {
            problems.append("還沒選影片：到了時間沒有記下任何結果")
        }
        if model.txPhase != .idle || !ptt.take().isEmpty { problems.append("還沒選影片卻開始發射了") }
        if next() != "12:30" { problems.append("12:15 過了之後下一次是 \(next())，應該是 12:30") }

        // 2. 開影片（循環播放，所以一開就播），再把來源換成鏡頭（沒有鏡頭可開，不影響）；影片停在快到結尾的地方
        var opened: Bool?
        model.openVideo(url) { opened = $0 }
        guard Demo.wait(timeout: 10, until: { opened != nil }), opened == true else {
            report(name, false, "影片開不起來")
            return
        }
        Demo.spin(0.4)
        model.setSourceKind(.camera)
        model.scrubVideo(to: 2.4, editing: false)
        Demo.spin(0.4)
        if model.videoPlaying { problems.append("來源換成鏡頭之後影片還在播") }

        // 3. 到了時間自動發射：從頭播、只播一次、播完自動停止、來源換回鏡頭
        rearm(12, 29, 59.0)
        _ = sent.take()
        _ = ptt.take()
        if Demo.wait(timeout: 4, until: { model.txPhase != .idle }) {
            let late = model.clock().timeIntervalSince(moment(12, 30))
            if late < 0 || late > 0.5 { problems.append(String(format: "開始的時間不對：比 12:30:00 %@ %.2f 秒", late < 0 ? "早" : "晚", abs(late))) }
            if !model.scheduledRun || settings.sourceKind != .video || model.scheduleSummary != "定時發射中" {
                problems.append("開始之後的狀態不對（定時發射中 \(model.scheduledRun)、來源 \(settings.sourceKind.rawValue)、頂列「\(model.scheduleSummary)」）")
            }
            var onAirLate: Double?
            var played = false
            let finished = Demo.wait(timeout: 25) {
                if onAirLate == nil, model.isOnAir { onAirLate = model.clock().timeIntervalSince(moment(12, 30)) }
                if model.txPhase != .idle, model.videoPlaying { played = true }
                return model.txPhase == .idle
            }
            if !finished {
                problems.append("影片播完之後沒有自動停止")
                model.stopTransmit()
                _ = Demo.wait(timeout: 10) { model.txPhase == .idle }
            }
            if !played { problems.append("開始之後影片沒有播放") }
            Demo.spin(0.3)   // PTT 的放開在背景佇列上做
            if ptt.take() != [true, false] { problems.append("PTT 不是「按下一次、放開一次」") }
            let frames = Demo.receive(sent.take() + Demo.silence(seconds: 0.5)).flatMap(\.frames)
            let good = frames.filter { $0.decoded && $0.headerOK && $0.callsign == "N0CALL" }
            let count = model.txFramesSent
            if count < 4 || count > 6 { problems.append("送出 \(count) 張，預期 4–6 張") }
            if good.count != count || frames.count != count { problems.append("送出 \(count) 張，解回來 \(good.count) 張") }
            if let head = good.first?.rgb, let tail = good.last?.rgb {
                if difference(head, first) >= difference(head, last) { problems.append("第一張不是影片開頭的畫面（沒有從頭播）") }
                if difference(tail, last) >= difference(tail, first) { problems.append("最後一張不是影片結尾的畫面") }
            }
            if settings.sourceKind != .camera { problems.append("結束之後來源沒有換回鏡頭") }
            if model.scheduledRun || !model.scheduleArmed || next() != "12:45" {
                problems.append("結束之後的狀態不對（定時發射中 \(model.scheduledRun)、開著 \(model.scheduleArmed)、下一次 \(next())）")
            }
            if model.scheduleLastEvent != "12:30 送出 \(count) 張" { problems.append("記下來的結果是「\(model.scheduleLastEvent ?? "沒有")」") }
            if let notice = model.notice { problems.append("結束之後底列有訊息：\(notice)") }
            details.append(String(format: "12:30:00 過了 %.2f 秒開始、%.2f 秒開始送音訊，送出 %d 張、解回 %d 張，來源換回鏡頭",
                                  late, onAirLate ?? -1, count, good.count))
        } else {
            problems.append("到了 12:30 沒有開始發射（\(model.scheduleLastEvent ?? "沒有記下結果")）")
        }

        // 3b. 影片停在開頭、那一格先前已經當預覽取過了（沒有勾循環播放的影片開了之後就是這樣），來源再換成鏡頭：
        //     到了時間手上沒有影片的畫面，影片也不必移動位置，照樣要從開頭那一格送起。順便換一個間隔（每 5 分鐘）
        settings.loopVideo = false
        model.setSourceKind(.video)
        model.scrubVideo(to: 0, editing: false)
        Demo.spin(0.5)
        model.setSourceKind(.camera)
        settings.scheduleIntervalMinutes = 5
        rearm(12, 34, 59.0)
        _ = sent.take()
        _ = ptt.take()
        if Demo.wait(timeout: 4, until: { model.txPhase != .idle }), Demo.wait(timeout: 25, until: { model.txPhase == .idle }) {
            Demo.spin(0.3)
            let frames = Demo.receive(sent.take() + Demo.silence(seconds: 0.5)).flatMap(\.frames)
            let good = frames.filter { $0.decoded && $0.headerOK && $0.callsign == "N0CALL" }
            let count = model.txFramesSent
            if model.scheduleLastEvent != "12:35 送出 \(count) 張" || count < 4 || count > 6 || good.count != count {
                problems.append("影片停在開頭、來源是鏡頭：記下來的是「\(model.scheduleLastEvent ?? "沒有")」，解回來 \(good.count) 張")
            }
            if let head = good.first?.rgb, difference(head, first) >= difference(head, last) {
                problems.append("影片停在開頭、來源是鏡頭：第一張不是影片開頭的畫面")
            }
            if ptt.take() != [true, false] { problems.append("影片停在開頭、來源是鏡頭：PTT 不是「按下一次、放開一次」") }
            if settings.sourceKind != .camera || next() != "12:40" {
                problems.append("影片停在開頭、來源是鏡頭：結束之後來源是 \(settings.sourceKind.rawValue)、下一次 \(next())")
            }
            details.append("影片停在開頭、來源是鏡頭時（每 5 分鐘）也送出 \(count) 張")
        } else {
            problems.append("影片停在開頭、來源是鏡頭：到了 12:35 沒有發射，或沒有自己結束（\(model.scheduleLastEvent ?? "沒有記下結果")）")
            model.stopTransmit()
            _ = Demo.wait(timeout: 10) { model.txPhase == .idle }
        }
        settings.scheduleIntervalMinutes = 15
        settings.loopVideo = true

        // 4. 到了時間正在發射（自己按的）：那一次跳過，正在進行的發射不受影響
        model.setSourceKind(.video)
        Demo.spin(0.3)
        rearm(12, 44, 59.0)
        _ = ptt.take()
        model.startTransmit()
        if let event = waitForEvent("12:45") {
            if !event.contains("正在發射") { problems.append("正在發射時到了時間：記下來的是「\(event)」") }
        } else {
            problems.append("正在發射時到了時間：沒有記下結果")
        }
        if model.txPhase == .idle || model.scheduledRun { problems.append("正在發射時到了時間：原來的發射被打斷，或被當成定時發射") }
        _ = Demo.wait(timeout: 10) { model.txPhase == .transmitting }
        model.stopTransmit()
        if !Demo.wait(timeout: 10, until: { model.txPhase == .idle }) { problems.append("自己按的發射停不下來") }
        Demo.spin(0.3)
        if ptt.take() != [true, false] { problems.append("正在發射時到了時間：PTT 不是「按下一次、放開一次」") }
        if next() != "13:00" { problems.append("12:45 過了之後下一次是 \(next())，應該是 13:00") }

        // 5. 到了時間正在收別人的畫面：跳過；把那個選項關掉就照常發射。
        //    這裡是對方連續送的時候兩張之間的空檔：到時間的前一秒多還同步著，那一瞬間剛好沒有
        rearm(12, 59, 58.6)
        var signal = RxUpdate()
        signal.synced = true
        signal.modeID = 12
        model.apply(signal)
        model.apply(RxUpdate())
        if model.synced { problems.append("測試的前提不對：同步的狀態沒有清掉") }
        if let event = waitForEvent("13:00") {
            if !event.contains("正在收別人的畫面") { problems.append("正在收畫面時到了時間：記下來的是「\(event)」") }
        } else {
            problems.append("正在收畫面時到了時間：沒有記下結果")
        }
        if model.txPhase != .idle { problems.append("正在收畫面時到了時間：還是發射了") }
        settings.scheduleSkipWhenBusy = false
        rearm(13, 14, 59.2)
        model.synced = true
        if Demo.wait(timeout: 4, until: { model.txPhase == .transmitting }) {
            model.stopTransmit()
            _ = Demo.wait(timeout: 10) { model.txPhase == .idle }
            if waitForEvent("13:15")?.contains("送出") != true { problems.append("選項關掉之後：記下來的是「\(model.scheduleLastEvent ?? "沒有")」") }
        } else {
            problems.append("「正在收畫面時不發射」關掉之後，到了時間沒有發射")
            _ = Demo.wait(timeout: 10) { model.txPhase == .idle }
        }
        settings.scheduleSkipWhenBusy = true
        model.synced = false
        Demo.spin(0.3)
        _ = ptt.take()

        // 6. 最後一分鐘倒數；時鐘一下子跳過那個時刻（電腦剛睡醒）不補發；時鐘被往回調、改了間隔，下一次跟著重算
        rearm(13, 29, 50.0)
        Demo.spin(0.3)
        if !(8...10).contains(model.scheduleCountdown ?? -1) || !model.scheduleSummary.hasSuffix("秒後發射") {
            problems.append("最後一分鐘的倒數不對：\(model.scheduleCountdown.map { "\($0)" } ?? "沒有")，頂列「\(model.scheduleSummary)」")
        }
        setClock(13, 30, 30.0)
        if let event = waitForEvent("13:30") {
            if !event.contains("錯過了時間") { problems.append("錯過了時間：記下來的是「\(event)」") }
        } else {
            problems.append("錯過了時間：沒有記下結果")
        }
        if model.txPhase != .idle || !ptt.take().isEmpty { problems.append("錯過了時間卻補發了") }
        if next() != "13:45" { problems.append("錯過 13:30 之後下一次是 \(next())，應該是 13:45") }
        setClock(13, 2, 0)
        Demo.spin(0.3)
        if next() != "13:15" { problems.append("時鐘調回 13:02 之後下一次是 \(next())，應該是 13:15") }
        settings.scheduleIntervalMinutes = 5
        let five = next()
        settings.scheduleIntervalMinutes = 60
        let sixty = next()
        settings.scheduleIntervalMinutes = 15
        if five != "13:05" || sixty != "14:00" || next() != "13:15" {
            problems.append("改間隔之後下一次的時刻不對：每 5 分鐘 \(five)、每 60 分鐘 \(sixty)、改回 15 分鐘 \(next())")
        }

        // 7. 沒有呼號：不發射
        rearm(14, 14, 59.3)
        settings.callsign = ""
        if waitForEvent("14:15")?.contains("還沒設定呼號") != true { problems.append("沒有呼號：記下來的是「\(model.scheduleLastEvent ?? "沒有")」") }
        if model.txPhase != .idle || !ptt.take().isEmpty { problems.append("沒有呼號卻發射了") }
        settings.callsign = "N0CALL"

        // 8. 影片一直沒有播完（開始之後來源被換成鏡頭，影片停住了）：超過上限自動停止
        //    上限是影片 3 秒＋兩張 2.08 秒＋測試時改成的 1 秒
        rearm(14, 29, 59.0)
        _ = sent.take()
        if Demo.wait(timeout: 5, until: { model.txPhase == .transmitting }) {
            let began = Date()
            model.setSourceKind(.camera)
            let stopped = Demo.wait(timeout: 14) { model.txPhase == .idle }
            let took = Date().timeIntervalSince(began)
            Demo.spin(0.3)
            if !stopped {
                problems.append("影片沒有播完：超過上限之後沒有自動停止")
                model.stopTransmit()
                _ = Demo.wait(timeout: 10) { model.txPhase == .idle }
            } else if took < 4 || took > 9 {
                problems.append(String(format: "影片沒有播完：%.1f 秒後停止，預期 6 秒左右", took))
            }
            if model.scheduleLastEvent?.contains("自動停止") != true || model.notice?.contains("自動停止") != true {
                problems.append("超過上限自動停止：記下來的是「\(model.scheduleLastEvent ?? "沒有")」，底列「\(model.notice ?? "沒有")」")
            }
            if ptt.take() != [true, false] { problems.append("超過上限自動停止：PTT 不是「按下一次、放開一次」") }
            if settings.sourceKind != .camera { problems.append("發射中自己換的來源，結束之後被換掉了") }
            details.append(String(format: "影片沒有播完時 %.1f 秒後自動停止", took))
        } else {
            problems.append("到了 14:30 沒有開始發射（\(model.scheduleLastEvent ?? "沒有記下結果")）")
        }

        // 9. 關掉：不再有下一次，睡眠的宣告也收回
        model.setScheduleArmed(false)
        if model.scheduleNextFire != nil || model.scheduleSummary != "定時發射" { problems.append("關掉之後還有下一次的時刻") }
        let heldWhileOff = holdsIdleSleepAssertion()
        if let heldWhileOn, let heldWhileOff {
            if !heldWhileOn { problems.append("開著的時候沒有向系統宣告不要睡眠") }
            if heldWhileOff { problems.append("關掉之後睡眠的宣告沒有收回") }
            details.append("開著的時候有不要睡眠的宣告、關掉就收回")
        } else {
            details.append("睡眠的宣告查不到（pmset 不能用），沒有檢查")
        }

        // 10. 上次選的影片檔：啟動時自動開啟（來源是鏡頭，所以只開啟、不播放）；檔案不在了就當作沒有
        for (path, expected) in [(url.path, true), (url.path + ".missing", false)] {
            let again = AppSettings(store: nil)
            again.sourceKind = .camera
            again.videoPath = path
            let second = AppModel(settings: again, live: true)
            second.start()
            let reopened = Demo.wait(timeout: expected ? 10 : 1) { second.videoName != nil }
            Demo.spin(0.3)
            if reopened != expected || second.videoPlaying {
                problems.append(expected ? "上次選的影片檔啟動時沒有自動開啟（或自己播起來了）" : "不存在的影片檔被當成開啟了")
            }
            second.shutdown()
        }
        details.append("上次選的影片檔啟動時自動開啟")

        report(name, problems.isEmpty, problems.isEmpty
               ? details.joined(separator: "；") + "；沒有影片、沒有呼號、正在發射、正在收畫面、錯過時間的那幾次都沒有發射"
               : problems.joined(separator: "；"))
    }

    // MARK: 序列埠

    /// 沒有接序列埠轉接器時只能測錯誤的處理：打不開的埠要回報原因，不能當掉或卡住。
    private static func testSerialErrors() {
        let ports = SerialPTT.availablePorts()
        do {
            try SerialPTT(path: "/dev/cu.nbtv-selftest-nonexistent", line: .rts).key(true)
            report("序列埠 PTT 的錯誤處理", false, "打不開的序列埠沒有報錯")
        } catch {
            report("序列埠 PTT 的錯誤處理", true, "\(error.localizedDescription)；系統上有 \(ports.count) 個序列埠，沒有去動它們")
        }
        // 用一個虛擬終端機（pty）走一遍「開啟 → 設定控制線 → 放開」。pty 沒有真的控制線，
        // 所以這只能確認這段程式跑得完、該報錯時報錯；真的 RTS／DTR 有沒有動要接硬體才知道。
        let master = posix_openpt(O_RDWR | O_NOCTTY)
        guard master >= 0, grantpt(master) == 0, unlockpt(master) == 0, let name = ptsname(master) else {
            report("序列埠 PTT（虛擬終端機）", nil, "開不了虛擬終端機")
            return
        }
        defer { close(master) }
        let path = String(cString: name)
        var outcomes: [String] = []
        for line in [SerialLine.rts, .dtr] {
            let ptt = SerialPTT(path: path, line: line)
            do {
                try ptt.key(true)
                try ptt.key(false)
                outcomes.append("\(line.title)：按下、放開都沒有報錯")
            } catch {
                outcomes.append("\(line.title)：\(error.localizedDescription)")
            }
        }
        report("序列埠 PTT（虛擬終端機 \(path)）", true, outcomes.joined(separator: "；") + "。真的控制線沒有測")
    }

    // MARK: 介面的語言

    /// 一句話裡要填的值，依位置排好的型別（%1$@ 這種指定位置的照它指定的）：「@」「d」「f」。
    private static func placeholders(_ text: String) -> [String] {
        var found: [(position: Int, kind: String)] = []
        let characters = Array(text)
        var index = 0, next = 0
        while index < characters.count {
            guard characters[index] == "%" else {
                index += 1
                continue
            }
            var cursor = index + 1
            if cursor < characters.count, characters[cursor] == "%" {   // %% 是一個百分比符號
                index = cursor + 1
                continue
            }
            var digits = ""
            while cursor < characters.count, characters[cursor].isNumber {
                digits.append(characters[cursor])
                cursor += 1
            }
            var explicit: Int?
            if cursor < characters.count, characters[cursor] == "$" {
                explicit = Int(digits)
                cursor += 1
            }
            while cursor < characters.count, "0123456789.-+ #l".contains(characters[cursor]) { cursor += 1 }
            guard cursor < characters.count, "@dfs".contains(characters[cursor]) else {
                index += 1
                continue
            }
            next += 1
            found.append((explicit ?? next, String(characters[cursor])))
            index = cursor + 1
        }
        return found.sorted { $0.position < $1.position }.map(\.kind)
    }

    /// 介面的語言：系統的語言對到哪一份翻譯、四份翻譯齊不齊、要填的值對不對得上，以及查表本身。
    /// （原始碼裡的每一句有沒有都翻到，是建置時由 Localization/check_strings.py 對著原始碼檢查的；這裡檢查的是 app 裡實際帶著的檔案。）
    private static func testLocalization() {
        let name = "介面的語言：偵測、翻譯表與查表"
        defer { L10n.use(L10n.source) }
        var problems: [String] = []

        // 1. 系統的語言清單 → 用哪一種語言。清單裡排最前面、而且有翻譯的那一個；都沒有就是英文
        let cases: [([String], String)] = [
            (["zh-Hant-TW"], "zh-Hant"), (["zh-TW"], "zh-Hant"), (["zh-HK"], "zh-Hant"), (["zh-Hant-HK", "en-HK"], "zh-Hant"),
            (["zh-Hans-CN"], "zh-Hans"), (["zh-CN"], "zh-Hans"), (["zh-SG"], "zh-Hans"), (["zh-Hans-TW", "zh-Hant-TW"], "zh-Hans"),
            (["ja-JP"], "ja"), (["ja"], "ja"), (["es-ES"], "es"), (["es-MX"], "es"), (["es-419"], "es"), (["es-US", "en-US"], "es"),
            (["en-US"], "en"), (["en-GB"], "en"), (["en-AU", "zh-Hant-TW"], "en"),
            (["fr-FR"], "en"), (["ko-KR"], "en"), (["de-DE"], "en"), (["pt-BR"], "en"), (["ru-RU"], "en"), (["th-TH", "en-TH"], "en"),
            (["fr-FR", "ja-JP"], "ja"), (["ko-KR", "zh-Hant-TW", "en-US"], "zh-Hant"), ([], "en"),
        ]
        for (preferences, expected) in cases {
            let got = L10n.language(forPreferences: preferences)
            if got != expected { problems.append("系統語言 \(preferences) 應該用 \(expected)，結果是 \(got)") }
        }

        // 2. 四份翻譯：每一份的句子都一樣多、同樣的那幾句，沒有空的，要填的值和原文對得上
        var tables: [String: [String: String]] = [:]
        for language in L10n.languages where language != L10n.source {
            let table = L10n.loadTable(language)
            tables[language] = table
            if table.count < 250 { problems.append("\(language) 的翻譯表只有 \(table.count) 句（檔案不在 app 裡，或讀不懂）") }
            for (key, value) in table {
                let original = key.hasSuffix("#1") ? String(key.dropLast(2)) : key
                if value.trimmingCharacters(in: .whitespaces).isEmpty, !original.trimmingCharacters(in: .whitespaces).isEmpty {
                    problems.append("\(language)：「\(key.prefix(20))」的翻譯是空的")
                }
                if placeholders(value) != placeholders(original) {
                    problems.append("\(language)：「\(key.prefix(24))」要填的值對不上（原文 \(placeholders(original))，翻譯 \(placeholders(value))）")
                }
            }
        }
        let plain = tables.mapValues { Set($0.keys.filter { !$0.hasSuffix("#1") }) }
        if let reference = plain["en"] {
            for (language, keys) in plain where keys != reference {
                problems.append("\(language) 和英文的句子不一樣：多了 \(keys.subtracting(reference).count) 句、少了 \(reference.subtracting(keys).count) 句")
            }
            // 原文必須是繁體字；簡體中文的翻譯裡不能留著繁體字（挑幾個兩邊寫法不同的常用字來看）
            let traditionalOnly = Set("設檔訊號畫發鏡會時這個裝輸連線網頻預選擇開關張圖顯還沒電錯誤麥風權隱與產儲無從聲隨"), simplifiedOnly = Set("设档讯号画发镜会时这个装输连线网频预选择开关张图显还没电错误麦风权隐与产储无从声随")
            for key in reference where key.contains(where: { simplifiedOnly.contains($0) }) { problems.append("原文裡有簡體字：「\(key.prefix(20))」") }
            for (key, value) in tables["zh-Hans"] ?? [:] where value.contains(where: { traditionalOnly.contains($0) }) {
                problems.append("簡體中文的翻譯裡有繁體字：「\(key.prefix(16))」→「\(value.prefix(16))」")
            }
        }

        // 3. 查表：每種語言各看幾句（含要填值的、調換順序的、單數的），不支援的語言用英文
        let samples: [(String, [String])] = [   // 依序是 zh-Hant、zh-Hans、ja、es、en
            (L10n.source, ["開始發射", "第 3 張", "1 張", "2 張", "無法控制 /dev/x 的 RTS：原因", "模式 11"]),
            ("zh-Hans", ["开始发射", "第 3 张", "1 张", "2 张", "无法控制 /dev/x 的 RTS：原因", "模式 11"]),
            ("ja", ["送信開始", "3 枚目", "1 枚", "2 枚", "/dev/x の RTS を制御できません：原因", "モード 11"]),
            ("es", ["Transmitir", "Imagen 3", "1 imagen", "2 imágenes", "No se puede controlar la línea RTS de /dev/x: 原因", "Modo 11"]),
            ("en", ["Start transmitting", "Frame 3", "1 frame", "2 frames", "Cannot control RTS on /dev/x: 原因", "Mode 11"]),
            ("fr", ["Start transmitting", "Frame 3", "1 frame", "2 frames", "Cannot control RTS on /dev/x: 原因", "Mode 11"]),
        ]
        for (language, expected) in samples {
            L10n.use(language)
            let got = [L("開始發射"), L("第 %d 張", 3), L("%d 張", 1), L("%d 張", 2), L("無法控制 %@ 的 %@：%@", "/dev/x", "RTS", "原因"), L("模式 %d", 11)]
            if got != expected { problems.append("\(language)：查出來的是 \(got)") }
            if language != "fr", L10n.language != language { problems.append("改用 \(language) 之後目前的語言是 \(L10n.language)") }
            // 每個語言都有 app 的名稱與兩句權限的說明
            let info = Bundle.main.url(forResource: "InfoPlist", withExtension: "strings", subdirectory: nil, localization: L10n.language)
                .flatMap { NSDictionary(contentsOf: $0) as? [String: String] } ?? [:]
            if (L10n.appName ?? "").isEmpty || (info["NSCameraUsageDescription"] ?? "").isEmpty || (info["NSMicrophoneUsageDescription"] ?? "").isEmpty {
                problems.append("\(L10n.language) 的 InfoPlist.strings 少了名稱或權限的說明")
            }
        }
        // 沒有翻譯的句子原樣顯示（不會變成空的或當掉）
        L10n.use("en")
        let untranslated = "這一句" + "沒有翻譯 %d"   // 拆開來寫：建置時的檢查會把 L 後面直接寫的句子都當成要翻譯的
        if L(untranslated, 5) != "這一句沒有翻譯 5" { problems.append("沒有翻譯的句子沒有原樣顯示") }

        let counts = L10n.languages.filter { $0 != L10n.source }.map { "\($0) \(tables[$0]?.count ?? 0) 句" }.joined(separator: "、")
        report(name, problems.isEmpty, problems.isEmpty
               ? "系統語言 \(cases.count) 種情形都對到該用的語言；翻譯表 \(counts)，句子相同、要填的值都對得上；這台電腦目前會用 \(L10n.systemLanguage())"
               : problems.prefix(8).joined(separator: "；") + (problems.count > 8 ? "…（共 \(problems.count) 個問題）" : ""))
    }

    // MARK: 用 CAT 指令的 PTT（Hamlib）

    /// 假的 rigctld：聽本機的一個臨時連接埠。前 dropFirst 條連線一接起來就掛斷；之後的每一條，讀到指令就回 reply
    /// （reply 是 nil 的話接起來之後一直不回）。用來重現真的 rigctld 剛啟動時偶爾把連線掛斷、以及電台沒有回應的情形。
    /// 給了 respond 的話，回什麼由它照收到的指令（「T 1」「T 0」）決定，回 nil 就是不回；收到的指令都記在 commands 裡。
    private final class FakeRigctld {
        private(set) var port = 0
        private let listener: Int32
        private let lock = NSLock()
        private var accepted = 0
        private var held: [Int32] = []
        private var received: [String] = []

        var connections: Int {
            lock.lock()
            defer { lock.unlock() }
            return accepted
        }

        /// 到目前為止收到的指令（去掉換行）
        var commands: [String] {
            lock.lock()
            defer { lock.unlock() }
            return received
        }

        init?(dropFirst: Int, reply: String?, respond: ((String) -> String?)? = nil) {
            listener = socket(AF_INET, SOCK_STREAM, 0)
            guard listener >= 0 else { return nil }
            var address = sockaddr_in()
            address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
            address.sin_family = sa_family_t(AF_INET)
            address.sin_addr.s_addr = inet_addr("127.0.0.1")
            var length = socklen_t(MemoryLayout<sockaddr_in>.size)
            let ready = withUnsafeMutablePointer(to: &address) { pointer in
                pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    bind(listener, $0, length) == 0 && listen(listener, 8) == 0 && getsockname(listener, $0, &length) == 0
                }
            }
            guard ready else {
                close(listener)
                return nil
            }
            port = Int(UInt16(bigEndian: address.sin_port))
            // 執行緒只帶著這個數字，不抓住 self。名稱不能和屬性 listener 一樣：上面的閉包用的是屬性，
            // 同一個範圍裡後面再宣告同名的區域變數，Xcode 26 的 Swift 會當成「宣告之前就用到」而編不過。
            let listeningSocket = self.listener
            Thread.detachNewThread { [weak self] in
                while true {
                    let client = accept(listeningSocket, nil, nil)
                    guard client >= 0, let self else { return }
                    self.lock.lock()
                    self.accepted += 1
                    let number = self.accepted
                    self.lock.unlock()
                    var one: Int32 = 1
                    setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
                    if number <= dropFirst {
                        close(client)
                    } else if let respond {
                        var buffer = [UInt8](repeating: 0, count: 64)
                        let count = recv(client, &buffer, buffer.count, 0)
                        let command = String(decoding: buffer[0..<max(0, count)], as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
                        self.lock.lock()
                        self.received.append(command)
                        self.lock.unlock()
                        if let answer = respond(command) {
                            _ = Array(answer.utf8).withUnsafeBufferPointer { send(client, $0.baseAddress, $0.count, 0) }
                            close(client)
                        } else {
                            self.lock.lock()
                            self.held.append(client)
                            self.lock.unlock()
                        }
                    } else if let reply {
                        var buffer = [UInt8](repeating: 0, count: 64)
                        _ = recv(client, &buffer, buffer.count, 0)
                        _ = Array(reply.utf8).withUnsafeBufferPointer { send(client, $0.baseAddress, $0.count, 0) }
                        close(client)
                    } else {
                        self.lock.lock()
                        self.held.append(client)   // 接起來之後不回也不掛斷，到 stop 為止
                        self.lock.unlock()
                    }
                }
            }
        }

        func stop() {
            close(listener)
            lock.lock()
            for client in held { close(client) }
            held.removeAll()
            lock.unlock()
        }
    }

    /// 用電台的 CAT 指令控制 PTT：型號清單與序列埠規格的拆法、交給 rigctld 的參數，以及整個流程——
    /// 按下時自己開一個 rigctld、送「T 1」，放開時送「T 0」再把它結束。用的是 Hamlib 不接電台的示範機（型號 1），
    /// rigctld 只聽本機的一個臨時連接埠，不碰任何序列埠。另外檢查看門的程序：app 突然不在了，它要替我們放開 PTT、結束 rigctld。
    private static func testHamlib() {
        // 1. 不需要裝 Hamlib 的部分：文字的拆法與參數
        func pad(_ text: String, _ width: Int) -> String { text + String(repeating: " ", count: max(1, width - text.count)) }
        func row(_ id: String, _ fields: [String]) -> String {
            String(repeating: " ", count: 6 - id.count) + id + "  " + pad(fields[0], 23) + pad(fields[1], 24) + pad(fields[2], 16) + pad(fields[3], 12) + fields[4]
        }
        let listing = [
            row("Rig #", ["Mfg", "Model", "Version", "Status", "Macro"]),
            row("1", ["Hamlib", "Dummy", "20240709.0", "Stable", "RIG_MODEL_DUMMY"]),
            row("2", ["Hamlib", "NET rigctl", "20250211.0", "Stable", "RIG_MODEL_NETRIGCTL"]),
            row("2014", ["Kenwood", "TS-2000", "20250515.2", "Stable", "RIG_MODEL_TS2000"]),
            row("3073", ["Icom", "IC-7300", "20250517.14", "Stable", "RIG_MODEL_IC7300"]),
            row("3093", ["Icom", "ID-52A/E PLUS", "20250517.0", "Beta", "RIG_MODEL_ID52PLUS"]),
            row("9001", ["Example", "A Model Name Longer Than Its Column", "1.0", "Alpha", "RIG_MODEL_LONG"]),
        ].joined(separator: "\n")
        let parsed = Hamlib.parseModels(listing)
        var problems: [String] = []
        let wanted = [Hamlib.Model(id: 1, manufacturer: "Hamlib", name: "Dummy"), Hamlib.Model(id: 2, manufacturer: "Hamlib", name: "NET rigctl"),
                      Hamlib.Model(id: 2014, manufacturer: "Kenwood", name: "TS-2000"), Hamlib.Model(id: 3073, manufacturer: "Icom", name: "IC-7300"),
                      Hamlib.Model(id: 3093, manufacturer: "Icom", name: "ID-52A/E PLUS"),
                      Hamlib.Model(id: 9001, manufacturer: "Example", name: "A Model Name Longer Than Its Column")]
        if parsed != wanted { problems.append("型號清單拆成 \(parsed.map { "\($0.id):\($0.manufacturer)/\($0.name)" })") }
        let plain = Hamlib.parseSerialCaps("Model name:\tIC-7300\nSerial speed: 4800..115200 baud, 8N1, ctrl=NONE\nWrite delay: 0ms")
        let handshake = Hamlib.parseSerialCaps("Serial speed: 1200..115200 baud, 8N1, ctrl=CTS/RTS")
        if plain != Hamlib.SerialCaps(minBaud: 4800, maxBaud: 115200, hardwareHandshake: false)
            || handshake != Hamlib.SerialCaps(minBaud: 1200, maxBaud: 115200, hardwareHandshake: true)
            || Hamlib.parseSerialCaps("Port type:\tNone") != nil {
            problems.append("序列埠規格拆錯了")
        }
        let arguments = HamlibPTT.arguments(model: 3073, port: "/dev/cu.example", baud: 19200, tcpPort: 4599, caps: plain)
        if arguments != ["-m", "3073", "-T", "127.0.0.1", "-t", "4599", "-r", "/dev/cu.example", "-s", "19200", "-C", "rts_state=OFF,dtr_state=OFF"] {
            problems.append("rigctld 的參數是 \(arguments)")
        }
        // 預設的速率不指定；要用 RTS／CTS 流量控制的型號，RTS 不去指定；不走序列埠的型號不設定控制線
        if HamlibPTT.arguments(model: 2014, port: "/dev/cu.example", baud: 0, tcpPort: 4599, caps: handshake)
            != ["-m", "2014", "-T", "127.0.0.1", "-t", "4599", "-r", "/dev/cu.example", "-C", "dtr_state=OFF"]
            || HamlibPTT.arguments(model: 1, port: "", baud: 0, tcpPort: 4599, caps: nil) != ["-m", "1", "-T", "127.0.0.1", "-t", "4599"] {
            problems.append("預設速率、流量控制或示範機的參數不對")
        }
        let failure = "rig_open: error = yaesu: initrigs4_yaesu called\nserial_open: Unable to open /dev/cu.example - No such file or directory\n"
            + "rig.c(1284):rig_open returning2(-6) IO error\n\nIO error\n"
        if HamlibPTT.openFailureReason(in: failure) != "Unable to open /dev/cu.example - No such file or directory"
            || HamlibPTT.openFailureReason(in: "rig_open: error = x\nrig.c(1284):rig_open returning2(-5) Communication timed out\n") != "Communication timed out"
            || HamlibPTT.openFailureReason(in: "Opened rig model 1, 'Dummy'\n") != nil {
            problems.append("rigctld 的錯誤訊息判斷錯了")
        }
        // 送指令的重試（對著假的 rigctld）：剛啟動的 rigctld 偶爾會把頭幾條連線直接掛斷，要再試才送得到；
        // 連上了卻一直沒有回覆的（電台沒有回應）不再試，等到逾時就回報；一直被掛斷的試到期限為止
        let retriesBefore = HamlibPTT.retries
        if let flaky = FakeRigctld(dropFirst: 2, reply: "RPRT 0\n") {
            let reply = try? HamlibPTT.request("T 1", port: flaky.port, timeoutSeconds: 3, retryUntil: Date().addingTimeInterval(2))
            if reply?.contains("RPRT 0") != true || flaky.connections != 3 || HamlibPTT.retries - retriesBefore != 2 {
                problems.append("頭兩條連線被掛斷時：回覆 \(reply ?? "無")、連了 \(flaky.connections) 次、重試 \(HamlibPTT.retries - retriesBefore) 次")
            }
            flaky.stop()
        } else {
            problems.append("假的 rigctld 開不起來")
        }
        if let silent = FakeRigctld(dropFirst: 0, reply: nil) {
            let began = Date()
            do {
                _ = try HamlibPTT.request("T 1", port: silent.port, timeoutSeconds: 1, retryUntil: Date().addingTimeInterval(5))
                problems.append("對方一直不回覆也當成成功")
            } catch {
                let spent = Date().timeIntervalSince(began)
                if (error as? PTTError)?.timedOut != true || spent < 0.9 || spent > 1.8 || silent.connections != 1 {
                    problems.append(String(format: "對方不回覆時等了 %.1f 秒、連了 %d 次", spent, silent.connections))
                }
            }
            silent.stop()
        }
        if let dead = FakeRigctld(dropFirst: 1000, reply: "RPRT 0\n") {
            let began = Date()
            do {
                _ = try HamlibPTT.request("T 0", port: dead.port, timeoutSeconds: 3, retryUntil: Date().addingTimeInterval(0.5))
                problems.append("連線一直被掛斷也當成成功")
            } catch {
                let spent = Date().timeIntervalSince(began)
                if (error as? PTTError)?.timedOut != false || spent < 0.4 || spent > 1.2 || dead.connections < 3 {
                    problems.append(String(format: "連線一直被掛斷時試了 %.1f 秒、連了 %d 次", spent, dead.connections))
                }
            }
            dead.stop()
        }
        // 按下失敗時補送放開：「T 1」沒有得到確認（電台回報失敗，或一直沒回覆），指令卻可能已經到了電台，
        // 所以回報失敗之前要補送一次「T 0」；補送的有成功就不必警告，沒成功的話錯誤上要標著「PTT 可能還按著」
        func pressFailure(_ label: String, t1: String?, t0: String?, expectKeyed: Bool, seconds: ClosedRange<Double>) {
            guard let fake = FakeRigctld(dropFirst: 0, reply: nil, respond: { $0 == "T 1" ? t1 : t0 }) else {
                problems.append("假的 rigctld 開不起來")
                return
            }
            defer { fake.stop() }
            let began = Date()
            do {
                try HamlibPTT.press(port: fake.port)
                problems.append("\(label)：按下失敗卻當成成功")
            } catch {
                let spent = Date().timeIntervalSince(began)
                if fake.commands != ["T 1", "T 0"] { problems.append("\(label)：送出的指令是 \(fake.commands)，應該是 T 1 之後補送 T 0") }
                if (error as? PTTError)?.mayBeKeyed != expectKeyed { problems.append("\(label)：「PTT 可能還按著」的標記不對") }
                if !seconds.contains(spent) { problems.append(String(format: "%@：花了 %.1f 秒", label, spent)) }
            }
        }
        pressFailure("電台回報失敗、補送的放開成功", t1: "RPRT -5\n", t0: "RPRT 0\n", expectKeyed: false, seconds: 0...1)
        pressFailure("電台回報失敗、補送的放開也失敗", t1: "RPRT -5\n", t0: "RPRT -5\n", expectKeyed: true, seconds: 0...1)
        pressFailure("按下沒有回覆、補送的放開成功", t1: nil, t0: "RPRT 0\n", expectKeyed: false, seconds: 2.8...4)
        pressFailure("按下與補送的放開都沒有回覆", t1: nil, t0: nil, expectKeyed: true, seconds: 3.8...5.5)
        // 連線一直被掛斷：指令根本沒有送到，不必警告
        if let dead = FakeRigctld(dropFirst: 1000, reply: "RPRT 0\n") {
            do {
                try HamlibPTT.press(port: dead.port)
                problems.append("連線一直被掛斷卻按下成功")
            } catch {
                if (error as? PTTError)?.mayBeKeyed != false { problems.append("連線一直被掛斷（指令沒送到）卻標著「PTT 可能還按著」") }
            }
            dead.stop()
        }
        // 放開失敗時最後再補送一次：第二次成功就算放開了；兩次都失敗要標著「PTT 可能還按著」
        var releaseReplies = ["RPRT -5\n", "RPRT 0\n"]
        let replyLock = NSLock()
        if let fake = FakeRigctld(dropFirst: 0, reply: nil, respond: { _ in
            replyLock.lock()
            defer { replyLock.unlock() }
            return releaseReplies.isEmpty ? "RPRT 0\n" : releaseReplies.removeFirst()
        }) {
            do {
                try HamlibPTT.release(port: fake.port)
                if fake.commands != ["T 0", "T 0"] { problems.append("放開第一次失敗時：送出的指令是 \(fake.commands)，應該再補送一次 T 0") }
            } catch {
                problems.append("放開第二次成功了卻回報失敗：\(error.localizedDescription)")
            }
            fake.stop()
        }
        if let fake = FakeRigctld(dropFirst: 0, reply: nil, respond: { _ in "RPRT -5\n" }) {
            do {
                try HamlibPTT.release(port: fake.port)
                problems.append("放開兩次都失敗卻當成成功")
            } catch {
                if (error as? PTTError)?.mayBeKeyed != true || fake.commands != ["T 0", "T 0"] {
                    problems.append("放開兩次都失敗時：指令 \(fake.commands)、「PTT 可能還按著」的標記 \((error as? PTTError)?.mayBeKeyed == true)")
                }
            }
            fake.stop()
        }
        // 還沒選型號：不用啟動任何東西就知道
        do {
            try HamlibPTT(model: 0, port: "", baud: 0).key(true)
            problems.append("沒有選電台型號也按下了 PTT")
        } catch {
            if !error.localizedDescription.contains("型號") { problems.append("沒有選型號時的訊息是「\(error.localizedDescription)」") }
        }
        report("CAT 指令的 PTT：型號清單、序列埠規格與參數", problems.isEmpty,
               problems.isEmpty ? "清單 \(parsed.count) 個型號（含名稱有空白、比欄位長的）；速率與控制線的參數正確；"
                   + "連線被掛斷會再試（頭兩次被掛斷、第三次送到），對方不回覆則等到逾時就回報；"
                   + "按下失敗會補送放開、放開失敗會再補送一次，還是不行的標著「PTT 可能還按著」" : problems.joined(separator: "；"))

        // 2. 需要這台電腦上的 Hamlib
        let name = "CAT 指令的 PTT：經 Hamlib 按下與放開（不接電台的示範機）"
        guard let rigctld = Hamlib.rigctld, Hamlib.rigctl != nil else {
            report(name, nil, "這台電腦沒有裝 Hamlib（找不到 rigctld）")
            return
        }
        problems = []
        let models = Hamlib.models()
        let groups = Hamlib.modelsByManufacturer()
        if models.count < 100 || !models.contains(Hamlib.Model(id: 1, manufacturer: "Hamlib", name: "Dummy"))
            || !groups.contains(where: { $0.manufacturer == "Icom" && $0.models.count > 20 }) || groups.map(\.models.count).reduce(0, +) != models.count {
            problems.append("Hamlib 的型號清單只有 \(models.count) 個，或裡面沒有該有的型號")
        }
        if Hamlib.serialCaps(model: 1) != nil { problems.append("示範機不走序列埠，卻有序列埠規格") }
        // 設定裡的型號選單：真的做成系統的選單（不跳出來），每個廠牌一個子選單、每個型號一列；
        // 目前選的型號與它的廠牌打勾；選一列（照系統選單送動作的方式）就回報那個型號
        var picked: Hamlib.Model?
        let menu = makePopupMenu(SettingsView.rigModelItems(selected: 3073) { picked = $0 })
        let leaves = menu.items.flatMap { $0.submenu?.items ?? [] }
        let checkedGroups = menu.items.filter { $0.state == .on }.map(\.title), checkedModels = leaves.filter { $0.state == .on }.map(\.title)
        if menu.items.count != groups.count || leaves.count != models.count || menu.items.contains(where: { $0.submenu == nil }) {
            problems.append("型號選單有 \(menu.items.count) 個廠牌、\(leaves.count) 個型號，應該是 \(groups.count) 個、\(models.count) 個")
        }
        if checkedGroups != ["Icom"] || checkedModels != ["IC-7300"] { problems.append("型號選單打勾的是 \(checkedGroups)、\(checkedModels)") }
        if let item = menu.items.first(where: { $0.title == "Kenwood" })?.submenu?.items.first(where: { $0.title == "TS-2000" }), let action = item.action {
            _ = NSApp.sendAction(action, to: item.target, from: item)
            if picked != Hamlib.Model(id: 2014, manufacturer: "Kenwood", name: "TS-2000") { problems.append("在選單裡選 Kenwood TS-2000，回報的是 \(picked?.title ?? "無")") }
        } else {
            problems.append("型號選單裡找不到 Kenwood TS-2000")
        }
        if makePopupMenu([], emptyTitle: "（空的）").items.map(\.title) != ["（空的）"] { problems.append("沒有項目的選單不對") }
        func query(_ port: Int) -> String {
            ((try? RigctldPTT(host: "127.0.0.1", port: String(port)).send(command: "t\n")) ?? "?").trimmingCharacters(in: .whitespacesAndNewlines)
        }
        // 按下：rigctld 起來了，PTT 是按下的；放開：PTT 放開，rigctld 也結束了
        var startup = 0.0
        let ptt = HamlibPTT(model: 1, port: "", baud: 0)
        do {
            let began = Date()
            try ptt.key(true)
            startup = Date().timeIntervalSince(began)
            guard let port = ptt.tcpPort else { throw PTTError(message: "按下之後不知道 rigctld 聽哪個連接埠") }
            if query(port) != "1" { problems.append("按下之後查詢 PTT 得到 \(query(port))") }
            try ptt.key(false)
            if ptt.tcpPort != nil || Demo.wait(timeout: 2, until: { !HamlibPTT.canConnect(port) }) == false { problems.append("放開之後 rigctld 還在") }
            try ptt.key(false)   // 再放開一次不能出錯
        } catch {
            problems.append("按下或放開失敗：\(error.localizedDescription)")
        }
        // 連續按下、放開幾次：rigctld 剛啟動的頭一瞬間偶爾（約一成）會把第一個指令的連線掛斷，要重試才按得下去，不能因此發射失敗。
        // 開發用：NBTV_SELFTEST_PTT_CYCLES=<次數> 多跑幾次（預設 4 次，每次約半秒）
        let cycles = ProcessInfo.processInfo.environment["NBTV_SELFTEST_PTT_CYCLES"].flatMap { Int($0) } ?? 4
        let cycleRetriesBefore = HamlibPTT.retries
        var cycleFailures = 0
        for _ in 0..<cycles {
            let driver = HamlibPTT(model: 1, port: "", baud: 0)
            do {
                try driver.key(true)
                if let port = driver.tcpPort, query(port) != "1" { cycleFailures += 1 }
                try driver.key(false)
            } catch {
                cycleFailures += 1
                try? driver.key(false)
            }
        }
        if cycleFailures > 0 { problems.append("連續按下、放開 \(cycles) 次，有 \(cycleFailures) 次失敗") }
        let retried = HamlibPTT.retries - cycleRetriesBefore
        // 序列埠打不開（不存在的裝置）：很快就回報原因，rigctld 不會留著。用的是需要序列埠的型號（Yaesu FT-857）
        var failureText = ""
        let broken = HamlibPTT(model: 1022, port: "/dev/cu.nbtv-selftest-nonexistent", baud: 9600)
        let began = Date()
        do {
            try broken.key(true)
            problems.append("序列埠不存在也按下了 PTT")
            try? broken.key(false)
        } catch {
            failureText = error.localizedDescription
            let spent = Date().timeIntervalSince(began)
            if !failureText.contains("開不了電台") || !failureText.contains("Unable to open") || spent > 8 || broken.tcpPort != nil {
                problems.append(String(format: "序列埠不存在時過了 %.1f 秒才回報「%@」", spent, failureText))
            }
        }
        do {
            try HamlibPTT(model: 1022, port: "", baud: 0).key(true)
            problems.append("沒有選序列埠也按下了 PTT")
        } catch {
            if !error.localizedDescription.contains("序列埠") { problems.append("沒有選序列埠時的訊息是「\(error.localizedDescription)」") }
        }

        // 看門的程序（一）：按下之後 app 突然不在了（這裡只放掉它手上的管線）——rigctld 要被結束
        let abandoned = HamlibPTT(model: 1, port: "", baud: 0)
        do {
            try abandoned.key(true)
            guard let port = abandoned.tcpPort else { throw PTTError(message: "不知道連接埠") }
            abandoned.debugAbandon()
            if !Demo.wait(timeout: 6, until: { !HamlibPTT.canConnect(port) }) { problems.append("app 不在了之後 rigctld 沒有被結束") }
        } catch {
            problems.append("看門程序的測試沒有跑起來：\(error.localizedDescription)")
        }
        // 看門的程序（二）：結束之前有沒有先放開 PTT。另外開一個不會被它結束的 rigctld 來看：
        // 讓看門的去結束一個不相干的 sleep，rigctld 留著，之後查得到 PTT 已經是放開的
        if let port = HamlibPTT.freePort() {
            let daemon = Process(), sleeper = Process()
            daemon.executableURL = URL(fileURLWithPath: rigctld)
            daemon.arguments = ["-m", "1", "-T", "127.0.0.1", "-t", String(port)]
            daemon.standardOutput = FileHandle.nullDevice
            daemon.standardError = FileHandle.nullDevice
            sleeper.executableURL = URL(fileURLWithPath: "/bin/sleep")
            sleeper.arguments = ["30"]
            do {
                try daemon.run()
                try sleeper.run()
                defer {
                    // 等它真的結束再往下走：測試跑完之後不留任何還在收尾的 rigctld
                    daemon.terminate()
                    daemon.waitUntilExit()
                    if sleeper.isRunning { sleeper.terminate() }
                }
                // rigctld 剛起來的頭一瞬間偶爾會把連線掛斷（app 自己的 HamlibPTT 也因此會重試）：按到成功為止
                guard Demo.wait(timeout: 5, until: { HamlibPTT.canConnect(port) }),
                      Demo.wait(timeout: 3, until: { (try? RigctldPTT(host: "127.0.0.1", port: String(port)).key(true)) != nil }) else {
                    throw PTTError(message: "自己開的 rigctld 沒有起來")
                }
                guard let dog = HamlibPTT.makeWatchdog(tcpPort: port, pid: sleeper.processIdentifier) else { throw PTTError(message: "看門的程序啟動不了") }
                Demo.spin(0.3)
                if query(port) != "1" || !sleeper.isRunning { problems.append("管線還在的時候看門的程序就動手了") }
                try? dog.pipe.close()
                if !Demo.wait(timeout: 6, until: { !sleeper.isRunning }) { problems.append("管線斷了之後看門的程序沒有結束指定的行程") }
                if query(port) != "0" { problems.append("管線斷了之後看門的程序沒有放開 PTT（查詢得到 \(query(port))）") }
            } catch {
                problems.append("看門程序的測試沒有跑起來：\(error.localizedDescription)")
            }
        }
        report(name, problems.isEmpty, problems.isEmpty
               ? String(format: "Hamlib 有 %d 個型號、%d 個廠牌，設定裡的選單每個廠牌一個子選單；按下花了 %.2f 秒，查詢得到 1，放開之後 rigctld 結束；"
                            + "連續按下、放開 %d 次都成功（其中 %d 次第一個指令被剛啟動的 rigctld 掛斷、重試後成功）；序列埠不存在時回報「%@」；"
                            + "app 不在了，看門的程序會放開 PTT 並結束 rigctld", models.count, groups.count, startup, cycles, retried, failureText)
               : problems.joined(separator: "；"))
    }

    // MARK: 3. rigctld

    private static func testRigctld(_ address: String) {
        let parts = address.split(separator: ":").map(String.init)
        guard parts.count == 2 else {
            report("rigctld PTT", false, "位址格式是 主機:連接埠")
            return
        }
        let ptt = RigctldPTT(host: parts[0], port: parts[1])
        do {
            try ptt.key(true)
            let on = try ptt.send(command: "t\n").trimmingCharacters(in: .whitespacesAndNewlines)
            try ptt.key(false)
            let off = try ptt.send(command: "t\n").trimmingCharacters(in: .whitespacesAndNewlines)
            report("rigctld PTT", on == "1" && off == "0", "按下後查詢得到 \(on)，放開後得到 \(off)")
        } catch {
            report("rigctld PTT", false, error.localizedDescription)
        }
        // 連不上的位址要回報錯誤而不是卡住
        do {
            try RigctldPTT(host: "127.0.0.1", port: "1").key(true)
            report("rigctld 連線失敗的處理", false, "連不上卻沒有報錯")
        } catch {
            report("rigctld 連線失敗的處理", true, error.localizedDescription)
        }
    }

    // MARK: 4、5. 音訊裝置

    private static func testAudio(_ image: Data) {
        guard let device = AudioDevices.all().first(where: { $0.name.contains("BlackHole") && $0.inputChannels > 0 && $0.outputChannels > 0 }) else {
            report("音訊輸出", nil, "找不到 BlackHole 虛擬音效卡")
            report("BlackHole 自發自收", nil, "找不到 BlackHole 虛擬音效卡")
            return
        }
        guard let mode = Modes.find(12) else { return }
        let tx = CoreTransmitter()
        tx.setMode(mode.id)
        tx.setCallsign("N0CALL")
        guard let first = tx.encode(rgb: image), let second = tx.encode(rgb: image) else {
            report("音訊輸出", false, "編碼失敗")
            return
        }
        let tail = tx.finish()

        // 4. 只測輸出：排進去的取樣要在差不多一幀的時間內被裝置取完
        let output = AudioOutput()
        do {
            try output.start(deviceID: device.id)
            let begin = Date()
            output.enqueue(first.samples)
            let done = Demo.wait(timeout: mode.frameSeconds + 3) { output.playedSamples >= Int64(first.samples.count) }
            let elapsed = Date().timeIntervalSince(begin)
            output.stop()
            report("音訊輸出（\(device.name)）", done && abs(elapsed - mode.frameSeconds) < 0.5,
                   String(format: "%.2f 秒的訊號花了 %.2f 秒送完", mode.frameSeconds, elapsed))
        } catch {
            report("音訊輸出（\(device.name)）", false, error.localizedDescription)
            return
        }

        // 5. 自發自收：要有麥克風權限才能從輸入裝置收音
        var status = AVCaptureDevice.authorizationStatus(for: .audio)
        if status == .notDetermined {
            print("[selftest] 需要麥克風權限，等使用者回應（最多 20 秒）…")
            var answered = false
            AVCaptureDevice.requestAccess(for: .audio) { _ in answered = true }
            _ = Demo.wait(timeout: 20) { answered }
            status = AVCaptureDevice.authorizationStatus(for: .audio)
        }
        guard status == .authorized else {
            report("BlackHole 自發自收", nil, "沒有麥克風權限（狀態 \(status.rawValue)），無法從輸入裝置收音")
            return
        }
        let receiver = CoreReceiver()
        let queue = DispatchQueue(label: "nbtv.selftest.rx")
        var frames: [DecodedFrame] = []
        var peak: Float = 0
        var received = 0
        let input = AudioInput()
        input.onSamples = { samples in
            queue.async {
                receiver.push(samples)
                received += samples.count
                peak = max(peak, samples.map { abs($0) }.max() ?? 0)
                while let frame = receiver.pollFrame() { frames.append(frame) }
            }
        }
        do {
            try input.start(deviceID: device.id)
            try output.start(deviceID: device.id)
        } catch {
            report("BlackHole 自發自收", false, error.localizedDescription)
            return
        }
        output.enqueue(Demo.silence(seconds: 0.3))
        output.enqueue(first.samples)
        output.enqueue(second.samples)
        output.enqueue(tail + Demo.silence(seconds: 0.6))
        let got = Demo.wait(timeout: 2 * mode.frameSeconds + 5) { queue.sync { frames.count >= 2 } }
        output.stop()
        input.stop()
        let (decoded, level, count) = queue.sync { (frames, peak, received) }
        let good = decoded.filter { $0.decoded && $0.callsign == "N0CALL" && $0.modeID == mode.id }
        // 收到的畫面和發射端的預覽只差削波失真與音效卡的轉換
        let close = good.count == 2 && difference(good[0].rgb ?? Data(), first.preview) < 12 && difference(good[1].rgb ?? Data(), second.preview) < 12
        report("BlackHole 自發自收（\(mode.label)）", got && good.count == 2 && close,
               String(format: "收到 %d/2 張，收到 %.1f 秒的音訊，峰值 %.2f，輸出佇列中途空掉 %d 次", good.count,
                      Double(count) / Double(NBTV.sampleRate), level, max(0, output.underruns - 1)))
    }
}
