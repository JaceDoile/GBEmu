import AVFoundation

/// Pulls samples from the core's ring buffer on the audio thread.
final class AudioOutput {
    static let sampleRate: Double = 48_000

    private let engine = AVAudioEngine()
    private var source: AVAudioSourceNode!
    private let scratch: UnsafeMutablePointer<Int16>
    private let gb: OpaquePointer
    private var running = false
    private var observers: [NSObjectProtocol] = []

    init(gb: OpaquePointer) {
        self.gb = gb
        scratch = UnsafeMutablePointer<Int16>.allocate(capacity: 8192 * 2)
        scratch.initialize(repeating: 0, count: 8192 * 2)

        let format = AVAudioFormat(standardFormatWithSampleRate: Self.sampleRate, channels: 2)!
        let buf = scratch
        source = AVAudioSourceNode(format: format) { _, _, frameCount, abl -> OSStatus in
            let list = UnsafeMutableAudioBufferListPointer(abl)
            let n = min(Int(frameCount), 8192)
            guard list.count >= 2,
                  let l = list[0].mData?.assumingMemoryBound(to: Float.self),
                  let r = list[1].mData?.assumingMemoryBound(to: Float.self) else { return noErr }
            let got = gb_audio_read(gb, buf, n)
            for i in 0..<got {
                l[i] = Float(buf[i * 2]) / 32768
                r[i] = Float(buf[i * 2 + 1]) / 32768
            }
            if got < Int(frameCount) {
                for i in got..<Int(frameCount) { l[i] = 0; r[i] = 0 }
            }
            return noErr
        }
        engine.attach(source)
        engine.connect(source, to: engine.mainMixerNode, format: format)

        let nc = NotificationCenter.default
        observers.append(nc.addObserver(forName: AVAudioSession.interruptionNotification, object: nil, queue: .main) { [weak self] note in
            guard let self, let raw = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt,
                  AVAudioSession.InterruptionType(rawValue: raw) == .ended, self.running else { return }
            self.startEngine()
        })
        observers.append(nc.addObserver(forName: .AVAudioEngineConfigurationChange, object: engine, queue: .main) { [weak self] _ in
            guard let self, self.running else { return }
            self.startEngine()
        })
    }

    deinit {
        observers.forEach { NotificationCenter.default.removeObserver($0) }
        engine.stop()
        scratch.deallocate()
    }

    var volume: Float {
        get { engine.mainMixerNode.outputVolume }
        set { engine.mainMixerNode.outputVolume = newValue }
    }

    func configureSession(playInSilentMode: Bool) {
        let s = AVAudioSession.sharedInstance()
        try? s.setCategory(playInSilentMode ? .playback : .ambient, mode: .default, options: [.mixWithOthers])
        try? s.setPreferredSampleRate(Self.sampleRate)
        try? s.setPreferredIOBufferDuration(0.01)
        try? s.setActive(true)
    }

    func start() {
        running = true
        startEngine()
    }

    private func startEngine() {
        if !engine.isRunning {
            engine.prepare()
            try? engine.start()
        }
    }

    func stop() {
        running = false
        engine.pause()
    }
}
