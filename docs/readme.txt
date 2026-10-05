Welcome to Fastplay!
Fastplay is a very lightweight, simple, yet powerful and flexible media player for Windows. It is designed to be accessible, minimalistic, yet powerful, all at the same time. It is also very small, and fast and responsive.
Note! Fastplay is not designed to be a replacement for a complete media player. Rather, it's designed to be a small, lightweight media player that you can either use seriously or for fun! It has a vast list of effects including an EQ, reverb, echo, tempo/pitch, stereo width, and more, which you can adjust in realtime (See below).
At the core of Fastplay are what we will call virtual sliders. Right now, there are two of these virtual sliders.
The first one adjusts things like your volume and any effects you choose to enable/add. You choose what this slider adjusts by using left and right brackets, and then adjust the value itself with up or down arrows.
The second slider allows you to do things such as seek and move between tracks. You adjust what this slider does by using comma and period, and you adjust the slider with left and right arrows.
You can choose what shows up in these sliders by heading to the options dialog, control comma.
Slash switches how the left and right arrows seek:
* Jump seeking (the default): each press jumps by the amount you chose with comma and period.
* Spring seeking: hold an arrow to play through the audio, forward or backward, sped up without changing its pitch. The longer you hold it, the faster it goes. Let go and it plays on from there. Comma and period set the top speed.
* Tape seeking: hold an arrow to play through the audio like a fast-forwarding or rewinding tape, pitched up with the speed. Let go and it winds back down (to normal speed going forward; going back, to a stop, then it plays on). Comma and period set the speed. In tape seeking, pausing and stopping slow the tape to a standstill first, like a tape deck; press again to pause or stop at once.
Live streams can be rewound and paused too, if you turn it on in the Advanced tab of the options, where you also choose how many minutes are kept. The arrows move back through what has been kept, and L goes back to live.
The library (Control+L) lists the music in your folders by songs, artists, albums, genres and folders. Add the folders in the options, on the Library tab; for each you can choose whether its songs go in the songs, artists, albums and genres lists or only show up under folders. The library keeps itself up to date as files are added, changed or removed. Type in the search box to narrow down the list you're in, and use the sort box to change its order. Enter opens an artist, album, genre or folder (an artist shows its albums, with all its songs first), or plays a song along with the rest of the list; Backspace goes back. The Add to Playlist button adds the selection to the end of the playlist.
The rest of the app is pretty explanitory. Just check out the menus for the rest of the keyboard shortcuts. Some of the other features include:
* Basic Youtube search
* Completely configurable Global hotkeys
* MIDI support
* ID3 tags with numbers 1 through 0.
* URL stream support
* Load all files in folder when clicking on a file.
* Remember playback position of files
* Able to set as default for filetypes.

Enjoy this early beta!
In Options > YouTube, Tool source lets you choose FastPlay managed (the default) or Installed tools. Managed mode downloads tools when needed and keeps yt-dlp updated. The existing yt-dlp path override still works in this mode. Installed mode uses your own yt-dlp, Deno, FFmpeg and ffprobe and never downloads or updates these tools. Leave paths empty to search PATH; on macOS, FastPlay also checks /opt/homebrew/bin and /usr/local/bin, so standard Homebrew installations work when launching from Finder. Optional yt-dlp and Deno paths override discovery. The FFmpeg folder must contain both ffmpeg and ffprobe. If an explicit path is invalid, FastPlay reports the problem instead of choosing another copy. You maintain installed tools yourself. Restart FastPlay after changing the system PATH.

Test tools checks the current values in the YouTube options without saving them. It shows executable paths and versions, checks yt-dlp runtime support, and verifies that Deno runs JavaScript. Tests run offline and do not download or update tools. A stalled check stops after ten seconds. Missing managed tools show Not installed yet and will be downloaded when needed. FFmpeg and ffprobe are needed for downloads that convert, merge, or embed metadata; they are not required for ordinary playback. A successful local test does not test YouTube access or video extraction. OK saves your settings; Cancel discards changes. Switching tool sources keeps existing managed copies on disk.
