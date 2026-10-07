import Foundation

/// Where you were in each folder: the file last playing from it, and how far
/// into that file. With Play All Resumes Where You Left Off on, Play All in a
/// folder starts there again rather than at its first file.
///
/// A folder's files are playing when they were played from it (Play All, or a
/// file opened with the rest of its folder). While they are, where playback is
/// is noted every few seconds and as the app goes to the background; playing
/// anything else (another folder, a station) stops the noting, as the playlist
/// is then no longer the folder's.
@MainActor
final class FolderResume {
    static let shared = FolderResume()

    nonisolated static var enabled: Bool {
        get { UserDefaults.standard.bool(forKey: "PlayAllResumes") }
        set { UserDefaults.standard.set(newValue, forKey: "PlayAllResumes") }
    }

    struct Place {
        let file: String       // as the folder's files are named (see begin)
        let position: Double   // seconds
    }

    private static let placesKey = "FolderResumePlaces"
    private static let keep = 500  // folders remembered, the oldest forgotten

    /// A folder, by its source and path.
    nonisolated static func key(source: FileSource, folder: String) -> String { source.id + "|" + local(folder) }

    /// A path in this device's files, as it stays put: below the app's documents
    /// folder, which moves when the app is restored or reinstalled. Others as they are.
    nonisolated static func local(_ path: String) -> String {
        let documents = FPEngine.shared.documentsPath
        return path.hasPrefix(documents) ? String(path.dropFirst(documents.count)) : path
    }

    private var key: String?
    private var files: [String] = []
    private var timer: Timer?

    /// A folder's `files` are now the playlist, in its order.
    func begin(key: String, files: [String]) {
        self.key = key
        self.files = files
        if timer == nil {
            timer = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { _ in
                MainActor.assumeIsolated { FolderResume.shared.record() }
            }
        }
    }

    /// Where Play All in this folder left off, if anywhere.
    func place(for key: String) -> Place? {
        guard let saved = (UserDefaults.standard.dictionary(forKey: Self.placesKey) as? [String: [String: Any]])?[key],
              let file = saved["file"] as? String
        else { return nil }
        return Place(file: file, position: saved["position"] as? Double ?? 0)
    }

    /// Notes where the playing folder is. Nothing if the playlist is not its any more.
    func record() {
        let engine = FPEngine.shared
        guard let key, engine.trackCount == files.count, files.indices.contains(engine.currentTrack) else {
            return
        }
        // Still the folder's: the track playing is the file it was given as
        let index = engine.currentTrack
        let path = engine.trackPathAtIndex(index)
        let file = files[index]
        guard path.hasPrefix("http") || FolderResume.local(path) == file else { return }
        save(key: key, place: Place(file: file, position: engine.loaded ? engine.position : 0))
    }

    /// Once the file starting has loaded, to where it was left: a few seconds
    /// back, to hear where you were again.
    func seek(to place: Place) {
        guard place.position > 5 else { return }
        Task { @MainActor in
            let engine = FPEngine.shared
            for _ in 0..<150 {  // up to 15 seconds for a file on a server to open
                if engine.loaded && engine.length > 0 {
                    engine.seekTo(max(0, place.position - 3))
                    return
                }
                try? await Task.sleep(nanoseconds: 100_000_000)
            }
        }
    }

    private func save(key: String, place: Place) {
        let defaults = UserDefaults.standard
        var places = defaults.dictionary(forKey: Self.placesKey) as? [String: [String: Any]] ?? [:]
        var order = defaults.stringArray(forKey: Self.placesKey + "Order") ?? []
        order.removeAll { $0 == key }
        order.append(key)
        places[key] = ["file": place.file, "position": place.position]
        while order.count > Self.keep { places[order.removeFirst()] = nil }
        defaults.set(places, forKey: Self.placesKey)
        defaults.set(order, forKey: Self.placesKey + "Order")
    }
}
