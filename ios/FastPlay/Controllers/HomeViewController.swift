import UIKit

/// The first screen: where music comes from, and the settings.
final class HomeViewController: FastPlayTableViewController {
    private struct Item {
        let title: String
        let detail: String
        let symbol: String
        let action: (HomeViewController) -> Void
    }

    private let sections: [[Item]] = [
        [
            Item(title: "Files", detail: "On this device, and in the Files app", symbol: "folder") {
                $0.show(FilesViewController(directory: URL(fileURLWithPath: $0.engine.documentsPath)))
            },
            Item(title: "Dropbox", detail: "Play from your Dropbox", symbol: "shippingbox") {
                $0.openDropbox()
            },
            Item(title: "Servers", detail: "FTP and SMB servers", symbol: "server.rack") {
                $0.show(ServersViewController())
            },
            Item(title: "Radio", detail: "Saved stations and the station directories",
                 symbol: "dot.radiowaves.left.and.right") {
                $0.show(RadioViewController())
            },
            Item(title: "Podcasts", detail: "Subscriptions and the podcast directory", symbol: "mic") {
                $0.show(PodcastsViewController())
            },
            Item(title: "Open Address", detail: "A stream or a file on the internet", symbol: "link") {
                $0.openAddress()
            },
        ],
        [
            Item(title: "Settings", detail: "Effects, seeking and speech", symbol: "gearshape") {
                $0.show(SettingsViewController())
            },
        ],
    ]

    init() {
        super.init(style: .insetGrouped)
        title = "FastPlay"
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    override func viewDidLoad() {
        super.viewDidLoad()
        tableView.register(UITableViewCell.self, forCellReuseIdentifier: "cell")
    }

    private func show(_ controller: UIViewController) {
        navigationController?.pushViewController(controller, animated: true)
    }

    /// The Dropbox browser, signing in first if FastPlay is not connected yet.
    private func openDropbox() {
        if DropboxClient.shared.isSignedIn {
            show(RemoteBrowserViewController(source: DropboxClient.shared, name: "Dropbox"))
            return
        }
        Task { @MainActor in
            do {
                try await DropboxClient.shared.signIn(from: self)
                UIAccessibility.post(notification: .announcement, argument: "Connected to Dropbox")
                show(RemoteBrowserViewController(source: DropboxClient.shared, name: "Dropbox"))
            } catch DropboxError.cancelled {
                // Their choice: nothing to say
            } catch {
                let alert = UIAlertController(title: "Dropbox", message: error.localizedDescription, preferredStyle: .alert)
                alert.addAction(UIAlertAction(title: "OK", style: .default))
                present(alert, animated: true)
            }
        }
    }

    private func openAddress() {
        let alert = UIAlertController(title: "Open Address", message: "The address of a stream or an audio file.",
                                      preferredStyle: .alert)
        alert.addTextField { field in
            field.placeholder = "https://"
            field.keyboardType = .URL
            field.autocapitalizationType = .none
            field.autocorrectionType = .no
            field.accessibilityLabel = "Address"
        }
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        alert.addAction(UIAlertAction(title: "Play", style: .default) { [weak self, weak alert] _ in
            let text = alert?.textFields?.first?.text?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            guard !text.isEmpty, let self else { return }
            self.engine.playURL(text.contains("://") ? text : "http://" + text, name: nil)
            self.openPlayer()
        })
        present(alert, animated: true)
    }

    // MARK: Table

    override func numberOfSections(in tableView: UITableView) -> Int { sections.count }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        sections[section].count
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let item = sections[indexPath.section][indexPath.row]
        let cell = tableView.dequeueReusableCell(withIdentifier: "cell", for: indexPath)
        var content = UIListContentConfiguration.subtitleCell()
        content.text = item.title
        content.secondaryText = item.detail
        content.image = UIImage(systemName: item.symbol)
        cell.contentConfiguration = content
        cell.accessoryType = .disclosureIndicator
        return cell
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        sections[indexPath.section][indexPath.row].action(self)
    }
}
