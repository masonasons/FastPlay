import Foundation
import Network

/// A plain FTP control connection, for what FFmpeg cannot do on a server: make
/// folders, rename, delete and upload. (Listing, playing and downloading stay
/// with FFmpeg, in FTPSource.) Opened for a piece of work and closed after it.
final class FTPClient {
    private let host: String
    private let port: UInt16
    private let queue = DispatchQueue(label: "FastPlay.FTPClient")
    private var control: NWConnection?
    private var received = Data()

    struct Reply {
        let code: Int
        let text: String
    }

    init(host: String, port: Int?) {
        self.host = host
        self.port = UInt16(port ?? 21)
    }

    deinit { control?.cancel() }

    // MARK: Signing in

    func open(user: String, password: String) async throws {
        let connection = NWConnection(host: NWEndpoint.Host(host), port: NWEndpoint.Port(rawValue: port)!, using: .tcp)
        control = connection
        try await Self.start(connection, on: queue)
        try check(try await reply(), 200..<300, "The server did not greet FastPlay.")
        let name = user.isEmpty ? "anonymous" : user
        var answer = try await command("USER \(name)")
        if answer.code == 331 {
            answer = try await command("PASS \(user.isEmpty ? "fastplay@" : password)")
        }
        try check(answer, 200..<300, "The server did not accept the user name and password.")
        try check(try await command("TYPE I"), 200..<300, "The server would not send files as they are.")
    }

    func close() {
        if let control {
            control.send(content: Data("QUIT\r\n".utf8), completion: .contentProcessed { _ in control.cancel() })
        }
        control = nil
    }

    // MARK: Commands

    func makeFolder(_ path: String) async throws {
        try check(try await command("MKD \(path)"), 200..<300, "The folder could not be made.")
    }

    func rename(_ path: String, to newPath: String) async throws {
        try check(try await command("RNFR \(path)"), 300..<400, "It is not there to rename.")
        try check(try await command("RNTO \(newPath)"), 200..<300, "It could not be renamed.")
    }

    func deleteFile(_ path: String) async throws {
        try check(try await command("DELE \(path)"), 200..<300, "It could not be deleted.")
    }

    func deleteEmptyFolder(_ path: String) async throws {
        try check(try await command("RMD \(path)"), 200..<300, "The folder could not be deleted.")
    }

    /// Sends a file on this device to `path`, replacing one there.
    func upload(_ file: URL, to path: String) async throws {
        let data = try await passiveConnection()
        defer { data.cancel() }
        let started = try await command("STOR \(path)")
        try check(started, 100..<200, "The server would not take the file.")
        let handle = try FileHandle(forReadingFrom: file)
        defer { try? handle.close() }
        while true {
            try Task.checkCancellation()
            guard let chunk = try handle.read(upToCount: 64 * 1024), !chunk.isEmpty else { break }
            try await Self.send(chunk, on: data)
        }
        // The end of the file is the end of the connection
        try await Self.send(nil, on: data, final: true)
        try check(try await reply(), 200..<300, "The file did not arrive whole.")
    }

    /// The server's data connection for one transfer: EPSV, else PASV. Either way
    /// to the address the control connection is to, which is the one known to
    /// work (a server behind a router gives its inside address in PASV).
    private func passiveConnection() async throws -> NWConnection {
        var dataPort: UInt16?
        let extended = try await command("EPSV")
        if extended.code == 229, let open = extended.text.firstIndex(of: "("), let shut = extended.text.lastIndex(of: ")") {
            // "(|||6446|)"
            let inside = extended.text[extended.text.index(after: open)..<shut]
            dataPort = UInt16(inside.split(separator: "|").last ?? "")
        }
        if dataPort == nil {
            let passive = try await command("PASV")
            try check(passive, 227..<228, "The server would not open a connection for the file.")
            // "(h1,h2,h3,h4,p1,p2)"
            if let open = passive.text.firstIndex(of: "("), let shut = passive.text.lastIndex(of: ")") {
                let numbers = passive.text[passive.text.index(after: open)..<shut].split(separator: ",").compactMap {
                    UInt16($0.trimmingCharacters(in: .whitespaces))
                }
                if numbers.count == 6 { dataPort = numbers[4] * 256 + numbers[5] }
            }
        }
        guard let dataPort, let port = NWEndpoint.Port(rawValue: dataPort) else {
            throw RemoteError.failed("The server did not say where to send the file.")
        }
        let connection = NWConnection(host: NWEndpoint.Host(host), port: port, using: .tcp)
        try await Self.start(connection, on: queue)
        return connection
    }

    // MARK: The conversation

    private func check(_ reply: Reply, _ wanted: Range<Int>, _ problem: String) throws {
        guard wanted.contains(reply.code) else {
            throw RemoteError.failed("\(problem) (\(reply.code) \(reply.text))")
        }
    }

    private func command(_ line: String) async throws -> Reply {
        guard let control else { throw RemoteError.failed("Not connected to the server.") }
        try await Self.send(Data((line + "\r\n").utf8), on: control)
        return try await reply()
    }

    /// The next reply, all its lines ("250-..." up to "250 ...").
    private func reply() async throws -> Reply {
        var code: Int?
        var lines: [String] = []
        while true {
            let line = try await readLine()
            let digits = Int(line.prefix(3))
            let separator = line.count > 3 ? line[line.index(line.startIndex, offsetBy: 3)] : " "
            if code == nil {
                guard let digits else { continue }
                code = digits
            }
            lines.append(line.count > 4 ? String(line.dropFirst(4)) : "")
            if digits == code, separator == " " { break }
        }
        return Reply(code: code ?? 0, text: lines.last ?? "")
    }

    private func readLine() async throws -> String {
        while true {
            if let end = received.range(of: Data("\n".utf8)) {
                let lineData = received[received.startIndex..<end.lowerBound]
                received.removeSubrange(received.startIndex..<end.upperBound)
                let line = String(decoding: lineData, as: UTF8.self)
                return line.hasSuffix("\r") ? String(line.dropLast()) : line
            }
            guard let control else { throw RemoteError.failed("Not connected to the server.") }
            let more = try await Self.receive(on: control)
            if more.isEmpty { throw RemoteError.failed("The server closed the connection.") }
            received.append(more)
        }
    }

    // MARK: Network calls, awaited

    private static func start(_ connection: NWConnection, on queue: DispatchQueue) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            var resumed = false
            connection.stateUpdateHandler = { state in
                guard !resumed else { return }
                switch state {
                case .ready:
                    resumed = true
                    continuation.resume()
                case let .failed(error), let .waiting(error):
                    resumed = true
                    connection.cancel()
                    continuation.resume(throwing: RemoteError.failed("Could not reach the server. \(error.localizedDescription)"))
                case .cancelled:
                    resumed = true
                    continuation.resume(throwing: RemoteError.failed("Could not reach the server."))
                default:
                    break
                }
            }
            connection.start(queue: queue)
        }
    }

    private static func send(_ data: Data?, on connection: NWConnection, final: Bool = false) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            connection.send(content: data, contentContext: final ? .finalMessage : .defaultMessage, isComplete: final,
                            completion: .contentProcessed { error in
                                if let error { continuation.resume(throwing: error) } else { continuation.resume() }
                            })
        }
    }

    private static func receive(on connection: NWConnection) async throws -> Data {
        try await withCheckedThrowingContinuation { continuation in
            connection.receive(minimumIncompleteLength: 1, maximumLength: 65536) { data, _, _, error in
                if let error {
                    continuation.resume(throwing: error)
                } else {
                    continuation.resume(returning: data ?? Data())
                }
            }
        }
    }
}
