import Foundation

/// FastPlay's own files on this device: what the Files app shows as On My iPhone >
/// FastPlay. A path here is a file system path.
final class LocalSource: FileSource {
    static let shared = LocalSource()

    let id = "local"
    let title = "Files"
    var rootPath: String { FPEngine.shared.documentsPath }
    var isLocal: Bool { true }
    var canWrite: Bool { true }

    private static let keys: [URLResourceKey] = [.isDirectoryKey, .contentModificationDateKey, .fileSizeKey]

    private func entry(_ url: URL) -> FileEntry {
        let values = try? url.resourceValues(forKeys: Set(Self.keys))
        return FileEntry(name: url.lastPathComponent, path: url.path, displayPath: url.path,
                         isFolder: values?.isDirectory ?? false, size: Int64(values?.fileSize ?? 0),
                         modified: values?.contentModificationDate)
    }

    func list(path: String) async throws -> [FileEntry] {
        try FileManager.default.contentsOfDirectory(at: URL(fileURLWithPath: path), includingPropertiesForKeys: Self.keys,
                                                    options: [.skipsHiddenFiles]).map { entry($0) }
    }

    func listAll(path: String) async throws -> [FileEntry] {
        guard let walker = FileManager.default.enumerator(at: URL(fileURLWithPath: path), includingPropertiesForKeys: Self.keys,
                                                          options: [.skipsHiddenFiles]) else { return [] }
        var files: [FileEntry] = []
        for case let url as URL in walker {
            let item = entry(url)
            if !item.isFolder { files.append(item) }
        }
        return files
    }

    func streamURL(for entry: FileEntry) async throws -> String { entry.path }

    func download(_ entry: FileEntry, to destination: URL) async throws {
        try await Self.work {
            let manager = FileManager.default
            try? manager.removeItem(at: destination)
            try manager.copyItem(at: URL(fileURLWithPath: entry.path), to: destination)
        }
    }

    func createFolder(named name: String, in folder: String) async throws {
        try FileManager.default.createDirectory(at: URL(fileURLWithPath: path(of: name, in: folder)),
                                                withIntermediateDirectories: false)
    }

    func upload(_ file: URL, named name: String, to folder: String) async throws {
        try await copy(from: file, to: URL(fileURLWithPath: path(of: name, in: folder)))
    }

    /// Copies within this device (a file or a folder), replacing what is there.
    func copy(from source: URL, to target: URL) async throws {
        try await Self.work {
            let manager = FileManager.default
            if manager.fileExists(atPath: target.path) { try manager.removeItem(at: target) }
            try manager.copyItem(at: source, to: target)
        }
    }

    func rename(_ entry: FileEntry, to name: String) async throws {
        let url = URL(fileURLWithPath: entry.path)
        try FileManager.default.moveItem(at: url, to: url.deletingLastPathComponent().appendingPathComponent(name))
    }

    func delete(_ entry: FileEntry) async throws {
        try await Self.work { try FileManager.default.removeItem(atPath: entry.path) }
    }

    func move(_ entry: FileEntry, to folder: String) async throws -> Bool {
        try FileManager.default.moveItem(atPath: entry.path, toPath: path(of: entry.name, in: folder))
        return true
    }

    func storageSpace() async throws -> (free: Int64, total: Int64)? {
        let values = try URL(fileURLWithPath: rootPath).resourceValues(
            forKeys: [.volumeAvailableCapacityForImportantUsageKey, .volumeTotalCapacityKey])
        guard let free = values.volumeAvailableCapacityForImportantUsage, let total = values.volumeTotalCapacity else {
            return nil
        }
        return (free, Int64(total))
    }

    /// Copying and deleting can take a while: not on the main thread.
    private static func work(_ job: @escaping () throws -> Void) async throws {
        try await Task.detached(priority: .userInitiated) { try job() }.value
    }
}
