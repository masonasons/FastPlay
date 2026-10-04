import UIKit
import UniformTypeIdentifiers

/// Files and folders cut or copied in the file browser, waiting to be pasted.
enum FileClipboard {
    static var items: [URL] = []
    /// Cut: pasting moves them. Copied: pasting leaves the originals.
    static var isCut = false

    static func set(_ urls: [URL], cut: Bool) {
        items = urls
        isCut = cut
    }
}

/// A folder of FastPlay's own files: the folders in it, and the files FastPlay
/// plays. The top one is what the Files app shows as On My iPhone > FastPlay.
///
/// Each row has the actions Cut, Copy and Delete (and Paste Into Folder, on a
/// folder), as VoiceOver actions, in the menu a long press brings up, and Delete
/// as a swipe. Paste, into the folder being shown, is in the Add menu.
final class FilesViewController: FastPlayTableViewController, UIDocumentPickerDelegate {
    private struct Entry {
        let url: URL
        let isFolder: Bool
        let modified: Date
        let size: Int
        var name: String { url.lastPathComponent }
    }

    /// What the list is ordered by: one choice for every folder, kept between runs.
    private enum SortKey: Int, CaseIterable {
        case name, date, size, type

        var title: String {
            switch self {
            case .name: return "Name"
            case .date: return "Date Modified"
            case .size: return "Size"
            case .type: return "Type"
            }
        }

        static var current: SortKey {
            get { SortKey(rawValue: UserDefaults.standard.integer(forKey: "FilesSortKey")) ?? .name }
            set { UserDefaults.standard.set(newValue.rawValue, forKey: "FilesSortKey") }
        }

        static var reversed: Bool {
            get { UserDefaults.standard.bool(forKey: "FilesSortReversed") }
            set { UserDefaults.standard.set(newValue, forKey: "FilesSortReversed") }
        }
    }

    private let directory: URL
    private var entries: [Entry] = []
    private let emptyLabel = UILabel()

    init(directory: URL) {
        self.directory = directory
        super.init(style: .plain)
        let isTop = directory.standardizedFileURL.path == URL(fileURLWithPath: FPEngine.shared.documentsPath)
            .standardizedFileURL.path
        title = isTop ? "Files" : directory.lastPathComponent
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    override func viewDidLoad() {
        super.viewDidLoad()
        tableView.register(UITableViewCell.self, forCellReuseIdentifier: "cell")
        navigationItem.largeTitleDisplayMode = .never

        // Built each time it opens, so Paste is there only with something to paste
        let add = UIBarButtonItem(image: UIImage(systemName: "plus"), menu: UIMenu(children: [
            UIDeferredMenuElement.uncached { [weak self] provide in
                var actions = [
                    UIAction(title: "Import Files", image: UIImage(systemName: "square.and.arrow.down")) { _ in
                        self?.importFiles()
                    },
                    UIAction(title: "New Folder", image: UIImage(systemName: "folder.badge.plus")) { _ in
                        self?.newFolder()
                    },
                ]
                if let self, !FileClipboard.items.isEmpty {
                    actions.append(UIAction(title: self.pasteTitle, image: UIImage(systemName: "doc.on.clipboard")) { _ in
                        self.paste(into: self.directory)
                    })
                }
                provide(actions)
            },
        ]))
        add.accessibilityLabel = "Add"
        let playAll = UIBarButtonItem(image: UIImage(systemName: "play.circle"), style: .plain, target: self,
                                      action: #selector(playAll))
        playAll.accessibilityLabel = "Play all"
        playAll.accessibilityHint = "Plays everything in this folder and the folders inside it"
        // Built each time it opens, so the ticks show the order as it is
        let sort = UIBarButtonItem(image: UIImage(systemName: "arrow.up.arrow.down"), menu: UIMenu(children: [
            UIDeferredMenuElement.uncached { [weak self] provide in
                let keys = SortKey.allCases.map { key in
                    UIAction(title: key.title, state: key == SortKey.current ? .on : .off) { _ in
                        SortKey.current = key
                        self?.sortChanged()
                    }
                }
                let reverse = UIAction(title: "Reversed", state: SortKey.reversed ? .on : .off) { _ in
                    SortKey.reversed.toggle()
                    self?.sortChanged()
                }
                provide([UIMenu(title: "Sort By", options: .displayInline, children: keys),
                         UIMenu(options: .displayInline, children: [reverse])])
            },
        ]))
        sort.accessibilityLabel = "Sort"
        navigationItem.rightBarButtonItems = [add, sort, playAll]

        emptyLabel.text = "Nothing here yet.\n\nAdd files with the Add button, or in the Files app: On My iPhone, FastPlay."
        emptyLabel.numberOfLines = 0
        emptyLabel.textAlignment = .center
        emptyLabel.textColor = .secondaryLabel
        emptyLabel.font = .preferredFont(forTextStyle: .body)
        emptyLabel.adjustsFontForContentSizeCategory = true

        refreshControl = UIRefreshControl()
        refreshControl?.addTarget(self, action: #selector(pulledToRefresh), for: .valueChanged)
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        reload()  // the Files app may have changed the folder meanwhile
    }

    private func reload() {
        let urls = (try? FileManager.default.contentsOfDirectory(
            at: directory, includingPropertiesForKeys: [.isDirectoryKey, .contentModificationDateKey, .fileSizeKey],
            options: [.skipsHiddenFiles])) ?? []
        entries = urls.compactMap { url in
            let values = try? url.resourceValues(forKeys: [.isDirectoryKey, .contentModificationDateKey, .fileSizeKey])
            let isFolder = values?.isDirectory ?? false
            guard isFolder || engine.isPlayableFile(url.path) else { return nil }
            return Entry(url: url, isFolder: isFolder, modified: values?.contentModificationDate ?? .distantPast,
                         size: values?.fileSize ?? 0)
        }
        // Folders first whatever the order. Names as a person orders them ("2" before
        // "10"), and the name decides between two of the same date, size or type.
        // Folders have no size or type of their own, so they go by name for those.
        let key = SortKey.current
        let reversed = SortKey.reversed
        entries.sort { a, b in
            if a.isFolder != b.isFolder { return a.isFolder }
            var order = ComparisonResult.orderedSame
            switch key {
            case .name:
                break
            case .date:
                order = a.modified.compare(b.modified)
            case .size:
                if !a.isFolder { order = a.size < b.size ? .orderedAscending : (a.size > b.size ? .orderedDescending : .orderedSame) }
            case .type:
                if !a.isFolder { order = a.url.pathExtension.localizedCaseInsensitiveCompare(b.url.pathExtension) }
            }
            if order == .orderedSame { order = a.name.localizedStandardCompare(b.name) }
            return order == (reversed ? .orderedDescending : .orderedAscending)
        }
        tableView.reloadData()
        tableView.backgroundView = entries.isEmpty ? emptyLabel : nil
    }

    private func sortChanged() {
        reload()
        UIAccessibility.post(notification: .announcement,
                             argument: "Sorted by \(SortKey.current.title)" + (SortKey.reversed ? ", reversed" : ""))
    }

    /// The date or size of an entry, shown (and spoken) while the list is in that order.
    private func detail(for entry: Entry) -> String? {
        switch SortKey.current {
        case .date:
            return entry.modified == .distantPast ? nil
                : DateFormatter.localizedString(from: entry.modified, dateStyle: .medium, timeStyle: .short)
        case .size:
            return entry.isFolder ? nil : ByteCountFormatter.string(fromByteCount: Int64(entry.size), countStyle: .file)
        case .name, .type:
            return nil
        }
    }

    @objc private func pulledToRefresh() {
        reload()
        refreshControl?.endRefreshing()
    }

    @objc private func playAll() {
        engine.playFolder(directory.path)
        if engine.trackCount > 0 { openPlayer() }
    }

    // MARK: Adding

    private func importFiles() {
        let picker = UIDocumentPickerViewController(forOpeningContentTypes: [.audio, .item], asCopy: true)
        picker.allowsMultipleSelection = true
        picker.delegate = self
        present(picker, animated: true)
    }

    func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        var failed = 0
        for url in urls {
            let target = Self.uniqueURL(for: url.lastPathComponent, in: directory)
            do {
                try FileManager.default.moveItem(at: url, to: target)
            } catch {
                failed += 1
            }
        }
        reload()
        let added = urls.count - failed
        UIAccessibility.post(notification: .announcement,
                             argument: added == 1 ? "1 file added" : "\(added) files added")
        if failed > 0 {
            let alert = UIAlertController(title: "Import",
                                          message: failed == 1 ? "1 file could not be copied."
                                                               : "\(failed) files could not be copied.",
                                          preferredStyle: .alert)
            alert.addAction(UIAlertAction(title: "OK", style: .default))
            present(alert, animated: true)
        }
    }

    /// A place in a folder for a file of that name: "Song.mp3", else "Song 2.mp3"...
    static func uniqueURL(for name: String, in folder: URL) -> URL {
        var target = folder.appendingPathComponent(name)
        let base = (name as NSString).deletingPathExtension
        let ext = (name as NSString).pathExtension
        var number = 2
        while FileManager.default.fileExists(atPath: target.path) {
            let numbered = ext.isEmpty ? "\(base) \(number)" : "\(base) \(number).\(ext)"
            target = folder.appendingPathComponent(numbered)
            number += 1
        }
        return target
    }

    private func newFolder() {
        let alert = UIAlertController(title: "New Folder", message: nil, preferredStyle: .alert)
        alert.addTextField { field in
            field.placeholder = "Name"
            field.autocapitalizationType = .words
            field.accessibilityLabel = "Folder name"
        }
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        alert.addAction(UIAlertAction(title: "Create", style: .default) { [weak self, weak alert] _ in
            guard let self else { return }
            let name = alert?.textFields?.first?.text?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            guard !name.isEmpty, !name.contains("/") else { return }
            try? FileManager.default.createDirectory(at: self.directory.appendingPathComponent(name),
                                                     withIntermediateDirectories: false)
            self.reload()
        })
        present(alert, animated: true)
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
        content.secondaryText = detail(for: entry)
        content.image = UIImage(systemName: entry.isFolder ? "folder" : "music.note")
        cell.contentConfiguration = content
        cell.accessoryType = entry.isFolder ? .disclosureIndicator : .none
        cell.accessibilityLabel = (entry.isFolder ? "\(entry.name), folder" : entry.name)
            + (detail(for: entry).map { ", \($0)" } ?? "")
        // The same actions as the long press menu, for VoiceOver's actions rotor
        cell.accessibilityCustomActions = actions(for: entry).map { action in
            UIAccessibilityCustomAction(name: action.title) { _ in
                action.run()
                return true
            }
        }
        return cell
    }

    /// What can be done with a file or folder.
    private func actions(for entry: Entry) -> [(title: String, symbol: String, destructive: Bool, run: () -> Void)] {
        var list: [(title: String, symbol: String, destructive: Bool, run: () -> Void)] = [
            ("Cut", "scissors", false, { [weak self] in
                FileClipboard.set([entry.url], cut: true)
                self?.announce("Cut \(entry.name)")
            }),
            ("Copy", "doc.on.doc", false, { [weak self] in
                FileClipboard.set([entry.url], cut: false)
                self?.announce("Copied \(entry.name)")
            }),
        ]
        if entry.isFolder, !FileClipboard.items.isEmpty {
            list.append(("Paste Into Folder", "doc.on.clipboard", false, { [weak self] in
                self?.paste(into: entry.url)
            }))
        }
        list.append(("Delete", "trash", true, { [weak self] in
            self?.confirmDelete(entry) { _ in }
        }))
        return list
    }

    override func tableView(_ tableView: UITableView, contextMenuConfigurationForRowAt indexPath: IndexPath,
                            point: CGPoint) -> UIContextMenuConfiguration? {
        let entry = entries[indexPath.row]
        return UIContextMenuConfiguration(identifier: nil, previewProvider: nil) { [weak self] _ in
            guard let self else { return nil }
            return UIMenu(children: self.actions(for: entry).map { action in
                UIAction(title: action.title, image: UIImage(systemName: action.symbol),
                         attributes: action.destructive ? .destructive : []) { _ in action.run() }
            })
        }
    }

    private func announce(_ text: String) {
        UIAccessibility.post(notification: .announcement, argument: text)
    }

    // MARK: Pasting

    private var pasteTitle: String {
        FileClipboard.items.count == 1 ? "Paste \(FileClipboard.items[0].lastPathComponent)"
                                       : "Paste \(FileClipboard.items.count) Items"
    }

    /// Moves (after Cut) or copies (after Copy) what is on the clipboard into a folder.
    private func paste(into folder: URL) {
        let manager = FileManager.default
        var done = 0
        var problem: String?
        for source in FileClipboard.items {
            let name = source.lastPathComponent
            // A folder cannot go inside itself
            let sourcePath = source.standardizedFileURL.path
            let folderPath = folder.standardizedFileURL.path
            if folderPath == sourcePath || folderPath.hasPrefix(sourcePath + "/") {
                problem = "\(name) cannot be put inside itself."
                continue
            }
            // Cut and pasted where it already is: nothing to do
            if FileClipboard.isCut, source.deletingLastPathComponent().standardizedFileURL.path == folderPath {
                done += 1
                continue
            }
            let target = Self.uniqueURL(for: name, in: folder)
            do {
                if FileClipboard.isCut {
                    try manager.moveItem(at: source, to: target)
                } else {
                    try manager.copyItem(at: source, to: target)
                }
                done += 1
            } catch {
                problem = "\(name) could not be pasted. \(error.localizedDescription)"
            }
        }
        if FileClipboard.isCut { FileClipboard.set([], cut: false) }  // moved: nothing left to paste
        reload()
        if let problem {
            let alert = UIAlertController(title: "Paste", message: problem, preferredStyle: .alert)
            alert.addAction(UIAlertAction(title: "OK", style: .default))
            present(alert, animated: true)
        } else {
            announce(done == 1 ? "Pasted 1 item" : "Pasted \(done) items")
        }
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        let entry = entries[indexPath.row]
        if entry.isFolder {
            navigationController?.pushViewController(FilesViewController(directory: entry.url), animated: true)
        } else {
            // With its folder, the folder plays on in the order it is shown in here
            let isPlaylist = ["m3u", "m3u8", "pls"].contains(entry.url.pathExtension.lowercased())
            let files = entries.filter { !$0.isFolder }
            if !isPlaylist, engine.number(forSetting: "loadFolder") != 0,
               let index = files.firstIndex(where: { $0.url == entry.url }) {
                engine.playURLs(files.map(\.url.path), names: [], startingAt: index)
            } else {
                engine.playFile(entry.url.path)
            }
            openPlayer()
        }
    }

    override func tableView(_ tableView: UITableView,
                            trailingSwipeActionsConfigurationForRowAt indexPath: IndexPath) -> UISwipeActionsConfiguration? {
        let entry = entries[indexPath.row]
        let delete = UIContextualAction(style: .destructive, title: "Delete") { [weak self] _, _, done in
            self?.confirmDelete(entry, done: done)
        }
        return UISwipeActionsConfiguration(actions: [delete])
    }

    private func confirmDelete(_ entry: Entry, done: @escaping (Bool) -> Void) {
        let what = entry.isFolder ? "the folder \(entry.name) and everything in it" : entry.name
        let alert = UIAlertController(title: "Delete", message: "Delete \(what)? This cannot be undone.",
                                      preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel) { _ in done(false) })
        alert.addAction(UIAlertAction(title: "Delete", style: .destructive) { [weak self] _ in
            let removed = (try? FileManager.default.removeItem(at: entry.url)) != nil
            if removed {
                FileClipboard.items.removeAll { $0 == entry.url }
                self?.announce("Deleted \(entry.name)")
            }
            self?.reload()
            done(removed)
        })
        present(alert, animated: true)
    }
}
