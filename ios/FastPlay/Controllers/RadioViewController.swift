import UIKit

/// Internet radio: the saved stations, and a search of the station directories.
final class RadioViewController: FastPlayTableViewController, UISearchBarDelegate {
    private var favorites: [FPRadioStation] = []
    /// What a search found; nil while the saved stations are showing.
    private var results: [FPRadioResult]?
    private let search = UISearchController(searchResultsController: nil)
    private let statusLabel = UILabel()
    private let files = FileTransfer()

    init() {
        super.init(style: .plain)
        title = "Radio"
    }

    // MARK: Importing and exporting

    /// The stations of an M3U or PLS playlist file, as the desktop FastPlay exports.
    private func importStations() {
        files.importFile(extensions: ["m3u", "m3u8", "pls"], from: self) { [weak self] url in
            guard let self else { return }
            var skipped = 0
            let added = self.engine.importRadioFavorites(fromFile: url.path, skipped: &skipped)
            try? FileManager.default.removeItem(at: url)
            self.reloadFavorites()
            var message = added == 1 ? "1 station added" : "\(added) stations added"
            if skipped > 0 { message += ", \(skipped) already saved" }
            self.tell("Import", message + ".")
        }
    }

    private func exportStations() {
        guard !engine.radioFavorites.isEmpty else { return tell("Export", "There are no saved stations to export.") }
        files.exportFile(named: "FastPlay Radio Stations.m3u", from: self) { engine.exportRadioFavorites(toFile: $0) }
    }

    private func tell(_ title: String, _ message: String) {
        let alert = UIAlertController(title: title, message: message, preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "OK", style: .default))
        present(alert, animated: true)
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    override func viewDidLoad() {
        super.viewDidLoad()
        navigationItem.largeTitleDisplayMode = .never
        tableView.register(UITableViewCell.self, forCellReuseIdentifier: "cell")

        search.searchBar.placeholder = "Search stations"
        search.searchBar.scopeButtonTitles = ["RadioBrowser", "TuneIn", "iHeartRadio"]
        search.searchBar.delegate = self
        search.obscuresBackgroundDuringPresentation = false
        navigationItem.searchController = search
        navigationItem.hidesSearchBarWhenScrolling = false
        definesPresentationContext = true

        let add = UIBarButtonItem(image: UIImage(systemName: "plus"), menu: UIMenu(children: [
            UIAction(title: "Add Station", image: UIImage(systemName: "plus")) { [weak self] _ in self?.addStation() },
            UIAction(title: "Import Stations", image: UIImage(systemName: "square.and.arrow.down")) { [weak self] _ in
                self?.importStations()
            },
            UIAction(title: "Export Stations", image: UIImage(systemName: "square.and.arrow.up")) { [weak self] _ in
                self?.exportStations()
            },
        ]))
        add.accessibilityLabel = "Add, import or export stations"
        navigationItem.rightBarButtonItem = add

        statusLabel.numberOfLines = 0
        statusLabel.textAlignment = .center
        statusLabel.textColor = .secondaryLabel
        statusLabel.font = .preferredFont(forTextStyle: .body)
        statusLabel.adjustsFontForContentSizeCategory = true
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        reloadFavorites()
    }

    private func reloadFavorites() {
        favorites = engine.radioFavorites
        if results == nil { show() }
    }

    /// Shows the list there is to show, or a line saying why it is empty.
    private func show(status: String? = nil) {
        tableView.reloadData()
        let empty = (results?.isEmpty ?? favorites.isEmpty)
        let text = status ?? (results != nil ? "No stations found."
                                             : "No stations saved yet.\n\nSearch for one, or add one by its address.")
        statusLabel.text = text
        tableView.backgroundView = (empty || status != nil) ? statusLabel : nil
    }

    // MARK: Searching

    func searchBarSearchButtonClicked(_ searchBar: UISearchBar) {
        runSearch()
    }

    func searchBar(_ searchBar: UISearchBar, selectedScopeButtonIndexDidChange selectedScope: Int) {
        if results != nil { runSearch() }
    }

    func searchBarCancelButtonClicked(_ searchBar: UISearchBar) {
        results = nil
        show()
    }

    #if DEBUG
    /// For trying a search from a script (see SceneDelegate).
    func debugSearch(_ query: String) {
        loadViewIfNeeded()
        search.searchBar.text = query
        runSearch()
    }
    #endif

    private func runSearch() {
        let query = search.searchBar.text?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        guard !query.isEmpty else { return }
        let directory = FPRadioDirectory(rawValue: search.searchBar.selectedScopeButtonIndex) ?? .radioBrowser
        results = []
        show(status: "Searching…")
        UIAccessibility.post(notification: .announcement, argument: "Searching")
        engine.searchRadio(query, directory: directory) { [weak self] found in
            guard let self, self.results != nil else { return }
            self.results = found
            self.show()
            UIAccessibility.post(notification: .announcement,
                                 argument: found.count == 1 ? "1 station" : "\(found.count) stations")
        }
    }

    // MARK: Adding

    private func addStation() {
        let alert = UIAlertController(title: "Add Station", message: nil, preferredStyle: .alert)
        alert.addTextField { field in
            field.placeholder = "Name"
            field.autocapitalizationType = .words
            field.accessibilityLabel = "Name"
        }
        alert.addTextField { field in
            field.placeholder = "Address"
            field.keyboardType = .URL
            field.autocapitalizationType = .none
            field.autocorrectionType = .no
            field.accessibilityLabel = "Address"
        }
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        alert.addAction(UIAlertAction(title: "Add", style: .default) { [weak self, weak alert] _ in
            guard let self, let fields = alert?.textFields, fields.count == 2 else { return }
            let url = fields[1].text?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            var name = fields[0].text?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            guard !url.isEmpty else { return }
            if name.isEmpty { name = url }
            if !self.engine.addRadioFavoriteNamed(name, url: url.contains("://") ? url : "http://" + url) {
                UIAccessibility.post(notification: .announcement, argument: "That station is already saved")
            }
            self.reloadFavorites()
        })
        present(alert, animated: true)
    }

    private func save(_ result: FPRadioResult) {
        engine.resolveRadioResult(result) { [weak self] url in
            guard let self else { return }
            guard let url else {
                UIAccessibility.post(notification: .announcement, argument: "Could not find the station's stream")
                return
            }
            let added = self.engine.addRadioFavoriteNamed(result.name, url: url)
            UIAccessibility.post(notification: .announcement,
                                 argument: added ? "Saved \(result.name)" : "That station is already saved")
            self.favorites = self.engine.radioFavorites
        }
    }

    // MARK: Table

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        results?.count ?? favorites.count
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: "cell", for: indexPath)
        var content = UIListContentConfiguration.subtitleCell()
        if let results {
            content.text = results[indexPath.row].name
            content.secondaryText = results[indexPath.row].detail
        } else {
            content.text = favorites[indexPath.row].name
        }
        content.image = UIImage(systemName: "dot.radiowaves.left.and.right")
        cell.contentConfiguration = content
        return cell
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        if let results {
            let result = results[indexPath.row]
            UIAccessibility.post(notification: .announcement, argument: "Opening \(result.name)")
            engine.resolveRadioResult(result) { [weak self] url in
                guard let self else { return }
                guard let url else {
                    UIAccessibility.post(notification: .announcement, argument: "Could not find the station's stream")
                    return
                }
                self.engine.playURL(url, name: result.name)
                self.openPlayer()
            }
        } else {
            let station = favorites[indexPath.row]
            engine.playURL(station.url, name: station.name)
            openPlayer()
        }
    }

    override func tableView(_ tableView: UITableView,
                            trailingSwipeActionsConfigurationForRowAt indexPath: IndexPath) -> UISwipeActionsConfiguration? {
        if let results {
            let result = results[indexPath.row]
            let save = UIContextualAction(style: .normal, title: "Save") { [weak self] _, _, done in
                self?.save(result)
                done(true)
            }
            save.backgroundColor = .systemBlue
            return UISwipeActionsConfiguration(actions: [save])
        }
        let station = favorites[indexPath.row]
        let delete = UIContextualAction(style: .destructive, title: "Delete") { [weak self] _, _, done in
            self?.engine.removeRadioFavorite(station.identifier)
            self?.reloadFavorites()
            done(true)
        }
        return UISwipeActionsConfiguration(actions: [delete])
    }
}
