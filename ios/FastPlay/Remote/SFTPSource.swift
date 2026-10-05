import Citadel
import Foundation
import NIOCore
import NIOSSH

/// An SFTP server (files over SSH): through Citadel. Files are streamed through
/// LoopbackServer, as SMB's are, since FFmpeg does not speak SFTP.
///
/// The server's key is remembered the first time FastPlay connects, and a server
/// that later shows another one is refused, as ssh does: it may not be the same
/// server. Deleting the server forgets its key.
final class SFTPSource: FileSource {
    private let server: RemoteServer
    private let password: String
    private var client: SSHClient?
    private var sftp: SFTPClient?
    /// The folder the server puts FastPlay in, for the top ("").
    private var home: String?

    /// `password`, when given, is used rather than the one saved for the server.
    init(server: RemoteServer, password: String? = nil) {
        self.server = server
        self.password = password ?? server.password
    }

    var id: String { "server-\(server.id.uuidString)" }
    var title: String { server.displayName }
    var canWrite: Bool { true }

    private func connection() async throws -> SFTPClient {
        if let sftp, sftp.isActive { return sftp }
        do {
            let client = try await SSHClient.connect(
                host: server.host, port: server.port ?? 22,
                authenticationMethod: .passwordBased(username: server.user, password: password),
                hostKeyValidator: .custom(KnownHost(server: server)), reconnect: .never)
            let sftp = try await client.openSFTP()
            self.client = client
            self.sftp = sftp
            return sftp
        } catch let error as RemoteError {
            throw error
        } catch {
            throw RemoteError.failed("Could not connect to \(server.displayName). \(error.localizedDescription)")
        }
    }

    deinit {
        let client = self.client
        Task { try? await client?.close() }
    }

    /// The path FastPlay asks for: the top is the server's home folder.
    private func resolved(_ path: String) async throws -> String {
        if !path.isEmpty { return path }
        if let home { return home }
        let found = try await connection().getRealPath(atPath: ".")
        home = found
        return found
    }

    func path(of name: String, in folder: String) -> String {
        let base = folder.isEmpty ? (home ?? ".") : folder
        return base.hasSuffix("/") ? base + name : base + "/" + name
    }

    // MARK: Reading

    func list(path: String) async throws -> [FileEntry] {
        let folder = try await resolved(path)
        let names = try await connection().listDirectory(atPath: folder)
        return names.flatMap(\.components).compactMap { item -> FileEntry? in
            guard item.filename != ".", item.filename != ".." else { return nil }
            let attributes = item.attributes
            // The kind is in the permissions; failing those, ls's long form
            let isFolder = attributes.permissions.map { $0 & 0o170000 == 0o040000 } ?? item.longname.hasPrefix("d")
            let full = self.path(of: item.filename, in: folder)
            return FileEntry(name: item.filename, path: full, displayPath: full, isFolder: isFolder,
                             size: Int64(attributes.size ?? 0),
                             modified: attributes.accessModificationTime?.modificationTime)
        }
    }

    func streamURL(for entry: FileEntry) async throws -> String {
        let sftp = try await connection()
        var size = entry.size
        if size <= 0 { size = Int64(try await sftp.getAttributes(at: entry.path).size ?? 0) }
        let opened = OpenFile()
        return try await LoopbackServer.shared.address(name: entry.name, size: size) { range in
            // The file opened once, for all the ranges asked of it
            let file = try await opened.get(sftp, entry.path)
            var data = Data()
            var offset = UInt64(range.lowerBound)
            let end = UInt64(range.upperBound)
            while offset < end {
                let piece = try await file.read(from: offset, length: UInt32(min(end - offset, 256 * 1024)))
                if piece.readableBytes == 0 { break }
                data.append(contentsOf: piece.readableBytesView)
                offset += UInt64(piece.readableBytes)
            }
            return data
        }
    }

    func download(_ entry: FileEntry, to destination: URL) async throws {
        let manager = FileManager.default
        try? manager.removeItem(at: destination)
        manager.createFile(atPath: destination.path, contents: nil)
        let output = try FileHandle(forWritingTo: destination)
        defer { try? output.close() }
        do {
            let file = try await connection().openFile(filePath: entry.path, flags: .read)
            defer { Task { try? await file.close() } }
            var offset: UInt64 = 0
            while true {
                try Task.checkCancellation()
                let piece = try await file.read(from: offset, length: 256 * 1024)
                if piece.readableBytes == 0 { break }
                try output.write(contentsOf: Data(piece.readableBytesView))
                offset += UInt64(piece.readableBytes)
            }
        } catch {
            try? manager.removeItem(at: destination)
            throw error
        }
    }

    // MARK: Changing things

    func createFolder(named name: String, in folder: String) async throws {
        let parent = try await resolved(folder)
        try await connection().createDirectory(atPath: path(of: name, in: parent))
    }

    func upload(_ file: URL, named name: String, to folder: String) async throws {
        let parent = try await resolved(folder)
        let input = try FileHandle(forReadingFrom: file)
        defer { try? input.close() }
        let remote = try await connection().openFile(filePath: path(of: name, in: parent),
                                                     flags: [.write, .create, .truncate])
        do {
            var offset: UInt64 = 0
            while true {
                try Task.checkCancellation()
                guard let chunk = try input.read(upToCount: 256 * 1024), !chunk.isEmpty else { break }
                try await remote.write(ByteBuffer(bytes: chunk), at: offset)
                offset += UInt64(chunk.count)
            }
            try await remote.close()
        } catch {
            try? await remote.close()
            throw error
        }
    }

    func rename(_ entry: FileEntry, to name: String) async throws {
        try await connection().rename(at: entry.path, to: path(of: name, in: FileOperations.parent(of: entry.path)))
    }

    func move(_ entry: FileEntry, to folder: String) async throws -> Bool {
        let parent = try await resolved(folder)
        try await connection().rename(at: entry.path, to: path(of: entry.name, in: parent))
        return true
    }

    func delete(_ entry: FileEntry) async throws {
        let sftp = try await connection()
        if entry.isFolder {
            // A server deletes only empty folders: what is in it first
            try await deleteContents(of: entry, file: { try await sftp.remove(at: $0.path) },
                                     emptyFolder: { try await sftp.rmdir(at: $0.path) })
        } else {
            try await sftp.remove(at: entry.path)
        }
    }

    // MARK: Pieces

    /// A remote file opened on first use and kept open (while it is being played).
    private final class OpenFile: @unchecked Sendable {
        private var opened: SFTPFile?

        func get(_ sftp: SFTPClient, _ path: String) async throws -> SFTPFile {
            if let opened { return opened }
            let file = try await sftp.openFile(filePath: path, flags: .read)
            opened = file
            return file
        }
    }

    /// Trust on first use: the server's key is kept in the defaults, by server.
    private final class KnownHost: NIOSSHClientServerAuthenticationDelegate, @unchecked Sendable {
        private let key: String
        private let name: String

        init(server: RemoteServer) {
            key = SFTPSource.hostKeyName(server)
            name = server.displayName
        }

        func validateHostKey(hostKey: NIOSSHPublicKey, validationCompletePromise: EventLoopPromise<Void>) {
            let presented = String(openSSHPublicKey: hostKey)
            let defaults = UserDefaults.standard
            if let known = defaults.string(forKey: key) {
                if known == presented {
                    validationCompletePromise.succeed(())
                } else {
                    validationCompletePromise.fail(RemoteError.failed(
                        "\(name) identified itself with a different key than before, so FastPlay did not connect: "
                        + "it may not be the same server. If the server was set up again, delete it from Servers and add it again."))
                }
            } else {
                defaults.set(presented, forKey: key)
                validationCompletePromise.succeed(())
            }
        }
    }

    static func hostKeyName(_ server: RemoteServer) -> String { "sftp-host-key-\(server.id.uuidString)" }

    /// Forgets a server's key (it was deleted, or now is somewhere else).
    static func forgetHostKey(_ server: RemoteServer) {
        UserDefaults.standard.removeObject(forKey: hostKeyName(server))
    }
}
