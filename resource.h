#pragma once

// Menu IDs
#define IDM_MAIN_MENU       100
#define IDM_FILE_OPEN       101
#define IDM_FILE_EXIT       102
#define IDM_TOOLS_OPTIONS   103
#define IDM_FILE_OPEN_URL   104
#define IDM_FILE_HIDE_TRAY  106
#define IDM_FILE_ADD_FOLDER 110
#define IDM_FILE_PLAYLIST   111
#define IDM_FILE_RECENT_BASE 6000  // Recent files use IDs 6000-6009

// Playlist manager dialog
#define IDD_PLAYLIST        930
#define IDC_PLAYLIST_LIST   931

// URL dialog
#define IDD_URL             650
#define IDC_URL_EDIT        651

// Jump to time dialog
#define IDD_JUMPTOTIME      660
#define IDC_JUMPTIME_EDIT   661
#define IDM_PLAY_PLAYPAUSE  201
#define IDM_PLAY_STOP       202
#define IDM_PLAY_PREV       203
#define IDM_PLAY_NEXT       204
#define IDM_PLAY_PLAY       212
#define IDM_PLAY_PAUSE      213
#define IDM_PLAY_SEEKBACK   205
#define IDM_PLAY_SEEKFWD    206
#define IDM_PLAY_VOLUP      207
#define IDM_PLAY_VOLDOWN    208
#define IDM_PLAY_ELAPSED    209
#define IDM_PLAY_REMAINING  210
#define IDM_PLAY_TOTAL      211
#define IDM_PLAY_NOWPLAYING 214
#define IDM_PLAY_SHUFFLE    215
#define IDM_PLAY_BEGINNING  216
#define IDM_PLAY_JUMPTOTIME 217
#define IDM_PLAY_MUTE       218

// Accelerator table
#define IDA_ACCEL           300

// Timer
#define IDT_UPDATE_TITLE    400
#define IDT_BATCH_FILES     401

// Custom messages
#define WM_SPEAK            (WM_USER + 1)
#define WM_ADDFILE          (WM_USER + 2)
#define WM_META_CHANGED     (WM_USER + 3)

// File association checkboxes
#define IDC_ASSOC_MP3       520
#define IDC_ASSOC_WAV       521
#define IDC_ASSOC_OGG       522
#define IDC_ASSOC_FLAC      523
#define IDC_ASSOC_M4A       524
#define IDC_ASSOC_WMA       525
#define IDC_ASSOC_AAC       526
#define IDC_ASSOC_OPUS      527
#define IDC_ASSOC_AIFF      528
#define IDC_ASSOC_APE       529
#define IDC_ASSOC_WV        530
#define IDC_BRING_TO_FRONT  531
#define IDC_LOAD_FOLDER     532
#define IDC_MINIMIZE_TO_TRAY 539
#define IDC_ASSOC_M3U       534
#define IDC_ASSOC_M3U8      535
#define IDC_ASSOC_PLS       536
#define IDC_ASSOC_MID       537
#define IDC_ASSOC_MIDI      538

// Global hotkeys tab
#define IDC_HOTKEY_LIST     540
#define IDC_HOTKEY_ADD      541
#define IDC_HOTKEY_EDIT     542
#define IDC_HOTKEY_REMOVE   543
#define IDC_HOTKEY_ENABLED  544

// Hotkey assignment dialog
#define IDD_HOTKEY          600
#define IDC_HOTKEY_KEY      601
#define IDC_HOTKEY_ACTION   602

// Options dialog
#define IDD_OPTIONS         500
#define IDC_TAB             501
#define IDC_SOUNDCARD       502
#define IDC_ALLOW_AMPLIFY   503
#define IDC_REMEMBER_STATE  504
#define IDC_REMEMBER_POS    505
#define IDC_VOLUME_STEP     506
#define IDC_SHOW_TITLE      507

// Movement settings (checkboxes for seek amounts)
#define IDC_SEEK_1S         510
#define IDC_SEEK_5S         511
#define IDC_SEEK_10S        512
#define IDC_SEEK_30S        513
#define IDC_SEEK_1M         514
#define IDC_SEEK_5M         515
#define IDC_SEEK_10M        516
#define IDC_SEEK_30M        545
#define IDC_SEEK_1H         546
#define IDC_SEEK_1T         517
#define IDC_SEEK_5T         518
#define IDC_SEEK_10T        519

// Menu commands for seek amount
#define IDM_SEEK_DECREASE   220
#define IDM_SEEK_INCREASE   221

// Effect controls
#define IDM_EFFECT_PREV     230
#define IDM_EFFECT_NEXT     231
#define IDM_EFFECT_UP       232
#define IDM_EFFECT_DOWN     233
#define IDM_EFFECT_RESET    234
#define IDM_EFFECT_MIN      235
#define IDM_EFFECT_MAX      236

// Audio device menu
#define IDM_SHOW_AUDIO_DEVICES  237
#define IDM_AUDIO_DEVICE_BASE   5000  // Actual device IDs are IDM_AUDIO_DEVICE_BASE + device index

// Effect toggles (for hotkeys)
#define IDM_TOGGLE_VOLUME       240
#define IDM_TOGGLE_PITCH        241
#define IDM_TOGGLE_TEMPO        242
#define IDM_TOGGLE_RATE         243
#define IDM_TOGGLE_REVERB       244
#define IDM_TOGGLE_ECHO         245
#define IDM_TOGGLE_EQ           246
#define IDM_TOGGLE_COMPRESSOR   247
#define IDM_TOGGLE_STEREOWIDTH  248
#define IDM_TOGGLE_CENTERCANCEL 249

// Speak commands
#define IDM_SPEAK_SEEK          250

// Tag reading commands (keys 1-0)
#define IDM_READ_TAG_TITLE      261
#define IDM_READ_TAG_ARTIST     262
#define IDM_READ_TAG_ALBUM      263
#define IDM_READ_TAG_YEAR       264
#define IDM_READ_TAG_TRACK      265
#define IDM_READ_TAG_GENRE      266
#define IDM_READ_TAG_COMMENT    267
#define IDM_READ_TAG_BITRATE    268
#define IDM_READ_TAG_DURATION   269
#define IDM_READ_TAG_FILENAME   260

// Effect settings (checkboxes)
#define IDC_EFFECT_VOLUME   550
#define IDC_EFFECT_PITCH    551
#define IDC_EFFECT_TEMPO    552
#define IDC_EFFECT_RATE     553
#define IDC_RATE_STEP_MODE  554

// DSP effect settings (checkboxes)
#define IDC_DSP_REVERB      560
#define IDC_DSP_ECHO        561
#define IDC_DSP_EQ          562
#define IDC_DSP_COMPRESSOR  563
#define IDC_DSP_STEREOWIDTH 564
#define IDC_DSP_CENTERCANCEL 565

// Advanced tab controls
#define IDC_BUFFER_SIZE     570
#define IDC_UPDATE_PERIOD   571
#define IDC_TEMPO_ALGORITHM 572
#define IDC_EQ_BASS_FREQ    573
#define IDC_EQ_MID_FREQ     574
#define IDC_EQ_TREBLE_FREQ  575
#define IDC_LEGACY_VOLUME   576

// SoundTouch settings (tab 7)
#define IDC_ST_AA_FILTER        580
#define IDC_ST_AA_LENGTH        581
#define IDC_ST_QUICK_ALGO       582
#define IDC_ST_SEQUENCE         583
#define IDC_ST_SEEKWINDOW       584
#define IDC_ST_OVERLAP          585
#define IDC_ST_PREVENT_CLICK    586
#define IDC_ST_ALGORITHM        587

// Rubber Band settings (tab 8)
#define IDC_RB_FORMANT          590
#define IDC_RB_PITCH_MODE       591
#define IDC_RB_WINDOW           592
#define IDC_RB_TRANSIENTS       593
#define IDC_RB_DETECTOR         594
#define IDC_RB_CHANNELS         595
#define IDC_RB_PHASE            596
#define IDC_RB_SMOOTHING        597

// Speedy settings (tab 9)
#define IDC_SPEEDY_NONLINEAR    610

// MIDI settings (tab 10)
#define IDC_MIDI_SOUNDFONT      620
#define IDC_MIDI_SF_BROWSE      621
#define IDC_MIDI_VOICES         622
#define IDC_MIDI_SINC           623

// System tray
#define IDM_TRAY_RESTORE    700
#define IDM_TRAY_EXIT       701
#define WM_TRAYICON         (WM_USER + 10)
#define IDM_TOGGLE_WINDOW   702

// YouTube menu and dialog
#define IDM_FILE_YOUTUBE    105
#define IDD_YOUTUBE         750
#define IDC_YT_SEARCH       751
#define IDC_YT_RESULTS      752
#define IDC_YT_LOADMORE     753

// YouTube tab in Options
#define IDC_YTDLP_PATH      760
#define IDC_YTDLP_BROWSE    761
#define IDC_YT_APIKEY       762

// Bookmarks
#define IDM_BOOKMARK_ADD    800
#define IDM_BOOKMARK_LIST   801
#define IDD_BOOKMARKS       810
#define IDC_BOOKMARK_LIST   811
#define IDC_BOOKMARK_FILTER 812

// Recording
#define IDM_RECORD_TOGGLE   820
#define IDC_REC_PATH        830
#define IDC_REC_BROWSE      831
#define IDC_REC_TEMPLATE    832
#define IDC_REC_FORMAT      833
#define IDC_REC_BITRATE     834

// Speech tab
#define IDC_SPEECH_TRACKCHANGE  840
#define IDC_SPEECH_VOLUME       841
#define IDC_SPEECH_EFFECT       842

// Radio dialog
#define IDM_FILE_RADIO      107
#define IDM_HELP_PLUGINS    109
#define IDD_RADIO           850
#define IDC_RADIO_TAB       851
#define IDC_RADIO_LIST      852
#define IDC_RADIO_ADD       853
#define IDC_RADIO_IMPORT    854
#define IDC_RADIO_SEARCH_EDIT   855
#define IDC_RADIO_SEARCH_BTN    856
#define IDC_RADIO_SEARCH_LIST   857
#define IDC_RADIO_SEARCH_ADD    858
#define IDC_RADIO_SEARCH_SOURCE 859

// Add station dialog
#define IDD_RADIO_ADD       860
#define IDC_RADIO_NAME      861
#define IDC_RADIO_URL       862
#define IDC_RADIO_EXPORT    863

// Scheduler
#define IDM_FILE_SCHEDULE   108
#define IDD_SCHEDULER       870
#define IDC_SCHED_LIST      871
#define IDC_SCHED_ADD       872
#define IDC_SCHED_EDIT      873

// Add schedule dialog
#define IDD_SCHED_ADD       880
#define IDC_SCHED_NAME      881
#define IDC_SCHED_ACTION    882
#define IDC_SCHED_SOURCE    883
#define IDC_SCHED_FILE      884
#define IDC_SCHED_BROWSE    885
#define IDC_SCHED_RADIO     886
#define IDC_SCHED_DATE      887
#define IDC_SCHED_TIME      888
#define IDC_SCHED_REPEAT    889
#define IDC_SCHED_ENABLED   890
#define IDC_SCHED_DURATION  891
#define IDC_SCHED_STOP      892

// Scheduler timer
#define IDT_SCHEDULER       402
#define IDT_SCHED_DURATION  403

// Chapter seeking
#define IDC_CHAPTER_SEEK    900

// Auto-advance playlist
#define IDC_AUTO_ADVANCE    901
#define IDC_REWIND_ON_PAUSE 902
#define IDC_REWIND_LABEL    903

// Tag view dialog
#define IDD_TAG_VIEW        910
#define IDC_TAG_TEXT        911

// Convolution reverb controls
#define IDC_DSP_CONVOLUTION     920
#define IDC_CONV_IR             921
#define IDC_CONV_BROWSE         922

// View tag commands (Shift+1-0)
#define IDM_VIEW_TAG_TITLE      270
#define IDM_VIEW_TAG_ARTIST     271
#define IDM_VIEW_TAG_ALBUM      272
#define IDM_VIEW_TAG_YEAR       273
#define IDM_VIEW_TAG_TRACK      274
#define IDM_VIEW_TAG_GENRE      275
#define IDM_VIEW_TAG_COMMENT    276
#define IDM_VIEW_TAG_BITRATE    277
#define IDM_VIEW_TAG_DURATION   278
#define IDM_VIEW_TAG_FILENAME   279

// Podcast dialog
#define IDM_FILE_PODCAST        940
#define IDD_PODCAST             950
#define IDC_PODCAST_TAB         951
#define IDC_PODCAST_SUBS_LIST   952
#define IDC_PODCAST_EPISODES    953
#define IDC_PODCAST_ADD_FEED    954
#define IDC_PODCAST_REFRESH     956
#define IDC_PODCAST_SEARCH_EDIT 960
#define IDC_PODCAST_SEARCH_BTN  961
#define IDC_PODCAST_SEARCH_LIST 962
#define IDC_PODCAST_SUBSCRIBE   963
#define IDC_PODCAST_ADD_URL     964
#define IDC_PODCAST_SUBS_LABEL  965
#define IDC_PODCAST_EP_LABEL    966
#define IDC_PODCAST_SUBS_HELP   967
#define IDC_PODCAST_SEARCH_LABEL 968
#define IDC_PODCAST_SEARCH_HELP 969
#define IDC_PODCAST_EP_DESC     955

// Add podcast subscription dialog
#define IDD_PODCAST_ADD         970
#define IDC_PODCAST_NAME        971
#define IDC_PODCAST_FEED_URL    972
#define IDC_PODCAST_USERNAME    973
#define IDC_PODCAST_PASSWORD    974
#define IDC_PODCAST_DOWNLOAD    975
#define IDC_PODCAST_IMPORT_OPML 976
#define IDC_PODCAST_DOWNLOAD_ALL 977
#define IDC_PODCAST_EXPORT_OPML 978

// Downloads tab
#define IDC_DOWNLOAD_PATH       980
#define IDC_DOWNLOAD_BROWSE     981
#define IDC_DOWNLOAD_ORGANIZE   982

#define IDOK                1
#define IDCANCEL            2
