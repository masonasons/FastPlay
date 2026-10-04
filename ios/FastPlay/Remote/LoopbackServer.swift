import Foundation
import Network

/// A tiny HTTP server on this device's loopback address, serving files that only
/// the app can read (an SMB share's) to the engine, which plays addresses. It
/// answers GET with or without a byte range, which is all FFmpeg asks, so the
/// engine streams and seeks in such a file as it does one on a web server.
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

    /// An address the engine can play the file from, good while FastPlay runs.
    func address(name: String, size: Int64, read: @escaping Reader) async throws -> String {
        let port = try await start()
        let id = UUID().uuidString
        queue.sync {
            items[id] = Item(size: size, read: read)
            order.append(id)
            while order.count > Self.keep { items[order.removeFirst()] = nil }
        }
        // The name is only for show (the engine names a stream by its address's end)
        let shown = name.addingPercentEncoding(withAllowedCharacters: .alphanumerics.union(CharacterSet(charactersIn: "-._"))) ?? "file"
        return "http://127.0.0.1:\(port)/\(id)/\(shown)"
    }

    /// Starts listening if it is not already; the port it is on.
    private func start() async throws -> UInt16 {
        if let port = queue.sync(execute: { self.listener?.port?.rawValue }), port != 0 { return port }
        let parameters = NWParameters.tcp
        parameters.requiredLocalEndpoint = .hostPort(host: "127.0.0.1", port: .any)  // this device only
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
        guard parts.count >= 2, parts[0] == "GET" || parts[0] == "HEAD",
              let id = parts[1].split(separator: "/").first.map(String.init), let item = items[id] else {
            return send(connection, head: "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", then: nil)
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
