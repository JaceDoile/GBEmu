import SwiftUI

struct SettingsView: View {
    @EnvironmentObject var settings: AppSettings

    var body: some View {
        Form {
            Section {
                ForEach(Palettes.all) { p in
                    Button { settings.paletteID = p.id } label: {
                        HStack {
                            HStack(spacing: 0) {
                                ForEach(0..<4, id: \.self) { i in p.color(i).frame(width: 18, height: 18) }
                            }
                            .clipShape(RoundedRectangle(cornerRadius: 4))
                            Text(p.name).foregroundStyle(.primary)
                            Spacer()
                            if settings.paletteID == p.id { Image(systemName: "checkmark").foregroundStyle(.tint) }
                        }
                    }
                }
            } header: {
                Text("Original Game Boy Colors")
            } footer: {
                Text("Game Boy Color games always use their own colors.")
            }

            Section("Display") {
                Toggle("GBC Color Correction", isOn: $settings.colorCorrection)
                Toggle("Pixel-Perfect Scaling", isOn: $settings.integerScaling)
                Toggle("Smooth Pixels", isOn: $settings.smoothing)
            }

            Section {
                HStack {
                    Text("Button Opacity")
                    Slider(value: $settings.controlsOpacity, in: 0.2...1)
                }
                Toggle("Vibration on Press", isOn: $settings.haptics)
                Toggle("Hide Touch Controls with Controller", isOn: $settings.hideTouchWithController)
                Picker("Controller A/B", selection: $settings.controllerByPosition) {
                    Text("By Position (Game Boy)").tag(true)
                    Text("By Label").tag(false)
                }
            } header: {
                Text("Controls")
            } footer: {
                Text("Controllers: D-pad or left stick, Menu = Start, Options = Select, right bumper/trigger = fast forward, left bumper/trigger = this menu. Keyboard: arrows/WASD, X = A, Z = B, Return = Start, Shift = Select, Space = fast forward, Esc = menu.")
            }

            Section("Sound") {
                Toggle("Sound", isOn: $settings.soundEnabled)
                HStack {
                    Image(systemName: "speaker.fill").foregroundStyle(.secondary)
                    Slider(value: $settings.volume, in: 0...1)
                    Image(systemName: "speaker.wave.3.fill").foregroundStyle(.secondary)
                }
                .disabled(!settings.soundEnabled)
                Toggle("Play with Silent Mode On", isOn: $settings.playInSilentMode)
            }

            Section {
                Picker("Fast Forward Speed", selection: $settings.fastForwardSpeed) {
                    ForEach([2, 3, 4, 6, 8], id: \.self) { Text("\($0)×").tag($0) }
                }
                Toggle("Hold to Fast Forward", isOn: $settings.fastForwardHold)
                Toggle("Resume Where You Left Off", isOn: $settings.autoResume)
            } header: {
                Text("Gameplay")
            } footer: {
                Text("When resuming is on, closing a game takes a snapshot so it reopens exactly where you were. Your in-game save file is always kept separately.")
            }

            Section("About") {
                LabeledContent("Version", value: Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "1.0")
                Text("Put ROMs, saves and zips in Files › On My iPhone › GB Emu. Saves named like the game (Pokemon Gold.sav next to Pokemon Gold.gbc) are picked up automatically.")
                    .font(.footnote).foregroundStyle(.secondary)
            }
        }
        .navigationTitle("Settings")
    }
}
