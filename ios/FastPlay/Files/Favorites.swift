import Foundation

/// A folder pinned to the Home screen: on this device, in Dropbox or on a server.
struct Favorite: Codable, Equatable {
    var id = UUID()
    /// Its source's id (FileSources).
    var sourceID: String
    var path: String
    var displayPath: String
    var name: String

    var folder: FileEntry {
        FileEntry(name: name, path: path, displayPath: displayPath, isFolder: true)
    }
}

/// The favorites, in the app's defaults, in the order they were added.
enum FavoritesStore {
    private static let key = "Favorites"

    static var all: [Favorite] {
        get {
            guard let data = UserDefaults.standard.data(forKey: key) else { return [] }
            return (try? JSONDecoder().decode([Favorite].self, from: data)) ?? []
        }
        set {
            UserDefaults.standard.set(try? JSONEncoder().encode(newValue), forKey: key)
        }
    }

    static func find(source: FileSource, path: String) -> Favorite? {
        all.first { $0.sourceID == source.id && FileOperations.same($0.path, path) }
    }

    static func add(source: FileSource, folder: FileEntry) {
        guard find(source: source, path: folder.path) == nil else { return }
        all.append(Favorite(sourceID: source.id, path: folder.path, displayPath: folder.displayPath, name: folder.name))
    }

    static func remove(_ favorite: Favorite) {
        all.removeAll { $0.id == favorite.id }
    }

    /// The favorites of a source that has gone (a server deleted).
    static func removeAll(sourceID: String) {
        all.removeAll { $0.sourceID == sourceID }
    }
}
