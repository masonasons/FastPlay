import UIKit

/// Asked while pasting when a name is already taken: what is being pasted against
/// what is there, the choices, and whether to do the same for the rest.
final class ConflictViewController: UITableViewController {
    private let pasting: FileEntry
    private let existing: FileEntry
    private let choices: [FileOperations.Resolution]
    private var applyToAll = false
    private var answer: ((FileOperations.Choice?) -> Void)?
    private let applySwitch = UISwitch()

    private init(pasting: FileEntry, existing: FileEntry, answer: @escaping (FileOperations.Choice?) -> Void) {
        self.pasting = pasting
        self.existing = existing
        choices = pasting.isFolder ? FileOperations.Resolution.forFolders : FileOperations.Resolution.forFiles
        self.answer = answer
        super.init(style: .insetGrouped)
        title = pasting.isFolder ? "Folder Already There" : "File Already There"
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    /// Asks, from `controller`. Nil: stop pasting.
    @MainActor
    static func ask(from controller: UIViewController, pasting: FileEntry,
                    existing: FileEntry) async -> FileOperations.Choice? {
        await withCheckedContinuation { continuation in
            let asking = ConflictViewController(pasting: pasting, existing: existing) { choice in
                continuation.resume(returning: choice)
            }
            let navigation = UINavigationController(rootViewController: asking)
            navigation.isModalInPresentation = true  // answered, not swiped away
            controller.present(navigation, animated: true)
        }
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        navigationItem.leftBarButtonItem = UIBarButtonItem(title: "Stop Pasting", style: .plain, target: self,
                                                           action: #selector(stop))
        applySwitch.addAction(UIAction { [weak self] _ in
            self?.applyToAll = self?.applySwitch.isOn ?? false
        }, for: .valueChanged)
        UIAccessibility.post(notification: .screenChanged, argument: "\(pasting.name) is already there")
    }

    @objc private func stop() {
        finish(nil)
    }

    private func finish(_ choice: FileOperations.Choice?) {
        let answer = self.answer
        self.answer = nil
        dismiss(animated: true) { answer?(choice) }
    }

    private static func describe(_ entry: FileEntry) -> String {
        var parts: [String] = []
        if !entry.isFolder { parts.append(ByteCountFormatter.string(fromByteCount: entry.size, countStyle: .file)) }
        if let modified = entry.modified {
            parts.append(DateFormatter.localizedString(from: modified, dateStyle: .medium, timeStyle: .short))
        }
        return parts.isEmpty ? "" : parts.joined(separator: ", ")
    }

    // MARK: Table

    override func numberOfSections(in tableView: UITableView) -> Int { 3 }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        switch section {
        case 0: return pasting.isFolder ? 1 : 2
        case 1: return 1
        default: return choices.count
        }
    }

    override func tableView(_ tableView: UITableView, titleForHeaderInSection section: Int) -> String? {
        switch section {
        case 0: return pasting.name
        case 2: return "What to Do"
        default: return nil
        }
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = UITableViewCell(style: .value1, reuseIdentifier: nil)
        var content = UIListContentConfiguration.valueCell()
        switch indexPath.section {
        case 0:
            cell.selectionStyle = .none
            if pasting.isFolder {
                content.text = "A folder of that name is already there"
            } else {
                content.text = indexPath.row == 0 ? "Being pasted" : "Already there"
                content.secondaryText = Self.describe(indexPath.row == 0 ? pasting : existing)
            }
        case 1:
            cell.selectionStyle = .none
            content.text = "Do the Same for the Rest"
            applySwitch.isOn = applyToAll
            cell.accessoryView = applySwitch
            cell.accessibilityLabel = "Do the same for the rest"
        default:
            content.text = choices[indexPath.row].title
            content.textProperties.color = view.tintColor
            cell.accessibilityTraits = .button
        }
        cell.contentConfiguration = content
        return cell
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        if indexPath.section == 1 {
            applySwitch.setOn(!applySwitch.isOn, animated: true)
            applyToAll = applySwitch.isOn
            return
        }
        guard indexPath.section == 2 else { return }
        finish(FileOperations.Choice(resolution: choices[indexPath.row], applyToAll: applyToAll))
    }
}
