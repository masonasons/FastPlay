import CryptoKit
import Foundation
import Network

/// A tiny HTTP server on this device's loopback address, serving files that only
/// the app can read (an SMB share's) to the engine, which plays addresses. It
/// answers GET with or without a byte range, which is all FFmpeg asks, so the
/// engine streams and seeks in such a file as it does one on a web server.
///
/// A file's address stays the same from one run of FastPlay to the next: the
/// same port where it can, and a name made of the file's source and path rather
/// than a new one each time. FastPlay remembers what it was playing, and where
/// it was in long files, by address; an address that changed every run left a
/// remembered file on a server unplayable (the engine waited out a server that
/// was no longer there). An address asked for that this run has not served yet
/// is looked up in what earlier runs served, and its server connected to again.
final class LoopbackServer {
    static let shared = LoopbackServer()

    /// The bytes of a range of the file.
    typealias Reader = (Range<Int64>) async throws -> Data

    private struct Item {
        let size: Int64
        let read: Reader
    }

    private let queue = DispatchQueue(label: "FastPlay.LoopbackServer")
    private var listener: NWListener?
    private var items: [String: Item] = [:]
    private var order: [String] = []
    private static let keep = 200         // files remembered, the oldest forgotten
    private static let chunk: Int64 = 512 * 1024
    // The port asked for first, so that addresses outlast the run that made them
    private static let preferredPort: UInt16 = 52801
    // What earlier runs served, by address name: enough to serve it again
    private static let servedKey = "LoopbackServed"
    private static let servedKeep = 1000

    /// An address the engine can play the file from: `path` in the source whose
    /// id is `source` (FileSource.id), the same each time it is asked for.
    func address(name: String, size: Int64, source: String, path: String,
                 read: @escaping Reader) async throws -> String {
        let port = try await start()
        let id = Self.stableID(source: source, path: path)
        queue.sync {
            if items[id] == nil { order.append(id) }
            items[id] = Item(size: size, read: read)
            while order.count > Self.keep { items[order.removeFirst()] = nil }
        }
        Self.remember(id: id, source: source, path: path, name: name)
        // The name is only for show (the engine names a stream by its address's end)
        let shown = name.addingPercentEncoding(withAllowedCharacters: .alphanumerics.union(CharacterSet(charactersIn: "-._"))) ?? "file"
        return "http://127.0.0.1:\(port)/\(id)/\(shown)"
    }

    /// The name in a file's address: the same for the same file, every run.
    private static func stableID(source: String, path: String) -> String {
        let digest = SHA256.hash(data: Data((source + "\n" + path).utf8))
        return digest.prefix(16).map { String(format: "%02x", $0) }.joined()
    }

    private static func remember(id: String, source: String, path: String, name: String) {
        let defaults = UserDefaults.standard
        var served = defaults.dictionary(forKey: servedKey) as? [String: [String]] ?? [:]
        var order = defaults.stringArray(forKey: servedKey + "Order") ?? []
        if served[id] == nil { order.append(id) }
        served[id] = [source, path, name]
        while order.count > servedKeep { served[order.removeFirst()] = nil }
        defaults.set(served, forKey: servedKey)
        defaults.set(order, forKey: servedKey + "Order")
    }

    /// An address served in an earlier run, served again: its server connected
    /// to, as its saved details say. False if it cannot be.
    private func revive(_ id: String) async -> Bool {
        guard let known = (UserDefaults.standard.dictionary(forKey: Self.servedKey) as? [String: [String]])?[id],
              known.count == 3,
              let server = ServerStore.all.first(where: { "server-\($0.id.uuidString)" == known[0] })
        else { return false }
        let entry = FileEntry(name: known[2], path: known[1], displayPath: known[1], isFolder: false)
        // Registers it under the same name again; a server that does not answer soon
        // is given up on, as the engine may be waiting on this while the app opens
        return await withTaskGroup(of: Bool.self) { group in
            group.addTask {
                _ = try? await server.makeSource().streamURL(for: entry)
                return self.queue.sync { self.items[id] != nil }
            }
            group.addTask {
                try? await Task.sleep(nanoseconds: 8_000_000_000)
                return false
            }
            let first = await group.next() ?? false
            group.cancelAll()
            return first
        }
    }

    /// Listens now, if a file on a server has ever been played: FastPlay reopens
    /// what it was playing as it starts, and its address must be answered.
    func startForRestore() {
        guard UserDefaults.standard.dictionary(forKey: Self.servedKey)?.isEmpty == false else { return }
        let done = DispatchSemaphore(value: 0)
        Task.detached {
            _ = try? await self.start()
            done.signal()
        }
        _ = done.wait(timeout: .now() + 2)
    }

    /// Starts listening if it is not already; the port it is on.
    private func start() async throws -> UInt16 {
        if let port = queue.sync(execute: { self.listener?.port?.rawValue }), port != 0 { return port }
        // The same port as last time where it is free, so remembered addresses still work
        if let port = try? await listen(on: NWEndpoint.Port(rawValue: Self.preferredPort) ?? .any) { return port }
        return try await listen(on: .any)
    }

    private func listen(on port: NWEndpoint.Port) async throws -> UInt16 {
        let parameters = NWParameters.tcp
        parameters.allowLocalEndpointReuse = true
        parameters.requiredLocalEndpoint = .hostPort(host: "127.0.0.1", port: port)  // this device only
        let newListener = try NWListener(using: parameters)
        newListener.newConnectionHandler = { [weak self] connection in self?.accept(connection) }
        return try await withCheckedThrowingContinuation { continuation in
            var answered = false
            newListener.stateUpdateHandler = { [weak self] state in
                guard !answered else { return }
                switch state {
                case .ready:
                    answered = true
                    self?.listener = newListener
                    continuation.resume(returning: newListener.port?.rawValue ?? 0)
                case let .failed(error):
                    answered = true
                    newListener.cancel()
                    continuation.resume(throwing: error)
                default:
                    break
                }
            }
            newListener.start(queue: queue)
        }
    }

    private func accept(_ connection: NWConnection) {
        connection.start(queue: queue)
        readRequest(connection, received: Data())
    }

    /// Reads up to the blank line that ends a request's headers.
    private func readRequest(_ connection: NWConnection, received: Data) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 16 * 1024) { [weak self] data, _, isComplete, error in
            guard let self, error == nil else { return connection.cancel() }
            var all = received
            if let data { all.append(data) }
            if let end = all.range(of: Data("\r\n\r\n".utf8)) {
                self.respond(connection, request: String(decoding: all[..<end.lowerBound], as: UTF8.self))
            } else if isComplete || all.count > 64 * 1024 {
                connection.cancel()
            } else {
                self.readRequest(connection, received: all)
            }
        }
    }

    private func respond(_ connection: NWConnection, request: String) {
        let lines = request.components(separatedBy: "\r\n")
        let parts = (lines.first ?? "").split(separator: " ")
        // "GET /<id>/<name> HTTP/1.1"
        let notFound = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
        guard parts.count >= 2, parts[0] == "GET" || parts[0] == "HEAD",
              let id = parts[1].split(separator: "/").first.map(String.init) else {
            return send(connection, head: notFound, then: nil)
        }
        guard let item = items[id] else {
            // From an earlier run (a remembered file): connect to its server again
            Task {
                let revived = await self.revive(id)
                self.queue.async {
                    if revived {
                        self.respond(connection, request: request)
                    } else {
                        self.send(connection, head: notFound, then: nil)
                    }
                }
            }
            return
        }
        var start: Int64 = 0
        var end = item.size - 1
        var partial = false
        for line in lines.dropFirst() where line.lowercased().hasPrefix("range:") {
            // "Range: bytes=100-" or "bytes=100-199"
            let value = line.dropFirst(6).trimmingCharacters(in: .whitespaces)
            guard value.hasPrefix("bytes=") else { continue }
            let bounds = value.dropFirst(6).split(separator: "-", omittingEmptySubsequences: false)
            if let first = bounds.first, let from = Int64(first) { start = from }
            if bounds.count > 1, let to = Int64(bounds[1]) { end = min(to, item.size - 1) }
            partial = true
        }
        guard start <= end, start >= 0 else {
            return send(connection, head: "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */\(item.size)\r\n" +
                "Content-Length: 0\r\nConnection: close\r\n\r\n", then: nil)
        }
        var head = partial ? "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes \(start)-\(end)/\(item.size)\r\n"
                           : "HTTP/1.1 200 OK\r\n"
        head += "Accept-Ranges: bytes\r\nContent-Type: application/octet-stream\r\n" +
            "Content-Length: \(end - start + 1)\r\nConnection: close\r\n\r\n"
        let wantsBody = parts[0] == "GET"
        send(connection, head: head) {
            guard wantsBody else { return connection.cancel() }
            Task { await self.stream(connection, item: item, from: start, through: end) }
        }
    }

    private func send(_ connection: NWConnection, head: String, then next: (() -> Void)?) {
        connection.send(content: Data(head.utf8), completion: .contentProcessed { error in
            if error != nil || next == nil { connection.cancel() } else { next?() }
        })
    }

    /// Sends the range a piece at a time, each read as the last has gone out, until
    /// it is all sent or the engine closes the connection (as it does to seek).
    private func stream(_ connection: NWConnection, item: Item, from start: Int64, through end: Int64) async {
        var offset = start
        while offset <= end {
            let upper = min(offset + Self.chunk, end + 1)
            guard let data = try? await item.read(offset..<upper), !data.isEmpty else { break }
            let sent: Bool = await withCheckedContinuation { continuation in
                connection.send(content: data, completion: .contentProcessed { error in
                    continuation.resume(returning: error == nil)
                })
            }
            if !sent { break }
            offset += Int64(data.count)
        }
        connection.cancel()
    }
}
