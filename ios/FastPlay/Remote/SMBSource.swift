import AMSMB2
import Foundation

/// An SMB server (a Windows share, a NAS, a Mac's file sharing).
///
/// A path here is "/Share/folder/file": its first part is the share. The top, "",
/// lists the server's shares. Files are streamed through LoopbackServer, since the
/// engine plays addresses and SMB is not one FFmpeg knows.
final class SMBSource: RemoteFileSource {
    private let server: RemoteServer
    private let password: String
    private var shares: [String: SMB2Manager] = [:]

    /// `password`, when given, is used rather than the one saved for the server.
    init(server: RemoteServer, password: String? = nil) {
        self.server = server
        self.password = password ?? server.password
    }

    private func makeManager() throws -> SMB2Manager {
        var components = URLComponents()
        components.scheme = "smb"
        components.host = server.host
        components.port = server.port
        // No name: the server's guest access, where it has one
        let credential = server.user.isEmpty ? nil
            : URLCredential(user: server.user, password: password, persistence: .forSession)
        guard let url = components.url, let manager = SMB2Manager(url: url, credential: credential) else {
            throw RemoteError.failed("The server's address is not valid.")
        }
        return manager
    }

    /// The connection to a share, made the first time it is needed.
    private func manager(forShare share: String) async throws -> SMB2Manager {
        if let manager = shares[share] { return manager }
        let manager = try makeManager()
        try await manager.connectShare(name: share)
        shares[share] = manager
        return manager
    }

    /// "/Share/a/b" as the share and the path within it.
    private func split(_ path: String) -> (share: String, path: String) {
        let parts = path.split(separator: "/", omittingEmptySubsequences: true)
        guard let share = parts.first else { return ("", "") }
        return (String(share), parts.dropFirst().joined(separator: "/"))
    }

    func list(path: String) async throws -> [RemoteEntry] {
        let (share, below) = split(path)
        if share.isEmpty {
            // The shares, as folders (not the hidden administrative ones)
            return try await makeManager().listShares().map { item in
                RemoteEntry(name: item.name, path: "/" + item.name, displayPath: "/" + item.name, isFolder: true)
            }
        }
        let manager = try await manager(forShare: share)
        let folder = "/" + share + (below.isEmpty ? "" : "/" + below)
        return try await manager.contentsOfDirectory(atPath: below.isEmpty ? "/" : below).compactMap { item in
            guard let name = item[.nameKey] as? String, name != ".", name != ".." else { return nil }
            return RemoteEntry(name: name, path: folder + "/" + name, displayPath: folder + "/" + name,
                               isFolder: item[.isDirectoryKey] as? Bool ?? false,
                               size: (item[.fileSizeKey] as? NSNumber)?.int64Value ?? 0,
                               modified: item[.contentModificationDateKey] as? Date)
        }
    }

    func streamURL(for entry: RemoteEntry) async throws -> String {
        let (share, below) = split(entry.path)
        let manager = try await manager(forShare: share)
        var size = entry.size
        if size <= 0 {
            size = (try await manager.attributesOfItem(atPath: below)[.fileSizeKey] as? NSNumber)?.int64Value ?? 0
        }
        return try await LoopbackServer.shared.address(name: entry.name, size: size) { range in
            try await manager.contents(atPath: below, range: UInt64(range.lowerBound)..<UInt64(range.upperBound))
        }
    }

    func download(_ entry: RemoteEntry, to destination: URL) async throws {
        let (share, below) = split(entry.path)
        let manager = try await manager(forShare: share)
        try? FileManager.default.removeItem(at: destination)
        try await manager.downloadItem(atPath: below, to: destination) { _, _ in !Task.isCancelled }
    }
}
