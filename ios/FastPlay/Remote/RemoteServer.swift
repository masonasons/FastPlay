import Foundation

/// An FTP, SFTP or SMB server the user has added.
struct RemoteServer: Codable, Equatable {
    enum Kind: String, Codable, CaseIterable {
        case ftp, sftp, smb

        var title: String {
            switch self {
            case .ftp: return "FTP"
            case .sftp: return "SFTP"
            case .smb: return "SMB"
            }
        }
    }

    var id = UUID()
    var kind: Kind = .ftp
    var name = ""
    var host = ""
    /// Nil: the protocol's usual port.
    var port: Int?
    /// Empty: anonymous (FTP) or guest (SMB). SFTP needs one.
    var user = ""
    /// SMB: the share to open. Empty: the server's shares are listed to choose from.
    var share = ""
    /// A folder to start in, below the top (FTP), the home folder (SFTP) or the share (SMB).
    var folder = ""

    var displayName: String { name.isEmpty ? host : name }

    /// The password, which is kept in the keychain rather than with the rest.
    var password: String {
        get { Keychain.read("server-\(id.uuidString)") ?? "" }
        nonmutating set {
            if newValue.isEmpty { Keychain.delete("server-\(id.uuidString)") } else { Keychain.write("server-\(id.uuidString)", newValue) }
        }
    }

    /// What browses it.
    func makeSource(password: String? = nil) -> FileSource {
        switch kind {
        case .ftp: return FTPSource(server: self, password: password)
        case .sftp: return SFTPSource(server: self, password: password)
        case .smb: return SMBSource(server: self, password: password)
        }
    }

    /// Where browsing starts, as its source writes paths.
    var startPath: String {
        let below = folder.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        switch kind {
        case .ftp:
            return below.isEmpty ? "" : "/" + below
        case .sftp:
            // Below the home folder, unless it starts at the top
            let trimmed = folder.hasSuffix("/") ? String(folder.dropLast()) : folder
            return trimmed.hasPrefix("/") ? trimmed : below
        case .smb:
            let top = share.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
            if top.isEmpty { return "" }
            return below.isEmpty ? "/" + top : "/" + top + "/" + below
        }
    }
}

/// The servers added, in the app's defaults (their passwords in the keychain).
enum ServerStore {
    private static let key = "RemoteServers"

    static var all: [RemoteServer] {
        get {
            guard let data = UserDefaults.standard.data(forKey: key) else { return [] }
            return (try? JSONDecoder().decode([RemoteServer].self, from: data)) ?? []
        }
        set {
            UserDefaults.standard.set(try? JSONEncoder().encode(newValue), forKey: key)
        }
    }

    static func save(_ server: RemoteServer, password: String) {
        var servers = all
        if let index = servers.firstIndex(where: { $0.id == server.id }) {
            // Somewhere else now: the key of the server that was there is no guide
            if servers[index].host != server.host || servers[index].port != server.port {
                SFTPSource.forgetHostKey(server)
            }
            servers[index] = server
        } else {
            servers.append(server)
        }
        all = servers
        server.password = password
    }

    static func remove(_ server: RemoteServer) {
        server.password = ""
        SFTPSource.forgetHostKey(server)
        FavoritesStore.removeAll(sourceID: "server-\(server.id.uuidString)")
        AutoSyncStore.removeAll(sourceID: "server-\(server.id.uuidString)")
        all = all.filter { $0.id != server.id }
    }
}
