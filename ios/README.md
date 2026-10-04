# FastPlay for iPhone and iPad

The same engine as the desktop FastPlay (the C++ in `src/`, without its wxWidgets
windows), under UIKit screens written in Swift.

## Building

Once, from the repository root:

    ./download-deps.sh                          # what a Mac build needs too
    ci/ffmpeg/build.sh ios ffmpeg-ios           # FFmpeg for the iPhone and the simulator
    cd ios && xcodegen                          # writes FastPlay.xcodeproj

Then open `ios/FastPlay.xcodeproj` and run. Each build first brings the engine up
to date for the SDK in use (`ios/build-core.sh`, a CMake build of the repository's
`CMakeLists.txt` for iOS, which makes `ios/build/lib/<sdk>/libFastPlayCore.a`).
Run `xcodegen` again after adding or removing files.

## TestFlight

`ios/testflight.sh` archives the app, exports it for the App Store and uploads it to
TestFlight, with an App Store Connect API key (`--no-upload` stops after the export).
GitHub runs it for every push to master that changes the app or the engine
(`.github/workflows/ios-testflight.yml`), with the key from the repository's secrets
`ASC_KEY_ID`, `ASC_ISSUER_ID` and `ASC_KEY_P8_B64`. The version is `APP_VERSION` in
`include/fastplay/version.h`; the build number is the count of commits.

The icon is drawn by `ios/scripts/make-icon.py`; replace `AppIcon.png` with real
artwork (1024 pixels square, no transparency) whenever there is some.

## How it is put together

- `FastPlay/Bridge`: `FPEngine`, the engine's Objective-C face for Swift, and where
  the core's calls to "the main window" (`app_ui.h`) arrive. `FPSources` adds radio
  and podcasts.
- `FastPlay/Controllers`, `FastPlay/Views`: the screens. The player has two modes:
  four adjustable sliders (Seek, Seek By, Effect, Adjust), and a touch area that
  takes swipes for the same four things, which VoiceOver passes straight through.
- `src/platform/*_ios.mm`: the engine's iOS side: paths (the Documents folder is
  the one the Files app shows), speech through VoiceOver, HTTP through NSURLSession.
- `FastPlay/Remote`: Dropbox, FTP and SMB servers behind one browser
  (`RemoteFileSource`): streaming, Download and Sync. FTP is FFmpeg's; SMB is
  AMSMB2 (libsmb2), served to the engine by a loopback HTTP server so it can seek.

A Debug build takes launch arguments for trying things from a script:
`-FPPlay <path in FastPlay's folder>`, `-FPPlayURL <address>`,
`-FPShow player|files|settings|playlist|radio|podcasts|servers`, `-FPRadioSearch <words>`,
`-FPFeed <feed address>`, and more for stress and server tests (see `SceneDelegate.swift`). With `FASTPLAY_NULL_AUDIO=1` in the environment the
engine plays to nothing, for a simulator that should stay quiet.
