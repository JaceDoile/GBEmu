import Foundation
import SwiftUI

enum Paths {
    static let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    static let roms = documents.appendingPathComponent("ROMs", isDirectory: true)
    static let saves = documents.appendingPathComponent("Saves", isDirectory: true)
    static let saveBackups = saves.appendingPathComponent("Backups", isDirectory: true)
    static let states = documents.appendingPathComponent("States", isDirectory: true)
    static let temp = FileManager.default.temporaryDirectory.appendingPathComponent("Export", isDirectory: true)

    static func ensure() {
        for u in [roms, saves, saveBackups, states, temp] {
            try? FileManager.default.createDirectory(at: u, withIntermediateDirectories: true)
        }
    }
}

let romExtensions: Set<String> = ["gb", "gbc", "cgb", "sgb"]
let saveExtensions: Set<String> = ["sav", "srm", "sa1", "dsv"]

struct Game: Identifiable, Hashable {
    let url: URL
    var id: String { url.lastPathComponent }
    var name: String { url.deletingPathExtension().lastPathComponent }
    var isColor: Bool { ["gbc", "cgb"].contains(url.pathExtension.lowercased()) }
    var saveURL: URL { Paths.saves.appendingPathComponent(name + ".sav") }
}

@MainActor
final class Library: ObservableObject {
    @Published private(set) var games: [Game] = []
    @Published var alert: String?

    private var lastPlayed: [String: Double] {
        get { UserDefaults.standard.dictionary(forKey: "lastPlayed") as? [String: Double] ?? [:] }
        set { UserDefaults.standard.set(newValue, forKey: "lastPlayed") }
    }

    init() {
        Paths.ensure()
        refresh()
    }

    func lastPlayedDate(_ g: Game) -> Date? {
        lastPlayed[g.id].map { Date(timeIntervalSince1970: $0) }
    }

    func markPlayed(_ g: Game) {
        var lp = lastPlayed
        lp[g.id] = Date().timeIntervalSince1970
        lastPlayed = lp
        refresh()
    }

    func refresh() {
        adoptLooseFiles()
        let fm = FileManager.default
        let urls = (try? fm.contentsOfDirectory(at: Paths.roms, includingPropertiesForKeys: nil)) ?? []
        let lp = lastPlayed
        games = urls
            .filter { romExtensions.contains($0.pathExtension.lowercased()) }
            .map(Game.init)
            .sorted { a, b in
                let la = lp[a.id] ?? 0, lb = lp[b.id] ?? 0
                if la != lb { return la > lb }
                return a.name.localizedCaseInsensitiveCompare(b.name) == .orderedAscending
            }
    }

    /// Files dropped into the top of the app's folder in the Files app get sorted into place.
    private func adoptLooseFiles() {
        let fm = FileManager.default
        guard let urls = try? fm.contentsOfDirectory(at: Paths.documents, includingPropertiesForKeys: nil) else { return }
        for u in urls {
            let ext = u.pathExtension.lowercased()
            if romExtensions.contains(ext) || ext == "zip" || saveExtensions.contains(ext) {
                _ = try? importOne(u, deleteSource: true)
            }
        }
    }

    /// Import ROMs, zipped ROMs and save files picked by the user or opened from another app.
    func importFiles(_ urls: [URL]) {
        var imported: [String] = []
        var errors: [String] = []
        for url in urls {
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            do {
                let names = try importOne(url, deleteSource: url.path.hasPrefix(Paths.documents.path + "/Inbox"))
                imported += names
            } catch {
                errors.append("\(url.lastPathComponent): \(error.localizedDescription)")
            }
        }
        refresh()
        if !errors.isEmpty {
            alert = errors.joined(separator: "\n")
        } else if imported.isEmpty {
            alert = "No Game Boy games or save files were found."
        }
    }

    enum ImportError: LocalizedError {
        case notRom, noMatchingGame(String), unknownType
        var errorDescription: String? {
            switch self {
            case .notRom: return "This doesn't look like a Game Boy ROM."
            case .noMatchingGame(let n): return "No game named “\(n)” in your library. Add the game first, or open the game and use Import Save File."
            case .unknownType: return "Unsupported file type."
            }
        }
    }

    @discardableResult
    private func importOne(_ url: URL, deleteSource: Bool) throws -> [String] {
        let fm = FileManager.default
        let ext = url.pathExtension.lowercased()
        let data = try Data(contentsOf: url)
        var names: [String] = []

        if romExtensions.contains(ext) {
            guard data.count >= 0x150 else { throw ImportError.notRom }
            let dest = Paths.roms.appendingPathComponent(url.lastPathComponent)
            if dest.standardizedFileURL != url.standardizedFileURL {
                try? fm.removeItem(at: dest)
                try data.write(to: dest)
            }
            names.append(url.lastPathComponent)
        } else if ext == "zip" {
            let entries = try ZipReader.extract(data) { romExtensions.contains(($0 as NSString).pathExtension.lowercased()) || saveExtensions.contains(($0 as NSString).pathExtension.lowercased()) }
            // ROMs first so saves inside the same zip find their game
            for e in entries.sorted(by: { romExtensions.contains(($0.name as NSString).pathExtension.lowercased()) && !romExtensions.contains(($1.name as NSString).pathExtension.lowercased()) }) {
                let eext = (e.name as NSString).pathExtension.lowercased()
                if romExtensions.contains(eext) {
                    guard e.data.count >= 0x150 else { continue }
                    try e.data.write(to: Paths.roms.appendingPathComponent(e.name))
                    names.append(e.name)
                } else {
                    try placeSave(data: e.data, name: e.name)
                    names.append(e.name)
                }
            }
            if names.isEmpty { throw ImportError.notRom }
        } else if saveExtensions.contains(ext) {
            try placeSave(data: data, name: url.lastPathComponent)
            names.append(url.lastPathComponent)
        } else {
            throw ImportError.unknownType
        }
        if deleteSource { try? fm.removeItem(at: url) }
        return names
    }

    /// Puts a save next to the game with the same file name, backing up any existing one.
    private func placeSave(data: Data, name: String) throws {
        let base = (name as NSString).deletingPathExtension
        let romNames = ((try? FileManager.default.contentsOfDirectory(atPath: Paths.roms.path)) ?? [])
            .filter { romExtensions.contains(($0 as NSString).pathExtension.lowercased()) }
            .map { ($0 as NSString).deletingPathExtension }
        guard let match = romNames.first(where: { $0.caseInsensitiveCompare(base) == .orderedSame }) else {
            throw ImportError.noMatchingGame(base)
        }
        let dest = Paths.saves.appendingPathComponent(match + ".sav")
        SaveBackups.backup(dest)
        try data.write(to: dest, options: .atomic)
        // An external save replaces whatever was in the resume snapshot.
        UserDefaults.standard.set(true, forKey: "saveReplaced.\(match)")
    }

    func delete(_ g: Game) {
        try? FileManager.default.removeItem(at: g.url)
        refresh()
    }
}

enum SaveBackups {
    /// Copies an existing save into Saves/Backups with a timestamp. Keeps the 20 newest per game.
    static func backup(_ save: URL) {
        let fm = FileManager.default
        guard fm.fileExists(atPath: save.path) else { return }
        let base = save.deletingPathExtension().lastPathComponent
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd HH.mm.ss"
        let dest = Paths.saveBackups.appendingPathComponent("\(base) (\(f.string(from: Date()))).sav")
        try? fm.copyItem(at: save, to: dest)

        let all = ((try? fm.contentsOfDirectory(at: Paths.saveBackups, includingPropertiesForKeys: [.contentModificationDateKey])) ?? [])
            .filter { $0.lastPathComponent.hasPrefix(base + " (") }
            .sorted {
                let a = (try? $0.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
                let b = (try? $1.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
                return a > b
            }
        for old in all.dropFirst(20) { try? fm.removeItem(at: old) }
    }
}
