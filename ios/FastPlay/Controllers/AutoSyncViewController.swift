import UIKit

/// Settings > Auto Sync: the folders that sync to this device by themselves each
/// time FastPlay starts, with how each one's last sync went, to sync now or remove.
final class AutoSyncViewController: UITableViewController {
    private var folders = AutoSyncStore.all
    private var observer: NSObjectProtocol?

    init() {
        super.init(style: .insetGrouped)
        title = "Auto Sync"
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    deinit {
        if let observer { NotificationCenter.default.removeObserver(observer) }
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        navigationItem.largeTitleDisplayMode = .never
        tableView.register(UITableViewCell.self, forCellReuseIdentifier: "cell")
        observer = NotificationCenter.default.addObserver(forName: .autoSyncDidFinish, object: nil, queue: .main) { [weak self] _ in
            self?.reload()
        }
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        navigationController?.setToolbarHidden(true, animated: animated)
        reload()
    }

    private func reload() {
        folders = AutoSyncStore.all
        navigationItem.rightBarButtonItem = folders.isEmpty ? nil
            : UIBarButtonItem(title: "Sync Now", style: .plain, target: self, action: #selector(syncNow))
        tableView.reloadData()
    }

    @objc private func syncNow() {
        UIAccessibility.post(notification: .announcement, argument: "Syncing")
        Task { @MainActor in await AutoSync.runAll() }
    }

    private func remove(at index: Int) {
        let folder = folders[index]
        AutoSyncStore.remove(folder)
        reload()
        UIAccessibility.post(notification: .announcement, argument: "Stopped auto syncing \(folder.name)")
    }

    private func detail(for folder: AutoSyncFolder) -> String {
        var lines = ["\(FileSources.title(id: folder.sourceID)): \(folder.displayPath.isEmpty ? "/" : folder.displayPath)"]
        if let date = folder.lastSynced {
            let when = DateFormatter.localizedString(from: date, dateStyle: .medium, timeStyle: .short)
            lines.append("Last synced \(when). \(folder.lastOutcome ?? "")")
        } else {
            lines.append("Not synced yet.")
        }
        return lines.joined(separator: "\n")
    }

    // MARK: Table

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        folders.count
    }

    override func tableView(_ tableView: UITableView, titleForFooterInSection section: Int) -> String? {
        folders.isEmpty
            ? "No folders sync by themselves yet. In Dropbox or on a server, choose Auto Sync This Folder on a folder, "
                + "and each time FastPlay starts it will be synced to this device, like Sync Folder to This Device."
            : "Each of these is synced to this device when FastPlay starts, and when it comes back to the front after "
                + "half an hour or more away: new and changed files are downloaded, and files no longer there are "
                + "deleted from this device, without asking. The copies are in FastPlay's files, under the folder's name."
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let folder = folders[indexPath.row]
        let cell = tableView.dequeueReusableCell(withIdentifier: "cell", for: indexPath)
        var content = UIListContentConfiguration.subtitleCell()
        content.text = folder.name
        content.secondaryText = detail(for: folder)
        content.secondaryTextProperties.numberOfLines = 0
        content.image = UIImage(systemName: "clock.arrow.2.circlepath")
        cell.contentConfiguration = content
        cell.selectionStyle = .none
        cell.accessibilityLabel = "\(folder.name). \(detail(for: folder))"
        cell.accessibilityCustomActions = [
            UIAccessibilityCustomAction(name: "Remove") { [weak self] _ in
                self?.remove(at: indexPath.row)
                return true
            },
        ]
        return cell
    }

    override func tableView(_ tableView: UITableView,
                            trailingSwipeActionsConfigurationForRowAt indexPath: IndexPath) -> UISwipeActionsConfiguration? {
        UISwipeActionsConfiguration(actions: [
            UIContextualAction(style: .destructive, title: "Remove") { [weak self] _, _, done in
                self?.remove(at: indexPath.row)
                done(true)
            },
        ])
    }
}
