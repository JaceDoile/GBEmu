import SwiftUI
import UIKit

struct LibraryView: View {
    @EnvironmentObject var library: Library
    @EnvironmentObject var settings: AppSettings
    @Environment(\.scenePhase) private var scenePhase
    @State private var importing = false
    @State private var showSettings = false
    @State private var toDelete: Game?

    var body: some View {
        NavigationStack {
            Group {
                if library.games.isEmpty {
                    emptyState
                } else {
                    List {
                        ForEach(library.games) { game in
                            Button { play(game) } label: { GameRow(game: game, lastPlayed: library.lastPlayedDate(game)) }
                                .swipeActions {
                                    Button(role: .destructive) { toDelete = game } label: { Label("Delete", systemImage: "trash") }
                                }
                                .contextMenu {
                                    Button { play(game) } label: { Label("Play", systemImage: "play.fill") }
                                    Button(role: .destructive) { toDelete = game } label: { Label("Delete Game", systemImage: "trash") }
                                }
                        }
                    }
                    .listStyle(.insetGrouped)
                    .refreshable { library.refresh() }
                }
            }
            .navigationTitle("Games")
            .toolbar {
                ToolbarItem(placement: .navigationBarLeading) {
                    Button { showSettings = true } label: { Image(systemName: "gearshape") }
                }
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button { importing = true } label: { Image(systemName: "plus") }
                }
            }
            .fileImporter(isPresented: $importing, allowedContentTypes: [.item], allowsMultipleSelection: true) { result in
                if case .success(let urls) = result { library.importFiles(urls) }
            }
            .sheet(isPresented: $showSettings) {
                NavigationStack {
                    SettingsView()
                        .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { showSettings = false } } }
                }
                .environmentObject(settings)
            }
            .confirmationDialog("Delete \(toDelete?.name ?? "")?", isPresented: Binding(
                get: { toDelete != nil }, set: { if !$0 { toDelete = nil } }), titleVisibility: .visible) {
                Button("Delete Game", role: .destructive) {
                    if let g = toDelete { library.delete(g) }
                    toDelete = nil
                }
            } message: {
                Text("The save file stays in the Saves folder, so re-adding the game brings your progress back.")
            }
            .alert("Heads up", isPresented: Binding(get: { library.alert != nil }, set: { if !$0 { library.alert = nil } })) {
                Button("OK", role: .cancel) {}
            } message: {
                Text(library.alert ?? "")
            }
        }
        .onChange(of: scenePhase) { phase in
            if phase == .active { library.refresh() }
        }
    }

    private var emptyState: some View {
        VStack(spacing: 18) {
            Image(systemName: "gamecontroller").font(.system(size: 56)).foregroundStyle(.secondary)
            Text("No Games Yet").font(.title2.weight(.semibold))
            Text("Add .gb, .gbc or .zip files you own. You can also drop them into Files › On My iPhone › GB Emu, along with any .sav files.")
                .multilineTextAlignment(.center)
                .foregroundStyle(.secondary)
                .padding(.horizontal, 32)
            Button { importing = true } label: {
                Label("Add Games", systemImage: "plus").padding(.horizontal, 8)
            }
            .buttonStyle(.borderedProminent)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private func play(_ game: Game) {
        GameLauncher.play(game, library: library)
    }
}

struct GameRow: View {
    let game: Game
    let lastPlayed: Date?

    var body: some View {
        HStack(spacing: 14) {
            ZStack {
                RoundedRectangle(cornerRadius: 10)
                    .fill(LinearGradient(colors: [tint.opacity(0.9), tint.opacity(0.55)], startPoint: .topLeading, endPoint: .bottomTrailing))
                Text(String(game.name.prefix(1)).uppercased())
                    .font(.system(size: 24, weight: .heavy, design: .rounded))
                    .foregroundStyle(.white)
            }
            .frame(width: 52, height: 52)
            VStack(alignment: .leading, spacing: 3) {
                Text(game.name).font(.body.weight(.medium)).foregroundStyle(.primary).lineLimit(2)
                HStack(spacing: 6) {
                    Text(game.isColor ? "Game Boy Color" : "Game Boy")
                    if FileManager.default.fileExists(atPath: game.saveURL.path) {
                        Image(systemName: "externaldrive.fill").imageScale(.small)
                    }
                    if let d = lastPlayed {
                        Text("· \(d.formatted(.relative(presentation: .named)))")
                    }
                }
                .font(.caption).foregroundStyle(.secondary)
            }
            Spacer(minLength: 0)
        }
        .padding(.vertical, 4)
        .contentShape(Rectangle())
    }

    private var tint: Color {
        let colors: [Color] = [.green, .teal, .indigo, .orange, .pink, .purple, .blue, .red, .mint]
        var h: UInt32 = 5381
        for b in game.name.utf8 { h = (h &* 33) &+ UInt32(b) }
        return colors[Int(h % UInt32(colors.count))]
    }
}

enum GameLauncher {
    @MainActor
    static func play(_ game: Game, library: Library) {
        guard let scene = UIApplication.shared.connectedScenes.compactMap({ $0 as? UIWindowScene }).first,
              let root = scene.windows.first(where: { $0.isKeyWindow })?.rootViewController ?? scene.windows.first?.rootViewController
        else { return }
        var top = root
        while let p = top.presentedViewController { top = p }
        do {
            let emu = try Emulator(game: game)
            let vc = GameViewController(emulator: emu)
            vc.onClose = { library.refresh() }
            library.markPlayed(game)
            top.present(vc, animated: true)
        } catch {
            library.alert = error.localizedDescription
        }
    }
}
