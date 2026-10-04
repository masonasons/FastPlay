import UIKit

/// Podcasts: the ones subscribed to, and a search of the podcast directory.
final class PodcastsViewController: FastPlayTableViewController, UISearchBarDelegate {
    private var podcasts: [FPPodcast] = []
    /// What a search found; nil while the subscriptions are showing.
    private var results: [FPPodcastResult]?
    private let search = UISearchController(searchResultsController: nil)
    private let statusLabel = UILabel()
    private let files = FileTransfer()

    init() {
        super.init(style: .plain)
        title = "Podcasts"
    }

    // MARK: Importing and exporting

    /// The feeds of an OPML file, as podcast apps (and the desktop FastPlay) export.
    private func importSubscriptions() {
        files.importFile(extensions: ["opml"], from: self) { [weak self] url in
            guard let self else { return }
            var skipped = 0
            let added = self.engine.importPodcasts(fromOPML: url.path, skipped: &skipped)
            try? FileManager.default.removeItem(at: url)
            self.podcasts = self.engine.podcasts
            if self.results == nil { self.show() }
            var message: String
            if added < 0 {
                message = "That file has no podcasts in it"
            } else {
                message = added == 1 ? "1 podcast added" : "\(added) podcasts added"
                if skipped > 0 { message += ", \(skipped) already subscribed to" }
            }
            self.tell("Import", message + ".")
        }
    }

    private func exportSubscriptions() {
        guard !engine.podcasts.isEmpty else { return tell("Export", "There are no podcasts to export.") }
        files.exportFile(named: "FastPlay Podcasts.opml", from: self) { engine.exportPodcasts(toOPML: $0) }
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

        search.searchBar.placeholder = "Search podcasts"
        search.searchBar.delegate = self
        search.obscuresBackgroundDuringPresentation = false
        navigationItem.searchController = search
        navigationItem.hidesSearchBarWhenScrolling = false
        definesPresentationContext = true

        let add = UIBarButtonItem(image: UIImage(systemName: "plus"), menu: UIMenu(children: [
            UIAction(title: "Add by Feed Address", image: UIImage(systemName: "plus")) { [weak self] _ in self?.addFeed() },
            UIAction(title: "Import OPML", image: UIImage(systemName: "square.and.arrow.down")) { [weak self] _ in
                self?.importSubscriptions()
            },
            UIAction(title: "Export OPML", image: UIImage(systemName: "square.and.arrow.up")) { [weak self] _ in
                self?.exportSubscriptions()
            },
        ]))
        add.accessibilityLabel = "Add, import or export podcasts"
        navigationItem.rightBarButtonItem = add

        statusLabel.numberOfLines = 0
        statusLabel.textAlignment = .center
        statusLabel.textColor = .secondaryLabel
        statusLabel.font = .preferredFont(forTextStyle: .body)
        statusLabel.adjustsFontForContentSizeCategory = true
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        podcasts = engine.podcasts
        if results == nil { show() }
    }

    private func show(status: String? = nil) {
        tableView.reloadData()
        let empty = (results?.isEmpty ?? podcasts.isEmpty)
        statusLabel.text = status ?? (results != nil ? "No podcasts found."
                                                     : "No podcasts yet.\n\nSearch for one, or add one by its feed address.")
        tableView.backgroundView = (empty || status != nil) ? statusLabel : nil
    }

    // MARK: Searching

    func searchBarSearchButtonClicked(_ searchBar: UISearchBar) {
        let query = searchBar.text?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        guard !query.isEmpty else { return }
        results = []
        show(status: "Searching…")
        UIAccessibility.post(notification: .announcement, argument: "Searching")
        engine.searchPodcasts(query) { [weak self] found in
            guard let self, self.results != nil else { return }
            self.results = found
            self.show()
            UIAccessibility.post(notification: .announcement,
                                 argument: found.count == 1 ? "1 podcast" : "\(found.count) podcasts")
        }
    }

    func searchBarCancelButtonClicked(_ searchBar: UISearchBar) {
        results = nil
        podcasts = engine.podcasts
        show()
    }

    private func addFeed() {
        let alert = UIAlertController(title: "Add Podcast", message: "The address of the podcast's feed.",
                                      preferredStyle: .alert)
        alert.addTextField { field in
            field.placeholder = "https://"
            field.keyboardType = .URL
            field.autocapitalizationType = .none
            field.autocorrectionType = .no
            field.accessibilityLabel = "Feed address"
        }
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        alert.addAction(UIAlertAction(title: "Open", style: .default) { [weak self, weak alert] _ in
            let url = alert?.textFields?.first?.text?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            guard !url.isEmpty else { return }
            let feed = url.contains("://") ? url : "https://" + url
            self?.navigationController?.pushViewController(EpisodesViewController(name: "Podcast", feedURL: feed),
                                                           animated: true)
        })
        present(alert, animated: true)
    }

    // MARK: Table

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        results?.count ?? podcasts.count
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: "cell", for: indexPath)
        var content = UIListContentConfiguration.subtitleCell()
        if let results {
            content.text = results[indexPath.row].name
            content.secondaryText = results[indexPath.row].author
        } else {
            content.text = podcasts[indexPath.row].name
        }
        content.image = UIImage(systemName: "mic")
        cell.contentConfiguration = content
        cell.accessoryType = .disclosureIndicator
        return cell
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        let name = results?[indexPath.row].name ?? podcasts[indexPath.row].name
        let feed = results?[indexPath.row].feedURL ?? podcasts[indexPath.row].feedURL
        navigationController?.pushViewController(EpisodesViewController(name: name, feedURL: feed), animated: true)
    }

    override func tableView(_ tableView: UITableView,
                            trailingSwipeActionsConfigurationForRowAt indexPath: IndexPath) -> UISwipeActionsConfiguration? {
        guard results == nil else { return nil }
        let podcast = podcasts[indexPath.row]
        let delete = UIContextualAction(style: .destructive, title: "Unsubscribe") { [weak self] _, _, done in
            guard let self else { return done(false) }
            self.engine.removePodcast(podcast.identifier)
            self.podcasts = self.engine.podcasts
            self.show()
            done(true)
        }
        return UISwipeActionsConfiguration(actions: [delete])
    }
}

/// A podcast's episodes, read from its feed. Choosing one plays it.
final class EpisodesViewController: FastPlayTableViewController {
    private var name: String
    private let feedURL: String
    private var episodes: [FPEpisode] = []
    private let statusLabel = UILabel()

    init(name: String, feedURL: String) {
        self.name = name
        self.feedURL = feedURL
        super.init(style: .plain)
        title = name
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
        statusLabel.text = "Loading episodes…"
        tableView.backgroundView = statusLabel
        updateSubscribeButton()

        engine.loadEpisodes(ofFeed: feedURL) { [weak self] title, episodes, error in
            guard let self else { return }
            if !title.isEmpty {
                self.name = title
                self.title = title
            }
            self.episodes = episodes
            self.tableView.reloadData()
            if let error {
                self.statusLabel.text = error
            } else {
                self.tableView.backgroundView = nil
                UIAccessibility.post(notification: .announcement,
                                     argument: episodes.count == 1 ? "1 episode" : "\(episodes.count) episodes")
            }
        }
    }

    private func updateSubscribeButton() {
        let subscribed = engine.isSubscribed(toFeed: feedURL)
        navigationItem.rightBarButtonItem = UIBarButtonItem(
            title: subscribed ? "Unsubscribe" : "Subscribe", style: .plain, target: self, action: #selector(toggleSubscription))
    }

    @objc private func toggleSubscription() {
        if let podcast = engine.podcasts.first(where: { $0.feedURL == feedURL }) {
            engine.removePodcast(podcast.identifier)
            UIAccessibility.post(notification: .announcement, argument: "Unsubscribed")
        } else {
            engine.subscribeToPodcast(named: name, feedURL: feedURL)
            UIAccessibility.post(notification: .announcement, argument: "Subscribed")
        }
        updateSubscribeButton()
    }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        episodes.count
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let episode = episodes[indexPath.row]
        let cell = tableView.dequeueReusableCell(withIdentifier: "cell", for: indexPath)
        var content = UIListContentConfiguration.subtitleCell()
        content.text = episode.title
        var details = [Self.shortDate(episode.date)]
        if episode.duration > 0 { details.append(FPEngine.formatTime(Double(episode.duration))) }
        content.secondaryText = details.filter { !$0.isEmpty }.joined(separator: " · ")
        content.textProperties.numberOfLines = 2
        cell.contentConfiguration = content
        return cell
    }

    /// A feed's date ("Fri, 02 Oct 2026 23:14:21 +0000") as the day alone, in the
    /// user's own form; anything else as it came.
    private static func shortDate(_ feedDate: String) -> String {
        for format in ["EEE, dd MMM yyyy HH:mm:ss Z", "dd MMM yyyy HH:mm:ss Z", "EEE, dd MMM yyyy HH:mm:ss zzz"] {
            feedDateParser.dateFormat = format
            if let date = feedDateParser.date(from: feedDate) {
                return DateFormatter.localizedString(from: date, dateStyle: .medium, timeStyle: .none)
            }
        }
        return feedDate
    }

    private static let feedDateParser: DateFormatter = {
        let parser = DateFormatter()
        parser.locale = Locale(identifier: "en_US_POSIX")
        return parser
    }()

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        engine.playEpisodes([episodes[indexPath.row]], startingAt: 0)
        openPlayer()
    }
}
