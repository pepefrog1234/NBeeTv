// 音訊裝置的輸入與輸出。直接用 AUHAL（HAL 輸出單元），才能各自指定裝置。
// 與核心之間一律是 48 kHz、單聲道、float；裝置的取樣率不同時在這裡轉換。
import AudioToolbox
import AVFoundation
import Foundation
import os

struct AudioIOError: LocalizedError {
    let message: String
    var errorDescription: String? { message }
}

private func check(_ status: OSStatus, _ what: String) throws {
    if status != noErr { throw AudioIOError(message: "\(what)失敗（錯誤碼 \(status)）") }
}

/// 即時音訊執行緒與一般執行緒之間的環狀緩衝。臨界區很短，用 os_unfair_lock 保護。
final class FloatRing {
    private let storage: UnsafeMutablePointer<Float>
    private let capacity: Int
    private var readIndex = 0
    private var stored = 0
    private var totalRead: Int64 = 0
    private let lock: UnsafeMutablePointer<os_unfair_lock>

    init(capacity: Int) {
        self.capacity = capacity
        storage = .allocate(capacity: capacity)
        storage.initialize(repeating: 0, count: capacity)
        lock = .allocate(capacity: 1)
        lock.initialize(to: os_unfair_lock())
    }

    deinit {
        storage.deallocate()
        lock.deallocate()
    }

    /// 寫入最多 count 個取樣，回傳實際寫入的個數（滿了就丟掉後面的）。
    @discardableResult
    func write(_ source: UnsafePointer<Float>, count: Int) -> Int {
        os_unfair_lock_lock(lock)
        defer { os_unfair_lock_unlock(lock) }
        let n = min(count, capacity - stored)
        var writeIndex = (readIndex + stored) % capacity
        var done = 0
        while done < n {
            let chunk = min(n - done, capacity - writeIndex)
            (storage + writeIndex).update(from: source + done, count: chunk)
            writeIndex = (writeIndex + chunk) % capacity
            done += chunk
        }
        stored += n
        return n
    }

    /// 讀出最多 count 個取樣，回傳實際讀到的個數。
    func read(into dest: UnsafeMutablePointer<Float>, count: Int) -> Int {
        os_unfair_lock_lock(lock)
        defer { os_unfair_lock_unlock(lock) }
        let n = min(count, stored)
        var done = 0
        while done < n {
            let chunk = min(n - done, capacity - readIndex)
            (dest + done).update(from: storage + readIndex, count: chunk)
            readIndex = (readIndex + chunk) % capacity
            done += chunk
        }
        stored -= n
        totalRead += Int64(n)
        return n
    }

    var available: Int {
        os_unfair_lock_lock(lock)
        defer { os_unfair_lock_unlock(lock) }
        return stored
    }

    /// 開始以來讀走的取樣總數。
    var consumed: Int64 {
        os_unfair_lock_lock(lock)
        defer { os_unfair_lock_unlock(lock) }
        return totalRead
    }

    /// 把位置在 position（從開始算起第幾個取樣）之後、還沒被讀走的部分丟掉。
    /// position 之前的已經被讀走了（來不及）或 position 超過目前寫到的地方時不動，回傳 false。
    func truncate(at position: Int64) -> Bool {
        os_unfair_lock_lock(lock)
        defer { os_unfair_lock_unlock(lock) }
        let keep = position - totalRead
        guard keep >= 0, keep <= Int64(stored) else { return false }
        stored = Int(keep)
        return true
    }

    func clear() {
        os_unfair_lock_lock(lock)
        readIndex = 0
        stored = 0
        totalRead = 0
        os_unfair_lock_unlock(lock)
    }
}

private func makeHALUnit() throws -> AudioUnit {
    var desc = AudioComponentDescription(componentType: kAudioUnitType_Output, componentSubType: kAudioUnitSubType_HALOutput,
                                         componentManufacturer: kAudioUnitManufacturer_Apple, componentFlags: 0, componentFlagsMask: 0)
    guard let component = AudioComponentFindNext(nil, &desc) else { throw AudioIOError(message: "找不到音訊輸出單元") }
    var unit: AudioUnit?
    try check(AudioComponentInstanceNew(component, &unit), "建立音訊單元")
    guard let unit else { throw AudioIOError(message: "建立音訊單元失敗") }
    return unit
}

private func floatFormat(rate: Double, channels: UInt32) -> AudioStreamBasicDescription {
    AudioStreamBasicDescription(mSampleRate: rate, mFormatID: kAudioFormatLinearPCM,
                                mFormatFlags: kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved,
                                mBytesPerPacket: 4, mFramesPerPacket: 1, mBytesPerFrame: 4, mChannelsPerFrame: channels,
                                mBitsPerChannel: 32, mReserved: 0)
}

private func setProperty<T>(_ unit: AudioUnit, _ id: AudioUnitPropertyID, _ scope: AudioUnitScope, _ element: AudioUnitElement,
                            _ value: T, _ what: String) throws {
    try withUnsafePointer(to: value) { pointer in
        try check(AudioUnitSetProperty(unit, id, scope, element, pointer, UInt32(MemoryLayout<T>.size)), what)
    }
}

// MARK: - 取樣率轉換

/// 串流式的取樣率轉換（單聲道 float）。可以一段一段餵，內部記住濾波器的狀態，段與段之間不會有接縫。
final class StreamResampler {
    private let converter: AVAudioConverter
    private let ratio: Double

    init?(from inputRate: Double, to outputRate: Double) {
        guard let from = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: inputRate, channels: 1, interleaved: false),
              let to = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: outputRate, channels: 1, interleaved: false),
              let converter = AVAudioConverter(from: from, to: to) else { return nil }
        self.converter = converter
        ratio = outputRate / inputRate
    }

    func process(_ samples: [Float]) -> [Float] {
        guard !samples.isEmpty,
              let input = AVAudioPCMBuffer(pcmFormat: converter.inputFormat, frameCapacity: AVAudioFrameCount(samples.count)),
              let output = AVAudioPCMBuffer(pcmFormat: converter.outputFormat,
                                            frameCapacity: AVAudioFrameCount(Double(samples.count) * ratio) + 64) else { return [] }
        input.frameLength = AVAudioFrameCount(samples.count)
        samples.withUnsafeBufferPointer { input.floatChannelData![0].update(from: $0.baseAddress!, count: samples.count) }
        // 這一段只給一次；之後回報「暫時沒有資料」，轉換器會把狀態留到下一段
        var supplied = false
        var error: NSError?
        converter.convert(to: output, error: &error) { _, status in
            if supplied {
                status.pointee = .noDataNow
                return nil
            }
            supplied = true
            status.pointee = .haveData
            return input
        }
        let produced = Int(output.frameLength)
        guard error == nil, produced > 0 else { return [] }
        return Array(UnsafeBufferPointer(start: output.floatChannelData![0], count: produced))
    }
}

// MARK: - 輸入

private let inputProc: AURenderCallback = { refCon, flags, timeStamp, _, frameCount, _ in
    Unmanaged<AudioInput>.fromOpaque(refCon).takeUnretainedValue().capture(flags, timeStamp, frameCount)
}

/// 從指定的裝置收音，轉成 48 kHz 單聲道後交給 onSamples（在內部的佇列上呼叫，不是即時執行緒）。
final class AudioInput {
    var onSamples: (([Float]) -> Void)?

    private var unit: AudioUnit?
    private let ring = FloatRing(capacity: 96000 * 4)
    private let scratchCapacity = 16384
    private let scratch: UnsafeMutablePointer<Float>
    private var deviceRate = 48000.0
    private var resampler: StreamResampler?
    private var timer: DispatchSourceTimer?
    private let queue = DispatchQueue(label: "nbtv.audio.input", qos: .userInitiated)
    private(set) var isRunning = false

    init() {
        scratch = .allocate(capacity: scratchCapacity)
        scratch.initialize(repeating: 0, count: scratchCapacity)
    }

    deinit {
        stop()
        scratch.deallocate()
    }

    func start(deviceID: AudioDeviceID) throws {
        stop()
        let unit = try makeHALUnit()
        do {
            // AUHAL 預設只開輸出；改成只開輸入（第 1 匯流排）
            try setProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, UInt32(1), "啟用輸入")
            try setProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, UInt32(0), "停用輸出")
            try setProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, deviceID, "指定輸入裝置")

            var deviceFormat = AudioStreamBasicDescription()
            var size = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
            try check(AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1, &deviceFormat, &size), "讀取輸入格式")
            guard deviceFormat.mSampleRate > 0, deviceFormat.mChannelsPerFrame > 0 else {
                throw AudioIOError(message: "這個裝置沒有輸入聲道")
            }
            deviceRate = deviceFormat.mSampleRate
            // 向 AUHAL 要單聲道 float（取第一個聲道），取樣率維持裝置的值；AUHAL 的輸入端不做取樣率轉換
            try setProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, floatFormat(rate: deviceRate, channels: 1), "設定輸入格式")
            let callback = AURenderCallbackStruct(inputProc: inputProc, inputProcRefCon: Unmanaged.passUnretained(self).toOpaque())
            try setProperty(unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, callback, "設定輸入回呼")
            try check(AudioUnitInitialize(unit), "初始化輸入裝置")

            // 裝置不是 48 kHz 時要轉換
            resampler = abs(deviceRate - Double(NBTV.sampleRate)) > 0.5 ? StreamResampler(from: deviceRate, to: Double(NBTV.sampleRate)) : nil
            ring.clear()
            self.unit = unit
            try check(AudioOutputUnitStart(unit), "啟動輸入裝置")
        } catch {
            self.unit = nil
            AudioUnitUninitialize(unit)
            AudioComponentInstanceDispose(unit)
            throw error
        }
        isRunning = true

        // 每 20 ms 把環狀緩衝裡的取樣取出來、轉成 48 kHz 後往上送
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + .milliseconds(20), repeating: .milliseconds(20), leeway: .milliseconds(5))
        timer.setEventHandler { [weak self] in self?.drain() }
        timer.resume()
        self.timer = timer
    }

    func stop() {
        timer?.cancel()
        timer = nil
        if let unit {
            AudioOutputUnitStop(unit)
            AudioUnitUninitialize(unit)
            AudioComponentInstanceDispose(unit)
        }
        unit = nil
        isRunning = false
        // 等佇列上正在跑的那一次 drain 結束，之後不會再有回呼
        queue.sync {}
    }

    /// 即時執行緒：向 AUHAL 取這一批輸入，寫進環狀緩衝。
    fileprivate func capture(_ flags: UnsafeMutablePointer<AudioUnitRenderActionFlags>, _ timeStamp: UnsafePointer<AudioTimeStamp>,
                             _ frameCount: UInt32) -> OSStatus {
        guard let unit, Int(frameCount) <= scratchCapacity else { return noErr }
        var list = AudioBufferList(mNumberBuffers: 1,
                                   mBuffers: AudioBuffer(mNumberChannels: 1, mDataByteSize: frameCount * 4, mData: UnsafeMutableRawPointer(scratch)))
        let status = AudioUnitRender(unit, flags, timeStamp, 1, frameCount, &list)
        if status == noErr { ring.write(scratch, count: Int(frameCount)) }
        return status
    }

    private func drain() {
        let count = ring.available
        guard count > 0 else { return }
        var block = [Float](repeating: 0, count: count)
        let got = block.withUnsafeMutableBufferPointer { ring.read(into: $0.baseAddress!, count: count) }
        guard got > 0 else { return }
        if got < count { block.removeLast(count - got) }
        if let resampler {
            let converted = resampler.process(block)
            if !converted.isEmpty { onSamples?(converted) }
        } else {
            onSamples?(block)
        }
    }
}

// MARK: - 輸出

private let outputProc: AURenderCallback = { refCon, _, _, _, frameCount, ioData in
    Unmanaged<AudioOutput>.fromOpaque(refCon).takeUnretainedValue().render(frameCount, ioData)
}

/// 把 48 kHz 單聲道的取樣送到指定的裝置。取樣先排進佇列，裝置依自己的節奏取走，
/// 所以幀與幀只要在播完之前排進去就不會有空隙。
final class AudioOutput {
    private var unit: AudioUnit?
    private let ring: FloatRing
    /// 輸出的線性增益；即時執行緒只讀取。
    var gain: Float = 1.0
    private(set) var isRunning = false
    private(set) var underruns = 0
    /// 裝置總共來取了幾格、其中有幾格是因為佇列空了而補的靜音（除錯用；即時執行緒寫、其他執行緒只讀）
    private(set) var renderedFrames: Int64 = 0
    private(set) var silentFrames: Int64 = 0

    /// 測試用：不開任何裝置，由計時器照實際的時間把佇列裡的取樣取走，就像有一個裝置在播。
    /// 自我測試用它走完整的發射流程（不需要音訊裝置，也不會真的發出聲音）。要在 start 之前設定。
    var simulated = false
    /// 模擬的裝置取走的取樣（佇列空了的部分是 0），在模擬用的佇列上呼叫。
    var simulatedSink: (([Float]) -> Void)?
    private var simulationTimer: DispatchSourceTimer?
    private let simulationQueue = DispatchQueue(label: "nbtv.audio.simulated", qos: .userInitiated)

    /// queueSeconds：佇列最多放幾秒的音訊。要放得下「目前這張還沒播完的部分＋下一張」。
    init(queueSeconds: Double = 14) {
        ring = FloatRing(capacity: Int(queueSeconds * Double(NBTV.sampleRate)))
    }

    deinit { stop() }

    func start(deviceID: AudioDeviceID) throws {
        stop()
        if simulated {
            startSimulation()
            return
        }
        let unit = try makeHALUnit()
        do {
            try setProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, deviceID, "指定輸出裝置")
            var deviceFormat = AudioStreamBasicDescription()
            var size = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
            try check(AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &deviceFormat, &size), "讀取輸出格式")
            guard deviceFormat.mChannelsPerFrame > 0 else { throw AudioIOError(message: "這個裝置沒有輸出聲道") }
            // 給 AUHAL 48 kHz 的 float；它會自己轉成裝置的取樣率。立體聲裝置兩個聲道送一樣的訊號。
            let channels = min(deviceFormat.mChannelsPerFrame, 2)
            try setProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                            floatFormat(rate: Double(NBTV.sampleRate), channels: channels), "設定輸出格式")
            let callback = AURenderCallbackStruct(inputProc: outputProc, inputProcRefCon: Unmanaged.passUnretained(self).toOpaque())
            try setProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, callback, "設定輸出回呼")
            try check(AudioUnitInitialize(unit), "初始化輸出裝置")
            ring.clear()
            underruns = 0
            renderedFrames = 0
            silentFrames = 0
            self.unit = unit
            try check(AudioOutputUnitStart(unit), "啟動輸出裝置")
        } catch {
            self.unit = nil
            AudioUnitUninitialize(unit)
            AudioComponentInstanceDispose(unit)
            throw error
        }
        isRunning = true
    }

    func stop() {
        if let simulationTimer {
            simulationTimer.cancel()
            simulationQueue.sync {}   // 等正在跑的那一次做完
        }
        simulationTimer = nil
        if let unit {
            AudioOutputUnitStop(unit)
            AudioUnitUninitialize(unit)
            AudioComponentInstanceDispose(unit)
        }
        unit = nil
        isRunning = false
    }

    /// 模擬的裝置：每 10 毫秒取走這段時間該播的取樣。計數的方式和真的裝置（render）相同。
    private func startSimulation() {
        ring.clear()
        underruns = 0
        renderedFrames = 0
        silentFrames = 0
        let began = DispatchTime.now().uptimeNanoseconds
        var taken: Int64 = 0
        let timer = DispatchSource.makeTimerSource(queue: simulationQueue)
        timer.schedule(deadline: .now() + .milliseconds(10), repeating: .milliseconds(10), leeway: .milliseconds(1))
        timer.setEventHandler { [weak self] in
            guard let self else { return }
            let due = Int64(Double(DispatchTime.now().uptimeNanoseconds - began) / 1e9 * Double(NBTV.sampleRate))
            let frames = Int(due - taken)
            guard frames > 0 else { return }
            taken = due
            var block = [Float](repeating: 0, count: frames)
            let got = block.withUnsafeMutableBufferPointer { self.ring.read(into: $0.baseAddress!, count: frames) }
            self.renderedFrames += Int64(frames)
            self.silentFrames += Int64(frames - got)
            if got > 0, got < frames { self.underruns += 1 }
            self.simulatedSink?(block)
        }
        simulationTimer = timer
        timer.resume()
        isRunning = true
    }

    /// 把取樣排進佇列；放不下時回傳 false。
    @discardableResult
    func enqueue(_ samples: [Float]) -> Bool {
        let written = samples.withUnsafeBufferPointer { ring.write($0.baseAddress!, count: $0.count) }
        return written == samples.count
    }

    /// 把排在 position（從 start 起算第幾個取樣）之後的都丟掉。裝置已經取到 position 之後了就不動，回傳 false。
    func discardQueued(from position: Int64) -> Bool { ring.truncate(at: position) }

    /// 裝置已經取走的取樣數（從 start 起算）。
    var playedSamples: Int64 { ring.consumed }
    var queuedSamples: Int { ring.available }

    /// 即時執行緒：填滿這一批輸出。佇列空了就補靜音。
    fileprivate func render(_ frameCount: UInt32, _ ioData: UnsafeMutablePointer<AudioBufferList>?) -> OSStatus {
        guard let ioData else { return noErr }
        let buffers = UnsafeMutableAudioBufferListPointer(ioData)
        guard let first = buffers.first, let data = first.mData else { return noErr }
        let frames = Int(frameCount)
        let dest = data.assumingMemoryBound(to: Float.self)
        let got = ring.read(into: dest, count: frames)
        renderedFrames += Int64(frames)
        silentFrames += Int64(frames - got)
        if got < frames {
            (dest + got).update(repeating: 0, count: frames - got)
            // 佇列在播放途中空掉一次算一次（發射結束時最後那一批不滿也會算到一次）
            if got > 0 { underruns += 1 }
        }
        let g = gain
        if g != 1.0 {
            for i in 0..<got { dest[i] *= g }
        }
        for index in 1..<buffers.count {
            if let other = buffers[index].mData { other.copyMemory(from: data, byteCount: frames * 4) }
        }
        return noErr
    }
}
