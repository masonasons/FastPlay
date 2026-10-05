import Foundation

/// An FTP server: listed, streamed and copied from through FFmpeg, which the
/// engine plays an ftp:// address with as it does an http:// one.
final class FTPSource: FileSource {
    private let server: RemoteServer
    private let password: String

    /// `password`, when given, is used rather than the one saved for the server.
    init(server: RemoteServer, password: String? = nil) {
        self.server = server
        self.password = password ?? server.password
    }

    var id: String { "server-\(server.id.uuidString)" }
    var title: String { server.displayName }
    var canWrite: Bool { true }

    // MARK: Changing things (FTPClient: FFmpeg only reads)

    /// `work` on a connection opened for it, and closed after.
    private func withClient<T>(_ work: (FTPClient) async throws -> T) async throws -> T {
        let client = FTPClient(host: server.host, port: server.port)
        defer { client.close() }
        try await client.open(user: server.user, password: password)
        return try await work(client)
    }

    func createFolder(named name: String, in folder: String) async throws {
        try await withClient { try await $0.makeFolder(path(of: name, in: folder)) }
    }

    func upload(_ file: URL, named name: String, to folder: String) async throws {
        try await withClient { try await $0.upload(file, to: path(of: name, in: folder)) }
    }

    func rename(_ entry: FileEntry, to name: String) async throws {
        let target = path(of: name, in: FileOperations.parent(of: entry.path))
        try await withClient { try await $0.rename(entry.path, to: target) }
    }

    func move(_ entry: FileEntry, to folder: String) async throws -> Bool {
        let target = path(of: entry.name, in: folder)
        try await withClient { try await $0.rename(entry.path, to: target) }
        return true
    }

    func delete(_ entry: FileEntry) async throws {
        try await withClient { client in
            if entry.isFolder {
                // A server deletes only empty folders: what is in it first
                try await deleteContents(of: entry, file: { try await client.deleteFile($0.path) },
                                         emptyFolder: { try await client.deleteEmptyFolder($0.path) })
            } else {
                try await client.deleteFile(entry.path)
            }
        }
    }

    /// The address of a path on the server, the user name and password in it.
    private func address(_ path: String) -> String {
        // Only the name and password are escaped: FFmpeg sends the path as it is.
        let safe = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "-._~"))
        var text = "ftp://"
        if !server.user.isEmpty {
            text += server.user.addingPercentEncoding(withAllowedCharacters: safe) ?? server.user
            if !password.isEmpty { text += ":" + (password.addingPercentEncoding(withAllowedCharacters: safe) ?? password) }
            text += "@"
        }
        text += server.host
        if let port = server.port { text += ":\(port)" }
        return text + (path.hasPrefix("/") ? path : "/" + path)
    }

    func list(path: String) async throws -> [FileEntry] {
        let folder = path.hasSuffix("/") ? path : path + "/"
        let url = address(folder)
        let items = try await Self.offMainThread { try FPRemote.listFolder(atURL: url) }
        return items.map { item in
            FileEntry(name: item.name, path: folder + item.name, displayPath: folder + item.name,
                        isFolder: item.isFolder, size: item.size, modified: item.modified)
        }
    }

    func streamURL(for entry: FileEntry) async throws -> String {
        address(entry.path)
    }

    func download(_ entry: FileEntry, to destination: URL) async throws {
        let url = address(entry.path)
        let stop = StopFlag()
        try await withTaskCancellationHandler {
            try await Self.offMainThread {
                try FPRemote.copy(url: url, toFile: destination.path) { stop.isSet }
            }
        } onCancel: {
            stop.isSet = true
        }
    }

    /// FFmpeg's calls block on the network.
    private static func offMainThread<T>(_ work: @escaping () throws -> T) async throws -> T {
        try await withCheckedThrowingContinuation { continuation in
            DispatchQueue.global(qos: .userInitiated).async {
                continuation.resume(with: Result { try work() })
            }
        }
    }
}

/// Set from one thread to ask another to give up.
final class StopFlag: @unchecked Sendable {
    private let lock = NSLock()
    private var value = false

    var isSet: Bool {
        get { lock.lock(); defer { lock.unlock() }; return value }
        set { lock.lock(); value = newValue; lock.unlock() }
    }
}
