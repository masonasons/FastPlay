import UIKit

/// The first screen: where music comes from, the folders made favorites, and the
/// settings.
final class HomeViewController: FastPlayTableViewController {
    private struct Item {
        let title: String
        let detail: String
        let symbol: String
        let action: (HomeViewController) -> Void
    }

    private let places: [Item] = [
        Item(title: "Files", detail: "On this device, and in the Files app", symbol: "folder") {
            $0.show(BrowserViewController(source: LocalSource.shared))
        },
        Item(title: "Dropbox", detail: "Play from your Dropbox", symbol: "shippingbox") {
            $0.openDropbox()
        },
        Item(title: "Servers", detail: "FTP, SFTP and SMB servers", symbol: "server.rack") {
            $0.show(ServersViewController())
        },
        Item(title: "Radio", detail: "Saved stations and the station directories",
             symbol: "dot.radiowaves.left.and.right") {
            $0.show(RadioViewController())
        },
        Item(title: "Podcasts", detail: "Subscriptions and the podcast directory", symbol: "mic") {
            $0.show(PodcastsViewController())
        },
        Item(title: "Open Address", detail: "A stream, a file or a playlist on the internet", symbol: "link") {
            $0.navigationController?.pushViewController(AddressViewController(), animated: true)
        },
    ]

    private let settings = Item(title: "Settings", detail: "Effects, seeking and speech", symbol: "gearshape") {
        $0.show(SettingsViewController())
    }

    private var favorites: [Favorite] = []
    /// Free space, for the Files and Dropbox rows, once known
    private var space: [String: String] = [:]

    private enum Section { case places, favorites, settings }
    private var sections: [Section] { favorites.isEmpty ? [.places, .settings] : [.places, .favorites, .settings] }

    init() {
        super.init(style: .insetGrouped)
        title = "FastPlay"
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    override func viewDidLoad() {
        super.viewDidLoad()
        tableView.register(UITableViewCell.self, forCellReuseIdentifier: "cell")
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        favorites = FavoritesStore.all
        tableView.reloadData()
        updateSpace()
    }

    /// "12.3 GB free of 128 GB", for this device and Dropbox (if connected).
    private func updateSpace() {
        Task { @MainActor in
            var sources: [FileSource] = [LocalSource.shared]
            if DropboxClient.shared.isSignedIn { sources.append(DropboxClient.shared) }
            for source in sources {
                guard let free = try? await source.storageSpace() else { continue }
                let text = ByteCountFormatter.string(fromByteCount: free.free, countStyle: .file) + " free of "
                    + ByteCountFormatter.string(fromByteCount: free.total, countStyle: .file)
                if space[source.title] != text {
                    space[source.title] = text
                    tableView.reloadData()
                }
            }
        }
    }

    private func show(_ controller: UIViewController) {
        navigationController?.pushViewController(controller, animated: true)
    }

    /// Dropbox, signing in first if FastPlay is not connected yet; then `then`.
    private func withDropbox(_ then: @escaping () -> Void) {
        if DropboxClient.shared.isSignedIn { return then() }
        Task { @MainActor in
            do {
                try await DropboxClient.shared.signIn(from: self)
                UIAccessibility.post(notification: .announcement, argument: "Connected to Dropbox")
                then()
            } catch DropboxError.cancelled {
                // Their choice: nothing to say
            } catch {
                tell("Dropbox", error.localizedDescription)
            }
        }
    }

    private func openDropbox() {
        withDropbox { [weak self] in self?.show(BrowserViewController(source: DropboxClient.shared)) }
    }

    private func open(_ favorite: Favorite) {
        guard let source = FileSources.source(id: favorite.sourceID) else {
            return tell(favorite.name, "The server it was on has been deleted.")
        }
        let browse: () -> Void = { [weak self] in
            self?.show(BrowserViewController(source: source, folder: favorite.folder, isTop: false))
        }
        if source.id == DropboxClient.shared.id { withDropbox(browse) } else { browse() }
    }

    private func remove(_ favorite: Favorite) {
        FavoritesStore.remove(favorite)
        favorites = FavoritesStore.all
        tableView.reloadData()
        UIAccessibility.post(notification: .announcement, argument: "Removed \(favorite.name) from favorites")
    }

    private func tell(_ title: String, _ message: String) {
        let alert = UIAlertController(title: title, message: message, preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "OK", style: .default))
        present(alert, animated: true)
    }

    // MARK: Table

    override func numberOfSections(in tableView: UITableView) -> Int { sections.count }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        switch sections[section] {
        case .places: return places.count
        case .favorites: return favorites.count
        case .settings: return 1
        }
    }

    override func tableView(_ tableView: UITableView, titleForHeaderInSection section: Int) -> String? {
        sections[section] == .favorites ? "Favorites" : nil
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: "cell", for: indexPath)
        var content = UIListContentConfiguration.subtitleCell()
        cell.accessibilityCustomActions = nil
        switch sections[indexPath.section] {
        case .places, .settings:
            let item = sections[indexPath.section] == .settings ? settings : places[indexPath.row]
            content.text = item.title
            content.secondaryText = space[item.title].map { "\(item.detail). \($0)" } ?? item.detail
            content.image = UIImage(systemName: item.symbol)
        case .favorites:
            let favorite = favorites[indexPath.row]
            content.text = favorite.name
            content.secondaryText = FileSources.title(id: favorite.sourceID)
            content.image = UIImage(systemName: "star")
            cell.accessibilityCustomActions = [
                UIAccessibilityCustomAction(name: "Remove from Favorites") { [weak self] _ in
                    self?.remove(favorite)
                    return true
                },
            ]
        }
        cell.contentConfiguration = content
        cell.accessoryType = .disclosureIndicator
        return cell
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        switch sections[indexPath.section] {
        case .places: places[indexPath.row].action(self)
        case .favorites: open(favorites[indexPath.row])
        case .settings: settings.action(self)
        }
    }

    override func tableView(_ tableView: UITableView, contextMenuConfigurationForRowAt indexPath: IndexPath,
                            point: CGPoint) -> UIContextMenuConfiguration? {
        guard sections[indexPath.section] == .favorites else { return nil }
        let favorite = favorites[indexPath.row]
        return UIContextMenuConfiguration(identifier: nil, previewProvider: nil) { [weak self] _ in
            UIMenu(children: [
                UIAction(title: "Remove from Favorites", image: UIImage(systemName: "star.slash"),
                         attributes: .destructive) { _ in self?.remove(favorite) },
            ])
        }
    }

    override func tableView(_ tableView: UITableView,
                            trailingSwipeActionsConfigurationForRowAt indexPath: IndexPath) -> UISwipeActionsConfiguration? {
        guard sections[indexPath.section] == .favorites else { return nil }
        let favorite = favorites[indexPath.row]
        let remove = UIContextualAction(style: .destructive, title: "Remove") { [weak self] _, _, done in
            self?.remove(favorite)
            done(true)
        }
        return UISwipeActionsConfiguration(actions: [remove])
    }
}
