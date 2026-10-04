import Foundation
import UIKit

/// A file or folder somewhere other than this device: in Dropbox, on an FTP or SMB
/// server.
struct RemoteEntry {
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

/// Somewhere files can be browsed, streamed and copied from. The browser
/// (RemoteBrowserViewController) is the same for all of them.
protocol RemoteFileSource: AnyObject {
    /// What is in a folder ("" is the top), in any order.
    func list(path: String) async throws -> [RemoteEntry]
    /// Every file in a folder and the folders below it.
    func listAll(path: String) async throws -> [RemoteEntry]
    /// An address the engine can play the file from.
    func streamURL(for entry: RemoteEntry) async throws -> String
    /// Copies the file to a place on this device.
    func download(_ entry: RemoteEntry, to destination: URL) async throws
    /// Something for the top folder's menu besides Download and Sync ("Sign Out").
    var accountAction: (title: String, run: (UIViewController) -> Void)? { get }
}

extension RemoteFileSource {
    var accountAction: (title: String, run: (UIViewController) -> Void)? { nil }

    /// Folder by folder, for a source with no way of asking for everything at once.
    func listAll(path: String) async throws -> [RemoteEntry] {
        var files: [RemoteEntry] = []
        var folders = [path]
        while let folder = folders.popLast() {
            try Task.checkCancellation()
            for entry in try await list(path: folder) {
                if entry.isFolder { folders.append(entry.path) } else { files.append(entry) }
            }
        }
        return files
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
