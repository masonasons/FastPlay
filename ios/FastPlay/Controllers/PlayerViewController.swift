import UIKit

/// The player: what is playing, the transport, and the controls for seeking and
/// the effects, in one of two modes.
///
/// Sliders: four adjustable controls, as the desktop's keys are paired. Seek (the
/// arrows) and Seek By (comma and period); Effect (the brackets) and Adjust (up
/// and down).
///
/// Touch: one area that takes swipes for the same four things (DirectTouchView).
final class PlayerViewController: UIViewController {
    private enum Mode: Int {
        case sliders = 0
        case touch = 1
    }

    private static let modeKey = "PlayerMode"
    /// The seek mode touch mode was last in. The sliders always jump: spring and
    /// tape seek while something is held, and a slider has nothing to hold.
    private static let touchSeekModeKey = "TouchSeekMode"

    private let engine = FPEngine.shared
    private var stateObserver: NSObjectProtocol?
    private var timer: Timer?
    private var draggingPosition = false

    private let titleLabel = UILabel()
    private let artistLabel = UILabel()
    private let statusLabel = UILabel()
    private let positionSlider = UISlider()
    private let elapsedLabel = UILabel()
    private let remainingLabel = UILabel()
    private let previousButton = UIButton(type: .system)
    private let playPauseButton = UIButton(type: .system)
    private let nextButton = UIButton(type: .system)
    private let recordButton = UIButton(type: .system)
    private let modeControl = UISegmentedControl(items: ["Sliders", "Touch"])

    private let seekRow = AdjustableRow(name: "Seek", hint: "Swipe up or down to seek forward or back",
                                        minusSymbol: "gobackward", plusSymbol: "goforward")
    private let seekUnitRow = AdjustableRow(name: "Seek By", hint: "Swipe up or down to change how far Seek moves")
    private let effectRow = AdjustableRow(name: "Effect", hint: "Swipe up or down to choose what Adjust changes",
                                          minusSymbol: "chevron.left.circle", plusSymbol: "chevron.right.circle")
    private let adjustRow = AdjustableRow(name: "Adjust", hint: "Swipe up or down to change the effect")
    private let seekModeButton = UIButton(type: .system)
    private let resetButton = UIButton(type: .system)
    private let slidersStack = UIStackView()
    private let touchView = DirectTouchView()
    private let scroll = UIScrollView()
    /// In touch mode the touch area takes whatever of the screen is left.
    private var fillScreen: NSLayoutConstraint?

    init() {
        super.init(nibName: nil, bundle: nil)
        title = "Player"
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    deinit {
        if let stateObserver { NotificationCenter.default.removeObserver(stateObserver) }
    }

    // MARK: Building the screen

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .systemBackground
        navigationItem.largeTitleDisplayMode = .never

        let settings = UIBarButtonItem(image: UIImage(systemName: "gearshape"), style: .plain, target: self,
                                       action: #selector(openSettings))
        settings.accessibilityLabel = "Settings"
        let playlist = UIBarButtonItem(image: UIImage(systemName: "list.bullet"), style: .plain, target: self,
                                       action: #selector(openPlaylist))
        playlist.accessibilityLabel = "Playlist"
        navigationItem.rightBarButtonItems = [settings, playlist]

        titleLabel.font = .preferredFont(forTextStyle: .title2)
        titleLabel.numberOfLines = 2
        titleLabel.accessibilityTraits = .header
        artistLabel.font = .preferredFont(forTextStyle: .body)
        artistLabel.textColor = .secondaryLabel
        statusLabel.font = .preferredFont(forTextStyle: .footnote)
        statusLabel.textColor = .secondaryLabel
        for label in [titleLabel, artistLabel, statusLabel] {
            label.textAlignment = .center
            label.adjustsFontForContentSizeCategory = true
        }

        positionSlider.accessibilityLabel = "Position"
        positionSlider.addTarget(self, action: #selector(positionTouched), for: .touchDown)
        positionSlider.addTarget(self, action: #selector(positionChanged), for: .valueChanged)
        positionSlider.addTarget(self, action: #selector(positionReleased),
                                 for: [.touchUpInside, .touchUpOutside, .touchCancel])
        for label in [elapsedLabel, remainingLabel] {
            label.font = .monospacedDigitSystemFont(ofSize: UIFont.preferredFont(forTextStyle: .footnote).pointSize,
                                                    weight: .regular)
            label.textColor = .secondaryLabel
            label.isAccessibilityElement = false  // the slider says both
        }
        let times = UIStackView(arrangedSubviews: [elapsedLabel, UIView(), remainingLabel])
        times.axis = .horizontal

        configureTransport(previousButton, symbol: "backward.fill", label: "Previous track", action: #selector(previousTrack))
        configureTransport(playPauseButton, symbol: "play.fill", label: "Play", action: #selector(playPause))
        configureTransport(nextButton, symbol: "forward.fill", label: "Next track", action: #selector(nextTrack))
        let transport = UIStackView(arrangedSubviews: [previousButton, playPauseButton, nextButton])
        transport.axis = .horizontal
        transport.distribution = .fillEqually

        recordButton.addAction(UIAction { [weak self] _ in self?.engine.toggleRecording() }, for: .touchUpInside)
        recordButton.accessibilityHint = "Records what is playing into the Recordings folder of FastPlay's files"

        modeControl.selectedSegmentIndex = UserDefaults.standard.integer(forKey: Self.modeKey)
        modeControl.addTarget(self, action: #selector(modeChanged), for: .valueChanged)
        modeControl.accessibilityLabel = "Controls"

        // The sliders. VoiceOver reads each one's new value itself, so what the
        // engine would say about the change is dropped.
        seekRow.onStep = { [weak self] step in self?.quietly { $0.seekStep(step) } }
        seekUnitRow.onStep = { [weak self] step in self?.quietly { $0.changeSeekUnit(step) } }
        effectRow.onStep = { [weak self] step in self?.quietly { $0.cycleParam(step) } }
        adjustRow.onStep = { [weak self] step in self?.quietly { $0.adjustParam(step) } }

        // The seek mode belongs to touch mode, above the touch area
        seekModeButton.addAction(UIAction { [weak self] _ in
            guard let self else { return }
            self.engine.cycleSeekMode()
            UserDefaults.standard.set(self.engine.seekMode.rawValue, forKey: Self.touchSeekModeKey)
            self.refresh()
        }, for: .touchUpInside)
        seekModeButton.accessibilityHint = "Jump: a swipe moves by the seek unit. Spring and tape: hold and drag to " +
            "scrub through the audio."
        resetButton.setTitle("Reset Effect", for: .normal)
        resetButton.addAction(UIAction { [weak self] _ in
            self?.engine.resetParam()
            self?.refresh()
        }, for: .touchUpInside)
        resetButton.accessibilityHint = "Puts the effect chosen back to its normal value"

        slidersStack.axis = .vertical
        slidersStack.spacing = 4
        for row in [seekRow, seekUnitRow, effectRow, adjustRow, resetButton] { slidersStack.addArrangedSubview(row) }

        // The touch area says what it did, through the engine's own speech
        touchView.onSeek = { [weak self] step in self?.engine.seekStep(step); self?.refresh() }
        touchView.onSeekUnit = { [weak self] step in self?.engine.changeSeekUnit(step); self?.refresh() }
        touchView.onParam = { [weak self] step in self?.engine.cycleParam(step); self?.refresh() }
        touchView.onAdjust = { [weak self] step in self?.engine.adjustParam(step); self?.refresh() }
        touchView.onPlayPause = { [weak self] in self?.engine.playPause() }
        touchView.onScrubStart = { [weak self] direction in self?.engine.startScrubbing(direction) }
        touchView.onScrubStop = { [weak self] in self?.engine.stopScrubbing(); self?.refresh() }
        touchView.heightAnchor.constraint(greaterThanOrEqualToConstant: 220).isActive = true
        touchView.setContentHuggingPriority(.defaultLow - 1, for: .vertical)

        let stack = UIStackView(arrangedSubviews: [
            titleLabel, artistLabel, statusLabel, positionSlider, times, transport, recordButton, modeControl,
            slidersStack, seekModeButton, touchView,
        ])
        stack.axis = .vertical
        stack.spacing = 10
        stack.setCustomSpacing(2, after: titleLabel)
        stack.setCustomSpacing(2, after: artistLabel)
        stack.setCustomSpacing(2, after: positionSlider)
        stack.setCustomSpacing(4, after: transport)
        stack.setCustomSpacing(12, after: recordButton)
        stack.translatesAutoresizingMaskIntoConstraints = false

        scroll.translatesAutoresizingMaskIntoConstraints = false
        scroll.alwaysBounceVertical = true
        scroll.addSubview(stack)
        view.addSubview(scroll)
        let content = scroll.contentLayoutGuide
        let frame = scroll.frameLayoutGuide
        NSLayoutConstraint.activate([
            scroll.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor),
            scroll.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor),
            scroll.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            stack.topAnchor.constraint(equalTo: content.topAnchor, constant: 12),
            stack.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -16),
            stack.leadingAnchor.constraint(equalTo: frame.leadingAnchor, constant: 20),
            stack.trailingAnchor.constraint(equalTo: frame.trailingAnchor, constant: -20),
        ])
        fillScreen = stack.heightAnchor.constraint(equalTo: frame.heightAnchor, constant: -28)

        stateObserver = NotificationCenter.default.addObserver(
            forName: .FPEngineStateDidChange, object: nil, queue: .main) { [weak self] _ in
            self?.refresh()
        }
        applyMode()
        refresh()
    }

    private func configureTransport(_ button: UIButton, symbol: String, label: String, action: Selector) {
        let size = UIImage.SymbolConfiguration(textStyle: .largeTitle)
        button.setImage(UIImage(systemName: symbol, withConfiguration: size), for: .normal)
        button.accessibilityLabel = label
        button.addTarget(self, action: action, for: .touchUpInside)
        button.heightAnchor.constraint(greaterThanOrEqualToConstant: 60).isActive = true
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        navigationController?.setToolbarHidden(true, animated: animated)  // this is the player
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            self?.refreshPosition()
        }
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        applyMode()  // the navigation controller's gestures are there to turn off now
    }

    override func viewWillDisappear(_ animated: Bool) {
        super.viewWillDisappear(animated)
        timer?.invalidate()
        timer = nil
        setBackSwipeEnabled(true)
    }

    /// The swipe that goes back a screen (from the edge, and on newer systems from
    /// anywhere), which would otherwise take the touch area's swipes to the right.
    private func setBackSwipeEnabled(_ enabled: Bool) {
        guard let navigation = navigationController else { return }
        navigation.interactivePopGestureRecognizer?.isEnabled = enabled
        // (The swipe from anywhere came with iOS 26; an older Xcode does not know it.)
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            navigation.interactiveContentPopGestureRecognizer?.isEnabled = enabled
        }
        #endif
    }

    // MARK: Actions

    /// Runs a step of one of the sliders with the engine's speech off.
    private func quietly(_ action: (FPEngine) -> Void) {
        engine.speechMuted = true
        action(engine)
        engine.speechMuted = false
        refresh()
    }

    @objc private func playPause() { engine.playPause() }
    @objc private func previousTrack() { engine.previousTrack() }
    @objc private func nextTrack() { engine.nextTrack() }

    @objc private func positionTouched() { draggingPosition = true }

    @objc private func positionChanged() {
        elapsedLabel.text = FPEngine.formatTime(Double(positionSlider.value))
        // VoiceOver's swipes on the slider change it without a touch: seek at once
        if !positionSlider.isTracking {
            engine.seek(to: Double(positionSlider.value))
        }
    }

    @objc private func positionReleased() {
        draggingPosition = false
        engine.seek(to: Double(positionSlider.value))
    }

    @objc private func modeChanged() {
        UserDefaults.standard.set(modeControl.selectedSegmentIndex, forKey: Self.modeKey)
        applyMode()
        refresh()
        // Take VoiceOver to the controls that just appeared
        let target: UIView = mode == .touch ? touchView : seekRow
        UIAccessibility.post(notification: .layoutChanged, argument: target)
    }

    @objc private func openSettings() {
        navigationController?.pushViewController(SettingsViewController(), animated: true)
    }

    @objc private func openPlaylist() {
        navigationController?.pushViewController(PlaylistViewController(), animated: true)
    }

    // MARK: Showing what is so

    private var mode: Mode { Mode(rawValue: modeControl.selectedSegmentIndex) ?? .sliders }

    private func applyMode() {
        let touch = mode == .touch
        slidersStack.isHidden = touch
        touchView.isHidden = !touch
        seekModeButton.isHidden = !touch
        // Touch mode seeks as it last did; the sliders jump
        let touchSeekMode = FPSeekMode(rawValue: UserDefaults.standard.integer(forKey: Self.touchSeekModeKey)) ?? .jump
        engine.seekMode = touch ? touchSeekMode : .jump
        // In touch mode every swipe belongs to the touch area: the screen does not
        // scroll and a swipe right does not go back (the Back button still does)
        scroll.isScrollEnabled = !touch
        fillScreen?.isActive = touch
        if viewIfLoaded?.window != nil { setBackSwipeEnabled(!touch) }
    }

    private func refresh() {
        guard isViewLoaded else { return }
        let loaded = engine.loaded
        titleLabel.text = loaded ? engine.title : "Nothing playing"
        // The title often has the artist in it already ("Artist - Title")
        let artist = engine.artist
        artistLabel.text = artist
        artistLabel.isHidden = artist.isEmpty || engine.title.localizedCaseInsensitiveContains(artist)
        statusLabel.text = engine.statusText
        statusLabel.isHidden = engine.statusText.isEmpty

        let playing = engine.playing
        let size = UIImage.SymbolConfiguration(textStyle: .largeTitle)
        playPauseButton.setImage(UIImage(systemName: playing ? "pause.fill" : "play.fill", withConfiguration: size),
                                 for: .normal)
        playPauseButton.accessibilityLabel = playing ? "Pause" : "Play"
        let several = engine.trackCount > 1
        previousButton.isEnabled = several
        nextButton.isEnabled = several
        playPauseButton.isEnabled = loaded || engine.trackCount > 0

        seekUnitRow.value = engine.seekUnitText
        let paramText = engine.paramText
        effectRow.value = engine.paramName.isEmpty ? "No effects are on" : engine.paramName
        adjustRow.value = paramText.isEmpty ? "Nothing to adjust" : paramText
        seekModeButton.setTitle("Seek Mode: \(engine.seekModeName)", for: .normal)
        resetButton.isEnabled = !paramText.isEmpty

        let recording = engine.recording
        recordButton.setTitle(recording ? "Stop Recording" : "Record", for: .normal)
        recordButton.setImage(UIImage(systemName: recording ? "stop.circle.fill" : "record.circle"), for: .normal)
        recordButton.tintColor = recording ? .systemRed : nil
        recordButton.isEnabled = loaded || recording

        touchView.scrubMode = engine.scrubSeekMode
        touchView.seekLabel.text = engine.scrubSeekMode ? "\(engine.seekModeName) seeking, \(engine.seekUnitText)"
                                                        : "Seek by \(engine.seekUnitText)"
        touchView.effectLabel.text = paramText.isEmpty ? "No effects are on" : paramText
        refreshPosition()
    }

    private func refreshPosition() {
        let length = engine.length
        let position = engine.position
        let seekable = engine.loaded && length > 0
        positionSlider.isEnabled = seekable
        positionSlider.maximumValue = Float(max(length, 1))
        if !draggingPosition { positionSlider.value = Float(min(position, max(length, 1))) }
        let elapsed = FPEngine.formatTime(position)
        elapsedLabel.text = engine.loaded ? elapsed : ""
        remainingLabel.text = seekable ? FPEngine.formatTime(length) : (engine.live ? "Live" : "")
        let spoken = seekable ? "\(elapsed) of \(FPEngine.formatTime(length))" : (engine.live ? "Live" : elapsed)
        positionSlider.accessibilityValue = spoken
        seekRow.value = engine.loaded ? spoken : "Nothing playing"
    }
}
