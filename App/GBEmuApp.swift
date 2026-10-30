import SwiftUI

@main
struct GBEmuApp: App {
    @StateObject private var library = Library()
    @StateObject private var settings = AppSettings.shared

    var body: some Scene {
        WindowGroup {
            LibraryView()
                .environmentObject(library)
                .environmentObject(settings)
                .onOpenURL { url in library.importFiles([url]) }
        }
    }
}
