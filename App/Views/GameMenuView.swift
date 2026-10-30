import SwiftUI
import UniformTypeIdentifiers

final class GameMenuModel: ObservableObject {
    let emulator: Emulator
    var close: ((String?) -> Void)?
    var quit: (() -> Void)?
    @Published var slots: [SaveSlotInfo] = []
    @Published var error: String?

    init(emulator: Emulator) {
        self.emulator = emulator
        reloadSlots()
    }
    func reloadSlots() { slots = emulator.slots() }
}

struct ShareItem: Identifiable {
    let url: URL
    var id: String { url.path }
}

struct ActivityView: UIViewControllerRepresentable {
    let url: URL
    func makeUIViewController(context: Context) -> UIActivityViewController {
        UIActivityViewController(activityItems: [url], applicationActivities: nil)
    }
    func updateUIViewController(_ vc: UIActivityViewController, context: Context) {}
}

struct GameMenuView: View {
    @ObservedObject var model: GameMenuModel
    @State private var importing = false
    @State private var pendingImport: URL?
    @State private var share: ShareItem?
    @State private var confirmReset = false

    private var emu: Emulator { model.emulator }

    var body: some View {
        NavigationStack {
            List {
                Section {
                    Button { model.close?(nil) } label: { Label("Resume", systemImage: "play.fill") }
                }

                Section("Save States") {
                    NavigationLink { SaveStatesView(model: model, mode: .save) } label: {
                        Label("Save State", systemImage: "square.and.arrow.down")
                    }
                    NavigationLink { SaveStatesView(model: model, mode: .load) } label: {
                        Label("Load State", systemImage: "clock.arrow.circlepath")
                    }
                    if emu.canUndoLoad {
                        Button {
                            emu.undoLoadState()
                            model.close?("Load undone")
                        } label: { Label("Undo Load State", systemImage: "arrow.uturn.backward") }
                    }
                }

                Section {
                    if emu.hasBattery {
                        Button { importing = true } label: {
                            Label("Import Save File…", systemImage: "tray.and.arrow.down")
                        }
                        Button {
                            if let u = emu.exportSave(retroArch: false) { share = ShareItem(url: u) }
                        } label: { Label("Export Save (.sav)", systemImage: "square.and.arrow.up") }
                        Button {
                            if let u = emu.exportSave(retroArch: true) { share = ShareItem(url: u) }
                        } label: { Label("Export for RetroArch (.srm)", systemImage: "square.and.arrow.up.on.square") }
                    } else {
                        Text("This game has no save battery. Use save states to keep your progress.")
                            .foregroundStyle(.secondary)
                    }
                } header: {
                    Text("Save File")
                } footer: {
                    if emu.hasBattery {
                        Text("Your save is stored automatically in Files › GB Emu › Saves. Importing keeps a backup of the old save in Saves › Backups. Exported .sav files work in mGBA, BGB, SameBoy, VBA-M, Delta and flash carts\(emu.hasClock ? ", and include the in-game clock" : "").")
                    }
                }

                Section {
                    NavigationLink { SettingsView() } label: { Label("Settings", systemImage: "gearshape") }
                    Button { confirmReset = true } label: { Label("Reset Game", systemImage: "arrow.counterclockwise") }
                    Button(role: .destructive) { model.quit?() } label: {
                        Label("Quit Game", systemImage: "xmark.circle")
                    }
                }
            }
            .navigationTitle(emu.game.name)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { model.close?(nil) }
                }
            }
            .fileImporter(isPresented: $importing, allowedContentTypes: [.item]) { result in
                switch result {
                case .success(let url): pendingImport = url
                case .failure(let e): model.error = e.localizedDescription
                }
            }
            .confirmationDialog("Replace your current save?", isPresented: Binding(
                get: { pendingImport != nil }, set: { if !$0 { pendingImport = nil } }), titleVisibility: .visible) {
                Button("Import and Restart Game", role: .destructive) {
                    guard let url = pendingImport else { return }
                    pendingImport = nil
                    do {
                        try emu.importSave(from: url)
                        model.close?("Save imported – game restarted")
                    } catch {
                        model.error = error.localizedDescription
                    }
                }
                Button("Cancel", role: .cancel) { pendingImport = nil }
            } message: {
                Text("The game will restart using “\(pendingImport?.lastPathComponent ?? "")”. Your current save is backed up first.")
            }
            .confirmationDialog("Reset the game?", isPresented: $confirmReset, titleVisibility: .visible) {
                Button("Reset", role: .destructive) {
                    emu.reset()
                    model.close?("Game reset")
                }
            } message: {
                Text("Progress since your last in-game save will be lost.")
            }
            .sheet(item: $share) { ActivityView(url: $0.url) }
            .alert("Something went wrong", isPresented: Binding(
                get: { model.error != nil }, set: { if !$0 { model.error = nil } })) {
                Button("OK", role: .cancel) {}
            } message: {
                Text(model.error ?? "")
            }
        }
    }
}

struct SaveStatesView: View {
    enum Mode { case save, load }
    @ObservedObject var model: GameMenuModel
    let mode: Mode
    @State private var overwrite: SaveSlotInfo?

    private let columns = [GridItem(.adaptive(minimum: 150), spacing: 14)]

    var body: some View {
        ScrollView {
            LazyVGrid(columns: columns, spacing: 14) {
                ForEach(model.slots.filter { mode == .load || $0.slot != 0 }) { s in
                    Button { tap(s) } label: { SlotCell(info: s) }
                        .buttonStyle(.plain)
                        .disabled(mode == .load && s.date == nil)
                        .contextMenu {
                            if s.date != nil {
                                Button(role: .destructive) {
                                    model.emulator.deleteState(slot: s.slot)
                                    model.reloadSlots()
                                } label: { Label("Delete", systemImage: "trash") }
                            }
                        }
                }
            }
            .padding()
        }
        .navigationTitle(mode == .save ? "Save State" : "Load State")
        .onAppear { model.reloadSlots() }
        .confirmationDialog("Overwrite \(overwrite.map(slotName) ?? "")?", isPresented: Binding(
            get: { overwrite != nil }, set: { if !$0 { overwrite = nil } }), titleVisibility: .visible) {
            Button("Overwrite", role: .destructive) {
                if let s = overwrite { save(s.slot) }
                overwrite = nil
            }
        }
    }

    private func tap(_ s: SaveSlotInfo) {
        switch mode {
        case .save:
            if s.date != nil { overwrite = s } else { save(s.slot) }
        case .load:
            if model.emulator.loadState(slot: s.slot) {
                model.close?("Loaded \(slotName(s))")
            } else {
                model.error = "That save state couldn't be loaded. It may be from an older version of the app."
            }
        }
    }

    private func save(_ slot: Int) {
        if model.emulator.saveState(slot: slot) {
            model.close?("Saved to Slot \(slot)")
        } else {
            model.error = "Couldn't write the save state."
        }
    }
}

func slotName(_ s: SaveSlotInfo) -> String { s.slot == 0 ? "Resume Point" : "Slot \(s.slot)" }

struct SlotCell: View {
    let info: SaveSlotInfo
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            ZStack {
                RoundedRectangle(cornerRadius: 8).fill(Color(.secondarySystemBackground))
                if let t = info.thumbnail {
                    Image(uiImage: t).resizable().interpolation(.none).aspectRatio(160.0 / 144.0, contentMode: .fit)
                        .clipShape(RoundedRectangle(cornerRadius: 8))
                } else {
                    Image(systemName: "square.dashed").font(.largeTitle).foregroundStyle(.tertiary)
                }
            }
            .aspectRatio(160.0 / 144.0, contentMode: .fit)
            Text(slotName(info)).font(.subheadline.weight(.semibold))
            Text(info.date.map { $0.formatted(date: .abbreviated, time: .shortened) } ?? "Empty")
                .font(.caption).foregroundStyle(.secondary)
        }
        .contentShape(Rectangle())
    }
}
