import Foundation

/// An FTP or SMB server the user has added.
struct RemoteServer: Codable, Equatable {
    enum Kind: String, Codable, CaseIterable {
        case ftp, smb

        var title: String { self == .ftp ? "FTP" : "SMB" }
    }

    var id = UUID()
    var kind: Kind = .ftp
    var name = ""
    var host = ""
    /// Nil: the protocol's usual port.
    var port: Int?
    /// Empty: anonymous (FTP) or guest (SMB).
    var user = ""
    /// SMB: the share to open. Empty: the server's shares are listed to choose from.
    var share = ""
    /// A folder to start in, below the top (FTP) or the share (SMB).
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
    func makeSource(password: String? = nil) -> RemoteFileSource {
        kind == .ftp ? FTPSource(server: self, password: password) : SMBSource(server: self, password: password)
    }

    /// Where browsing starts, as its source writes paths.
    var startPath: String {
        let below = folder.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        switch kind {
        case .ftp:
            return below.isEmpty ? "" : "/" + below
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
            servers[index] = server
        } else {
            servers.append(server)
        }
        all = servers
        server.password = password
    }

    static func remove(_ server: RemoteServer) {
        server.password = ""
        all = all.filter { $0.id != server.id }
    }
}
