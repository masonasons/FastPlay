import Foundation

/// An FTP server: listed, streamed and copied from through FFmpeg, which the
/// engine plays an ftp:// address with as it does an http:// one.
final class FTPSource: RemoteFileSource {
    private let server: RemoteServer
    private let password: String

    /// `password`, when given, is used rather than the one saved for the server.
    init(server: RemoteServer, password: String? = nil) {
        self.server = server
        self.password = password ?? server.password
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

    func list(path: String) async throws -> [RemoteEntry] {
        let folder = path.hasSuffix("/") ? path : path + "/"
        let url = address(folder)
        let items = try await Self.offMainThread { try FPRemote.listFolder(atURL: url) }
        return items.map { item in
            RemoteEntry(name: item.name, path: folder + item.name, displayPath: folder + item.name,
                        isFolder: item.isFolder, size: item.size, modified: item.modified)
        }
    }

    func streamURL(for entry: RemoteEntry) async throws -> String {
        address(entry.path)
    }

    func download(_ entry: RemoteEntry, to destination: URL) async throws {
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
