import Foundation

/// Reading playlist files (M3U, M3U8, PLS) as text.
enum PlaylistText {
    static func isPlaylistName(_ name: String) -> Bool {
        ["m3u", "m3u8", "pls"].contains((name as NSString).pathExtension.lowercased())
    }

    /// An M3U8 that is a stream in pieces (HLS), which FFmpeg plays as one thing.
    static func isHLS(_ text: String) -> Bool {
        text.contains("#EXT-X-")
    }

    /// The entries written in a playlist: an M3U's lines that are not comments, or a
    /// PLS's File1=, File2=... in order.
    static func entries(in text: String, isPLS: Bool) -> [String] {
        let lines = text.split(whereSeparator: \.isNewline).map { $0.trimmingCharacters(in: .whitespaces) }
        if isPLS {
            var numbered: [(Int, String)] = []
            for line in lines {
                let lower = line.lowercased()
                guard lower.hasPrefix("file"), let equals = line.firstIndex(of: "=") else { continue }
                let number = Int(line[line.index(line.startIndex, offsetBy: 4)..<equals]) ?? 0
                let value = line[line.index(after: equals)...].trimmingCharacters(in: .whitespaces)
                if !value.isEmpty { numbered.append((number, value)) }
            }
            return numbered.sorted { $0.0 < $1.0 }.map(\.1)
        }
        return lines.filter { !$0.isEmpty && !$0.hasPrefix("#") }
            .map { $0.hasPrefix("\u{FEFF}") ? String($0.dropFirst()) : $0 }
    }

    /// Fetches a playlist at an address and gives its entries as full addresses, in
    /// order; nil if it is not a playlist (an HLS stream, or not a playlist's name),
    /// or could not be read, in which case the address is for playing as it is.
    static func entries(atAddress address: String) async -> [String]? {
        guard let url = URL(string: address), isPlaylistName(url.lastPathComponent) else { return nil }
        guard let (data, _) = try? await URLSession.shared.data(from: url),
              let text = String(data: data, encoding: .utf8) ?? String(data: data, encoding: .isoLatin1) else { return nil }
        if isHLS(text) { return nil }
        let items = entries(in: text, isPLS: url.lastPathComponent.lowercased().hasSuffix(".pls"))
        let full = items.compactMap { item -> String? in
            if item.contains("://") { return item }
            // Relative to the playlist, as a web page's links are
            let escaped = item.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? item
            return URL(string: escaped, relativeTo: url)?.absoluteURL.absoluteString
        }
        return full.isEmpty ? nil : full
    }

    /// Plays an address: a playlist's entries as the playlist, anything else as it is.
    @MainActor
    static func play(address: String) async {
        if let items = await entries(atAddress: address) {
            let names = items.map { (URL(string: $0)?.lastPathComponent.removingPercentEncoding) ?? $0 }
            FPEngine.shared.playURLs(items, names: names, startingAt: 0)
        } else {
            FPEngine.shared.playURL(address, name: nil)
        }
    }
}
