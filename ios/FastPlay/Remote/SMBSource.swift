import AMSMB2
import Foundation

/// An SMB server (a Windows share, a NAS, a Mac's file sharing).
///
/// A path here is "/Share/folder/file": its first part is the share. The top, "",
/// lists the server's shares. Files are streamed through LoopbackServer, since the
/// engine plays addresses and SMB is not one FFmpeg knows.
final class SMBSource: FileSource {
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

    func list(path: String) async throws -> [FileEntry] {
        let (share, below) = split(path)
        if share.isEmpty {
            // The shares, as folders (not the hidden administrative ones)
            return try await makeManager().listShares().map { item in
                FileEntry(name: item.name, path: "/" + item.name, displayPath: "/" + item.name, isFolder: true)
            }
        }
        let manager = try await manager(forShare: share)
        let folder = "/" + share + (below.isEmpty ? "" : "/" + below)
        return try await manager.contentsOfDirectory(atPath: below.isEmpty ? "/" : below).compactMap { item in
            guard let name = item[.nameKey] as? String, name != ".", name != ".." else { return nil }
            return FileEntry(name: name, path: folder + "/" + name, displayPath: folder + "/" + name,
                               isFolder: item[.isDirectoryKey] as? Bool ?? false,
                               size: (item[.fileSizeKey] as? NSNumber)?.int64Value ?? 0,
                               modified: item[.contentModificationDateKey] as? Date)
        }
    }

    func streamURL(for entry: FileEntry) async throws -> String {
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

    var id: String { "server-\(server.id.uuidString)" }
    var title: String { server.displayName }
    var canWrite: Bool { true }

    /// The share's connection and the path within it, for something in a share
    /// (the top, the list of shares, holds none).
    private func place(_ path: String) async throws -> (SMB2Manager, String) {
        let (share, below) = split(path)
        guard !share.isEmpty else { throw RemoteError.failed("Open a share first.") }
        return (try await manager(forShare: share), below)
    }

    func createFolder(named name: String, in folder: String) async throws {
        let (manager, below) = try await place(path(of: name, in: folder))
        try await manager.createDirectory(atPath: below)
    }

    func upload(_ file: URL, named name: String, to folder: String) async throws {
        let (manager, below) = try await place(path(of: name, in: folder))
        // It writes only a new file: one there goes first
        if (try? await manager.attributesOfItem(atPath: below)) != nil { try await manager.removeItem(atPath: below) }
        try await manager.uploadItem(at: file, toPath: below) { _ in !Task.isCancelled }
    }

    func rename(_ entry: FileEntry, to name: String) async throws {
        let (manager, below) = try await place(entry.path)
        let (_, target) = try await place(path(of: name, in: FileOperations.parent(of: entry.path)))
        try await manager.moveItem(atPath: below, toPath: target)
    }

    func move(_ entry: FileEntry, to folder: String) async throws -> Bool {
        // Within one share only
        guard split(entry.path).share == split(folder).share else { return false }
        let (manager, below) = try await place(entry.path)
        let (_, target) = try await place(path(of: entry.name, in: folder))
        try await manager.moveItem(atPath: below, toPath: target)
        return true
    }

    func delete(_ entry: FileEntry) async throws {
        let (manager, below) = try await place(entry.path)
        try await manager.removeItem(atPath: below)
    }

    func storageSpace() async throws -> (free: Int64, total: Int64)? {
        let share = split(server.startPath).share
        guard !share.isEmpty else { return nil }
        let attributes = try await manager(forShare: share).attributesOfFileSystem(forPath: "/")
        guard let free = (attributes[.systemFreeSize] as? NSNumber)?.int64Value,
              let total = (attributes[.systemSize] as? NSNumber)?.int64Value else { return nil }
        return (free, total)
    }

    func download(_ entry: FileEntry, to destination: URL) async throws {
        let (share, below) = split(entry.path)
        let manager = try await manager(forShare: share)
        try? FileManager.default.removeItem(at: destination)
        try await manager.downloadItem(atPath: below, to: destination) { _, _ in !Task.isCancelled }
    }
}
