import Foundation
import UIKit

/// A file or folder: on this device, in Dropbox, or on a server.
struct FileEntry {
    let name: String
    /// Its path as its source wants it in requests.
    let path: String
    /// Its path as the user knows it, the names with their own capitals. The part
    /// of it below a folder is where a download of that folder puts the file.
    let displayPath: String
    let isFolder: Bool
    var size: Int64 = 0
    var modified: Date?
}

/// Somewhere files are: this device's own (LocalSource), Dropbox, an FTP, SFTP or
/// SMB server. The browser (BrowserViewController) and pasting
/// (FileOperations) are the same for all of them; what a source cannot do, it
/// says so by throwing.
protocol FileSource: AnyObject {
    /// Who it is, for the clipboard and favorites: "local", "dropbox", "server-<id>".
    var id: String { get }
    /// What it is called ("Files", "Dropbox", a server's name).
    var title: String { get }
    /// Its top folder.
    var rootPath: String { get }
    /// This device's own files: read and written where they are, nothing copied.
    var isLocal: Bool { get }
    /// Whether files can be added, renamed and deleted here.
    var canWrite: Bool { get }

    /// What is in a folder, in any order.
    func list(path: String) async throws -> [FileEntry]
    /// Every file in a folder and the folders below it.
    func listAll(path: String) async throws -> [FileEntry]
    /// An address (or a path, here) the engine can play the file from.
    func streamURL(for entry: FileEntry) async throws -> String
    /// Copies the file to a place on this device (replacing what is there).
    func download(_ entry: FileEntry, to destination: URL) async throws
    /// Something for the top folder's menu ("Sign Out").
    var accountAction: (title: String, run: (UIViewController) -> Void)? { get }

    /// The path of `name` in `folder`.
    func path(of name: String, in folder: String) -> String
    func createFolder(named name: String, in folder: String) async throws
    /// Copies a file on this device into `folder` as `name`, replacing one there.
    func upload(_ file: URL, named name: String, to folder: String) async throws
    func rename(_ entry: FileEntry, to name: String) async throws
    /// Deletes a file, or a folder and everything in it.
    func delete(_ entry: FileEntry) async throws
    /// Moves within this source, nothing in the way. False if it cannot (it is then
    /// copied and the original deleted).
    func move(_ entry: FileEntry, to folder: String) async throws -> Bool
    /// Free and total space, where the source can say.
    func storageSpace() async throws -> (free: Int64, total: Int64)?
}

extension FileSource {
    var rootPath: String { "" }
    var isLocal: Bool { false }
    var canWrite: Bool { false }
    var accountAction: (title: String, run: (UIViewController) -> Void)? { nil }

    /// Folder by folder, for a source with no way of asking for everything at once.
    func listAll(path: String) async throws -> [FileEntry] {
        var files: [FileEntry] = []
        var folders = [path]
        while let folder = folders.popLast() {
            try Task.checkCancellation()
            for entry in try await list(path: folder) {
                if entry.isFolder { folders.append(entry.path) } else { files.append(entry) }
            }
        }
        return files
    }

    func path(of name: String, in folder: String) -> String {
        folder.hasSuffix("/") ? folder + name : folder + "/" + name
    }

    func createFolder(named name: String, in folder: String) async throws { throw cannot("make folders") }
    func upload(_ file: URL, named name: String, to folder: String) async throws { throw cannot("add files") }
    func rename(_ entry: FileEntry, to name: String) async throws { throw cannot("rename") }
    func delete(_ entry: FileEntry) async throws { throw cannot("delete") }
    func move(_ entry: FileEntry, to folder: String) async throws -> Bool { false }
    func storageSpace() async throws -> (free: Int64, total: Int64)? { nil }

    private func cannot(_ what: String) -> RemoteError {
        .failed("FastPlay cannot \(what) in \(title).")
    }

    /// The entry for a path just made here (a folder made, a file uploaded).
    func entry(named name: String, in folder: String, isFolder: Bool, size: Int64 = 0) -> FileEntry {
        let path = path(of: name, in: folder)
        return FileEntry(name: name, path: path, displayPath: path, isFolder: isFolder, size: size, modified: Date())
    }

    /// Deletes a folder's contents, deepest first, then the folder: for a source
    /// whose server deletes only empty folders.
    func deleteContents(of folder: FileEntry, file: (FileEntry) async throws -> Void,
                        emptyFolder: (FileEntry) async throws -> Void) async throws {
        for entry in try await list(path: folder.path) {
            try Task.checkCancellation()
            if entry.isFolder {
                try await deleteContents(of: entry, file: file, emptyFolder: emptyFolder)
            } else {
                try await file(entry)
            }
        }
        try await emptyFolder(folder)
    }
}

/// The sources there are, by id: for favorites and the clipboard, which keep only
/// the id.
enum FileSources {
    static func source(id: String) -> FileSource? {
        switch id {
        case LocalSource.shared.id: return LocalSource.shared
        case DropboxClient.shared.id: return DropboxClient.shared
        default:
            guard id.hasPrefix("server-") else { return nil }
            return ServerStore.all.first { "server-\($0.id.uuidString)" == id }?.makeSource()
        }
    }

    /// "Files", "Dropbox", a server's name, for a source that may be gone.
    static func title(id: String) -> String {
        source(id: id)?.title ?? "A server no longer added"
    }
}

enum RemoteError: LocalizedError {
    case failed(String)

    var errorDescription: String? {
        switch self {
        case let .failed(reason): return reason
        }
    }
}
