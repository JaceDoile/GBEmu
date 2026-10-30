import Foundation
import SwiftUI

struct PalettePreset: Identifiable, Hashable {
    let id: String
    let name: String
    let colors: [UInt32]   // lightest -> darkest, 0xRRGGBB

    func color(_ i: Int) -> Color {
        let c = colors[i]
        return Color(red: Double((c >> 16) & 0xFF) / 255, green: Double((c >> 8) & 0xFF) / 255, blue: Double(c & 0xFF) / 255)
    }
}

enum Palettes {
    static let all: [PalettePreset] = [
        PalettePreset(id: "green", name: "Classic Green", colors: [0xE0F8D0, 0x88C070, 0x346856, 0x081820]),
        PalettePreset(id: "pea", name: "Original Screen", colors: [0x9BBC0F, 0x8BAC0F, 0x306230, 0x0F380F]),
        PalettePreset(id: "pocket", name: "Pocket", colors: [0xC4CFA1, 0x8B956D, 0x4D533C, 0x1F1F1F]),
        PalettePreset(id: "gray", name: "Grayscale", colors: [0xFFFFFF, 0xAAAAAA, 0x555555, 0x000000]),
        PalettePreset(id: "light", name: "Backlit Teal", colors: [0x00B581, 0x009A71, 0x00694A, 0x004F3B]),
        PalettePreset(id: "sepia", name: "Warm Sepia", colors: [0xF8E8C8, 0xD8A878, 0x986848, 0x302018]),
        PalettePreset(id: "ice", name: "Ice", colors: [0xE8F4FF, 0x8CB8E0, 0x3C6894, 0x0C1C34]),
    ]
    static func preset(_ id: String) -> PalettePreset { all.first { $0.id == id } ?? all[0] }
}

/// User preferences, persisted in UserDefaults.
final class AppSettings: ObservableObject {
    static let shared = AppSettings()
    private let d = UserDefaults.standard

    @Published var paletteID: String { didSet { d.set(paletteID, forKey: "paletteID") } }
    @Published var colorCorrection: Bool { didSet { d.set(colorCorrection, forKey: "colorCorrection") } }
    @Published var integerScaling: Bool { didSet { d.set(integerScaling, forKey: "integerScaling") } }
    @Published var smoothing: Bool { didSet { d.set(smoothing, forKey: "smoothing") } }

    @Published var controlsOpacity: Double { didSet { d.set(controlsOpacity, forKey: "controlsOpacity") } }
    @Published var haptics: Bool { didSet { d.set(haptics, forKey: "haptics") } }
    @Published var hideTouchWithController: Bool { didSet { d.set(hideTouchWithController, forKey: "hideTouchWithController") } }
    /// Gamepads: true = face buttons by position (right = A, bottom = B, like a Game Boy), false = by printed label.
    @Published var controllerByPosition: Bool { didSet { d.set(controllerByPosition, forKey: "controllerByPosition") } }

    @Published var soundEnabled: Bool { didSet { d.set(soundEnabled, forKey: "soundEnabled") } }
    @Published var volume: Double { didSet { d.set(volume, forKey: "volume") } }
    @Published var playInSilentMode: Bool { didSet { d.set(playInSilentMode, forKey: "playInSilentMode") } }

    @Published var fastForwardSpeed: Int { didSet { d.set(fastForwardSpeed, forKey: "fastForwardSpeed") } }
    @Published var fastForwardHold: Bool { didSet { d.set(fastForwardHold, forKey: "fastForwardHold") } }
    @Published var autoResume: Bool { didSet { d.set(autoResume, forKey: "autoResume") } }

    var palette: [UInt32] { Palettes.preset(paletteID).colors }

    private init() {
        d.register(defaults: [
            "paletteID": "green", "colorCorrection": true, "integerScaling": true, "smoothing": false,
            "controlsOpacity": 0.75, "haptics": true, "hideTouchWithController": true, "controllerByPosition": true,
            "soundEnabled": true, "volume": 1.0, "playInSilentMode": false,
            "fastForwardSpeed": 3, "fastForwardHold": false, "autoResume": true,
        ])
        paletteID = d.string(forKey: "paletteID") ?? "green"
        colorCorrection = d.bool(forKey: "colorCorrection")
        integerScaling = d.bool(forKey: "integerScaling")
        smoothing = d.bool(forKey: "smoothing")
        controlsOpacity = d.double(forKey: "controlsOpacity")
        haptics = d.bool(forKey: "haptics")
        hideTouchWithController = d.bool(forKey: "hideTouchWithController")
        controllerByPosition = d.bool(forKey: "controllerByPosition")
        soundEnabled = d.bool(forKey: "soundEnabled")
        volume = d.double(forKey: "volume")
        playInSilentMode = d.bool(forKey: "playInSilentMode")
        fastForwardSpeed = d.integer(forKey: "fastForwardSpeed")
        fastForwardHold = d.bool(forKey: "fastForwardHold")
        autoResume = d.bool(forKey: "autoResume")
    }
}
