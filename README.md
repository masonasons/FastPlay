# FastPlay

A fast, accessible audio player for Windows with support for tempo/pitch shifting, effects, and screen reader accessibility.

## Features

- Tempo, pitch, and rate adjustment with multiple algorithms (SoundTouch, Rubber Band, Speedy)
- Audio effects (reverb, echo, EQ, compressor, stereo width, center/vocal cancel)
- Recording/encoding to MP3, OGG, FLAC
- Internet radio streaming with favorites
- YouTube audio playback
- Screen reader support via Universal Speech
- Hotkey support
- Chapter navigation for audiobooks

## Prerequisites

- **Visual C++ Build Tools** (or Visual Studio with C++ workload)
  - Download from [Visual Studio Downloads](https://visualstudio.microsoft.com/downloads/) → "Tools for Visual Studio" → "Build Tools for Visual Studio"
  - Install the "Desktop development with C++" workload

## Dependencies

### BASS Audio Libraries

Download from [un4seen.com](https://www.un4seen.com/bass.html) and place the following in the `lib/` folder:

**Libraries (.lib files for x64):**
- bass.lib
- bass_fx.lib
- bass_aac.lib
- bassmidi.lib
- bassenc.lib
- bassenc_mp3.lib
- bassenc_ogg.lib
- bassenc_flac.lib

**DLLs (place in lib/ folder):**
- bass.dll
- bass_fx.dll
- bass_aac.dll
- bassmidi.dll
- bassenc.dll
- bassenc_mp3.dll
- bassenc_ogg.dll
- bassenc_flac.dll

### SQLite

Download the [SQLite amalgamation](https://www.sqlite.org/download.html) and place `sqlite3.c` in the `src/` folder.

### Universal Speech (optional, for screen reader support)

Clone [UniversalSpeechMSVCStatic](https://github.com/samtupy/UniversalSpeechMSVCStatic), build it with `c.bat`, then place `bin-x64\UniversalSpeechStatic.lib` in the `lib/` folder.

### Rubber Band (optional, for high-quality pitch shifting)

Download [Rubber Band Library](https://breakfastquay.com/rubberband/) v4.0.0 and extract to `deps/rubberband-4.0.0/`.

### Speedy/Sonic (optional, for fast tempo algorithm)

Place the following in `deps/`:
- `deps/speedy/` - Speedy library source
- `deps/sonic/` - Sonic library source
- `deps/kissfft/` - KissFFT source

## Building

Open a command prompt and run:

```batch
build_new.bat
```

### Build Options

Disable screen reader support:
```batch
build_new.bat no-speech
```

Disable Rubber Band:
```batch
build_new.bat no-rubberband
```

Combine options:
```batch
build_new.bat no-speech no-rubberband
```

## Running

After building, run `FastPlay.exe`. DLLs are loaded from the `lib/` subfolder.

## Project Structure

```
FastPlay/
├── src/              # Source files
├── include/          # Header files
├── lib/              # BASS libraries and DLLs
├── deps/             # Third-party dependencies (Rubber Band, Speedy, etc.)
├── build_new.bat     # Build script
├── FastPlay.rc       # Resource file
└── FastPlay.exe      # Output executable
```

## License

This project uses the following third-party libraries:
- BASS and related libraries (commercial/free for non-commercial use)
- SQLite (public domain)
- Rubber Band Library (GPL)
- Universal Speech (MIT)
