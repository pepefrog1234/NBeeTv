// 開發用，都不開視窗：
//   --transmit-test／--receive-test：把測試圖送到某個輸出裝置／從某個輸入裝置接收（見各函式的說明）
//   --selftest [--rig 主機:連接埠] [--no-camera] [--no-audio] [--no-devices]：逐項檢查外殼自己的零件
//   （--no-camera 不開鏡頭，--no-audio 不經過音訊裝置，--no-devices 兩者都不要；
//     --ask-camera 還沒有相機權限時向系統要求，--save-camera-frame <檔名> 把鏡頭取到的那一格存成 PPM）：
//   1. Swift 與核心之間的收發（不經音訊裝置），包含類比模式送全黑、全白的畫面
//   2. 影片檔來源（做一段影片、開啟、跳到指定位置、取一格），以及用影片當來源的整個發射流程
//      （音訊輸出換成模擬的、PTT 換成記錄用的：按下發射自動播放，播完自動停止、放開 PTT）
//   3. rigctld 的 PTT（要給 --rig，對著真的 rigctld 測）
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
    /// --flat 改送整張同一個亮度的畫面：類比模式下這種畫面整幀的能量只在少數幾個格點上，是接收端最容易出問題的內容
    /// （很暗、過曝或被蓋住的鏡頭拍到的就是這種畫面）。
    static func transmitTest(arguments: [String]) -> Int32 {
        NSApplication.shared.setActivationPolicy(.prohibited)
        setvbuf(stdout, nil, _IOLBF, 0)
        let name = option("--device", in: arguments) ?? "BlackHole"
        guard let device = AudioDevices.outputs().first(where: { $0.name.contains(name) }) else {
            print("[transmit-test] 找不到輸出裝置「\(name)」")
            return 1
        }
        let modeID = option("--mode", in: arguments).flatMap(Int.init) ?? 5
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
                    print("[receive-test] \(Modes.find(frame.modeID)?.label ?? "?")：\(frame.decoded ? "解碼成功" : "解碼失敗")，呼號 \(frame.callsign)，"
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
        print("[receive-test] \(Int(seconds)) 秒內解出 \(count) 張")
        return 0
    }

    static func run(arguments: [String]) -> Int32 {
        NSApplication.shared.setActivationPolicy(.prohibited)
        setvbuf(stdout, nil, _IOLBF, 0)
        print("[selftest] 核心 \(NBTV.version)，影像編解碼器：\(NBTV.hasImageCodec ? "有" : "沒有")，模式 \(Modes.all.count) 個")
        guard let coffee = Demo.testImage("coffee"), let rocket = Demo.testImage("rocket") else {
            report("內附的測試圖", false)
            return 1
        }
        // --no-camera：不開鏡頭。--no-audio：不經過音訊裝置（BlackHole 上有別的程式在收發時，自發自收的結果沒有意義，
        // 送出去的測試訊號也會混進別人的聲音裡）。--no-devices：兩者都不要。
        let devices = !arguments.contains("--no-devices")
        let useCamera = devices && !arguments.contains("--no-camera")
        let useAudio = devices && !arguments.contains("--no-audio")
        // 學習式模式各有一份權重檔；少了哪一份，那個模式就不會列在 Modes 裡
        let models = Modes.learned.map {
            "\($0.name)：識別碼 \(LearnedModel.idText(values: $0.learnedValues, width: $0.learnedWidth) ?? "?")、"
                + "\($0.learnedValues) 個實數\($0.largeModel ? "、大型網路" : "")"
        }
        report("學習式模式的模型", models.count == LearnedModel.resourceNames.count ? true : nil,
               models.isEmpty ? "app 裡沒有權重檔，學習式模式不能用"
                   : models.joined(separator: "；") + (models.count < LearnedModel.resourceNames.count ? "；其餘的權重檔不在 app 裡" : ""))
        testCoreLoopback(coffee)
        testFlatPictures()
        testExport(coffee)
        testDroppedFrameTail(coffee, rocket)
        testResampler(coffee)
        testVideoFile(rocket)
        testReceivedHistory(coffee)
        testVideoTransmit(rocket)
        if useCamera {
            testCamera(ask: arguments.contains("--ask-camera"), savePath: option("--save-camera-frame", in: arguments))
        } else {
            report("鏡頭", nil, "指定了不開鏡頭")
        }
        testSerialErrors()
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
        for modeID in [5, 9, 11, 12, 13] {  // 模式 11、12、13 要 app 裡有權重檔才會列在 Modes 裡
            guard let mode = Modes.find(modeID), mode.isDigital ? NBTV.hasImageCodec : true else { continue }
            let tx = CoreTransmitter()
            tx.setMode(modeID)
            tx.setCallsign("bx4acp")
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
            let good = frames.filter { $0.decoded && $0.headerOK && $0.callsign == "BX4ACP" && $0.rgb != nil }
            // 數位：收到的畫面與發射端的預覽逐位元相同；類比與學習式：只差削波失真，檢查有畫面即可
            let same = !mode.isDigital || zip(good, previews).allSatisfy { $0.rgb == $1 }
            report("核心收發（\(mode.label)）", good.count == 2 && same,
                   "解出 \(good.count)/2 張\(mode.isDigital ? "，畫面\(same ? "與預覽相同" : "與預覽不同")" : "")")
        }
    }

    /// 類比模式送沒有細節的畫面（全黑、全白）：很暗、過曝或被蓋住的鏡頭拍到的就是這種畫面。
    /// 這種幀整幀的能量只在少數幾個格點上；訊號很乾淨時，接收機曾經完全認不出來（核心的導頻驗證）。
    private static func testFlatPictures() {
        var details: [String] = []
        var ok = true
        for modeID in [8, 9, 10] {
            guard let mode = Modes.find(modeID) else { continue }
            for level: UInt8 in [0, 255] {
                let picture = Data(repeating: level, count: NBTV.rgbBytes)
                let tx = CoreTransmitter()
                tx.setMode(modeID)
                tx.setCallsign("BX4ACP")
                var audio = Demo.silence(seconds: 0.3)
                for _ in 0..<2 {
                    guard let frame = tx.encode(rgb: picture) else {
                        report("類比模式送全黑、全白的畫面", false, "\(mode.name) 編碼失敗")
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
        report("類比模式送全黑、全白的畫面", ok, details.joined(separator: "、"))
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

    /// 「儲存圖片」寫出去的檔案內容：數位模式是標準的 .avif（系統的影像解碼器要讀得出同一張畫面），類比與學習式模式是 PNG。
    private static func testExport(_ image: Data) {
        var details: [String] = []
        var ok = true
        for modeID in [1, 9, 11, 12] {
            guard let mode = Modes.find(modeID), mode.isDigital ? NBTV.hasImageCodec : true else { continue }
            let tx = CoreTransmitter()
            tx.setMode(modeID)
            tx.setCallsign("BX4ACP/P")
            guard let encoded = tx.encode(rgb: image) else {
                report("存檔的內容", false, "編碼失敗")
                return
            }
            let audio = Demo.silence(seconds: 0.3) + encoded.samples + tx.finish() + Demo.silence(seconds: 0.5)
            guard let frame = Demo.receive(audio).flatMap(\.frames).first(where: { $0.decoded }), let rgb = frame.rgb,
                  let picture = NBTV.image(fromRGB: rgb) else {
                report("存檔的內容", false, "\(mode.name) 沒有解出畫面")
                return
            }
            let received = AppModel.ReceivedFrame(image: picture, payload: frame.payload, isDigital: frame.isDigital, modeID: frame.modeID,
                                                  callsign: frame.callsign, sequence: frame.sequence, time: Date(timeIntervalSince1970: 1_800_000_000),
                                                  snrDb: frame.snrDb)
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
            let wanted = mode.isDigital ? ".avif" : ".png"
            let nameOK = file.name.hasPrefix("NBTV-BX4ACP-P-") && file.name.hasSuffix(wanted)   // 呼號裡的斜線不能出現在檔名裡
            // AVIF 的色彩轉換各家解碼器的進位略有不同，容許平均差 3／255；PNG 要完全一樣
            let limit = mode.isDigital ? 3.0 : 0.0
            ok = ok && nameOK && size == "96×72" && difference >= 0 && difference <= limit
            details.append(String(format: "%@ → %@（%d 位元組，系統解出 %@，與收到的畫面平均差 %.2f／255）", mode.name, file.name, file.data.count,
                                  size.isEmpty ? "失敗" : size, difference))
        }
        report("存檔的內容", ok, details.joined(separator: "；"))
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
            tx.setCallsign("BX4ACP")
            return tx
        }
        var details: [String] = []
        var ok = true
        for modeID in [NBTV.hasImageCodec ? 5 : 8, 9] {
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
            let decoded = frames.count == 1 && frames[0].decoded && frames[0].callsign == "BX4ACP"
            ok = ok && alone.samples == sent.samples && discarded && !again && !trueTail.isEmpty && tail == trueTail
                && dropped.sequence == sent.sequence + 1 && next?.sequence == dropped.sequence && decoded
            details.append(String(format: "%@：尾巴 %d 個取樣，取消下一張之後%@，解出 %d 張",
                                  Modes.find(modeID)?.name ?? "", trueTail.count, tail == trueTail ? "與只送一張時完全相同" : "不同", frames.count))
        }
        report(name, ok, details.joined(separator: "；"))
    }

    // MARK: 取樣率轉換

    /// 模擬 44.1 kHz 的音訊裝置：48 kHz 的訊號先轉成 44.1 kHz，再像輸入裝置那樣每 20 ms 一段轉回 48 kHz 給接收機。
    private static func testResampler(_ image: Data) {
        guard let mode = Modes.find(NBTV.hasImageCodec ? 5 : 8), let down = StreamResampler(from: 48000, to: 44100),
              let up = StreamResampler(from: 44100, to: 48000) else {
            report("取樣率轉換", false, "無法建立轉換器")
            return
        }
        let tx = CoreTransmitter()
        tx.setMode(mode.id)
        tx.setCallsign("BX4ACP")
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
        let good = frames.filter { $0.decoded && $0.callsign == "BX4ACP" }
        report("取樣率轉換（48 k → 44.1 k → 48 k，分段）", good.count == 2,
               String(format: "解出 %d/2 張，%d 個取樣轉成 %d 個再轉回 %d 個", good.count, audio.count, device.count, produced))
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
            // 亮度的平均與標準差（0–255）。標準差只有個位數的畫面（很暗、過曝、對著白牆）幾乎沒有細節，
            // 類比模式下整幀的能量會集中在少數幾個格點上，是接收端最難處理的內容。
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
            // 拿鏡頭拍到的這一格實際收發一次（不經音訊裝置）：每個類比模式與一個數位模式各送兩張
            var results: [String] = []
            for modeID in [8, 9, 10, 5] {
                guard let mode = Modes.find(modeID), mode.isDigital ? NBTV.hasImageCodec : true else { continue }
                let tx = CoreTransmitter()
                tx.setMode(modeID)
                tx.setCallsign("BX4ACP")
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
        tx.setMode(8)
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
            report(name, false, "示範的訊號解出 \(frames.count)/2 張")
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
        // 點選的那一張被擠掉之後，主畫面回到最新的
        if let oldest = model.history.last {
            model.select(oldest)
            ok = ok && model.displayedFrame?.id == oldest.id
            for _ in 0..<3 { model.add(frames[0], time: Date()) }
            ok = ok && model.displayedFrame?.id == model.history.first?.id
        }
        report(name, ok, String(format: "兩張的訊雜比 %@、%@（通道是 18、6 dB）；最多留 %d 張", AppModel.snrText(frames[0].snrDb),
                                AppModel.snrText(frames[1].snrDb), AppModel.historyLimit))
    }

    // MARK: 影片來源的發射流程

    /// 記錄用的 PTT：把每一次按下、放開記下來，不碰任何硬體。
    private final class RecordingPTT: PTTDriver {
        private let lock = NSLock()
        private var states: [Bool] = []
        func key(_ on: Bool) throws {
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
        settings.txModeID = 8   // 類比 8：每張 1.04 秒，不需要影像編解碼器
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
        guard let mode = Modes.find(NBTV.hasImageCodec ? 5 : 8) else { return }
        let tx = CoreTransmitter()
        tx.setMode(mode.id)
        tx.setCallsign("BX4ACP")
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
        let good = decoded.filter { $0.decoded && $0.callsign == "BX4ACP" && $0.modeID == mode.id }
        let same = !mode.isDigital || (good.count == 2 && good[0].rgb == first.preview && good[1].rgb == second.preview)
        report("BlackHole 自發自收（\(mode.label)）", got && good.count == 2 && same,
               String(format: "解出 %d/2 張，收到 %.1f 秒的音訊，峰值 %.2f，輸出佇列中途空掉 %d 次", good.count,
                      Double(count) / Double(NBTV.sampleRate), level, max(0, output.underruns - 1)))
    }
}
