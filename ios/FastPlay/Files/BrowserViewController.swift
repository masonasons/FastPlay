import UIKit
import UniformTypeIdentifiers

/// A folder anywhere FastPlay reaches: this device's own files, Dropbox, an FTP,
/// SFTP or SMB server. It lists the folders in it and the files FastPlay plays.
///
/// Choosing a file plays it, with the files after it in the folder; choosing a
/// folder opens it. Each row's actions are VoiceOver actions and in the menu a
/// long press brings up: Cut, Copy, Paste Into Folder, Rename, Share, Add to
/// Favorites, Properties, Select and Delete, and from elsewhere than this device,
/// Download and Sync to This Device. Select puts the list in select mode, where
/// the toolbar acts on everything selected.
///
/// Pasting goes between any two places: from a server to Dropbox, from this
/// device to a server and so on (FileOperations), asking what to do with names
/// already taken.
final class BrowserViewController: FastPlayTableViewController, UIDocumentPickerDelegate {
    /// How many files one choice queues from elsewhere: each may need an address asked for.
    private static let playlistLimit = 50

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

    private typealias Action = (title: String, symbol: String, destructive: Bool, run: () -> Void)

    let source: FileSource
    let folder: FileEntry
    private let isTop: Bool
    private var entries: [FileEntry] = []
    private var hasLoaded = false
    private let statusLabel = UILabel()
    /// Where a range selection starts: the row last selected.
    private var anchor: Int?

    /// A source's top folder, or `path` in it.
    convenience init(source: FileSource, path: String? = nil, name: String? = nil) {
        let path = path ?? source.rootPath
        let isTop = path == source.rootPath
        let name = name ?? (isTop ? source.title : (path as NSString).lastPathComponent)
        self.init(source: source, folder: FileEntry(name: name, path: path, displayPath: path, isFolder: true),
                  isTop: isTop)
    }

    init(source: FileSource, folder: FileEntry, isTop: Bool) {
        self.source = source
        self.folder = folder
        self.isTop = isTop
        super.init(style: .plain)
        title = folder.name
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    private var selecting: Bool { tableView.isEditing }

    override func viewDidLoad() {
        super.viewDidLoad()
        navigationItem.largeTitleDisplayMode = .never
        tableView.register(UITableViewCell.self, forCellReuseIdentifier: "cell")
        tableView.allowsMultipleSelectionDuringEditing = true
        statusLabel.numberOfLines = 0
        statusLabel.textAlignment = .center
        statusLabel.textColor = .secondaryLabel
        statusLabel.font = .preferredFont(forTextStyle: .body)
        statusLabel.adjustsFontForContentSizeCategory = true
        refreshControl = UIRefreshControl()
        refreshControl?.addTarget(self, action: #selector(refreshPulled), for: .valueChanged)
        updateNavigation()
        if !source.isLocal { load() }
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        // This device's files may have changed in the Files app meanwhile
        if source.isLocal { load() }
    }

    // MARK: Listing

    @objc private func refreshPulled() {
        load()
    }

    private func load() {
        if entries.isEmpty { show(status: "Loading…") }
        let selected = Set(selectedEntries.map(\.path))
        Task { @MainActor in
            do {
                // Folders, and the files FastPlay plays
                let listed = try await source.list(path: folder.path)
                entries = sorted(listed.filter { $0.isFolder || engine.isPlayableFile($0.name) })
                show(status: entries.isEmpty ? emptyText : nil)
                if !hasLoaded, !source.isLocal {
                    announce(entries.count == 1 ? "1 item" : "\(entries.count) items")
                }
                hasLoaded = true
                // What was selected stays so
                for (row, entry) in entries.enumerated() where selected.contains(entry.path) {
                    tableView.selectRow(at: IndexPath(row: row, section: 0), animated: false, scrollPosition: .none)
                }
                if selecting { selectionChanged(announcing: false) }
            } catch {
                entries = []
                show(status: error.localizedDescription)
            }
            refreshControl?.endRefreshing()
        }
    }

    private var emptyText: String {
        source.isLocal && isTop
            ? "Nothing here yet.\n\nAdd files with the Add button, or in the Files app: On My iPhone, FastPlay."
            : "Nothing here that FastPlay plays."
    }

    private func show(status: String?) {
        tableView.reloadData()
        statusLabel.text = status
        tableView.backgroundView = status == nil ? nil : statusLabel
    }

    /// Folders first whatever the order. Names as a person orders them ("2" before
    /// "10"), and the name decides between two of the same date, size or type.
    /// Folders have no size or type of their own, so they go by name for those.
    private func sorted(_ list: [FileEntry]) -> [FileEntry] {
        let key = SortKey.current
        let reversed = SortKey.reversed
        return list.sorted { a, b in
            if a.isFolder != b.isFolder { return a.isFolder }
            var order = ComparisonResult.orderedSame
            switch key {
            case .name:
                break
            case .date:
                order = (a.modified ?? .distantPast).compare(b.modified ?? .distantPast)
            case .size:
                if !a.isFolder { order = a.size < b.size ? .orderedAscending : (a.size > b.size ? .orderedDescending : .orderedSame) }
            case .type:
                if !a.isFolder {
                    order = (a.name as NSString).pathExtension.localizedCaseInsensitiveCompare((b.name as NSString).pathExtension)
                }
            }
            if order == .orderedSame { order = a.name.localizedStandardCompare(b.name) }
            return order == (reversed ? .orderedDescending : .orderedAscending)
        }
    }

    private func sortChanged() {
        entries = sorted(entries)
        tableView.reloadData()
        announce("Sorted by \(SortKey.current.title)" + (SortKey.reversed ? ", reversed" : ""))
    }

    /// The date or size of an entry, shown (and spoken) while the list is in that order.
    private func detail(for entry: FileEntry) -> String? {
        switch SortKey.current {
        case .date:
            return entry.modified.map { DateFormatter.localizedString(from: $0, dateStyle: .medium, timeStyle: .short) }
        case .size:
            return entry.isFolder ? nil : ByteCountFormatter.string(fromByteCount: entry.size, countStyle: .file)
        case .name, .type:
            return nil
        }
    }

    // MARK: The bar at the top

    private func updateNavigation() {
        if selecting {
            let done = UIBarButtonItem(barButtonSystemItem: .done, target: self, action: #selector(endSelecting))
            done.accessibilityLabel = "Done selecting"
            navigationItem.rightBarButtonItems = [done]
            return
        }
        var items: [UIBarButtonItem] = []
        let more = UIBarButtonItem(image: UIImage(systemName: "ellipsis.circle"), menu: UIMenu(children: [
            UIDeferredMenuElement.uncached { [weak self] provide in provide(self?.folderMenu() ?? []) },
        ]))
        more.accessibilityLabel = "Folder actions"
        items.append(more)
        if source.canWrite {
            // Built each time it opens, so Paste is there only with something to paste
            let add = UIBarButtonItem(image: UIImage(systemName: "plus"), menu: UIMenu(children: [
                UIDeferredMenuElement.uncached { [weak self] provide in provide(self?.addMenu() ?? []) },
            ]))
            add.accessibilityLabel = "Add"
            items.append(add)
        }
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
        items.append(sort)
        let playAll = UIBarButtonItem(image: UIImage(systemName: "play.circle"), style: .plain, target: self,
                                      action: #selector(playAll))
        playAll.accessibilityLabel = "Play all"
        playAll.accessibilityHint = source.isLocal ? "Plays everything in this folder and the folders inside it"
                                                   : "Plays the files in this folder"
        items.append(playAll)
        navigationItem.rightBarButtonItems = items
    }

    private func addMenu() -> [UIMenuElement] {
        var actions: [UIMenuElement] = [
            UIAction(title: "Import Files", image: UIImage(systemName: "square.and.arrow.down")) { [weak self] _ in
                self?.importFiles()
            },
            UIAction(title: "New Folder", image: UIImage(systemName: "folder.badge.plus")) { [weak self] _ in
                self?.newFolder()
            },
        ]
        if !FileClipboard.items.isEmpty {
            actions.append(UIAction(title: pasteTitle, image: UIImage(systemName: "doc.on.clipboard")) { [weak self] _ in
                guard let self else { return }
                self.paste(into: self.folder.path)
            })
        }
        return actions
    }

    /// The menu for the folder shown.
    private func folderMenu() -> [UIMenuElement] {
        var items: [UIMenuElement] = [
            UIAction(title: "Select", image: UIImage(systemName: "checkmark.circle")) { [weak self] _ in
                self?.startSelecting(row: nil)
            },
        ]
        let favorite = FavoritesStore.find(source: source, path: folder.path)
        items.append(UIAction(title: favorite == nil ? "Add to Favorites" : "Remove from Favorites",
                              image: UIImage(systemName: favorite == nil ? "star" : "star.slash")) { [weak self] _ in
            guard let self else { return }
            self.toggleFavorite(self.folder)
        })
        items.append(UIAction(title: "Properties", image: UIImage(systemName: "info.circle")) { [weak self] _ in
            guard let self else { return }
            self.showProperties(self.folder)
        })
        if !source.isLocal {
            items.append(UIAction(title: "Download This Folder", image: UIImage(systemName: "arrow.down.circle")) { [weak self] _ in
                guard let self else { return }
                self.transfer(self.folder, sync: false)
            })
            items.append(UIAction(title: "Sync This Folder", image: UIImage(systemName: "arrow.triangle.2.circlepath")) { [weak self] _ in
                guard let self else { return }
                self.transfer(self.folder, sync: true)
            })
        }
        if isTop, let account = source.accountAction {
            items.append(UIAction(title: account.title, attributes: .destructive) { [weak self] _ in
                guard let self else { return }
                account.run(self)
            })
        }
        return items
    }

    // MARK: Playing

    @objc private func playAll() {
        if source.isLocal {
            engine.playFolder(folder.path)
            if engine.trackCount > 0 { openPlayer() }
        } else if let first = entries.firstIndex(where: { !$0.isFolder }) {
            play(from: first)
        } else {
            announce("No files here to play")
        }
    }

    private func play(from index: Int) {
        let entry = entries[index]
        if source.isLocal {
            // With its folder, the folder plays on in the order it is shown in here
            let isPlaylist = ["m3u", "m3u8", "pls"].contains((entry.name as NSString).pathExtension.lowercased())
            let files = entries.filter { !$0.isFolder }
            if !isPlaylist, engine.number(forSetting: "loadFolder") != 0,
               let start = files.firstIndex(where: { $0.path == entry.path }) {
                engine.playURLs(files.map(\.path), names: [], startingAt: start)
            } else {
                engine.playFile(entry.path)
            }
            openPlayer()
            return
        }
        if ["m3u", "m3u8", "pls"].contains((entry.name as NSString).pathExtension.lowercased()) {
            playRemotePlaylist(entry)
            return
        }
        // The file chosen and those after it in the folder
        let files = entries[index...].filter { !$0.isFolder }.prefix(Self.playlistLimit)
        announce("Opening \(entry.name)")
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
                tell(folder.name, error.localizedDescription)
            }
        }
    }

    /// A playlist file on a server or in Dropbox: its entries, played from there.
    /// (Given to the engine as an address, a playlist is taken for a radio station's,
    /// and only its first entry plays.) An address plays as it is; a file is looked
    /// for under the playlist's folder, by as much of the end of its written path as
    /// matches, down to its name alone, since a playlist written on a computer
    /// names files by that computer's paths.
    private func playRemotePlaylist(_ playlist: FileEntry) {
        announce("Opening \(playlist.name)")
        Task { @MainActor in
            do {
                let copy = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
                defer { try? FileManager.default.removeItem(at: copy) }
                try await source.download(playlist, to: copy)
                let data = try Data(contentsOf: copy)
                let text = String(data: data, encoding: .utf8) ?? String(data: data, encoding: .isoLatin1) ?? ""
                let written = Self.playlistEntries(in: text, isPLS: playlist.name.lowercased().hasSuffix(".pls"))
                guard !written.isEmpty else {
                    tell(playlist.name, "The playlist has no entries.")
                    return
                }
                // Every file under the folder, found by the end of its path or its name
                let below = try await source.listAll(path: folder.path).filter { !$0.isFolder }
                let prefix = folder.displayPath.isEmpty ? "" : folder.displayPath + "/"
                var byPath: [String: FileEntry] = [:]
                var byName: [String: FileEntry] = [:]
                for file in below {
                    var relative = file.displayPath
                    if relative.lowercased().hasPrefix(prefix.lowercased()) { relative.removeFirst(prefix.count) }
                    byPath[relative.lowercased()] = file
                    byName[file.name.lowercased()] = file
                }
                var urls: [String] = []
                var names: [String] = []
                var missing = 0
                for item in written.prefix(Self.playlistLimit) {
                    if item.contains("://") {
                        urls.append(item)
                        names.append((item as NSString).lastPathComponent.removingPercentEncoding ?? item)
                        continue
                    }
                    let parts = item.replacingOccurrences(of: "\\", with: "/").split(separator: "/").map(String.init)
                        .filter { !$0.hasSuffix(":") }  // a Windows drive
                    var found: FileEntry?
                    for first in parts.indices {
                        if let file = byPath[parts[first...].joined(separator: "/").lowercased()] {
                            found = file
                            break
                        }
                    }
                    if found == nil, let last = parts.last { found = byName[last.lowercased()] }
                    guard let file = found else {
                        missing += 1
                        continue
                    }
                    urls.append(try await source.streamURL(for: file))
                    names.append(file.name)
                }
                guard !urls.isEmpty else {
                    tell(playlist.name, "None of the playlist's files are here.")
                    return
                }
                engine.playURLs(urls, names: names, startingAt: 0)
                openPlayer()
                if missing > 0 {
                    announce(missing == 1 ? "1 file of the playlist is not here" : "\(missing) files of the playlist are not here")
                }
            } catch {
                tell(playlist.name, error.localizedDescription)
            }
        }
    }

    /// The entries written in a playlist: an M3U's lines that are not comments, or a
    /// PLS's File1=, File2=... in order.
    static func playlistEntries(in text: String, isPLS: Bool) -> [String] {
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

    // MARK: Rows

    /// What can be done with a row, besides playing it or going into it.
    private func actions(for entry: FileEntry, at row: Int) -> [Action] {
        if selecting {
            let isSelected = tableView.indexPathsForSelectedRows?.contains(IndexPath(row: row, section: 0)) ?? false
            return [
                (isSelected ? "Deselect" : "Select", isSelected ? "circle" : "checkmark.circle", false,
                 { [weak self] in self?.toggleSelection(row) }),
                ("Select Up to Here", "checkmark.circle.badge.plus", false, { [weak self] in self?.selectRange(to: row) }),
                ("Done Selecting", "xmark.circle", false, { [weak self] in self?.endSelecting() }),
            ]
        }
        var list: [Action] = [
            ("Cut", "scissors", false, { [weak self] in self?.putOnClipboard([entry], cut: true) }),
            ("Copy", "doc.on.doc", false, { [weak self] in self?.putOnClipboard([entry], cut: false) }),
        ]
        if entry.isFolder, source.canWrite, !FileClipboard.items.isEmpty {
            list.append(("Paste Into Folder", "doc.on.clipboard", false, { [weak self] in self?.paste(into: entry.path) }))
        }
        if source.canWrite {
            list.append(("Rename", "pencil", false, { [weak self] in self?.askToRename(entry) }))
        }
        if source.isLocal || !entry.isFolder {
            list.append(("Share", "square.and.arrow.up", false, { [weak self] in self?.share([entry], row: row) }))
        }
        if entry.isFolder {
            let isFavorite = FavoritesStore.find(source: source, path: entry.path) != nil
            list.append((isFavorite ? "Remove from Favorites" : "Add to Favorites", isFavorite ? "star.slash" : "star",
                         false, { [weak self] in self?.toggleFavorite(entry) }))
        }
        list.append(("Properties", "info.circle", false, { [weak self] in self?.showProperties(entry) }))
        if !source.isLocal {
            list.append((entry.isFolder ? "Download Folder to This Device" : "Download to This Device", "arrow.down.circle",
                         false, { [weak self] in self?.transfer(entry, sync: false) }))
            if entry.isFolder {
                list.append(("Sync Folder to This Device", "arrow.triangle.2.circlepath", false,
                             { [weak self] in self?.transfer(entry, sync: true) }))
            }
        }
        list.append(("Select", "checkmark.circle", false, { [weak self] in self?.startSelecting(row: row) }))
        if source.canWrite {
            list.append(("Delete", "trash", true, { [weak self] in self?.confirmDelete([entry]) }))
        }
        return list
    }

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
        cell.accessibilityCustomActions = actions(for: entry, at: indexPath.row).map { action in
            UIAccessibilityCustomAction(name: action.title) { _ in
                action.run()
                return true
            }
        }
        return cell
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        if selecting {
            anchor = indexPath.row
            selectionChanged(announcing: true)
            return
        }
        tableView.deselectRow(at: indexPath, animated: true)
        let entry = entries[indexPath.row]
        if entry.isFolder {
            navigationController?.pushViewController(BrowserViewController(source: source, folder: entry, isTop: false),
                                                     animated: true)
        } else {
            play(from: indexPath.row)
        }
    }

    override func tableView(_ tableView: UITableView, didDeselectRowAt indexPath: IndexPath) {
        if selecting { selectionChanged(announcing: true) }
    }

    override func tableView(_ tableView: UITableView, contextMenuConfigurationForRowAt indexPath: IndexPath,
                            point: CGPoint) -> UIContextMenuConfiguration? {
        let entry = entries[indexPath.row]
        let row = indexPath.row
        return UIContextMenuConfiguration(identifier: nil, previewProvider: nil) { [weak self] _ in
            guard let self else { return nil }
            return UIMenu(children: self.actions(for: entry, at: row).map { action in
                UIAction(title: action.title, image: UIImage(systemName: action.symbol),
                         attributes: action.destructive ? .destructive : []) { _ in action.run() }
            })
        }
    }

    override func tableView(_ tableView: UITableView,
                            trailingSwipeActionsConfigurationForRowAt indexPath: IndexPath) -> UISwipeActionsConfiguration? {
        let entry = entries[indexPath.row]
        var swipes: [UIContextualAction] = []
        if source.canWrite {
            swipes.append(UIContextualAction(style: .destructive, title: "Delete") { [weak self] _, _, done in
                self?.confirmDelete([entry])
                done(true)
            })
        }
        if !source.isLocal {
            let download = UIContextualAction(style: .normal, title: "Download") { [weak self] _, _, done in
                self?.transfer(entry, sync: false)
                done(true)
            }
            download.backgroundColor = .systemBlue
            swipes.append(download)
        }
        return swipes.isEmpty ? nil : UISwipeActionsConfiguration(actions: swipes)
    }

    // MARK: Selecting

    private var selectedEntries: [FileEntry] {
        (tableView.indexPathsForSelectedRows ?? []).map(\.row).sorted().filter { $0 < entries.count }.map { entries[$0] }
    }

    private func startSelecting(row: Int?) {
        guard !selecting else {
            if let row { toggleSelection(row) }
            return
        }
        setEditing(true, animated: true)
        if let row {
            tableView.selectRow(at: IndexPath(row: row, section: 0), animated: false, scrollPosition: .none)
            anchor = row
        }
        updateNavigation()
        tableView.reloadData()  // the rows' actions are the selecting ones now
        if let row { tableView.selectRow(at: IndexPath(row: row, section: 0), animated: false, scrollPosition: .none) }
        selectionChanged(announcing: false)
        announce(row == nil ? "Selecting. Choose items." : "Selecting. \(entries[row!].name) selected.")
    }

    @objc private func endSelecting() {
        setEditing(false, animated: true)
        anchor = nil
        title = folder.name
        ownToolbarItems = nil
        updateNavigation()
        tableView.reloadData()
        announce("Done selecting")
    }

    private func toggleSelection(_ row: Int) {
        let path = IndexPath(row: row, section: 0)
        if tableView.indexPathsForSelectedRows?.contains(path) ?? false {
            tableView.deselectRow(at: path, animated: true)
        } else {
            tableView.selectRow(at: path, animated: true, scrollPosition: .none)
            anchor = row
        }
        selectionChanged(announcing: true)
    }

    /// Everything from the row last selected to this one.
    private func selectRange(to row: Int) {
        let from = anchor ?? row
        for index in min(from, row)...max(from, row) {
            tableView.selectRow(at: IndexPath(row: index, section: 0), animated: false, scrollPosition: .none)
        }
        anchor = row
        selectionChanged(announcing: true)
    }

    @objc private func toggleSelectAll() {
        let all = (tableView.indexPathsForSelectedRows?.count ?? 0) == entries.count
        for row in entries.indices {
            let path = IndexPath(row: row, section: 0)
            if all { tableView.deselectRow(at: path, animated: false) } else {
                tableView.selectRow(at: path, animated: false, scrollPosition: .none)
            }
        }
        selectionChanged(announcing: true)
    }

    /// The count in the title and the toolbar for it; the rows' actions follow.
    private func selectionChanged(announcing: Bool) {
        let count = tableView.indexPathsForSelectedRows?.count ?? 0
        let text = count == 1 ? "1 selected" : "\(count) selected"
        title = text
        let any = count > 0
        func button(_ title: String, _ action: Selector) -> UIBarButtonItem {
            let item = UIBarButtonItem(title: title, style: .plain, target: self, action: action)
            item.isEnabled = any
            return item
        }
        var items = [button("Cut", #selector(cutSelected)), button("Copy", #selector(copySelected))]
        if source.isLocal || selectedEntries.contains(where: { !$0.isFolder }) {
            items.append(button("Share", #selector(shareSelected)))
        }
        if source.canWrite { items.append(button("Delete", #selector(deleteSelected))) }
        items.append(.flexibleSpace())
        let all = UIBarButtonItem(title: count == entries.count && count > 0 ? "Deselect All" : "Select All",
                                  style: .plain, target: self, action: #selector(toggleSelectAll))
        items.append(all)
        ownToolbarItems = items
        if let visible = tableView.indexPathsForVisibleRows { tableView.reconfigureRows(at: visible) }
        if announcing { announce(text) }
    }

    @objc private func cutSelected() { putOnClipboard(selectedEntries, cut: true) }
    @objc private func copySelected() { putOnClipboard(selectedEntries, cut: false) }
    @objc private func shareSelected() { share(selectedEntries, row: nil) }
    @objc private func deleteSelected() { confirmDelete(selectedEntries) }

    // MARK: Clipboard and pasting

    private func putOnClipboard(_ list: [FileEntry], cut: Bool) {
        guard !list.isEmpty else { return }
        FileClipboard.set(list.map { FileClipboard.Item(source: source, entry: $0) }, cut: cut)
        let what = list.count == 1 ? list[0].name : "\(list.count) items"
        announce((cut ? "Cut " : "Copied ") + what)
        if selecting { endSelecting() }
    }

    private var pasteTitle: String {
        FileClipboard.items.count == 1 ? "Paste \(FileClipboard.items[0].entry.name)"
                                       : "Paste \(FileClipboard.items.count) Items"
    }

    /// Moves (after Cut) or copies (after Copy) what is on the clipboard into a folder here.
    private func paste(into path: String) {
        let items = FileClipboard.items
        guard !items.isEmpty else { return }
        let move = FileClipboard.isCut
        run(items, move: move, into: path, title: move ? "Moving" : "Pasting") { [weak self] finished in
            guard finished else { return }
            if move { FileClipboard.set([], cut: false) }  // moved: nothing left to paste
            self?.announce(items.count == 1 ? "Pasted \(items[0].entry.name)" : "Pasted \(items.count) items")
        }
    }

    /// Copies or moves `items` into `path`, with a progress alert that can stop it,
    /// and the question about names already taken. `done` is told whether it all went.
    private func run(_ items: [FileClipboard.Item], move: Bool, into path: String, title: String,
                     done: @escaping (Bool) -> Void) {
        let progress = UIAlertController(title: title, message: "Starting…", preferredStyle: .alert)
        let state = ProgressState()
        let task = Task { @MainActor in
            let operations = FileOperations(move: move, resolver: { [weak self] pasting, existing in
                guard let self else { return nil }
                // The progress is in the way: aside while asking
                state.paused = true
                await Self.dismiss(progress)
                let choice = await ConflictViewController.ask(from: self, pasting: pasting, existing: existing)
                state.paused = false
                if choice != nil, !Task.isCancelled { self.present(progress, animated: false) }
                return choice
            }, progress: { name, count in
                progress.message = "\(count + 1): \(name)"
            })
            var problem: String?
            do {
                try await operations.paste(items, into: source, folder: path)
            } catch is CancellationError {
                problem = "Stopped."
            } catch is FileOperations.Stopped {
                problem = "Stopped."
            } catch {
                problem = Self.describe(error)
            }
            state.finished = true
            await Self.dismiss(progress)
            load()
            if let problem { tell(title == "Moving" ? "Move" : "Paste", problem) }
            done(problem == nil)
        }
        progress.addAction(UIAlertAction(title: "Stop", style: .cancel) { _ in task.cancel() })
        showSoon(progress, state)
    }

    /// Where a piece of work with a progress alert has got to.
    private final class ProgressState {
        var finished = false
        var paused = false  // something else is being asked meanwhile
    }

    /// Shows `progress` once the work has taken more than a moment: quick work
    /// flashes nothing, and nothing is dismissed while it is still appearing.
    private func showSoon(_ progress: UIAlertController, _ state: ProgressState) {
        Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 300_000_000)
            guard let self, !state.finished, !state.paused, progress.presentingViewController == nil,
                  self.presentedViewController == nil else { return }
            self.present(progress, animated: false)
        }
    }

    @MainActor
    private static func dismiss(_ controller: UIViewController) async {
        guard controller.presentingViewController != nil else { return }
        await withCheckedContinuation { continuation in
            controller.dismiss(animated: false) { continuation.resume() }
        }
    }

    private static func describe(_ error: Error) -> String {
        let nsError = error as NSError
        if (error as? URLError)?.code == .cancelled
            || (nsError.domain == NSCocoaErrorDomain && nsError.code == NSUserCancelledError) {
            return "Stopped."
        }
        return error.localizedDescription
    }

    // MARK: Adding

    private func importFiles() {
        let picker = UIDocumentPickerViewController(forOpeningContentTypes: [.audio, .item], asCopy: true)
        picker.allowsMultipleSelection = true
        picker.delegate = self
        present(picker, animated: true)
    }

    func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        // The copies the picker made, moved (or uploaded) into this folder
        let items = urls.map { url -> FileClipboard.Item in
            let size = (try? url.resourceValues(forKeys: [.fileSizeKey]))?.fileSize ?? 0
            return FileClipboard.Item(source: LocalSource.shared,
                                      entry: FileEntry(name: url.lastPathComponent, path: url.path, displayPath: url.path,
                                                       isFolder: false, size: Int64(size), modified: Date()))
        }
        run(items, move: true, into: folder.path, title: "Importing") { [weak self] finished in
            guard finished else { return }
            self?.announce(urls.count == 1 ? "1 file added" : "\(urls.count) files added")
        }
    }

    private func newFolder() {
        askForName(title: "New Folder", initial: "", action: "Create") { [weak self] name in
            guard let self else { return }
            Task { @MainActor in
                do {
                    try await self.source.createFolder(named: name, in: self.folder.path)
                    self.load()
                    self.announce("Made \(name)")
                } catch {
                    self.tell("New Folder", Self.describe(error))
                }
            }
        }
    }

    private func askForName(title: String, initial: String, action: String, named: @escaping (String) -> Void) {
        let alert = UIAlertController(title: title, message: nil, preferredStyle: .alert)
        alert.addTextField { field in
            field.text = initial
            field.placeholder = "Name"
            field.autocapitalizationType = .words
            field.accessibilityLabel = "Name"
        }
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        alert.addAction(UIAlertAction(title: action, style: .default) { [weak alert] _ in
            let name = alert?.textFields?.first?.text?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            guard !name.isEmpty, !name.contains("/"), name != initial else { return }
            named(name)
        })
        present(alert, animated: true) {
            // The name without its extension selected, ready to type over
            guard let field = alert.textFields?.first, !initial.isEmpty else { return }
            let base = (initial as NSString).deletingPathExtension
            if let start = field.position(from: field.beginningOfDocument, offset: 0),
               let end = field.position(from: start, offset: base.count) {
                field.selectedTextRange = field.textRange(from: start, to: end)
            }
        }
    }

    // MARK: Renaming, deleting, sharing

    private func askToRename(_ entry: FileEntry) {
        askForName(title: "Rename", initial: entry.name, action: "Rename") { [weak self] name in
            guard let self else { return }
            Task { @MainActor in
                do {
                    try await self.source.rename(entry, to: name)
                    FileClipboard.forget(source: self.source, path: entry.path)
                    if entry.isFolder, var favorite = FavoritesStore.find(source: self.source, path: entry.path) {
                        // The favorite follows the folder
                        FavoritesStore.remove(favorite)
                        let renamed = self.source.entry(named: name, in: FileOperations.parent(of: entry.path),
                                                        isFolder: true)
                        favorite.path = renamed.path
                        favorite.displayPath = renamed.displayPath
                        favorite.name = name
                        FavoritesStore.all.append(favorite)
                    }
                    self.load()
                    self.announce("Renamed to \(name)")
                } catch {
                    self.tell("Rename", Self.describe(error))
                }
            }
        }
    }

    private func confirmDelete(_ list: [FileEntry]) {
        guard !list.isEmpty else { return }
        let what: String
        if list.count == 1 {
            what = list[0].isFolder ? "the folder \(list[0].name) and everything in it" : list[0].name
        } else {
            what = "\(list.count) items" + (list.contains(where: \.isFolder) ? " and everything in the folders" : "")
        }
        let place = source.isLocal ? "" : " from \(source.title)"
        let alert = UIAlertController(title: "Delete", message: "Delete \(what)\(place)? This cannot be undone.",
                                      preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        alert.addAction(UIAlertAction(title: "Delete", style: .destructive) { [weak self] _ in
            self?.performDelete(list)
        })
        present(alert, animated: true)
    }

    private func performDelete(_ list: [FileEntry]) {
        let progress = UIAlertController(title: "Deleting", message: nil, preferredStyle: .alert)
        let state = ProgressState()
        let task = Task { @MainActor in
            var deleted = 0
            var problem: String?
            for (index, entry) in list.enumerated() {
                if Task.isCancelled { break }
                progress.message = "\(index + 1) of \(list.count): \(entry.name)"
                do {
                    try await source.delete(entry)
                    FileClipboard.forget(source: source, path: entry.path)
                    if let favorite = FavoritesStore.find(source: source, path: entry.path) { FavoritesStore.remove(favorite) }
                    deleted += 1
                } catch {
                    problem = "\(entry.name) could not be deleted. \(Self.describe(error))"
                    break
                }
            }
            state.finished = true
            await Self.dismiss(progress)
            if selecting { endSelecting() }
            load()
            if let problem { tell("Delete", problem) } else {
                announce(deleted == 1 ? "Deleted \(list[0].name)" : "Deleted \(deleted) items")
            }
        }
        progress.addAction(UIAlertAction(title: "Stop", style: .cancel) { _ in task.cancel() })
        showSoon(progress, state)
    }

    /// The share sheet for files (and, on this device, folders). From elsewhere, they
    /// are downloaded first.
    private func share(_ list: [FileEntry], row: Int?) {
        let anchorView: UIView = row.flatMap { tableView.cellForRow(at: IndexPath(row: $0, section: 0)) } ?? view
        if source.isLocal {
            presentShareSheet(list.map { URL(fileURLWithPath: $0.path) }, from: anchorView, cleanUp: nil)
            return
        }
        let files = list.filter { !$0.isFolder }
        guard !files.isEmpty else { return tell("Share", "Folders from \(source.title) cannot be shared.") }
        let progress = UIAlertController(title: "Getting Ready to Share", message: nil, preferredStyle: .alert)
        let holder = FileManager.default.temporaryDirectory.appendingPathComponent("Sharing-\(UUID().uuidString)")
        let state = ProgressState()
        let task = Task { @MainActor in
            var urls: [URL] = []
            do {
                try FileManager.default.createDirectory(at: holder, withIntermediateDirectories: true)
                for (index, file) in files.enumerated() {
                    progress.message = "\(index + 1) of \(files.count): \(file.name)"
                    let url = holder.appendingPathComponent(file.name)
                    try await source.download(file, to: url)
                    urls.append(url)
                }
                state.finished = true
                await Self.dismiss(progress)
                presentShareSheet(urls, from: anchorView) { try? FileManager.default.removeItem(at: holder) }
            } catch {
                state.finished = true
                await Self.dismiss(progress)
                try? FileManager.default.removeItem(at: holder)
                tell("Share", Self.describe(error))
            }
        }
        progress.addAction(UIAlertAction(title: "Stop", style: .cancel) { _ in task.cancel() })
        showSoon(progress, state)
    }

    private func presentShareSheet(_ urls: [URL], from anchorView: UIView, cleanUp: (() -> Void)?) {
        let sheet = UIActivityViewController(activityItems: urls, applicationActivities: nil)
        sheet.completionWithItemsHandler = { _, _, _, _ in cleanUp?() }
        // An iPad shows it as a popover, which needs somewhere to point
        sheet.popoverPresentationController?.sourceView = anchorView
        sheet.popoverPresentationController?.sourceRect = anchorView.bounds
        present(sheet, animated: true)
    }

    // MARK: Favorites and properties

    private func toggleFavorite(_ entry: FileEntry) {
        if let favorite = FavoritesStore.find(source: source, path: entry.path) {
            FavoritesStore.remove(favorite)
            announce("Removed \(entry.name) from favorites")
        } else {
            FavoritesStore.add(source: source, folder: entry)
            announce("Added \(entry.name) to favorites")
        }
        if let visible = tableView.indexPathsForVisibleRows { tableView.reconfigureRows(at: visible) }
    }

    /// Where it is, as the user knows it: "Files/Music/Album", "Dropbox/Music".
    private func location(of entry: FileEntry) -> String {
        var below = entry.displayPath
        if source.isLocal, below.hasPrefix(source.rootPath) { below.removeFirst(source.rootPath.count) }
        below = below.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        let parent = (below as NSString).deletingLastPathComponent
        return parent.isEmpty ? source.title : source.title + "/" + parent
    }

    private func showProperties(_ entry: FileEntry) {
        var lines: [String] = []
        let ext = (entry.name as NSString).pathExtension
        lines.append("Kind: " + (entry.isFolder ? "Folder" : (ext.isEmpty ? "File" : "\(ext.uppercased()) file")))
        if !entry.isFolder { lines.append("Size: " + ByteCountFormatter.string(fromByteCount: entry.size, countStyle: .file)) }
        if let modified = entry.modified {
            lines.append("Modified: " + DateFormatter.localizedString(from: modified, dateStyle: .long, timeStyle: .short))
        }
        let isThisTop = entry.path == source.rootPath
        if !isThisTop { lines.append("Where: " + location(of: entry)) }
        let counting = "Size: counting…"
        if entry.isFolder { lines.append(counting) }
        let alert = UIAlertController(title: entry.name, message: lines.joined(separator: "\n"), preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "Close", style: .cancel))
        present(alert, animated: true)
        guard entry.isFolder else { return }
        Task { @MainActor in
            var more: [String] = []
            do {
                let files = try await source.listAll(path: entry.path)
                let total = files.reduce(Int64(0)) { $0 + $1.size }
                more.append("Size: " + ByteCountFormatter.string(fromByteCount: total, countStyle: .file)
                            + (files.count == 1 ? ", 1 file" : ", \(files.count) files"))
            } catch {
                more.append("Size: could not be counted")
            }
            if isThisTop, let space = try? await source.storageSpace() {
                more.append("Space: \(ByteCountFormatter.string(fromByteCount: space.free, countStyle: .file)) free of "
                            + ByteCountFormatter.string(fromByteCount: space.total, countStyle: .file))
            }
            guard alert.presentingViewController != nil else { return }
            alert.message = lines.map { $0 == counting ? more.joined(separator: "\n") : $0 }.joined(separator: "\n")
            UIAccessibility.post(notification: .layoutChanged, argument: nil)
        }
    }

    // MARK: Downloading and syncing (from elsewhere to this device)

    /// Copies a file, or a folder with everything in it that FastPlay plays, into
    /// FastPlay's own files, keeping the folders as they are there.
    ///
    /// Download leaves alone what is already here. Sync makes the folder here match
    /// the one there: files that changed are fetched again, and files here that are
    /// no longer there are deleted (after asking).
    private func transfer(_ entry: FileEntry, sync: Bool) {
        let progress = UIAlertController(title: "\(sync ? "Syncing" : "Downloading") \(entry.name)",
                                         message: "Looking…", preferredStyle: .alert)
        let task = Task { @MainActor in
            var outcome: String
            do {
                let manager = FileManager.default
                let documents = URL(fileURLWithPath: engine.documentsPath)
                let root = documents.appendingPathComponent(entry.name)

                // What is there, and where each file goes here
                var wanted: [(file: FileEntry, destination: URL)] = []
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
                outcome = Self.describe(error)
            }
            let show: () -> Void = { [weak self] in self?.tell(entry.name, outcome) }
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

    // MARK: Telling

    private func announce(_ text: String) {
        UIAccessibility.post(notification: .announcement, argument: text)
    }

    private func tell(_ title: String, _ message: String) {
        let alert = UIAlertController(title: title, message: message, preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "OK", style: .default))
        present(alert, animated: true)
    }
}
