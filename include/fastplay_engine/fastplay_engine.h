/*
 * FastPlay Engine: FastPlay's audio engine as a library.
 *
 * Plays files and internet streams (anything FFmpeg reads, plus MIDI and
 * tracker modules), and YouTube videos by their links: yt-dlp, and the deno it
 * needs, are downloaded into the data folder the first time and yt-dlp kept up
 * to date (Windows and macOS; not iOS, which cannot run other programs).
 *
 * Any number of players, each playing one thing through its own output device,
 * all at once. Every function may be called from any thread. Strings are
 * UTF-8, in and out.
 *
 * A player's events (what was asked for opened, failed, ended...) go to the
 * callback it was created with, on a thread of the library's own: hand them to
 * your UI thread rather than doing UI work there. Calling back into the library
 * from the callback is allowed. Once fpe_player_destroy() returns, that player's
 * callback is not called again.
 */
#ifndef FASTPLAY_ENGINE_H
#define FASTPLAY_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  if defined(FPE_BUILDING)
#    define FPE_API __declspec(dllexport)
#  else
#    define FPE_API __declspec(dllimport)
#  endif
#else
#  define FPE_API __attribute__((visibility("default")))
#endif

/* The version of this interface. fpe_init() refuses a config made for another. */
#define FPE_API_VERSION 2

/* ---- the library ---- */

enum fpe_ios_session {
    /* Set the audio session to playback when a player's device first starts:
       sound with the ring switch off and the screen locked. */
    FPE_IOS_SESSION_PLAYBACK = 0,
    /* Leave the audio session to the app. */
    FPE_IOS_SESSION_APP = 1
};

typedef struct fpe_config {
    int api_version;  /* FPE_API_VERSION */
    /* Where yt-dlp and deno are kept, and YouTube's cookies. Created if needed.
       Required for YouTube. */
    const char* data_dir;
    int ios_session;  /* enum fpe_ios_session (iOS only) */
} fpe_config;

/* Before anything else. 1 on success, 0 if the config is not usable. */
FPE_API int fpe_init(const fpe_config* config);
/* After every player is destroyed. fpe_init() may be called again. */
FPE_API void fpe_shutdown(void);

/* "FastPlay Engine 0.7.0 (FFmpeg n8.1.3)" */
FPE_API const char* fpe_version(void);

/* ---- output devices ---- */

FPE_API int fpe_device_count(void);
/* The name of device `index` into `buffer` (always terminated); returns the
   length of the whole name, so a bigger buffer can be tried. -1 if no such device. */
FPE_API int fpe_device_name(int index, char* buffer, int size);

/* ---- YouTube ---- */

/* Whether YouTube links can be played here (not on iOS). */
FPE_API int fpe_youtube_supported(void);
/* Whether `url` is a link to a YouTube video (youtube.com/watch, youtu.be,
   shorts, live...). */
FPE_API int fpe_is_youtube_url(const char* url);
/* Gets yt-dlp and deno ready in the background now (downloading or updating
   them), so the first video plays sooner. Optional. */
FPE_API void fpe_prepare_youtube(void);

/* ---- players ---- */

typedef struct fpe_player fpe_player;

enum fpe_event {
    /* Something slow is under way while opening, worth telling the user about
       ("Downloading yt-dlp"). text: what. */
    FPE_EVENT_STATUS = 1,
    /* It opened, and is playing if autoplay was asked for. text: its title. */
    FPE_EVENT_OPENED = 2,
    /* It could not be opened. text: why. */
    FPE_EVENT_FAILED = 3,
    /* It played to its end. */
    FPE_EVENT_ENDED = 4,
    /* A stream's title changed (internet radio). text: the new one. */
    FPE_EVENT_TITLE = 5,
    /* A tape release or tape stop has wound down (fpe_scrub_release(),
       fpe_tape_stop()). */
    FPE_EVENT_SCRUB_ENDED = 6
};

/* request: the id fpe_open() returned for what the event is about. */
typedef void (*fpe_event_callback)(void* user, fpe_player* player, int request, int event, const char* text);

typedef struct fpe_player_config {
    /* The output device by name (fpe_device_name()); NULL or "" for the system's. */
    const char* device;
    /* How much audio is buffered ahead of the device, in ms; 0 for the default. */
    int buffer_ms;
    fpe_event_callback on_event;
    void* user;
} fpe_player_config;

/* A player. Its device starts on its first fpe_open(). NULL before fpe_init(). */
FPE_API fpe_player* fpe_player_create(const fpe_player_config* config);
/* Stops it and closes its device. */
FPE_API void fpe_player_destroy(fpe_player* player);

/* Moves it to the named device (NULL or "" for the system's): what is playing stops. */
FPE_API int fpe_set_device(fpe_player* player, const char* name);

/* Opens a file, a stream URL or a YouTube link, in the background: whatever was
   playing stops, and FPE_EVENT_OPENED or FPE_EVENT_FAILED follows. Returns the
   request's id (above 0), which the events carry; opening something else, or
   fpe_close(), abandons it. `autoplay`: start playing once opened. */
FPE_API int fpe_open(fpe_player* player, const char* url_or_path, int autoplay);
/* Stops and unloads, abandoning any open in progress. */
FPE_API void fpe_close(fpe_player* player);

enum fpe_state {
    FPE_STATE_EMPTY = 0,
    FPE_STATE_OPENING = 1,
    FPE_STATE_PLAYING = 2,
    FPE_STATE_PAUSED = 3,
    FPE_STATE_ENDED = 4
};
FPE_API int fpe_state(fpe_player* player);

FPE_API void fpe_play(fpe_player* player);
FPE_API void fpe_pause(fpe_player* player);
/* Returns 1 if it is now playing, 0 if paused (or nothing is loaded). */
FPE_API int fpe_toggle_pause(fpe_player* player);

/* Seconds. The length is 0 for a live stream, or when it is not known. */
FPE_API double fpe_position(fpe_player* player);
FPE_API double fpe_length(fpe_player* player);
FPE_API int fpe_is_live(fpe_player* player);
/* To `seconds` (clamped to the length); 0 if it cannot seek (a live stream). */
FPE_API int fpe_seek(fpe_player* player, double seconds);
FPE_API int fpe_seek_by(fpe_player* player, double delta_seconds);

/* The output gain, linear: 1 is as it is, 0 silent. Applies to what plays now
   and what plays next. */
FPE_API void fpe_set_volume(fpe_player* player, float gain);
/* Tempo in percent (0 normal), pitch in semitones, rate as a multiplier of
   speed and pitch together (1 normal). Kept for what plays next. Not for a live
   stream. */
FPE_API void fpe_set_tempo(fpe_player* player, float percent);
FPE_API void fpe_set_pitch(fpe_player* player, float semitones);
FPE_API void fpe_set_rate(fpe_player* player, float rate);

/* The title of what is loaded: a YouTube video's, a stream's current title,
   the tags' artist and title, or the file's name. Returns the length of the
   whole title (as fpe_device_name()). */
FPE_API int fpe_title(fpe_player* player, char* buffer, int size);

/* A tag of what is loaded by its common name (TITLE, ARTIST, ALBUM, DATE,
   TRACK, GENRE, COMMENT...) or a stream header (icy-name, icy-genre), into
   `buffer`; returns its whole length, 0 if there is none. */
FPE_API int fpe_tag(fpe_player* player, const char* name, char* buffer, int size);

typedef struct fpe_stream_info {
    char codec[32];       /* "mp3", "aac", "flac"... */
    int bitrate_kbps;     /* the recent average for VBR, else the nominal one */
    int vbr;
    int channels;         /* the source's own, before it became stereo float */
    int sample_rate;
    int bits;             /* 0 for a lossy format */
} fpe_stream_info;
FPE_API int fpe_get_stream_info(fpe_player* player, fpe_stream_info* info);

/* Chapters (M4B, MKV, MP3 with chapter frames...): how many, and chapter
   `index`'s start in seconds and its title. */
FPE_API int fpe_chapter_count(fpe_player* player);
FPE_API int fpe_chapter(fpe_player* player, int index, double* start, char* title, int size);

/* The engine's time stretcher for tempo changes, from the next fpe_open():
   1 Speedy (speech: speeds up the gaps more than the words), 2 Signalsmith
   (music; the default). */
FPE_API void fpe_set_tempo_algorithm(fpe_player* player, int algorithm);

/* Short fades (8 ms) around seeks, pauses and track changes, so none clicks.
   On by default. */
FPE_API void fpe_set_smooth_transitions(fpe_player* player, int on);

/* ---- live streams ---- */

/* Live streams opened from now on keep their last `seconds` (by any player),
   so they can be paused and rewound; 0 (the default) keeps none. */
FPE_API void fpe_set_live_rewind(int seconds);
/* A live stream kept for rewinding: where it can be played from, as positions
   (fpe_position()): the oldest kept, and the live edge. 0 for anything else. */
FPE_API int fpe_live_range(fpe_player* player, double* oldest, double* live);

/* ---- scrubbing: playing through the audio at speed while a key is held ---- */

enum fpe_scrub_style {
    /* Faster, pitch and all, like a tape; winds up and down. */
    FPE_SCRUB_TAPE = 0,
    /* Keeps the pitch, and speeds up the longer it goes. */
    FPE_SCRUB_SPRING = 1
};
/* From what is heard now, forward (direction 1) or back (-1), up to `speed`
   times normal. Not for a live stream unless it is kept for rewinding. */
FPE_API int fpe_scrub_start(fpe_player* player, int style, int direction, float speed);
FPE_API void fpe_scrub_speed(fpe_player* player, float speed);
/* Carries on playing normally from wherever it got to. */
FPE_API int fpe_scrub_stop(fpe_player* player);
/* Tape let go of: winds back down (to normal speed forward, to a stop going
   back), then FPE_EVENT_SCRUB_ENDED. 0 for spring (use fpe_scrub_stop()). */
FPE_API int fpe_scrub_release(fpe_player* player);
/* The tape stop: from normal speed to a standstill, then FPE_EVENT_SCRUB_ENDED
   (pause or stop for real then). */
FPE_API int fpe_tape_stop(fpe_player* player);

/* ---- effects ---- */

/* The effects, by key: "reverb", "echo", "eq", "compressor", "stereo_width",
   "center_cancel", "convolution", "3d_audio", "normalizer". */
FPE_API int fpe_effect_count(void);
FPE_API const char* fpe_effect_key(int index);

/* Each player's effects are its own, off until turned on. */
FPE_API int fpe_set_effect(fpe_player* player, const char* effect, int on);
FPE_API int fpe_effect_enabled(fpe_player* player, const char* effect);
/* The reverb's kind: 0 off, 1 simple (a room you size), 2 advanced (the EFX
   environments). Turning "reverb" on with fpe_set_effect() chooses simple. */
FPE_API int fpe_set_reverb_type(fpe_player* player, int type);

/* Every parameter there is, the same for every player. */
typedef struct fpe_param_info {
    const char* key;       /* "eq_bass", for fpe_set_param() */
    const char* name;      /* "EQ Bass" */
    const char* unit;      /* "dB" */
    const char* effect;    /* the effect's key; "" for volume, pitch, tempo and rate */
    float min_value, max_value, step, default_value;
    /* How many named values it takes (fpe_param_choice()): a reverb room, an
       environment, a 3D mode. 0 for a plain number. */
    int choices;
} fpe_param_info;
FPE_API int fpe_param_count(void);
FPE_API int fpe_param_at(int index, fpe_param_info* info);
/* The name of value `value` of a choice parameter ("Cathedral"). */
FPE_API int fpe_param_choice(const char* key, int value, char* buffer, int size);

/* A parameter's value (clamped to its range), heard at once. "volume",
   "pitch", "tempo" and "rate" are the same as fpe_set_volume() and the rest
   (volume is a linear gain here too). 0 for a key there is no such parameter. */
FPE_API int fpe_set_param(fpe_player* player, const char* key, float value);
FPE_API float fpe_get_param(fpe_player* player, const char* key);

/* The EQ's three band centres, in Hz (50, 1000 and 12000 to begin with). */
FPE_API void fpe_set_eq_frequencies(fpe_player* player, float bass, float mid, float treble);
/* The convolution reverb's impulse response, a WAV file. */
FPE_API int fpe_load_impulse_response(fpe_player* player, const char* path);

/* ---- recording what plays ---- */

enum fpe_record_format {
    FPE_RECORD_WAV = 0,
    FPE_RECORD_MP3 = 1,
    FPE_RECORD_OGG = 2,
    FPE_RECORD_FLAC = 3
};
/* Records what the player plays, into `path`, until fpe_record_stop(): after
   its effects (or before them, `before_effects`), before its volume. Encoded on
   a thread of its own. `bitrate_kbps` for MP3 and OGG. Something must be
   playing (its device's rate is the recording's). 1 if it started. */
FPE_API int fpe_record_start(fpe_player* player, const char* path, int format, int bitrate_kbps,
                             int before_effects);
/* Finishes the file. */
FPE_API void fpe_record_stop(fpe_player* player);
FPE_API int fpe_recording(fpe_player* player);

#ifdef __cplusplus
}
#endif

#endif /* FASTPLAY_ENGINE_H */
