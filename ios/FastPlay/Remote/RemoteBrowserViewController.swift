import UIKit

/// A folder somewhere other than this device (Dropbox, an FTP or SMB server).
///
/// Choosing a file streams it, with the files after it in the folder as the
/// playlist. Download to This Device, on a file or a folder, copies it into
/// FastPlay's own files; Sync, on a folder, makes the copy here match the folder
/// there, deleting what is no longer in it. Both are VoiceOver actions, in the long
/// press menu, and swipes; the button at the top has them for the folder shown.
final class RemoteBrowserViewController: FastPlayTableViewController {
    /// How many files one choice queues: each may need an address asked for.
    private static let playlistLimit = 50

    private let source: RemoteFileSource
    private let folder: RemoteEntry
    private let isTop: Bool
    private var entries: [RemoteEntry] = []
    private let statusLabel = UILabel()

    /// The top folder of a source, or with `path` a folder to start in.
    convenience init(source: RemoteFileSource, name: String, path: String = "") {
        self.init(source: source, folder: RemoteEntry(name: name, path: path, displayPath: path, isFolder: true),
                  isTop: true)
    }

    private init(source: RemoteFileSource, folder: RemoteEntry, isTop: Bool) {
        self.source = source
        self.folder = folder
        self.isTop = isTop
        super.init(style: .plain)
        title = folder.name
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    override func viewDidLoad() {
        super.viewDidLoad()
        navigationItem.largeTitleDisplayMode = .never
        tableView.register(UITableViewCell.self, forCellReuseIdentifier: "cell")
        statusLabel.numberOfLines = 0
        statusLabel.textAlignment = .center
        statusLabel.textColor = .secondaryLabel
        statusLabel.font = .preferredFont(forTextStyle: .body)
        statusLabel.adjustsFontForContentSizeCategory = true

        var items = [
            UIAction(title: "Download This Folder", image: UIImage(systemName: "arrow.down.circle")) { [weak self] _ in
                guard let self else { return }
                self.transfer(self.folder, sync: false)
            },
            UIAction(title: "Sync This Folder", image: UIImage(systemName: "arrow.triangle.2.circlepath")) { [weak self] _ in
                guard let self else { return }
                self.transfer(self.folder, sync: true)
            },
        ]
        if isTop, let account = source.accountAction {
            items.append(UIAction(title: account.title, attributes: .destructive) { [weak self] _ in
                guard let self else { return }
                account.run(self)
            })
        }
        let more = UIBarButtonItem(image: UIImage(systemName: "ellipsis.circle"), menu: UIMenu(children: items))
        more.accessibilityLabel = "Folder actions"
        navigationItem.rightBarButtonItem = more

        refreshControl = UIRefreshControl()
        refreshControl?.addTarget(self, action: #selector(load), for: .valueChanged)
        load()
    }

    @objc private func load() {
        if entries.isEmpty { show(status: "Loading…") }
        Task { @MainActor in
            do {
                // Folders, and the files FastPlay plays: folders first, then names
                // as a person orders them ("2" before "10")
                entries = try await source.list(path: folder.path)
                    .filter { $0.isFolder || engine.isPlayableFile($0.name) }
                    .sorted {
                        if $0.isFolder != $1.isFolder { return $0.isFolder }
                        return $0.name.localizedStandardCompare($1.name) == .orderedAscending
                    }
                show(status: entries.isEmpty ? "Nothing here that FastPlay plays." : nil)
                UIAccessibility.post(notification: .announcement,
                                     argument: entries.count == 1 ? "1 item" : "\(entries.count) items")
            } catch {
                entries = []
                show(status: error.localizedDescription)
            }
            refreshControl?.endRefreshing()
        }
    }

    private func show(status: String?) {
        tableView.reloadData()
        statusLabel.text = status
        tableView.backgroundView = status == nil ? nil : statusLabel
    }

    private func play(from index: Int) {
        // The file chosen and those after it in the folder
        let files = entries[index...].filter { !$0.isFolder }.prefix(Self.playlistLimit)
        UIAccessibility.post(notification: .announcement, argument: "Opening \(entries[index].name)")
        Task { @MainActor in
            do {
                var urls: [String] = []
                var names: [String] = []
                for file in files {
                    urls.append(try await source.streamURL(for: file))
                    names.append(file.name)
                }
                engine.playURLs(urls, names: names, startingAt: 0)
                openPlayer()
            } catch {
                let alert = UIAlertController(title: folder.name, message: error.localizedDescription,
                                              preferredStyle: .alert)
                alert.addAction(UIAlertAction(title: "OK", style: .default))
                present(alert, animated: true)
            }
        }
    }

    // MARK: Downloading and syncing

    /// Copies a file, or a folder with everything in it that FastPlay plays, into
    /// FastPlay's own files, keeping the folders as they are there.
    ///
    /// Download leaves alone what is already here. Sync makes the folder here match
    /// the one there: files that changed are fetched again, and files here that are
    /// no longer there are deleted (after asking).
    private func transfer(_ entry: RemoteEntry, sync: Bool) {
        let progress = UIAlertController(title: "\(sync ? "Syncing" : "Downloading") \(entry.name)",
                                         message: "Looking…", preferredStyle: .alert)
        let task = Task { @MainActor in
            var outcome: String
            do {
                let manager = FileManager.default
                let documents = URL(fileURLWithPath: engine.documentsPath)
                let root = documents.appendingPathComponent(entry.name)

                // What is there, and where each file goes here
                var wanted: [(file: RemoteEntry, destination: URL)] = []
                var sizes: [String: Int64] = [:]  // by destination path
                if entry.isFolder {
                    let prefix = entry.displayPath.count
                    for item in try await source.listAll(path: entry.path)
                    where !item.isFolder && engine.isPlayableFile(item.name) {
                        // The path below the folder being fetched
                        let relative = String(item.displayPath.dropFirst(prefix)).trimmingCharacters(
                            in: CharacterSet(charactersIn: "/"))
                        let destination = root.appendingPathComponent(relative)
                        wanted.append((item, destination))
                        sizes[destination.standardizedFileURL.path] = item.size
                    }
                } else {
                    wanted.append((entry, documents.appendingPathComponent(entry.name)))
                }

                // Syncing: what is here that is no longer there
                var stale: [URL] = []
                if sync, entry.isFolder,
                   let walker = manager.enumerator(at: root, includingPropertiesForKeys: [.isRegularFileKey]) {
                    for case let url as URL in walker {
                        let isFile = (try? url.resourceValues(forKeys: [.isRegularFileKey]))?.isRegularFile ?? false
                        if isFile, sizes[url.standardizedFileURL.path] == nil { stale.append(url) }
                    }
                }
                if !stale.isEmpty {
                    progress.dismiss(animated: false)
                    guard await confirmDeleting(stale.count, from: entry.name) else { return }
                    present(progress, animated: false)
                }

                var fetched = 0, kept = 0
                for (index, item) in wanted.enumerated() {
                    try Task.checkCancellation()
                    let destination = item.destination
                    progress.message = "\(index + 1) of \(wanted.count): \(destination.lastPathComponent)"
                    if manager.fileExists(atPath: destination.path) {
                        // Download keeps what is here. Sync fetches it again if the
                        // one there is another size, or changed after this copy was made.
                        let attributes = try? manager.attributesOfItem(atPath: destination.path)
                        let localSize = (attributes?[.size] as? NSNumber)?.int64Value ?? -1
                        let localDate = attributes?[.modificationDate] as? Date ?? .distantPast
                        let changed = (item.file.size > 0 && localSize != item.file.size)
                            || (item.file.modified.map { $0 > localDate.addingTimeInterval(1) } ?? false)
                        if !sync || !changed {
                            kept += 1
                            continue
                        }
                    }
                    try manager.createDirectory(at: destination.deletingLastPathComponent(),
                                                withIntermediateDirectories: true)
                    try await source.download(item.file, to: destination)
                    // Dated as it is there, so a later sync can tell if it changed
                    if let modified = item.file.modified {
                        try? manager.setAttributes([.modificationDate: modified], ofItemAtPath: destination.path)
                    }
                    fetched += 1
                }

                var removed = 0
                for url in stale where (try? manager.removeItem(at: url)) != nil { removed += 1 }
                if removed > 0 { Self.removeEmptyFolders(under: root) }

                if wanted.isEmpty && removed == 0 {
                    outcome = "Nothing there that FastPlay plays."
                } else {
                    var parts = [fetched == 1 ? "1 file downloaded" : "\(fetched) files downloaded"]
                    if kept > 0 { parts.append(sync ? "\(kept) up to date" : "\(kept) already here") }
                    if removed > 0 {
                        parts.append(removed == 1 ? "1 deleted from this device" : "\(removed) deleted from this device")
                    }
                    outcome = parts.joined(separator: ", ") + "."
                }
            } catch is CancellationError {
                outcome = "Stopped."
            } catch {
                let nsError = error as NSError
                let stopped = (error as? URLError)?.code == .cancelled
                    || (nsError.domain == NSCocoaErrorDomain && nsError.code == NSUserCancelledError)
                outcome = stopped ? "Stopped." : error.localizedDescription
            }
            let show = { [weak self] in
                let done = UIAlertController(title: entry.name, message: outcome, preferredStyle: .alert)
                done.addAction(UIAlertAction(title: "OK", style: .default))
                self?.present(done, animated: true)
            }
            if progress.presentingViewController != nil { progress.dismiss(animated: true, completion: show) } else { show() }
        }
        progress.addAction(UIAlertAction(title: "Stop", style: .cancel) { _ in task.cancel() })
        present(progress, animated: true)
    }

    /// Asks before a sync deletes anything. False if the user says no.
    @MainActor
    private func confirmDeleting(_ count: Int, from name: String) async -> Bool {
        await withCheckedContinuation { continuation in
            let what = count == 1 ? "1 file" : "\(count) files"
            let alert = UIAlertController(
                title: "Sync \(name)",
                message: "\(what) on this device \(count == 1 ? "is" : "are") no longer in that folder and will be deleted from this device.",
                preferredStyle: .alert)
            alert.addAction(UIAlertAction(title: "Cancel", style: .cancel) { _ in continuation.resume(returning: false) })
            alert.addAction(UIAlertAction(title: "Sync and Delete", style: .destructive) { _ in
                continuation.resume(returning: true)
            })
            present(alert, animated: true)
        }
    }

    /// Folders left with nothing in them after a sync deleted their files.
    private static func removeEmptyFolders(under root: URL) {
        let manager = FileManager.default
        guard let walker = manager.enumerator(at: root, includingPropertiesForKeys: [.isDirectoryKey]) else { return }
        var folders: [URL] = []
        for case let url as URL in walker where (try? url.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory == true {
            folders.append(url)
        }
        // Deepest first, so a folder emptied of its empty folders goes too
        for folder in folders.sorted(by: { $0.path.count > $1.path.count }) {
            if ((try? manager.contentsOfDirectory(atPath: folder.path)) ?? ["x"]).isEmpty {
                try? manager.removeItem(at: folder)
            }
        }
    }

    /// What can be done with a row, besides playing it or going into it.
    private func actions(for entry: RemoteEntry) -> [(title: String, symbol: String, run: () -> Void)] {
        var list: [(title: String, symbol: String, run: () -> Void)] = [
            (entry.isFolder ? "Download Folder to This Device" : "Download to This Device", "arrow.down.circle",
             { [weak self] in self?.transfer(entry, sync: false) }),
        ]
        if entry.isFolder {
            list.append(("Sync Folder to This Device", "arrow.triangle.2.circlepath",
                         { [weak self] in self?.transfer(entry, sync: true) }))
        }
        return list
    }

    override func tableView(_ tableView: UITableView, contextMenuConfigurationForRowAt indexPath: IndexPath,
                            point: CGPoint) -> UIContextMenuConfiguration? {
        let entry = entries[indexPath.row]
        return UIContextMenuConfiguration(identifier: nil, previewProvider: nil) { [weak self] _ in
            guard let self else { return nil }
            return UIMenu(children: self.actions(for: entry).map { action in
                UIAction(title: action.title, image: UIImage(systemName: action.symbol)) { _ in action.run() }
            })
        }
    }

    override func tableView(_ tableView: UITableView,
                            trailingSwipeActionsConfigurationForRowAt indexPath: IndexPath) -> UISwipeActionsConfiguration? {
        let entry = entries[indexPath.row]
        let download = UIContextualAction(style: .normal, title: "Download") { [weak self] _, _, done in
            self?.transfer(entry, sync: false)
            done(true)
        }
        download.backgroundColor = .systemBlue
        guard entry.isFolder else { return UISwipeActionsConfiguration(actions: [download]) }
        let sync = UIContextualAction(style: .normal, title: "Sync") { [weak self] _, _, done in
            self?.transfer(entry, sync: true)
            done(true)
        }
        sync.backgroundColor = .systemGreen
        return UISwipeActionsConfiguration(actions: [download, sync])
    }

    // MARK: Table

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        entries.count
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let entry = entries[indexPath.row]
        let cell = tableView.dequeueReusableCell(withIdentifier: "cell", for: indexPath)
        var content = UIListContentConfiguration.cell()
        content.text = entry.name
        content.image = UIImage(systemName: entry.isFolder ? "folder" : "music.note")
        cell.contentConfiguration = content
        cell.accessoryType = entry.isFolder ? .disclosureIndicator : .none
        cell.accessibilityLabel = entry.isFolder ? "\(entry.name), folder" : entry.name
        cell.accessibilityCustomActions = actions(for: entry).map { action in
            UIAccessibilityCustomAction(name: action.title) { _ in
                action.run()
                return true
            }
        }
        return cell
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        let entry = entries[indexPath.row]
        if entry.isFolder {
            navigationController?.pushViewController(
                RemoteBrowserViewController(source: source, folder: entry, isTop: false), animated: true)
        } else {
            play(from: indexPath.row)
        }
    }
}
