import UIKit

/// Open Address: a field for the address of a stream, an audio file or a playlist
/// on the internet, and the last ten addresses opened, to open again.
final class AddressViewController: FastPlayTableViewController, UITextFieldDelegate {
    private static let recentKey = "RecentAddresses"
    private static let keep = 10

    private let field = UITextField()
    private var recent: [String] = UserDefaults.standard.stringArray(forKey: recentKey) ?? []

    init() {
        super.init(style: .insetGrouped)
        title = "Open Address"
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    override func viewDidLoad() {
        super.viewDidLoad()
        navigationItem.largeTitleDisplayMode = .never
        tableView.register(UITableViewCell.self, forCellReuseIdentifier: "cell")
        field.placeholder = "https://"
        field.keyboardType = .URL
        field.returnKeyType = .go
        field.autocapitalizationType = .none
        field.autocorrectionType = .no
        field.clearButtonMode = .whileEditing
        field.accessibilityLabel = "Address"
        field.delegate = self
        field.font = .preferredFont(forTextStyle: .body)
        field.adjustsFontForContentSizeCategory = true
        updateClearButton()
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        if recent.isEmpty { field.becomeFirstResponder() }
    }

    private func updateClearButton() {
        navigationItem.rightBarButtonItem = recent.isEmpty ? nil
            : UIBarButtonItem(title: "Clear", style: .plain, target: self, action: #selector(clearRecent))
    }

    // MARK: Opening

    private func open(_ typed: String) {
        let text = typed.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { return }
        let address = text.contains("://") ? text : "https://" + text
        remember(address)
        field.resignFirstResponder()
        field.text = ""
        UIAccessibility.post(notification: .announcement, argument: "Opening")
        Task { @MainActor in
            await PlaylistText.play(address: address)
            if engine.trackCount > 0 { openPlayer() }
        }
    }

    func textFieldShouldReturn(_ textField: UITextField) -> Bool {
        open(textField.text ?? "")
        return true
    }

    // MARK: Recent addresses

    private func remember(_ address: String) {
        recent.removeAll { $0 == address }
        recent.insert(address, at: 0)
        if recent.count > Self.keep { recent.removeLast(recent.count - Self.keep) }
        save()
    }

    private func forget(at index: Int) {
        recent.remove(at: index)
        save()
    }

    @objc private func clearRecent() {
        let alert = UIAlertController(title: "Clear Recent Addresses",
                                      message: "Removes the \(recent.count) addresses listed here.", preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        alert.addAction(UIAlertAction(title: "Clear", style: .destructive) { [weak self] _ in
            self?.recent.removeAll()
            self?.save()
        })
        present(alert, animated: true)
    }

    private func save() {
        UserDefaults.standard.set(recent, forKey: Self.recentKey)
        updateClearButton()
        tableView.reloadData()
    }

    // MARK: Table

    override func numberOfSections(in tableView: UITableView) -> Int { recent.isEmpty ? 1 : 2 }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        section == 0 ? 2 : recent.count
    }

    override func tableView(_ tableView: UITableView, titleForHeaderInSection section: Int) -> String? {
        section == 1 ? "Recent" : nil
    }

    override func tableView(_ tableView: UITableView, titleForFooterInSection section: Int) -> String? {
        section == 0 ? "A stream, an audio file, or a playlist (M3U or PLS), whose tracks all play." : nil
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: "cell", for: indexPath)
        cell.contentView.subviews.forEach { if $0 === field { $0.removeFromSuperview() } }
        cell.accessoryType = .none
        cell.accessibilityTraits = []
        if indexPath.section == 0 && indexPath.row == 0 {
            cell.contentConfiguration = nil
            cell.selectionStyle = .none
            field.translatesAutoresizingMaskIntoConstraints = false
            cell.contentView.addSubview(field)
            NSLayoutConstraint.activate([
                field.leadingAnchor.constraint(equalTo: cell.contentView.layoutMarginsGuide.leadingAnchor),
                field.trailingAnchor.constraint(equalTo: cell.contentView.layoutMarginsGuide.trailingAnchor),
                field.topAnchor.constraint(equalTo: cell.contentView.topAnchor, constant: 12),
                field.bottomAnchor.constraint(equalTo: cell.contentView.bottomAnchor, constant: -12),
            ])
            return cell
        }
        var content = UIListContentConfiguration.cell()
        if indexPath.section == 0 {
            content.text = "Play"
            content.textProperties.color = view.tintColor
            content.textProperties.alignment = .center
            cell.accessibilityTraits = .button
            cell.accessibilityHint = "Plays the address typed above"
        } else {
            content.text = recent[indexPath.row]
            content.textProperties.numberOfLines = 0
            content.textProperties.lineBreakMode = .byCharWrapping
            cell.accessibilityHint = "Plays it"
            cell.accessibilityCustomActions = [
                UIAccessibilityCustomAction(name: "Remove") { [weak self] _ in
                    self?.forget(at: indexPath.row)
                    return true
                },
            ]
        }
        cell.contentConfiguration = content
        cell.selectionStyle = .default
        return cell
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        if indexPath.section == 0 {
            if indexPath.row == 1 { open(field.text ?? "") }
        } else {
            open(recent[indexPath.row])
        }
    }

    override func tableView(_ tableView: UITableView,
                            trailingSwipeActionsConfigurationForRowAt indexPath: IndexPath) -> UISwipeActionsConfiguration? {
        guard indexPath.section == 1 else { return nil }
        return UISwipeActionsConfiguration(actions: [
            UIContextualAction(style: .destructive, title: "Remove") { [weak self] _, _, done in
                self?.forget(at: indexPath.row)
                done(true)
            },
        ])
    }
}
