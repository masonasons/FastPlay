import UIKit

/// One row of a settings screen.
enum SettingRow {
    case toggle(title: String, isOn: () -> Bool, set: (Bool) -> Void)
    /// One of a few named values.
    case choice(title: String, options: [String], selected: () -> Int, set: (Int) -> Void)
    /// A number typed in, kept within `range`.
    case number(title: String, unit: String, range: ClosedRange<Double>, value: () -> Double, set: (Double) -> Void)
    /// A file chosen in the Files app (and Clear, once one is).
    case file(title: String, extensions: [String], name: () -> String, set: (String?) -> Bool)
    case info(title: String, detail: String)
    /// Opens another screen.
    case screen(title: String, detail: String, symbol: String, make: () -> UIViewController)
}

struct SettingSection {
    var title: String?
    var footer: String?
    var rows: [SettingRow]
}

/// A screen of settings: switches, choices, numbers and files, in sections.
class SettingsPageViewController: UITableViewController {
    let engine = FPEngine.shared
    private var sections: [SettingSection] = []
    private let makeSections: (FPEngine) -> [SettingSection]
    private let files = FileTransfer()

    init(title: String, sections: @escaping (FPEngine) -> [SettingSection]) {
        makeSections = sections
        super.init(style: .insetGrouped)
        self.title = title
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    override func viewDidLoad() {
        super.viewDidLoad()
        navigationItem.largeTitleDisplayMode = .never
        tableView.register(UITableViewCell.self, forCellReuseIdentifier: "cell")
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        navigationController?.setToolbarHidden(true, animated: animated)
        sections = makeSections(engine)  // one choice can change what another offers
        tableView.reloadData()
    }

    private func row(at indexPath: IndexPath) -> SettingRow {
        sections[indexPath.section].rows[indexPath.row]
    }

    private static func text(_ value: Double) -> String {
        value == value.rounded() ? String(Int(value)) : String(format: "%g", value)
    }

    // MARK: Table

    override func numberOfSections(in tableView: UITableView) -> Int { sections.count }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        sections[section].rows.count
    }

    override func tableView(_ tableView: UITableView, titleForHeaderInSection section: Int) -> String? {
        sections[section].title
    }

    override func tableView(_ tableView: UITableView, titleForFooterInSection section: Int) -> String? {
        sections[section].footer
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: "cell", for: indexPath)
        cell.accessoryView = nil
        cell.accessoryType = .none
        cell.selectionStyle = .default
        cell.accessibilityHint = nil
        var content = UIListContentConfiguration.valueCell()
        content.textProperties.numberOfLines = 0
        switch row(at: indexPath) {
        case let .toggle(title, isOn, set):
            content.text = title
            let toggle = UISwitch()
            toggle.isOn = isOn()
            toggle.addAction(UIAction { action in
                set((action.sender as? UISwitch)?.isOn ?? false)
            }, for: .valueChanged)
            toggle.accessibilityLabel = title
            cell.accessoryView = toggle
            cell.selectionStyle = .none
        case let .choice(title, options, selected, _):
            content.text = title
            content.secondaryText = options.isEmpty ? "" : options[min(max(selected(), 0), options.count - 1)]
            cell.accessoryType = .disclosureIndicator
        case let .number(title, unit, _, value, _):
            content.text = title
            content.secondaryText = unit.isEmpty ? Self.text(value()) : "\(Self.text(value())) \(unit)"
            cell.accessibilityHint = "Opens a box to type a new value"
        case let .file(title, _, name, _):
            content.text = title
            content.secondaryText = name().isEmpty ? "None" : name()
            cell.accessibilityHint = "Chooses a file"
        case let .info(title, detail):
            content = UIListContentConfiguration.subtitleCell()
            content.text = title
            content.secondaryText = detail
            content.secondaryTextProperties.numberOfLines = 0
            cell.selectionStyle = .none
        case let .screen(title, detail, symbol, _):
            content = UIListContentConfiguration.subtitleCell()
            content.text = title
            content.secondaryText = detail
            content.image = UIImage(systemName: symbol)
            cell.accessoryType = .disclosureIndicator
        }
        cell.contentConfiguration = content
        return cell
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        let cell = tableView.cellForRow(at: indexPath)
        switch row(at: indexPath) {
        case .toggle, .info:
            break
        case let .screen(_, _, _, make):
            navigationController?.pushViewController(make(), animated: true)
        case let .choice(title, options, selected, set):
            let sheet = UIAlertController(title: title, message: nil, preferredStyle: .actionSheet)
            for (index, option) in options.enumerated() {
                let action = UIAlertAction(title: option, style: .default) { [weak self] _ in
                    set(index)
                    self?.refresh()
                }
                if index == selected() { action.setValue(true, forKey: "checked") }
                sheet.addAction(action)
            }
            sheet.addAction(UIAlertAction(title: "Cancel", style: .cancel))
            sheet.popoverPresentationController?.sourceView = cell
            sheet.popoverPresentationController?.sourceRect = cell?.bounds ?? .zero
            present(sheet, animated: true)
        case let .number(title, unit, range, value, set):
            let bounds = "From \(Self.text(range.lowerBound)) to \(Self.text(range.upperBound))" + (unit.isEmpty ? "." : " \(unit).")
            let alert = UIAlertController(title: title, message: bounds, preferredStyle: .alert)
            alert.addTextField { field in
                field.text = Self.text(value())
                field.keyboardType = range.lowerBound < 0 ? .numbersAndPunctuation : .decimalPad
                field.accessibilityLabel = title
                field.clearButtonMode = .whileEditing
            }
            alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
            alert.addAction(UIAlertAction(title: "Set", style: .default) { [weak self, weak alert] _ in
                let typed = (alert?.textFields?.first?.text ?? "").replacingOccurrences(of: ",", with: ".")
                guard let number = Double(typed) else { return }
                set(min(max(number, range.lowerBound), range.upperBound))
                self?.refresh()
            })
            present(alert, animated: true)
        case let .file(title, extensions, name, set):
            let sheet = UIAlertController(title: title, message: name().isEmpty ? nil : name(), preferredStyle: .actionSheet)
            sheet.addAction(UIAlertAction(title: "Choose a File", style: .default) { [weak self] _ in
                guard let self else { return }
                self.files.importFile(extensions: extensions, from: self) { url in
                    let ok = set(url.path)
                    try? FileManager.default.removeItem(at: url)
                    self.refresh()
                    if !ok {
                        let alert = UIAlertController(title: title, message: "That file could not be used.", preferredStyle: .alert)
                        alert.addAction(UIAlertAction(title: "OK", style: .default))
                        self.present(alert, animated: true)
                    }
                }
            })
            if !name().isEmpty {
                sheet.addAction(UIAlertAction(title: "Clear", style: .destructive) { [weak self] _ in
                    _ = set(nil)
                    self?.refresh()
                })
            }
            sheet.addAction(UIAlertAction(title: "Cancel", style: .cancel))
            sheet.popoverPresentationController?.sourceView = cell
            sheet.popoverPresentationController?.sourceRect = cell?.bounds ?? .zero
            present(sheet, animated: true)
        }
    }

    private func refresh() {
        sections = makeSections(engine)
        tableView.reloadData()
    }
}

/// The settings, a screen for each of the desktop Options window's tabs that means
/// something on a phone, in the same order and with the same options in each.
final class SettingsViewController: SettingsPageViewController {
    init() {
        super.init(title: "Settings") { _ in
            [SettingSection(title: nil, footer: nil, rows: Self.pages.map { page in
                .screen(title: page.title, detail: page.detail, symbol: page.symbol) { page.screen() }
            })]
        }
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    /// One of the screens by its title, for opening it directly.
    static func page(titled title: String) -> UIViewController? {
        pages.first { $0.title == title }?.screen()
    }

    private struct Page {
        let title: String
        let detail: String
        let symbol: String
        /// A screen of its own, instead of one made from `sections`.
        var make: (() -> UIViewController)? = nil
        let sections: (FPEngine) -> [SettingSection]

        func screen() -> UIViewController {
            make?() ?? SettingsPageViewController(title: title, sections: sections)
        }
    }

    // Shorthands for an option kept by name (FPSettings)
    private static func toggle(_ title: String, _ name: String) -> SettingRow {
        .toggle(title: title, isOn: { FPEngine.shared.number(forSetting: name) != 0 },
                set: { FPEngine.shared.setNumber($0 ? 1 : 0, forSetting: name) })
    }

    /// A choice among values, each with its name: the option holds the value.
    private static func choice(_ title: String, _ name: String, _ options: [(String, Double)]) -> SettingRow {
        .choice(title: title, options: options.map(\.0),
                selected: { options.firstIndex { $0.1 == FPEngine.shared.number(forSetting: name) } ?? 0 },
                set: { FPEngine.shared.setNumber(options[$0].1, forSetting: name) })
    }

    private static func number(_ title: String, _ name: String, unit: String, _ range: ClosedRange<Double>) -> SettingRow {
        .number(title: title, unit: unit, range: range, value: { FPEngine.shared.number(forSetting: name) },
                set: { FPEngine.shared.setNumber($0, forSetting: name) })
    }

    private static func file(_ title: String, _ name: String, _ extensions: [String]) -> SettingRow {
        .file(title: title, extensions: extensions, name: { FPEngine.shared.fileName(forSetting: name) },
              set: { FPEngine.shared.setFile($0, forSetting: name) })
    }

    private static func effect(_ title: String, _ identifier: Int) -> SettingRow {
        .toggle(title: title,
                isOn: { FPEngine.shared.effects.first { $0.identifier == identifier }?.enabled ?? false },
                set: { FPEngine.shared.setEffect(identifier, enabled: $0) })
    }

    private static let pages: [Page] = [
        Page(title: "Playback", detail: "Volume, remembering, ReplayGain", symbol: "play.circle") { engine in [
            SettingSection(title: nil, footer: nil, rows: [
                toggle("Allow Volume Above 100%", "allowAmplify"),
                toggle("Remember Playback State on Exit", "rememberState"),
                choice("Remember Position if Longer Than", "rememberPosMinutes",
                       [("Off", 0), ("5 minutes", 5), ("10 minutes", 10), ("20 minutes", 20), ("30 minutes", 30),
                        ("45 minutes", 45), ("60 minutes", 60)]),
                toggle("Load All Files in Folder When Opening a Single File", "loadFolder"),
                toggle("Auto-Advance to Next Playlist Item", "autoAdvance"),
                toggle("Shuffle", "shuffle"),
                choice("Repeat", "repeatMode", [("Off", 0), ("One", 1), ("All", 2)]),
                number("Rewind on Pause", "rewindOnPauseMs", unit: "ms", 0...60000),
                choice("Volume Step", "volumeStepPercent",
                       [1, 2, 5, 10, 15, 20, 25].map { ("\($0)%", Double($0)) }),
            ]),
            SettingSection(title: "ReplayGain", footer: "Evens out loudness from the gain saved in a file's tags.", rows: [
                choice("ReplayGain", "replayGainMode", [("Off", 0), ("Track", 1), ("Album", 2)]),
                number("Preamp", "replayGainPreamp", unit: "dB", -20...20),
                toggle("Prevent Clipping", "replayGainPreventClip"),
            ]),
            SettingSection(title: "This Device",
                           footer: "On, FastPlay plays alongside other apps instead of stopping them. " +
                               "The lock screen and headphone controls then go to the other app.",
                           rows: [
                               .toggle(title: "Mix With Others", isOn: { engine.mixWithOthers },
                                       set: { engine.mixWithOthers = $0 }),
                           ]),
        ] },
        Page(title: "Recording", detail: "Format and bitrate", symbol: "record.circle") { _ in [
            SettingSection(title: nil,
                           footer: "The Record button in the player records what is playing into the Recordings " +
                               "folder of FastPlay's files. The bitrate applies to MP3 and Ogg Vorbis. With Record " +
                               "With Effects off, recordings have the sound before the effects (tempo, pitch and " +
                               "rate still apply).",
                           rows: [
                               choice("Format", "recordFormat", [("WAV", 0), ("MP3", 1), ("Ogg Vorbis", 2), ("FLAC", 3)]),
                               choice("Bitrate", "recordBitrate", [128, 192, 256, 320].map { ("\($0) kbps", Double($0)) }),
                               toggle("Record With Effects", "recordEffects"),
                           ]),
        ] },
        Page(title: "Speech", detail: "What FastPlay says", symbol: "speaker.wave.2") { _ in [
            SettingSection(title: nil, footer: "FastPlay speaks through VoiceOver, while it is on.", rows: [
                toggle("Announce Track Changes", "speechTrackChange"),
                toggle("Speak Volume When Adjusted", "speechVolume"),
                toggle("Speak Effect Value When Adjusted", "speechEffect"),
            ]),
        ] },
        Page(title: "Movement", detail: "The seek amounts", symbol: "arrow.left.arrow.right") { engine in [
            SettingSection(title: "Seek Amounts",
                           footer: "The amounts Seek By goes through. Tracks need more than one track in the " +
                               "playlist, and chapters a file that has them.",
                           rows: engine.seekUnits.map { unit in
                               let id = unit.identifier
                               return .toggle(title: id == 12 ? "1 chapter (if available)" : unit.name,
                                              isOn: { engine.seekUnits.first { $0.identifier == id }?.enabled ?? false },
                                              set: { engine.setSeekUnit(id, enabled: $0) })
                           }),
        ] },
        Page(title: "Effects", detail: "Which effects are on", symbol: "slider.horizontal.3") { _ in [
            SettingSection(title: "Stream Effects", footer: nil, rows: [
                effect("Volume (0 to 400%)", 0),
                effect("Pitch (-12 to +12 semitones)", 1),
                effect("Tempo (-50% to +100%)", 2),
                effect("Playback Rate (0.5x to 2x)", 3),
                choice("Rate Step", "rateStepMode", [("0.01x", 0), ("Semitone", 1)]),
            ]),
            SettingSection(title: "DSP Effects",
                           footer: "The player's Effect control goes through the settings of the effects that are on. " +
                               "Convolution reverb needs an impulse response file.",
                           rows: [
                               choice("Reverb", "reverbAlgorithm", [("Off", 0), ("Simple", 1), ("Advanced", 2)]),
                               effect("Echo", 101),
                               effect("EQ (Bass, Mid, Treble)", 102),
                               effect("Compressor", 103),
                               effect("Normalizer (a steady level)", 108),
                               effect("Stereo Width (0 to 200%)", 104),
                               effect("Center Cancel (-100 to +100%)", 105),
                               effect("3D Audio (HRTF, Binaural)", 107),
                               effect("Convolution Reverb", 106),
                               file("Impulse Response File", "convolutionIR", ["wav", "flac", "ogg", "mp3"]),
                           ]),
        ] },
        Page(title: "Advanced", detail: "Buffer, tempo algorithm, live streams", symbol: "gearshape.2") { engine in [
            SettingSection(title: "Audio Buffer",
                           footer: "How far ahead audio is prepared: lower answers effect changes sooner, higher is safer.",
                           rows: [
                               choice("Buffer Size", "bufferSize", [100, 200, 300, 500, 1000, 2000].map { ("\($0) ms", Double($0)) }),
                           ]),
            SettingSection(title: "Tempo and Pitch",
                           footer: "Speedy is made for speech, Signalsmith for music. A change applies on the next file.",
                           rows: [
                               choice("Tempo and Pitch Algorithm", "tempoAlgorithm", [("Speedy (Google)", 1), ("Signalsmith Stretch", 2)]),
                           ]),
            SettingSection(title: "EQ Frequencies", footer: "Changes apply the next time the EQ is turned on.", rows: [
                number("Bass", "eqBassFreq", unit: "Hz", 20...500),
                number("Mid", "eqMidFreq", unit: "Hz", 200...5000),
                number("Treble", "eqTrebleFreq", unit: "Hz", 2000...20000),
            ]),
            SettingSection(title: nil,
                           footer: "A live stream kept for rewinding takes about 1 MB a minute at 128 kbps. " +
                               "Applies to streams opened after.",
                           rows: [
                               toggle("Smooth Seeking", "smoothSeek"),
                               toggle("Allow Rewinding and Pausing Live Streams", "liveRewind"),
                               choice("Rewind Length", "liveRewindMinutes",
                                      [5, 10, 15, 30, 60, 120].map { ("\($0) minutes", Double($0)) }),
                           ]),
        ] },
        Page(title: "Speedy", detail: "The speech speedup", symbol: "hare") { _ in [
            SettingSection(title: nil,
                           footer: "Speedy uses a nonlinear speedup made for speech: it compresses vowels more than " +
                               "consonants, for clarity.",
                           rows: [toggle("Enable Nonlinear Speedup", "speedyNonlinear")]),
        ] },
        Page(title: "Signalsmith", detail: "The music time stretch", symbol: "waveform") { _ in [
            SettingSection(title: nil,
                           footer: "Changes apply on the next file. A higher tonality limit keeps more harmonics " +
                               "when shifting pitch: 0 for automatic, 4000 to 8000 for speech, 8000 to 16000 for music.",
                           rows: [
                               choice("Quality Preset", "ssPreset", [("Default", 0), ("Cheaper", 1)]),
                               number("Tonality Limit", "ssTonalityLimit", unit: "Hz", 0...20000),
                           ]),
        ] },
        Page(title: "MIDI", detail: "SoundFont and voices", symbol: "pianokeys") { _ in [
            SettingSection(title: nil, footer: "MIDI files play through the SoundFont chosen here.", rows: [
                file("SoundFont", "midiSoundFont", ["sf2", "sf3", "dls"]),
                number("Max Voices", "midiMaxVoices", unit: "", 1...1000),
                toggle("Sinc Interpolation (Higher Quality, More CPU)", "midiSincInterp"),
            ]),
        ] },
        Page(title: "Auto Sync", detail: "Folders synced to this device when FastPlay starts",
             symbol: "clock.arrow.2.circlepath", make: { AutoSyncViewController() }) { _ in [] },
        Page(title: "About", detail: "Version and audio engine", symbol: "info.circle") { engine in [
            SettingSection(title: nil, footer: nil, rows: [
                .info(title: "Version", detail: engine.version),
                .info(title: "Audio Engine", detail: engine.engineInfo),
            ]),
        ] },
    ]
}
