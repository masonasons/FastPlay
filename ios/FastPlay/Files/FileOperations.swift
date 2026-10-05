import Foundation

/// Files and folders cut or copied, from any source, waiting to be pasted.
enum FileClipboard {
    struct Item {
        let source: FileSource
        let entry: FileEntry
    }

    static var items: [Item] = []
    /// Cut: pasting moves them. Copied: pasting leaves the originals.
    static var isCut = false

    static func set(_ items: [Item], cut: Bool) {
        self.items = items
        isCut = cut
    }

    /// Something pasted, deleted or renamed: take it off the clipboard.
    static func forget(source: FileSource, path: String) {
        items.removeAll { $0.source.id == source.id && $0.entry.path == path }
    }
}

/// Copying and moving between any two sources (FastFiles' paste).
///
/// A file goes by way of this device: a copy within it is a copy, anything else
/// is downloaded here (when it is not here already) and uploaded there. A move
/// within one source is that source's own move where it has one. When a name is
/// already taken, `resolver` is asked (with what to do for the rest, if wanted).
@MainActor
final class FileOperations {
    enum Resolution {
        case replace
        /// Only if the one being pasted is newer or another size.
        case replaceIfDifferent
        case keepBoth
        case skip
        /// A folder: what is in the one pasted goes into the one there.
        case merge
        /// A folder: merged, then what is in the one there and not in the one
        /// pasted is deleted.
        case sync

        var title: String {
            switch self {
            case .replace: return "Replace"
            case .replaceIfDifferent: return "Replace if Newer or Another Size"
            case .keepBoth: return "Keep Both"
            case .skip: return "Skip"
            case .merge: return "Merge Into the Folder There"
            case .sync: return "Sync (Make the Folder There the Same)"
            }
        }

        static let forFiles: [Resolution] = [.replace, .replaceIfDifferent, .keepBoth, .skip]
        static let forFolders: [Resolution] = [.merge, .sync, .replace, .keepBoth, .skip]
    }

    struct Choice {
        let resolution: Resolution
        let applyToAll: Bool
    }

    /// Asked about a name already taken: what is being pasted and what is there.
    /// Nil stops the paste.
    typealias Resolver = @MainActor (_ pasting: FileEntry, _ existing: FileEntry) async -> Choice?
    typealias Progress = @MainActor (_ name: String, _ done: Int) -> Void

    private let isMove: Bool
    private let resolver: Resolver
    private let progress: Progress
    private var rememberedFile: Resolution?
    private var rememberedFolder: Resolution?
    private(set) var filesDone = 0
    private let temporary = FileManager.default.temporaryDirectory.appendingPathComponent("Pasting-\(UUID().uuidString)")

    init(move: Bool, resolver: @escaping Resolver, progress: @escaping Progress) {
        isMove = move
        self.resolver = resolver
        self.progress = progress
    }

    struct Stopped: Error {}

    /// Pastes `items` into `folder` of `destination`.
    func paste(_ items: [FileClipboard.Item], into destination: FileSource, folder: String) async throws {
        defer { try? FileManager.default.removeItem(at: temporary) }
        var there = try await names(in: destination, folder: folder)
        for item in items {
            try Task.checkCancellation()
            if item.source.id == destination.id, item.entry.isFolder,
               Self.same(folder, item.entry.path) || Self.isBelow(folder, item.entry.path) {
                throw RemoteError.failed("\(item.entry.name) cannot go inside itself.")
            }
            // Cut and pasted where it already is: nothing to do
            if isMove, item.source.id == destination.id, Self.same(Self.parent(of: item.entry.path), folder) { continue }
            try await process(item.source, item.entry, into: destination, folder: folder, there: &there, cascade: nil)
        }
    }

    // MARK: One item

    /// `there`: what is in `folder` now, by name in lower case (sources such as
    /// Dropbox and SMB take two names differing only in case as the same).
    private func process(_ source: FileSource, _ entry: FileEntry, into destination: FileSource, folder: String,
                         there: inout [String: FileEntry], cascade: Resolution?) async throws {
        try Task.checkCancellation()
        let existing = there[entry.name.lowercased()]
        if entry.isFolder {
            try await folderItem(source, entry, into: destination, folder: folder, existing: existing, there: &there,
                                 cascade: cascade)
        } else {
            try await fileItem(source, entry, into: destination, folder: folder, existing: existing, there: &there,
                               cascade: cascade)
        }
    }

    private func fileItem(_ source: FileSource, _ entry: FileEntry, into destination: FileSource, folder: String,
                          existing: FileEntry?, there: inout [String: FileEntry], cascade: Resolution?) async throws {
        guard let existing else {
            if try await fastMove(source, entry, into: destination, folder: folder) { return }
            try await copyFile(source, entry, into: destination, folder: folder, as: entry.name, there: &there)
            if isMove { try await source.delete(entry) }
            return
        }
        // Inside a folder being merged or synced, changed files are replaced without asking
        let resolution: Resolution
        if cascade != nil {
            resolution = .replaceIfDifferent
        } else if let remembered = rememberedFile {
            resolution = remembered
        } else {
            resolution = try await ask(entry, existing)
        }
        switch resolution {
        case .replace:
            try await replaceFile(source, entry, into: destination, folder: folder, existing: existing, there: &there)
        case .replaceIfDifferent:
            let newer = (entry.modified ?? .distantPast) > (existing.modified ?? .distantPast).addingTimeInterval(1)
            if entry.size != existing.size || newer {
                try await replaceFile(source, entry, into: destination, folder: folder, existing: existing, there: &there)
            }
        case .keepBoth:
            try await copyFile(source, entry, into: destination, folder: folder, as: Self.uniqueName(entry.name, there),
                               there: &there)
            if isMove { try await source.delete(entry) }
        default:
            break  // skip (or a folder's choice)
        }
    }

    private func replaceFile(_ source: FileSource, _ entry: FileEntry, into destination: FileSource, folder: String,
                             existing: FileEntry, there: inout [String: FileEntry]) async throws {
        // Under the name already there, whatever its capitals
        try await copyFile(source, entry, into: destination, folder: folder, as: existing.name, there: &there)
        if isMove { try await source.delete(entry) }
    }

    private func folderItem(_ source: FileSource, _ entry: FileEntry, into destination: FileSource, folder: String,
                            existing: FileEntry?, there: inout [String: FileEntry], cascade: Resolution?) async throws {
        guard let existing, existing.isFolder else {
            if existing == nil, try await fastMove(source, entry, into: destination, folder: folder) {
                there[entry.name.lowercased()] = destination.entry(named: entry.name, in: folder, isFolder: true)
                return
            }
            // A file holds the name: the folder alongside, under another
            let name = existing == nil ? entry.name : Self.uniqueName(entry.name, there)
            let made = try await makeFolder(name, in: destination, folder: folder, there: &there)
            try await copyContents(source, entry, into: destination, folder: made.path, cascade: nil)
            if isMove { try await deleteIfEmpty(source, entry) }
            return
        }
        let resolution: Resolution
        if let cascade {
            resolution = cascade
        } else if let remembered = rememberedFolder {
            resolution = remembered
        } else {
            resolution = try await ask(entry, existing)
        }
        switch resolution {
        case .merge:
            try await copyContents(source, entry, into: destination, folder: existing.path, cascade: .merge)
            if isMove { try await deleteIfEmpty(source, entry) }
        case .sync:
            let pasted = try await source.list(path: entry.path)
            var inside = try await names(in: destination, folder: existing.path)
            for child in pasted {
                try await process(source, child, into: destination, folder: existing.path, there: &inside, cascade: .sync)
            }
            let keep = Set(pasted.map { $0.name.lowercased() })
            for extra in try await destination.list(path: existing.path) where !keep.contains(extra.name.lowercased()) {
                try Task.checkCancellation()
                try await destination.delete(extra)
            }
            if isMove { try await deleteIfEmpty(source, entry) }
        case .replace:
            try await destination.delete(existing)
            there[existing.name.lowercased()] = nil
            let made = try await makeFolder(entry.name, in: destination, folder: folder, there: &there)
            try await copyContents(source, entry, into: destination, folder: made.path, cascade: nil)
            if isMove { try await deleteIfEmpty(source, entry) }
        case .keepBoth:
            let made = try await makeFolder(Self.uniqueName(entry.name, there), in: destination, folder: folder,
                                            there: &there)
            try await copyContents(source, entry, into: destination, folder: made.path, cascade: nil)
            if isMove { try await deleteIfEmpty(source, entry) }
        default:
            break  // skip
        }
    }

    // MARK: Pieces

    private func copyContents(_ source: FileSource, _ folder: FileEntry, into destination: FileSource,
                              folder destinationFolder: String, cascade: Resolution?) async throws {
        var inside = try await names(in: destination, folder: destinationFolder)
        for child in try await source.list(path: folder.path) {
            try await process(source, child, into: destination, folder: destinationFolder, there: &inside, cascade: cascade)
        }
    }

    private func makeFolder(_ name: String, in destination: FileSource, folder: String,
                            there: inout [String: FileEntry]) async throws -> FileEntry {
        try await destination.createFolder(named: name, in: folder)
        let made = destination.entry(named: name, in: folder, isFolder: true)
        there[name.lowercased()] = made
        return made
    }

    /// A move within one source, where it has its own.
    private func fastMove(_ source: FileSource, _ entry: FileEntry, into destination: FileSource,
                          folder: String) async throws -> Bool {
        guard isMove, source.id == destination.id else { return false }
        return try await source.move(entry, to: folder)
    }

    private func copyFile(_ source: FileSource, _ entry: FileEntry, into destination: FileSource, folder: String,
                          as name: String, there: inout [String: FileEntry]) async throws {
        progress(name, filesDone)
        let target = destination.path(of: name, in: folder)
        if source.isLocal, let local = destination as? LocalSource {
            try await local.copy(from: URL(fileURLWithPath: entry.path), to: URL(fileURLWithPath: target))
        } else {
            // By way of this device
            var file = URL(fileURLWithPath: entry.path)
            var downloaded = false
            if !source.isLocal {
                let manager = FileManager.default
                let holder = temporary.appendingPathComponent(UUID().uuidString)
                try manager.createDirectory(at: holder, withIntermediateDirectories: true)
                file = holder.appendingPathComponent(entry.name)
                try await source.download(entry, to: file)
                downloaded = true
            }
            defer { if downloaded { try? FileManager.default.removeItem(at: file.deletingLastPathComponent()) } }
            try Task.checkCancellation()
            try await destination.upload(file, named: name, to: folder)
        }
        there[name.lowercased()] = destination.entry(named: name, in: folder, isFolder: false, size: entry.size)
        filesDone += 1
    }

    private func deleteIfEmpty(_ source: FileSource, _ folder: FileEntry) async throws {
        if try await source.list(path: folder.path).isEmpty { try await source.delete(folder) }
    }

    private func ask(_ pasting: FileEntry, _ existing: FileEntry) async throws -> Resolution {
        guard let choice = await resolver(pasting, existing) else { throw Stopped() }
        if choice.applyToAll {
            if pasting.isFolder { rememberedFolder = choice.resolution } else { rememberedFile = choice.resolution }
        }
        return choice.resolution
    }

    private func names(in source: FileSource, folder: String) async throws -> [String: FileEntry] {
        var names: [String: FileEntry] = [:]
        for entry in try await source.list(path: folder) { names[entry.name.lowercased()] = entry }
        return names
    }

    // MARK: Names and paths

    /// "Song (2).mp3", "Song (3).mp3"... the first not taken.
    nonisolated static func uniqueName(_ name: String, _ there: [String: FileEntry]) -> String {
        guard there[name.lowercased()] != nil else { return name }
        let base = (name as NSString).deletingPathExtension
        let ext = (name as NSString).pathExtension
        var number = 2
        while true {
            let candidate = ext.isEmpty ? "\(base) (\(number))" : "\(base) (\(number)).\(ext)"
            if there[candidate.lowercased()] == nil { return candidate }
            number += 1
        }
    }

    nonisolated static func parent(of path: String) -> String {
        let trimmed = path.hasSuffix("/") ? String(path.dropLast()) : path
        guard let slash = trimmed.lastIndex(of: "/") else { return "" }
        return String(trimmed[..<slash])
    }

    nonisolated static func same(_ a: String, _ b: String) -> Bool {
        trimmed(a).caseInsensitiveCompare(trimmed(b)) == .orderedSame
    }

    /// Whether `path` is inside `folder`.
    nonisolated static func isBelow(_ path: String, _ folder: String) -> Bool {
        trimmed(path).lowercased().hasPrefix(trimmed(folder).lowercased() + "/")
    }

    nonisolated private static func trimmed(_ path: String) -> String {
        path.count > 1 && path.hasSuffix("/") ? String(path.dropLast()) : path
    }
}
