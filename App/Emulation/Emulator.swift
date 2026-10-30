import Foundation
import QuartzCore
import UIKit
import Combine

enum Btn {
    static let a: UInt8 = 1, b: UInt8 = 2, select: UInt8 = 4, start: UInt8 = 8
    static let right: UInt8 = 16, left: UInt8 = 32, up: UInt8 = 64, down: UInt8 = 128
}

enum EmulatorError: LocalizedError {
    case read(String), mapper(UInt8), invalid
    var errorDescription: String? {
        switch self {
        case .read(let m): return "Couldn't open the game: \(m)"
        case .mapper(let t): return String(format: "This cartridge uses hardware that isn't supported yet (type 0x%02X).", t)
        case .invalid: return "This file isn't a valid Game Boy ROM."
        }
    }
}

struct SaveSlotInfo: Identifiable {
    let slot: Int          // 0 = auto (resume)
    let date: Date?
    let thumbnail: UIImage?
    var id: Int { slot }
}

/// Owns the emulation core, drives it from the display refresh and handles saves.
final class Emulator {
    static let frameRate = 4_194_304.0 / 70_224.0   // 59.7275 Hz

    let game: Game
    let gb: OpaquePointer
    let audio: AudioOutput
    let settings = AppSettings.shared
    let gameKey: String

    var onFrame: ((CGImage) -> Void)?
    private(set) var lastImage: CGImage?

    private var displayLink: CADisplayLink?
    private var lastTimestamp: CFTimeInterval = 0
    private var accumulator: Double = 0
    private var framesSinceSaveCheck = 0
    private var paused = true
    private var cancellables: Set<AnyCancellable> = []
    private let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!

    private var undoState: Data?
    var canUndoLoad: Bool { undoState != nil }

    var fastForward = false {
        didSet {
            guard fastForward != oldValue else { return }
            gb_set_audio_enabled(gb, fastForward ? 0 : 1)
            accumulator = 0
        }
    }

    // Inputs from different sources are combined.
    var touchButtons: UInt8 = 0 { didSet { pushButtons() } }
    var padButtons: UInt8 = 0 { didSet { pushButtons() } }
    var keyButtons: UInt8 = 0 { didSet { pushButtons() } }
    private func pushButtons() { gb_set_buttons(gb, touchButtons | padButtons | keyButtons) }

    var title: String { String(cString: gb_title(gb)) }
    var isColor: Bool { gb_is_cgb(gb) != 0 }
    var hasBattery: Bool { gb_has_battery(gb) != 0 && gb_save_export_size(gb, 0) > 0 }
    var hasClock: Bool { gb_has_rtc(gb) != 0 }

    init(game: Game) throws {
        self.game = game
        let data: Data
        do { data = try Data(contentsOf: game.url) } catch { throw EmulatorError.read(error.localizedDescription) }
        guard data.count >= 0x150 else { throw EmulatorError.invalid }
        guard let gb = gb_create() else { throw EmulatorError.invalid }
        let result = data.withUnsafeBytes { raw -> Int32 in
            gb_load_rom(gb, raw.bindMemory(to: UInt8.self).baseAddress, data.count)
        }
        if result != 0 {
            gb_destroy(gb)
            if result == -3 { throw EmulatorError.mapper(data[0x147]) }
            throw EmulatorError.invalid
        }
        self.gb = gb
        gameKey = String(format: "%08X", gb_rom_crc32(gb))
        audio = AudioOutput(gb: gb)

        loadBattery()
        applySettings()
        settings.objectWillChange
            .receive(on: RunLoop.main)
            .sink { [weak self] _ in DispatchQueue.main.async { self?.applySettings() } }
            .store(in: &cancellables)

        resumeIfPossible()
        lastImage = makeImage()
    }

    deinit {
        displayLink?.invalidate()
        audio.stop()
        gb_destroy(gb)
    }

    private func applySettings() {
        settings.palette.withUnsafeBufferPointer { gb_set_palette(gb, $0.baseAddress) }
        gb_set_color_correction(gb, settings.colorCorrection ? 1 : 0)
        audio.volume = settings.soundEnabled ? Float(settings.volume) : 0
        audio.configureSession(playInSilentMode: settings.playInSilentMode)
        if let img = makeImage() { lastImage = img; onFrame?(img) }
    }

    // MARK: Run loop

    func start() {
        guard displayLink == nil else { resume(); return }
        let link = CADisplayLink(target: DisplayLinkProxy(self), selector: #selector(DisplayLinkProxy.tick(_:)))
        if #available(iOS 15.0, *) {
            link.preferredFrameRateRange = CAFrameRateRange(minimum: 60, maximum: 120, preferred: 60)
        }
        link.add(to: .main, forMode: .common)
        displayLink = link
        resume()
    }

    func pause() {
        guard !paused else { return }
        paused = true
        displayLink?.isPaused = true
        audio.stop()
        saveBatteryIfNeeded(force: true)
    }

    func resume() {
        paused = false
        lastTimestamp = 0
        accumulator = 0
        gb_audio_clear(gb)
        displayLink?.isPaused = false
        audio.start()
    }

    /// App going to background: persist everything in case iOS kills us.
    func suspend() {
        let wasRunning = !paused
        pause()
        if settings.autoResume { saveState(slot: 0) }
        rememberSaveFileStamp()
        suspendedWhileRunning = wasRunning
    }
    private(set) var suspendedWhileRunning = false

    /// Call when leaving the game.
    func stop() {
        suspend()
        displayLink?.invalidate()
        displayLink = nil
    }

    fileprivate func tick(_ link: CADisplayLink) {
        let now = link.timestamp
        if lastTimestamp == 0 { lastTimestamp = now; return }
        let dt = min(now - lastTimestamp, 0.1)
        lastTimestamp = now

        let speed = fastForward ? Double(max(2, settings.fastForwardSpeed)) : 1
        accumulator += dt * Self.frameRate * speed
        var frames = Int(accumulator)
        accumulator -= Double(frames)
        let cap = fastForward ? Int(speed) * 2 + 2 : 3
        if frames > cap { frames = cap; accumulator = 0 }

        if !fastForward {
            // Gently steer the audio resampler so the buffer stays ~50 ms deep.
            let buffered = Double(gb_audio_buffered(gb))
            let target = AudioOutput.sampleRate * 0.05
            if buffered > target * 4 { gb_audio_clear(gb) }
            let err = max(-1, min(1, (target - buffered) / target))
            gb_set_sample_rate(gb, AudioOutput.sampleRate * (1 + err * 0.005))
        }

        guard frames > 0 else { return }
        for _ in 0..<frames { gb_run_frame(gb) }
        if let img = makeImage() {
            lastImage = img
            onFrame?(img)
        }

        framesSinceSaveCheck += frames
        if framesSinceSaveCheck >= 90 {
            framesSinceSaveCheck = 0
            saveBatteryIfNeeded(force: false)
        }
    }

    private func makeImage() -> CGImage? {
        guard let px = gb_framebuffer(gb) else { return nil }
        let data = Data(bytes: px, count: 160 * 144 * 4) as CFData
        guard let provider = CGDataProvider(data: data) else { return nil }
        return CGImage(width: 160, height: 144, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: 160 * 4,
                       space: colorSpace,
                       bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    }

    func reset() {
        saveBatteryIfNeeded(force: true)
        gb_reset(gb)
        gb_audio_clear(gb)
    }

    // MARK: Battery saves

    private var now: Int64 { Int64(Date().timeIntervalSince1970) }

    private func loadBattery() {
        guard let data = try? Data(contentsOf: game.saveURL), !data.isEmpty else { return }
        _ = data.withUnsafeBytes { raw in
            gb_save_import(gb, raw.bindMemory(to: UInt8.self).baseAddress, data.count, now)
        }
    }

    func batteryData(includeClock: Bool) -> Data? {
        let size = gb_save_export_size(gb, includeClock ? 1 : 0)
        guard size > 0 else { return nil }
        var data = Data(count: size)
        let n = data.withUnsafeMutableBytes { raw in
            gb_save_export(gb, raw.bindMemory(to: UInt8.self).baseAddress, size, now, includeClock ? 1 : 0)
        }
        return n == size ? data : nil
    }

    func saveBatteryIfNeeded(force: Bool) {
        guard gb_has_battery(gb) != 0 else { return }
        guard force || gb_sram_dirty(gb) != 0 else { return }
        guard let data = batteryData(includeClock: true) else { return }
        do {
            try data.write(to: game.saveURL, options: .atomic)
            gb_sram_clear_dirty(gb)
        } catch {
            NSLog("save failed: \(error)")
        }
    }

    enum SaveImportError: LocalizedError {
        case noBattery, failed
        var errorDescription: String? {
            switch self {
            case .noBattery: return "This game doesn't have a save battery, so it can't use save files. Use save states instead."
            case .failed: return "That save file couldn't be read."
            }
        }
    }

    /// Imports a save from another emulator / flash cart and restarts the game with it.
    func importSave(from url: URL) throws {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        let data = try Data(contentsOf: url)
        guard gb_has_battery(gb) != 0 || gb_save_export_size(gb, 0) > 0 else { throw SaveImportError.noBattery }
        saveBatteryIfNeeded(force: true)
        SaveBackups.backup(game.saveURL)
        let r = data.withUnsafeBytes { raw in
            gb_save_import(gb, raw.bindMemory(to: UInt8.self).baseAddress, data.count, now)
        }
        if r == -5 { throw SaveImportError.noBattery }
        if r != 0 { throw SaveImportError.failed }
        gb_reset(gb)
        gb_audio_clear(gb)
        saveBatteryIfNeeded(force: true)
        deleteState(slot: 0)
    }

    /// Writes an export file to a temp folder for the share sheet.
    func exportSave(retroArch: Bool) -> URL? {
        saveBatteryIfNeeded(force: true)
        guard let data = batteryData(includeClock: !retroArch) else { return nil }
        let url = Paths.temp.appendingPathComponent(game.name + (retroArch ? ".srm" : ".sav"))
        try? FileManager.default.removeItem(at: url)
        do { try data.write(to: url) } catch { return nil }
        return url
    }

    // MARK: Save states

    private var statesDir: URL {
        let d = Paths.states.appendingPathComponent(game.name + " [" + gameKey + "]", isDirectory: true)
        try? FileManager.default.createDirectory(at: d, withIntermediateDirectories: true)
        return d
    }
    private func stateURL(_ slot: Int) -> URL { statesDir.appendingPathComponent(slot == 0 ? "resume.state" : "slot\(slot).state") }
    private func thumbURL(_ slot: Int) -> URL { statesDir.appendingPathComponent(slot == 0 ? "resume.png" : "slot\(slot).png") }

    private func captureState() -> Data? {
        let size = gb_state_size(gb)
        var data = Data(count: size)
        let r = data.withUnsafeMutableBytes { raw in gb_state_save(gb, raw.bindMemory(to: UInt8.self).baseAddress, size) }
        return r == 0 ? data : nil
    }

    private func restoreState(_ data: Data) -> Bool {
        let r = data.withUnsafeBytes { raw in gb_state_load(gb, raw.bindMemory(to: UInt8.self).baseAddress, data.count) }
        if r == 0, let img = makeImage() { lastImage = img; onFrame?(img) }
        return r == 0
    }

    @discardableResult
    func saveState(slot: Int) -> Bool {
        guard let data = captureState() else { return false }
        do { try data.write(to: stateURL(slot), options: .atomic) } catch { return false }
        if let img = lastImage, let png = UIImage(cgImage: img).pngData() { try? png.write(to: thumbURL(slot)) }
        return true
    }

    @discardableResult
    func loadState(slot: Int) -> Bool {
        guard let data = try? Data(contentsOf: stateURL(slot)) else { return false }
        let before = captureState()
        SaveBackups.backup(game.saveURL)
        guard restoreState(data) else { return false }
        undoState = before
        saveBatteryIfNeeded(force: true)
        return true
    }

    func undoLoadState() {
        guard let d = undoState else { return }
        if restoreState(d) { saveBatteryIfNeeded(force: true) }
        undoState = nil
    }

    func deleteState(slot: Int) {
        try? FileManager.default.removeItem(at: stateURL(slot))
        try? FileManager.default.removeItem(at: thumbURL(slot))
    }

    func slots() -> [SaveSlotInfo] {
        (0...9).map { i in
            let url = stateURL(i)
            let date = (try? url.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate
            let thumb = date == nil ? nil : UIImage(contentsOfFile: thumbURL(i).path)
            return SaveSlotInfo(slot: i, date: date, thumbnail: thumb)
        }
    }

    // MARK: Resume

    private var stampKey: String { "savStamp.\(gameKey)" }

    private func saveFileStamp() -> Double {
        let d = (try? game.saveURL.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate
        return d?.timeIntervalSince1970 ?? 0
    }

    private func rememberSaveFileStamp() {
        UserDefaults.standard.set(saveFileStamp(), forKey: stampKey)
    }

    /// Restores the snapshot taken when the game was last closed, unless the save file
    /// was replaced since (imported, or changed through the Files app).
    private func resumeIfPossible() {
        let replacedKey = "saveReplaced.\(game.name)"
        let replaced = UserDefaults.standard.bool(forKey: replacedKey)
        UserDefaults.standard.removeObject(forKey: replacedKey)
        guard FileManager.default.fileExists(atPath: stateURL(0).path) else { return }
        let remembered = UserDefaults.standard.double(forKey: stampKey)
        let changed = replaced || abs(saveFileStamp() - remembered) > 1
        if !settings.autoResume || changed {
            if changed { deleteState(slot: 0) }
            return
        }
        if let data = try? Data(contentsOf: stateURL(0)) { _ = restoreState(data) }
    }
}

/// Avoids a retain cycle between CADisplayLink and the emulator.
private final class DisplayLinkProxy: NSObject {
    weak var target: Emulator?
    init(_ t: Emulator) { target = t }
    @objc func tick(_ link: CADisplayLink) {
        if let t = target { t.tick(link) } else { link.invalidate() }
    }
}
