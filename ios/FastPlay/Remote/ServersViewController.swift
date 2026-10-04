import UIKit

/// The FTP and SMB servers added: choose one to browse it. Edit and Delete are
/// VoiceOver actions on each, in the long press menu, and swipes.
final class ServersViewController: FastPlayTableViewController {
    private var servers: [RemoteServer] = []
    private let emptyLabel = UILabel()

    init() {
        super.init(style: .insetGrouped)
        title = "Servers"
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    override func viewDidLoad() {
        super.viewDidLoad()
        navigationItem.largeTitleDisplayMode = .never
        tableView.register(UITableViewCell.self, forCellReuseIdentifier: "cell")
        let add = UIBarButtonItem(barButtonSystemItem: .add, target: self, action: #selector(addServer))
        add.accessibilityLabel = "Add server"
        navigationItem.rightBarButtonItem = add

        emptyLabel.text = "No servers yet.\n\nAdd an FTP or SMB server with the Add button."
        emptyLabel.numberOfLines = 0
        emptyLabel.textAlignment = .center
        emptyLabel.textColor = .secondaryLabel
        emptyLabel.font = .preferredFont(forTextStyle: .body)
        emptyLabel.adjustsFontForContentSizeCategory = true
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        reload()
    }

    private func reload() {
        servers = ServerStore.all
        tableView.reloadData()
        tableView.backgroundView = servers.isEmpty ? emptyLabel : nil
    }

    @objc private func addServer() {
        edit(RemoteServer(), isNew: true)
    }

    private func edit(_ server: RemoteServer, isNew: Bool) {
        let editor = ServerEditViewController(server: server, isNew: isNew)
        present(UINavigationController(rootViewController: editor), animated: true)
        editor.onDone = { [weak self] in self?.reload() }
    }

    private func delete(_ server: RemoteServer) {
        let alert = UIAlertController(title: "Delete \(server.displayName)",
                                      message: "FastPlay will forget this server and its password.",
                                      preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        alert.addAction(UIAlertAction(title: "Delete", style: .destructive) { [weak self] _ in
            ServerStore.remove(server)
            self?.reload()
            UIAccessibility.post(notification: .announcement, argument: "Deleted \(server.displayName)")
        })
        present(alert, animated: true)
    }

    // MARK: Table

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        servers.count
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let server = servers[indexPath.row]
        let cell = tableView.dequeueReusableCell(withIdentifier: "cell", for: indexPath)
        var content = UIListContentConfiguration.subtitleCell()
        content.text = server.displayName
        content.secondaryText = "\(server.kind.title), \(server.host)"
        content.image = UIImage(systemName: "server.rack")
        cell.contentConfiguration = content
        cell.accessoryType = .disclosureIndicator
        cell.accessibilityCustomActions = [
            UIAccessibilityCustomAction(name: "Edit") { [weak self] _ in
                self?.edit(server, isNew: false)
                return true
            },
            UIAccessibilityCustomAction(name: "Delete") { [weak self] _ in
                self?.delete(server)
                return true
            },
        ]
        return cell
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        let server = servers[indexPath.row]
        navigationController?.pushViewController(
            RemoteBrowserViewController(source: server.makeSource(), name: server.displayName, path: server.startPath),
            animated: true)
    }

    override func tableView(_ tableView: UITableView, contextMenuConfigurationForRowAt indexPath: IndexPath,
                            point: CGPoint) -> UIContextMenuConfiguration? {
        let server = servers[indexPath.row]
        return UIContextMenuConfiguration(identifier: nil, previewProvider: nil) { [weak self] _ in
            UIMenu(children: [
                UIAction(title: "Edit", image: UIImage(systemName: "pencil")) { _ in self?.edit(server, isNew: false) },
                UIAction(title: "Delete", image: UIImage(systemName: "trash"), attributes: .destructive) { _ in
                    self?.delete(server)
                },
            ])
        }
    }

    override func tableView(_ tableView: UITableView,
                            trailingSwipeActionsConfigurationForRowAt indexPath: IndexPath) -> UISwipeActionsConfiguration? {
        let server = servers[indexPath.row]
        let delete = UIContextualAction(style: .destructive, title: "Delete") { [weak self] _, _, done in
            self?.delete(server)
            done(true)
        }
        let edit = UIContextualAction(style: .normal, title: "Edit") { [weak self] _, _, done in
            self?.edit(server, isNew: false)
            done(true)
        }
        return UISwipeActionsConfiguration(actions: [delete, edit])
    }
}

/// Adding or changing a server: its kind, where it is, and how to sign in.
final class ServerEditViewController: UITableViewController, UITextFieldDelegate {
    var onDone: (() -> Void)?

    private enum Field: Int, CaseIterable {
        case name, host, port, share, user, password, folder

        var title: String {
            switch self {
            case .name: return "Name"
            case .host: return "Address"
            case .port: return "Port"
            case .share: return "Share"
            case .user: return "User Name"
            case .password: return "Password"
            case .folder: return "Folder"
            }
        }

        func placeholder(_ kind: RemoteServer.Kind) -> String {
            switch self {
            case .name: return "What to call it"
            case .host: return "nas.local or 192.168.1.10"
            case .port: return kind == .ftp ? "21" : "445"
            case .share: return "Leave empty to choose from a list"
            case .user: return kind == .ftp ? "Leave empty for anonymous" : "Leave empty for guest"
            case .password: return ""
            case .folder: return "A folder to start in (optional)"
            }
        }
    }

    private var server: RemoteServer
    private let isNew: Bool
    private var values: [Field: String] = [:]
    private let kindControl = UISegmentedControl(items: RemoteServer.Kind.allCases.map(\.title))

    init(server: RemoteServer, isNew: Bool) {
        self.server = server
        self.isNew = isNew
        super.init(style: .insetGrouped)
        title = isNew ? "Add Server" : "Edit Server"
        values = [
            .name: server.name, .host: server.host, .port: server.port.map(String.init) ?? "",
            .share: server.share, .user: server.user, .password: isNew ? "" : server.password, .folder: server.folder,
        ]
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    override func viewDidLoad() {
        super.viewDidLoad()
        navigationItem.leftBarButtonItem = UIBarButtonItem(barButtonSystemItem: .cancel, target: self,
                                                           action: #selector(cancel))
        navigationItem.rightBarButtonItem = UIBarButtonItem(barButtonSystemItem: .save, target: self,
                                                            action: #selector(save))
        kindControl.selectedSegmentIndex = RemoteServer.Kind.allCases.firstIndex(of: server.kind) ?? 0
        kindControl.accessibilityLabel = "Kind of server"
        kindControl.addAction(UIAction { [weak self] _ in self?.tableView.reloadData() }, for: .valueChanged)
        tableView.keyboardDismissMode = .interactive
    }

    private var kind: RemoteServer.Kind {
        RemoteServer.Kind.allCases[max(kindControl.selectedSegmentIndex, 0)]
    }

    /// The fields that apply to the kind chosen (a share is an SMB thing).
    private var fields: [Field] {
        Field.allCases.filter { $0 != .share || kind == .smb }
    }

    @objc private func cancel() {
        dismiss(animated: true)
    }

    @objc private func save() {
        view.endEditing(true)
        func value(_ field: Field) -> String {
            (values[field] ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        }
        var host = value(.host)
        // An address pasted with its scheme is still an address
        for scheme in ["ftp://", "smb://"] where host.lowercased().hasPrefix(scheme) { host.removeFirst(scheme.count) }
        host = host.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        guard !host.isEmpty else {
            let alert = UIAlertController(title: "Address Needed", message: "Enter the server's name or address.",
                                          preferredStyle: .alert)
            alert.addAction(UIAlertAction(title: "OK", style: .default))
            return present(alert, animated: true)
        }
        server.kind = kind
        server.name = value(.name)
        server.host = host
        server.port = Int(value(.port))
        server.share = kind == .smb ? value(.share) : ""
        server.user = value(.user)
        server.folder = value(.folder)
        ServerStore.save(server, password: values[.password] ?? "")
        dismiss(animated: true) { [onDone] in onDone?() }
    }

    // MARK: Table

    override func numberOfSections(in tableView: UITableView) -> Int { 2 }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        section == 0 ? 1 : fields.count
    }

    override func tableView(_ tableView: UITableView, titleForFooterInSection section: Int) -> String? {
        guard section == 1 else { return nil }
        return kind == .ftp ? "Plain FTP. The password is kept in this device's keychain."
                            : "A Windows share, a NAS or a Mac's file sharing. The password is kept in this device's keychain."
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = UITableViewCell(style: .default, reuseIdentifier: nil)
        cell.selectionStyle = .none
        if indexPath.section == 0 {
            kindControl.translatesAutoresizingMaskIntoConstraints = false
            cell.contentView.addSubview(kindControl)
            NSLayoutConstraint.activate([
                kindControl.leadingAnchor.constraint(equalTo: cell.contentView.layoutMarginsGuide.leadingAnchor),
                kindControl.trailingAnchor.constraint(equalTo: cell.contentView.layoutMarginsGuide.trailingAnchor),
                kindControl.topAnchor.constraint(equalTo: cell.contentView.topAnchor, constant: 8),
                kindControl.bottomAnchor.constraint(equalTo: cell.contentView.bottomAnchor, constant: -8),
            ])
            return cell
        }
        let field = fields[indexPath.row]
        let label = UILabel()
        label.text = field.title
        label.font = .preferredFont(forTextStyle: .body)
        label.adjustsFontForContentSizeCategory = true
        label.setContentHuggingPriority(.required, for: .horizontal)
        label.isAccessibilityElement = false  // the text field carries the name

        let text = UITextField()
        text.text = values[field]
        text.placeholder = field.placeholder(kind)
        text.tag = field.rawValue
        text.delegate = self
        text.font = .preferredFont(forTextStyle: .body)
        text.adjustsFontForContentSizeCategory = true
        text.textAlignment = .right
        text.clearButtonMode = .whileEditing
        text.accessibilityLabel = field.title
        text.autocorrectionType = .no
        text.autocapitalizationType = field == .name ? .words : .none
        text.isSecureTextEntry = field == .password
        text.keyboardType = field == .port ? .numberPad : (field == .host ? .URL : .default)
        text.returnKeyType = .done
        text.addAction(UIAction { [weak self] action in
            guard let sender = action.sender as? UITextField, let changed = Field(rawValue: sender.tag) else { return }
            self?.values[changed] = sender.text ?? ""
        }, for: .editingChanged)

        let row = UIStackView(arrangedSubviews: [label, text])
        row.axis = .horizontal
        row.spacing = 12
        row.translatesAutoresizingMaskIntoConstraints = false
        cell.contentView.addSubview(row)
        NSLayoutConstraint.activate([
            row.leadingAnchor.constraint(equalTo: cell.contentView.layoutMarginsGuide.leadingAnchor),
            row.trailingAnchor.constraint(equalTo: cell.contentView.layoutMarginsGuide.trailingAnchor),
            row.topAnchor.constraint(equalTo: cell.contentView.topAnchor, constant: 11),
            row.bottomAnchor.constraint(equalTo: cell.contentView.bottomAnchor, constant: -11),
        ])
        return cell
    }

    func textFieldShouldReturn(_ textField: UITextField) -> Bool {
        textField.resignFirstResponder()
        return true
    }
}
