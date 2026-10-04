import UIKit

/// What is queued to play: choose a track to play it.
final class PlaylistViewController: FastPlayTableViewController {
    init() {
        super.init(style: .plain)
        title = "Playlist"
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    override func viewDidLoad() {
        super.viewDidLoad()
        tableView.register(UITableViewCell.self, forCellReuseIdentifier: "cell")
        navigationItem.largeTitleDisplayMode = .never
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        let current = engine.currentTrack
        if current >= 0, current < engine.trackCount {
            tableView.scrollToRow(at: IndexPath(row: current, section: 0), at: .middle, animated: false)
        }
    }

    override func engineStateChanged() {
        super.engineStateChanged()
        tableView.reloadData()
    }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        engine.trackCount
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: "cell", for: indexPath)
        let name = engine.trackName(at: indexPath.row)
        let current = indexPath.row == engine.currentTrack
        var content = UIListContentConfiguration.cell()
        content.text = name
        cell.contentConfiguration = content
        cell.accessoryType = current ? .checkmark : .none
        cell.accessibilityLabel = current ? "\(name), playing" : name
        return cell
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        engine.playTrack(at: indexPath.row)
    }
}
