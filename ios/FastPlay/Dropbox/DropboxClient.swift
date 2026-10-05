import AuthenticationServices
import CryptoKit
import Foundation
import UIKit

enum DropboxError: LocalizedError {
    case notSignedIn
    case cancelled
    case failed(String)

    var errorDescription: String? {
        switch self {
        case .notSignedIn: return "FastPlay is not connected to Dropbox."
        case .cancelled: return "Signing in was cancelled."
        case let .failed(reason): return reason
        }
    }
}

/// FastPlay's connection to Dropbox: signing in (OAuth with PKCE, which needs the
/// app's key and no secret), listing folders, and getting an address a file can be
/// streamed from. What lasts between runs, the refresh token, is in the keychain.
final class DropboxClient: NSObject, ASWebAuthenticationPresentationContextProviding, FileSource {
    static let shared = DropboxClient()

    let id = "dropbox"
    let title = "Dropbox"
    var canWrite: Bool { true }

    private let appKey = "4f6qjnm23npa6wk"
    private var redirectScheme: String { "db-\(appKey)" }
    private var redirectURI: String { "\(redirectScheme)://2/token" }

    private var accessToken: String?
    private var accessTokenExpires = Date.distantPast
    private var authSession: ASWebAuthenticationSession?
    private weak var presentingWindow: UIWindow?

    var isSignedIn: Bool { Keychain.read(Self.refreshTokenKey) != nil }

    // MARK: Signing in and out

    /// Opens Dropbox's sign in page; returns once FastPlay has been let in.
    @MainActor
    func signIn(from controller: UIViewController) async throws {
        let verifier = Self.randomVerifier()
        let challenge = Data(SHA256.hash(data: Data(verifier.utf8))).base64URLEncoded
        var components = URLComponents(string: "https://www.dropbox.com/oauth2/authorize")!
        components.queryItems = [
            URLQueryItem(name: "client_id", value: appKey),
            URLQueryItem(name: "response_type", value: "code"),
            URLQueryItem(name: "code_challenge", value: challenge),
            URLQueryItem(name: "code_challenge_method", value: "S256"),
            URLQueryItem(name: "token_access_type", value: "offline"),  // a refresh token, to stay connected
            URLQueryItem(name: "redirect_uri", value: redirectURI),
        ]
        presentingWindow = controller.view.window

        let callback: URL = try await withCheckedThrowingContinuation { continuation in
            let session = ASWebAuthenticationSession(url: components.url!, callbackURLScheme: redirectScheme) { url, error in
                if let url {
                    continuation.resume(returning: url)
                } else if let error = error as? ASWebAuthenticationSessionError, error.code == .canceledLogin {
                    continuation.resume(throwing: DropboxError.cancelled)
                } else {
                    continuation.resume(throwing: DropboxError.failed(error?.localizedDescription ?? "Signing in failed."))
                }
            }
            session.presentationContextProvider = self
            self.authSession = session
            if !session.start() {
                continuation.resume(throwing: DropboxError.failed("The sign in page could not be opened."))
            }
        }
        authSession = nil

        let items = URLComponents(url: callback, resolvingAgainstBaseURL: false)?.queryItems ?? []
        guard let code = items.first(where: { $0.name == "code" })?.value else {
            let reason = items.first(where: { $0.name == "error_description" })?.value
                ?? items.first(where: { $0.name == "error" })?.value ?? "Dropbox did not let FastPlay in."
            throw DropboxError.failed(reason)
        }
        let token = try await requestToken([
            "grant_type": "authorization_code",
            "code": code,
            "code_verifier": verifier,
            "client_id": appKey,
            "redirect_uri": redirectURI,
        ])
        guard let refresh = token["refresh_token"] as? String else {
            throw DropboxError.failed("Dropbox gave no lasting sign in.")
        }
        Keychain.write(Self.refreshTokenKey, refresh)
        store(token)
    }

    func signOut() {
        // Tell Dropbox the token is finished with, if it can be reached; forget it either way
        if let token = accessToken {
            var request = URLRequest(url: URL(string: "https://api.dropboxapi.com/2/auth/token/revoke")!)
            request.httpMethod = "POST"
            request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
            URLSession.shared.dataTask(with: request).resume()
        }
        Keychain.delete(Self.refreshTokenKey)
        accessToken = nil
        accessTokenExpires = .distantPast
    }

    func presentationAnchor(for session: ASWebAuthenticationSession) -> ASPresentationAnchor {
        presentingWindow ?? ASPresentationAnchor()
    }

    // MARK: As a place to browse (FileSource)

    func list(path: String) async throws -> [FileEntry] {
        try await list(path: path, recursive: false)
    }

    /// Dropbox lists a folder and all below it in one request.
    func listAll(path: String) async throws -> [FileEntry] {
        try await list(path: path, recursive: true).filter { !$0.isFolder }
    }

    func streamURL(for entry: FileEntry) async throws -> String {
        try await temporaryLink(path: entry.path)
    }

    func download(_ entry: FileEntry, to destination: URL) async throws {
        try await download(path: entry.path, to: destination)
    }

    var accountAction: (title: String, run: (UIViewController) -> Void)? {
        ("Sign Out of Dropbox", { [weak self] controller in
            let alert = UIAlertController(title: "Sign Out of Dropbox",
                                          message: "FastPlay will forget its connection to your Dropbox.",
                                          preferredStyle: .alert)
            alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
            alert.addAction(UIAlertAction(title: "Sign Out", style: .destructive) { _ in
                self?.signOut()
                controller.navigationController?.popToRootViewController(animated: true)
            })
            controller.present(alert, animated: true)
        })
    }

    // MARK: Changing things

    func createFolder(named name: String, in folder: String) async throws {
        _ = try await call("files/create_folder_v2", ["path": path(of: name, in: folder), "autorename": false])
    }

    func rename(_ entry: FileEntry, to name: String) async throws {
        _ = try await call("files/move_v2", ["from_path": entry.path,
                                             "to_path": path(of: name, in: FileOperations.parent(of: entry.path)),
                                             "autorename": false])
    }

    func move(_ entry: FileEntry, to folder: String) async throws -> Bool {
        _ = try await call("files/move_v2", ["from_path": entry.path, "to_path": path(of: entry.name, in: folder),
                                             "autorename": false])
        return true
    }

    func delete(_ entry: FileEntry) async throws {
        _ = try await call("files/delete_v2", ["path": entry.path])
    }

    /// Up to 150 MB in one request; beyond that, in pieces through an upload session.
    func upload(_ file: URL, named name: String, to folder: String) async throws {
        let target = path(of: name, in: folder)
        let size = (try FileManager.default.attributesOfItem(atPath: file.path)[.size] as? NSNumber)?.int64Value ?? 0
        let commit: [String: Any] = ["path": target, "mode": "overwrite", "autorename": false, "mute": true]
        let piece = 64 * 1024 * 1024
        if size <= 150 * 1024 * 1024 {
            _ = try await content("files/upload", commit, body: .file(file))
            return
        }
        let handle = try FileHandle(forReadingFrom: file)
        defer { try? handle.close() }
        var data = try handle.read(upToCount: piece) ?? Data()
        let started = try await content("files/upload_session/start", ["close": false], body: .data(data))
        guard let session = started["session_id"] as? String else {
            throw DropboxError.failed("Dropbox did not start the upload of \(name).")
        }
        var offset = Int64(data.count)
        while true {
            try Task.checkCancellation()
            data = try handle.read(upToCount: piece) ?? Data()
            let cursor: [String: Any] = ["session_id": session, "offset": offset]
            if data.isEmpty || offset + Int64(data.count) >= size {
                _ = try await content("files/upload_session/finish", ["cursor": cursor, "commit": commit],
                                      body: .data(data))
                return
            }
            _ = try await content("files/upload_session/append_v2", ["cursor": cursor, "close": false], body: .data(data))
            offset += Int64(data.count)
        }
    }

    func storageSpace() async throws -> (free: Int64, total: Int64)? {
        let usage = try await call("users/get_space_usage", nil)
        guard let used = (usage["used"] as? NSNumber)?.int64Value,
              let allocation = usage["allocation"] as? [String: Any],
              let allocated = (allocation["allocated"] as? NSNumber)?.int64Value, allocated > 0 else { return nil }
        return (max(0, allocated - used), allocated)
    }

    private enum Body {
        case file(URL)
        case data(Data)
    }

    /// A request to Dropbox's content endpoint: the arguments in a header, the bytes as the body.
    private func content(_ endpoint: String, _ arguments: [String: Any], body: Body,
                         retried: Bool = false) async throws -> [String: Any] {
        let token = try await validAccessToken()
        var request = URLRequest(url: URL(string: "https://content.dropboxapi.com/2/\(endpoint)")!)
        request.httpMethod = "POST"
        request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        request.setValue("application/octet-stream", forHTTPHeaderField: "Content-Type")
        request.setValue(Self.headerJSON(arguments), forHTTPHeaderField: "Dropbox-API-Arg")
        let (data, response): (Data, URLResponse)
        switch body {
        case let .file(url): (data, response) = try await URLSession.shared.upload(for: request, fromFile: url)
        case let .data(bytes): (data, response) = try await URLSession.shared.upload(for: request, from: bytes)
        }
        let status = (response as? HTTPURLResponse)?.statusCode ?? 0
        if status == 401, !retried {
            accessToken = nil
            return try await content(endpoint, arguments, body: body, retried: true)
        }
        return try Self.result(status, data)
    }

    // MARK: Files

    /// What is in a folder, or with `recursive` in it and every folder below it.
    func list(path: String, recursive: Bool) async throws -> [FileEntry] {
        var entries: [FileEntry] = []
        var page = try await call("files/list_folder", ["path": path, "limit": 2000, "recursive": recursive])
        while true {
            for item in page["entries"] as? [[String: Any]] ?? [] {
                guard let name = item["name"] as? String, let tag = item[".tag"] as? String,
                      let itemPath = (item["path_lower"] as? String) ?? (item["path_display"] as? String) else { continue }
                entries.append(FileEntry(name: name, path: itemPath,
                                            displayPath: item["path_display"] as? String ?? itemPath,
                                            isFolder: tag == "folder",
                                            size: (item["size"] as? NSNumber)?.int64Value ?? 0,
                                            modified: (item["server_modified"] as? String)
                                                .flatMap { ISO8601DateFormatter().date(from: $0) }))
            }
            guard page["has_more"] as? Bool == true, let cursor = page["cursor"] as? String else { break }
            page = try await call("files/list_folder/continue", ["cursor": cursor])
        }
        return entries
    }

    /// An address the file can be streamed from, good for four hours.
    func temporaryLink(path: String) async throws -> String {
        let result = try await call("files/get_temporary_link", ["path": path])
        guard let link = result["link"] as? String else { throw DropboxError.failed("Dropbox gave no address for the file.") }
        return link
    }

    /// Downloads a file to a place on this device (replacing what is there).
    func download(path: String, to destination: URL) async throws {
        let token = try await validAccessToken()
        var request = URLRequest(url: URL(string: "https://content.dropboxapi.com/2/files/download")!)
        request.httpMethod = "POST"
        request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        request.setValue(Self.headerJSON(["path": path]), forHTTPHeaderField: "Dropbox-API-Arg")
        let (temporary, response) = try await URLSession.shared.download(for: request)
        let status = (response as? HTTPURLResponse)?.statusCode ?? 0
        guard status == 200 else {
            try? FileManager.default.removeItem(at: temporary)
            throw DropboxError.failed("Dropbox answered \(status) for \((path as NSString).lastPathComponent).")
        }
        let manager = FileManager.default
        try manager.createDirectory(at: destination.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? manager.removeItem(at: destination)
        try manager.moveItem(at: temporary, to: destination)
    }

    /// JSON for a request header: letters outside ASCII as \u escapes, as headers need.
    private static func headerJSON(_ object: [String: Any]) -> String {
        let data = (try? JSONSerialization.data(withJSONObject: object)) ?? Data()
        var text = ""
        for unit in (String(data: data, encoding: .utf8) ?? "{}").utf16 {
            if unit < 0x80, let scalar = Unicode.Scalar(unit) {
                text.unicodeScalars.append(scalar)
            } else {
                text += String(format: "\\u%04x", unit)
            }
        }
        return text
    }

    // MARK: Requests

    private static let refreshTokenKey = "dropbox-refresh-token"

    /// An API call. Nil arguments: an endpoint that takes none.
    private func call(_ endpoint: String, _ arguments: [String: Any]?, retried: Bool = false) async throws -> [String: Any] {
        let token = try await validAccessToken()
        var request = URLRequest(url: URL(string: "https://api.dropboxapi.com/2/\(endpoint)")!)
        request.httpMethod = "POST"
        request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try arguments.map { try JSONSerialization.data(withJSONObject: $0) } ?? Data("null".utf8)
        let (data, response) = try await URLSession.shared.data(for: request)
        let status = (response as? HTTPURLResponse)?.statusCode ?? 0
        if status == 401, !retried {
            accessToken = nil  // it ran out early: a new one, once
            return try await call(endpoint, arguments, retried: true)
        }
        return try Self.result(status, data)
    }

    /// A response's JSON, or what went wrong said plainly where it is a common case.
    private static func result(_ status: Int, _ data: Data) throws -> [String: Any] {
        let object = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] ?? [:]
        guard status == 200 else {
            let summary = object["error_summary"] as? String ?? String(data: data, encoding: .utf8) ?? ""
            if summary.contains("missing_scope") {
                throw DropboxError.failed("FastPlay's connection to Dropbox is not allowed to do this. Sign out of "
                                          + "Dropbox (in the Dropbox folder's menu) and sign in again to allow it.")
            }
            if summary.contains("insufficient_space") { throw DropboxError.failed("Your Dropbox is full.") }
            if summary.contains("conflict") { throw DropboxError.failed("Something of that name is already there.") }
            if summary.contains("not_found") { throw DropboxError.failed("It is no longer in your Dropbox.") }
            throw DropboxError.failed("Dropbox answered \(status). \(summary)")
        }
        return object
    }

    private func validAccessToken() async throws -> String {
        if let accessToken, accessTokenExpires > Date().addingTimeInterval(60) { return accessToken }
        guard let refresh = Keychain.read(Self.refreshTokenKey) else { throw DropboxError.notSignedIn }
        let token = try await requestToken([
            "grant_type": "refresh_token",
            "refresh_token": refresh,
            "client_id": appKey,
        ])
        store(token)
        guard let accessToken else { throw DropboxError.failed("Dropbox gave no access.") }
        return accessToken
    }

    private func store(_ token: [String: Any]) {
        accessToken = token["access_token"] as? String
        let lifetime = (token["expires_in"] as? Double) ?? 3600
        accessTokenExpires = Date().addingTimeInterval(lifetime)
    }

    private func requestToken(_ fields: [String: String]) async throws -> [String: Any] {
        var request = URLRequest(url: URL(string: "https://api.dropboxapi.com/oauth2/token")!)
        request.httpMethod = "POST"
        request.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
        var form = URLComponents()
        form.queryItems = fields.map { URLQueryItem(name: $0.key, value: $0.value) }
        request.httpBody = form.percentEncodedQuery?.replacingOccurrences(of: "+", with: "%2B").data(using: .utf8)
        let (data, response) = try await URLSession.shared.data(for: request)
        let object = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] ?? [:]
        guard (response as? HTTPURLResponse)?.statusCode == 200 else {
            if object["error"] as? String == "invalid_grant" {
                // The connection was ended from Dropbox's side: signed out here too
                Keychain.delete(Self.refreshTokenKey)
                throw DropboxError.notSignedIn
            }
            throw DropboxError.failed(object["error_description"] as? String ?? "Dropbox refused the sign in.")
        }
        return object
    }

    private static func randomVerifier() -> String {
        var bytes = [UInt8](repeating: 0, count: 48)
        _ = SecRandomCopyBytes(kSecRandomDefault, bytes.count, &bytes)
        return Data(bytes).base64URLEncoded
    }
}

private extension Data {
    var base64URLEncoded: String {
        base64EncodedString().replacingOccurrences(of: "+", with: "-").replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
    }
}

/// The few keychain calls FastPlay needs: one secret under one name.
enum Keychain {
    private static let service = "me.masonasons.fastplay"

    private static func query(_ name: String) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service, kSecAttrAccount as String: name]
    }

    static func read(_ name: String) -> String? {
        var query = query(name)
        query[kSecReturnData as String] = true
        var result: AnyObject?
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess, let data = result as? Data else {
            return nil
        }
        return String(data: data, encoding: .utf8)
    }

    static func write(_ name: String, _ value: String) {
        delete(name)
        var query = query(name)
        query[kSecValueData as String] = Data(value.utf8)
        query[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlock  // playing with the screen locked
        SecItemAdd(query as CFDictionary, nil)
    }

    static func delete(_ name: String) {
        SecItemDelete(query(name) as CFDictionary)
    }
}
