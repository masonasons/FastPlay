import UIKit

/// A list screen. While something is loaded, its toolbar is a small player: what
/// is playing (which opens the player) and play or pause.
class FastPlayTableViewController: UITableViewController {
    let engine = FPEngine.shared
    private var stateObserver: NSObjectProtocol?

    override func viewDidLoad() {
        super.viewDidLoad()
        stateObserver = NotificationCenter.default.addObserver(
            forName: .FPEngineStateDidChange, object: nil, queue: .main) { [weak self] _ in
            self?.engineStateChanged()
        }
    }

    deinit {
        if let stateObserver { NotificationCenter.default.removeObserver(stateObserver) }
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        updateMiniPlayer(animated: animated)
    }

    /// What is playing, its state or the playlist changed.
    func engineStateChanged() {
        if viewIfLoaded?.window != nil { updateMiniPlayer(animated: true) }
    }

    private func updateMiniPlayer(animated: Bool) {
        guard engine.loaded else {
            navigationController?.setToolbarHidden(true, animated: animated)
            return
        }
        let title = UIBarButtonItem(title: engine.title, style: .plain, target: self, action: #selector(openPlayer))
        title.accessibilityLabel = "Now playing: \(engine.title)"
        title.accessibilityHint = "Opens the player"
        let playing = engine.playing
        let playPause = UIBarButtonItem(
            image: UIImage(systemName: playing ? "pause.fill" : "play.fill"),
            style: .plain, target: self, action: #selector(playPause))
        playPause.accessibilityLabel = playing ? "Pause" : "Play"
        toolbarItems = [title, .flexibleSpace(), playPause]
        navigationController?.setToolbarHidden(false, animated: animated)
    }

    @objc func openPlayer() {
        navigationController?.pushViewController(PlayerViewController(), animated: true)
    }

    @objc private func playPause() {
        engine.playPause()
    }
}
