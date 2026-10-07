import UIKit

final class SceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?
    private var messageObserver: NSObjectProtocol?

    func scene(_ scene: UIScene, willConnectTo session: UISceneSession, options: UIScene.ConnectionOptions) {
        guard let windowScene = scene as? UIWindowScene else { return }
        let navigation = UINavigationController(rootViewController: HomeViewController())
        navigation.navigationBar.prefersLargeTitles = true
        let window = FastPlayWindow(windowScene: windowScene)
        window.rootViewController = navigation
        window.makeKeyAndVisible()
        self.window = window

        // What the engine has to tell the user (a file that will not play, say)
        messageObserver = NotificationCenter.default.addObserver(
            forName: .FPEngineMessage, object: nil, queue: .main) { [weak self] note in
            let title = note.userInfo?[FPEngineMessageTitleKey] as? String ?? "FastPlay"
            let text = note.userInfo?[FPEngineMessageTextKey] as? String ?? ""
            self?.showMessage(title: title, text: text)
        }
        if let app = UIApplication.shared.delegate as? AppDelegate, !app.engineStarted {
            showMessage(title: "FastPlay", text: "FastPlay could not start its audio engine.")
        }
        open(options.urlContexts)
        #if DEBUG
        runLaunchArguments()
        #endif
    }

    #if DEBUG
    /// For trying things from a script: "-FPPlay <path in FastPlay's folder>" plays a
    /// file, and "-FPShow player|files|settings|playlist|radio|podcasts" opens a screen.
    private func runLaunchArguments() {
        let arguments = CommandLine.arguments
        guard let navigation = window?.rootViewController as? UINavigationController else { return }
        if let index = arguments.firstIndex(of: "-FPPlay"), index + 1 < arguments.count {
            let path = (FPEngine.shared.documentsPath as NSString).appendingPathComponent(arguments[index + 1])
            FPEngine.shared.playFile(path)
            let engine = FPEngine.shared
            NSLog("FPPlay: %d tracks: %@", engine.trackCount,
                  (0..<engine.trackCount).map { engine.trackName(at: $0) }.joined(separator: " | "))
        }
        if let index = arguments.firstIndex(of: "-FPPlayURL"), index + 1 < arguments.count {
            FPEngine.shared.playURL(arguments[index + 1], name: nil)
        }
        // "-FPAutoSyncTest <folder in FastPlay's files>": makes that folder (of this
        // device's own files, which the app never offers) auto sync, so that it is
        // copied under its name, and runs the sync, logging each folder's outcome
        if let index = arguments.firstIndex(of: "-FPAutoSyncTest"), index + 1 < arguments.count {
            let path = (FPEngine.shared.documentsPath as NSString).appendingPathComponent(arguments[index + 1])
            let name = (arguments[index + 1] as NSString).lastPathComponent
            AutoSyncStore.add(source: LocalSource.shared,
                              folder: FileEntry(name: name, path: path, displayPath: path, isFolder: true))
            Task { @MainActor in
                await AutoSync.runAll(announce: false)
                for folder in AutoSyncStore.all { NSLog("FPAutoSync: %@: %@", folder.name, folder.lastOutcome ?? "-") }
            }
        }
        // "-FPAddress <address>": as Open Address plays it (a playlist's entries as tracks)
        if let index = arguments.firstIndex(of: "-FPAddress"), index + 1 < arguments.count {
            Task { @MainActor in
                await PlaylistText.play(address: arguments[index + 1])
                let engine = FPEngine.shared
                NSLog("FPAddress: %d tracks: %@", engine.trackCount,
                      (0..<engine.trackCount).map { engine.trackName(at: $0) }.joined(separator: " | "))
            }
        }
        // "-FPStress <address>": starts the stream again and again, each cutting the
        // last one off as it connects, to shake out trouble in abandoning a connection
        if let index = arguments.firstIndex(of: "-FPStress"), index + 1 < arguments.count {
            let url = arguments[index + 1]
            var left = 40
            Timer.scheduledTimer(withTimeInterval: 0.35, repeats: true) { timer in
                FPEngine.shared.playURL(url, name: "Stress \(left)")
                NSLog("FPStress: %d left", left)
                left -= 1
                if left == 0 {
                    timer.invalidate()
                    NSLog("FPStress: finished")
                }
            }
        }
        // "-FPStressSeek <address>": plays the stream and seeks all over it, so the
        // download reconnects again and again, starting it afresh now and then
        if let index = arguments.firstIndex(of: "-FPStressSeek"), index + 1 < arguments.count {
            let url = arguments[index + 1]
            var tick = 0
            FPEngine.shared.playURL(url, name: "Stress")
            Timer.scheduledTimer(withTimeInterval: 0.4, repeats: true) { timer in
                tick += 1
                let engine = FPEngine.shared
                if tick % 12 == 0 {
                    engine.playURL(url, name: "Stress")
                } else if engine.length > 0 {
                    engine.seek(to: Double.random(in: 0..<engine.length))
                }
                NSLog("FPStressSeek: tick %d, at %.0f of %.0f", tick, engine.position, engine.length)
                if tick == 90 {
                    timer.invalidate()
                    NSLog("FPStressSeek: finished")
                }
            }
        }
        // "-FPStressFx <address>": plays the stream while going through the effect
        // parameters and changing each, as fast as a hand could and faster
        if let index = arguments.firstIndex(of: "-FPStressFx"), index + 1 < arguments.count {
            let url = arguments[index + 1]
            var tick = 0
            FPEngine.shared.playURL(url, name: "Stress")
            Timer.scheduledTimer(withTimeInterval: 0.12, repeats: true) { timer in
                tick += 1
                let engine = FPEngine.shared
                if tick % 7 == 0 { engine.cycleParam(1) } else { engine.adjustParam(Bool.random() ? 1 : -1) }
                if tick % 10 == 0 { NSLog("FPStressFx: tick %d, %@, at %.0f", tick, engine.paramText, engine.position) }
                if tick == 400 {
                    timer.invalidate()
                    NSLog("FPStressFx: finished")
                }
            }
        }
        // "-FPServer ftp|sftp|smb,host,port,user,password,share": browses a server without
        // saving it; with "-FPServerPlay <path>" plays that file from it, and with
        // "-FPServerDownload <path>" copies it here, logging how each went
        if let index = arguments.firstIndex(of: "-FPServer"), index + 1 < arguments.count {
            let parts = arguments[index + 1].components(separatedBy: ",")
            if parts.count >= 6 {
                var server = RemoteServer()
                server.kind = RemoteServer.Kind(rawValue: parts[0]) ?? .ftp
                server.host = parts[1]
                server.port = Int(parts[2])
                server.user = parts[3]
                server.share = parts[5]
                server.name = "Test server"
                let source = server.makeSource(password: parts[4])
                if !arguments.contains("-FPServerQuiet") {
                    navigation.pushViewController(
                        BrowserViewController(source: source, path: server.startPath, name: server.displayName),
                        animated: false)
                }
                func argument(_ name: String) -> String? {
                    arguments.firstIndex(of: name).flatMap { $0 + 1 < arguments.count ? arguments[$0 + 1] : nil }
                }
                Task { @MainActor in
                    do {
                        let started = Date()
                        let listed = try await source.listAll(path: server.startPath)
                        NSLog("FPServer: listing took %.1f s", Date().timeIntervalSince(started))
                        NSLog("FPServer: %d files: %@", listed.count, listed.map(\.displayPath).sorted().joined(separator: " | "))
                        if let path = argument("-FPServerPlay"), let entry = listed.first(where: { $0.displayPath == path }) {
                            let url = try await source.streamURL(for: entry)
                            FPEngine.shared.playURLs([url], names: [entry.name], startingAt: 0)
                            for second in 1...8 {
                                try await Task.sleep(nanoseconds: 1_000_000_000)
                                if second == 4 { FPEngine.shared.seek(to: FPEngine.shared.length * 0.7) }
                                NSLog("FPServer: playing %@ at %.1f of %.1f", FPEngine.shared.title,
                                      FPEngine.shared.position, FPEngine.shared.length)
                            }
                        }
                        if let path = argument("-FPServerDownload"), let entry = listed.first(where: { $0.displayPath == path }) {
                            let target = URL(fileURLWithPath: FPEngine.shared.documentsPath).appendingPathComponent("server-test-" + entry.name)
                            try await source.download(entry, to: target)
                            let size = (try? FileManager.default.attributesOfItem(atPath: target.path)[.size] as? NSNumber)?.int64Value ?? -1
                            NSLog("FPServer: downloaded %lld bytes (the server says %lld)", size, entry.size)
                            try? FileManager.default.removeItem(at: target)
                        }
                    } catch {
                        NSLog("FPServer: failed: %@", error.localizedDescription)
                    }
                    NSLog("FPServer: finished")
                }
            }
        }
        // "-FPTransferTest": saves stations and a podcast, exports them, deletes them,
        // imports them back, and logs what came of each step
        if arguments.contains("-FPTransferTest") {
            let engine = FPEngine.shared
            let folder = FileManager.default.temporaryDirectory
            let m3u = folder.appendingPathComponent("test.m3u").path
            let opml = folder.appendingPathComponent("test.opml").path
            engine.addRadioFavoriteNamed("Test One", url: "http://example.com/one.mp3")
            engine.addRadioFavoriteNamed("Zwei \u{00FC}ber", url: "http://example.com/zwei.mp3")
            engine.subscribeToPodcast(named: "Test Cast", feedURL: "https://example.com/feed.xml")
            NSLog("FPTransfer: before: %d stations, %d podcasts", engine.radioFavorites.count, engine.podcasts.count)
            NSLog("FPTransfer: export m3u %d, opml %d", engine.exportRadioFavorites(toFile: m3u) ? 1 : 0,
                  engine.exportPodcasts(toOPML: opml) ? 1 : 0)
            for station in engine.radioFavorites where station.url.contains("example.com") {
                engine.removeRadioFavorite(station.identifier)
            }
            for podcast in engine.podcasts where podcast.feedURL.contains("example.com") {
                engine.removePodcast(podcast.identifier)
            }
            NSLog("FPTransfer: after deleting: %d stations, %d podcasts", engine.radioFavorites.count, engine.podcasts.count)
            var skipped = 0
            let stations = engine.importRadioFavorites(fromFile: m3u, skipped: &skipped)
            NSLog("FPTransfer: imported %d stations (%d skipped): %@", stations, skipped,
                  engine.radioFavorites.map { "\($0.name) = \($0.url)" }.joined(separator: " | "))
            let again = engine.importRadioFavorites(fromFile: m3u, skipped: &skipped)
            NSLog("FPTransfer: imported again: %d added, %d skipped", again, skipped)
            let casts = engine.importPodcasts(fromOPML: opml, skipped: &skipped)
            NSLog("FPTransfer: imported %d podcasts (%d skipped): %@", casts, skipped,
                  engine.podcasts.map { "\($0.name) = \($0.feedURL)" }.joined(separator: " | "))
            for station in engine.radioFavorites where station.url.contains("example.com") {
                engine.removeRadioFavorite(station.identifier)
            }
            for podcast in engine.podcasts where podcast.feedURL.contains("example.com") {
                engine.removePodcast(podcast.identifier)
            }
            NSLog("FPTransfer: finished")
        }
        // "-FPRecordTest <format 0 to 3>": records four seconds of what is playing
        if let index = arguments.firstIndex(of: "-FPRecordTest"), index + 1 < arguments.count {
            let engine = FPEngine.shared
            engine.recordingFormat = Int(arguments[index + 1]) ?? 0
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) {
                engine.toggleRecording()
                NSLog("FPRecord: recording %d", engine.recording ? 1 : 0)
                DispatchQueue.main.asyncAfter(deadline: .now() + 4) {
                    engine.toggleRecording()
                    let folder = (engine.documentsPath as NSString).appendingPathComponent("Recordings")
                    let files = (try? FileManager.default.contentsOfDirectory(atPath: folder)) ?? []
                    for file in files {
                        let path = (folder as NSString).appendingPathComponent(file)
                        let size = (try? FileManager.default.attributesOfItem(atPath: path)[.size] as? NSNumber)?.intValue ?? 0
                        NSLog("FPRecord: %@ %d bytes", file, size)
                    }
                    NSLog("FPRecord: finished, recording %d", engine.recording ? 1 : 0)
                }
            }
        }
        // "-FPSettingsPage <title>" opens one of the settings screens; "-FPSettingsTest"
        // changes every named option, reads it back, and puts it back as it was
        if let index = arguments.firstIndex(of: "-FPSettingsPage"), index + 1 < arguments.count,
           let page = SettingsViewController.page(titled: arguments[index + 1]) {
            navigation.pushViewController(page, animated: false)
        }
        if arguments.contains("-FPSettingsTest") {
            let engine = FPEngine.shared
            let trials: [(String, Double)] = [
                ("allowAmplify", 1), ("rememberState", 0), ("rememberPosMinutes", 20), ("loadFolder", 0),
                ("autoAdvance", 0), ("shuffle", 1), ("repeatMode", 2), ("rewindOnPauseMs", 1500),
                ("volumeStepPercent", 10), ("replayGainMode", 2), ("replayGainPreamp", -3.5),
                ("replayGainPreventClip", 0), ("recordFormat", 3), ("recordBitrate", 320), ("recordEffects", 0),
                ("speechTrackChange", 0), ("speechVolume", 0), ("speechEffect", 0), ("rateStepMode", 1),
                ("reverbAlgorithm", 2), ("bufferSize", 1000), ("tempoAlgorithm", 1), ("eqBassFreq", 80),
                ("eqMidFreq", 1500), ("eqTrebleFreq", 9000), ("smoothSeek", 0), ("liveRewind", 1),
                ("liveRewindMinutes", 30), ("speedyNonlinear", 0), ("ssPreset", 1), ("ssTonalityLimit", 6000),
                ("midiMaxVoices", 64), ("midiSincInterp", 1),
            ]
            var failed: [String] = []
            for (name, value) in trials {
                let before = engine.number(forSetting: name)
                engine.setNumber(value, forSetting: name)
                if abs(engine.number(forSetting: name) - value) > 0.001 { failed.append(name) }
                engine.setNumber(before, forSetting: name)
                if abs(engine.number(forSetting: name) - before) > 0.001 { failed.append(name + " (restore)") }
            }
            NSLog("FPSettings: %d options tried, failed: %@", trials.count, failed.isEmpty ? "none" : failed.joined(separator: ", "))
        }
        if let index = arguments.firstIndex(of: "-FPRadioSearch"), index + 1 < arguments.count {
            let radio = RadioViewController()
            navigation.pushViewController(radio, animated: false)
            radio.debugSearch(arguments[index + 1])
        }
        if let index = arguments.firstIndex(of: "-FPFeed"), index + 1 < arguments.count {
            navigation.pushViewController(EpisodesViewController(name: "Podcast", feedURL: arguments[index + 1]),
                                          animated: false)
        }
        if let index = arguments.firstIndex(of: "-FPShow"), index + 1 < arguments.count {
            let screen: UIViewController?
            switch arguments[index + 1] {
            case "player": screen = PlayerViewController()
            case "files": screen = BrowserViewController(source: LocalSource.shared)
            case "settings": screen = SettingsViewController()
            case "address": screen = AddressViewController()
            case "autosync": screen = SettingsViewController.page(titled: "Auto Sync")
            case "playlist": screen = PlaylistViewController()
            case "radio": screen = RadioViewController()
            case "servers": screen = ServersViewController()
            case "podcasts": screen = PodcastsViewController()
            default: screen = nil
            }
            if let screen { navigation.pushViewController(screen, animated: false) }
        }
    }
    #endif

    func scene(_ scene: UIScene, openURLContexts URLContexts: Set<UIOpenURLContext>) {
        open(URLContexts)
    }

    func sceneDidEnterBackground(_ scene: UIScene) {
        FolderResume.shared.record()
        FPEngine.shared.saveState()
    }

    // At the start, and on coming back to the front after a while: the folders
    // that sync by themselves
    func sceneWillEnterForeground(_ scene: UIScene) {
        #if DEBUG
        if CommandLine.arguments.contains("-FPAutoSyncTest") { return }
        #endif
        AutoSync.runIfDue()
    }

    /// A file handed over by Files or another app ("Open in FastPlay"): played where it is.
    private func open(_ contexts: Set<UIOpenURLContext>) {
        guard let url = contexts.first?.url, url.isFileURL else { return }
        // A file outside FastPlay's own folder is only readable while this is held,
        // so it is held for as long as FastPlay runs.
        _ = url.startAccessingSecurityScopedResource()
        FPEngine.shared.playFile(url.path)
        showPlayer()
    }

    private func showPlayer() {
        guard let navigation = window?.rootViewController as? UINavigationController else { return }
        if navigation.topViewController is PlayerViewController { return }
        navigation.presentedViewController?.dismiss(animated: false)
        navigation.pushViewController(PlayerViewController(), animated: true)
    }

    private func showMessage(title: String, text: String) {
        guard var top = window?.rootViewController else { return }
        while let presented = top.presentedViewController { top = presented }
        let alert = UIAlertController(title: title, message: text, preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "OK", style: .default))
        top.present(alert, animated: true)
    }
}

/// The window answers VoiceOver's magic tap (a two finger double tap anywhere):
/// play or pause, wherever in FastPlay you are.
final class FastPlayWindow: UIWindow {
    override func accessibilityPerformMagicTap() -> Bool {
        guard FPEngine.shared.loaded else { return false }
        FPEngine.shared.playPause()
        return true
    }
}
