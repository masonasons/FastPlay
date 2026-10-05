// The Options dialog: one notebook page per group of settings.

#include "ui/dialogs.h"
#include "ui/ui_common.h"
#include "ui/main_frame.h"
#include "ui/hotkey_dialog.h"

#include "globals.h"
#include "app_ui.h"
#include "player.h"
#include "settings.h"
#include "hotkeys.h"
#include "accessibility.h"
#include "effects.h"
#include "audio.h"
#include "convolution.h"
#include "database.h"
#include "file_assoc.h"
#include "youtube.h"
#include "youtube_tools.h"
#include "utils.h"
#include "library.h"

#include <algorithm>
#include <cwchar>
#include <wx/display.h>
#include <wx/notebook.h>
#include <wx/filedlg.h>
#include <wx/dirdlg.h>
#include <wx/stdpaths.h>
#include <wx/valtext.h>
#include <cstdlib>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <wx/scrolwin.h>

namespace {

// Seek amount checkbox labels, with their mnemonics, in g_seekAmounts order.
const wchar_t* const kSeekLabels[] = {
    L"&1 second", L"&5 seconds", L"1&0 seconds", L"&30 seconds", L"1 &minute", L"5 m&inutes",
    L"10 min&utes", L"3&0 minutes", L"1 &hour", L"1 &track", L"5 t&racks", L"10 trac&ks"
};

const int kVolumeSteps[] = {1, 2, 5, 10, 15, 20, 25};
const int kBitrates[] = {128, 160, 192, 224, 256, 320};

// A tab's page: it scrolls, so a tall one (or a small screen) hides nothing.
// A control focused below what shows is scrolled to.
wxScrolledWindow* NewPage(wxWindow* book) {
    auto* page = new wxScrolledWindow(book);
    page->SetScrollRate(0, 10);
    return page;
}

std::wstring FileNameOnly(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        return path.substr(pos + 1);
    }
    return path;
}

std::wstring HotkeyListItem(const GlobalHotkey& hk) {
    return FormatHotkey(hk.modifiers, hk.vk) + L" - " + g_hotkeyActions[hk.actionIdx].name +
           (hk.global ? L"" : L" (in FastPlay)");
}

// Put changed hotkeys to work: the global ones registered again (when on), the
// local ones in the main window's key table.
void ApplyHotkeys() {
    MainFrame* frame = GetMainFrame();
    if (!frame) return;
    frame->UnregisterGlobalHotkeys();
    frame->RegisterGlobalHotkeys();
    frame->BuildAccelerators();
}

class OptionsDialog : public wxDialog {
public:
    explicit OptionsDialog(wxWindow* parent);
	~OptionsDialog() override { *m_toolTestAlive = false; }

private:
    // Page building helpers. Each adds a label followed by the control it names.
    wxCheckBox* AddCheck(wxWindow* page, wxBoxSizer* sizer, const wxString& label, bool checked);
    wxChoice* AddChoice(wxWindow* page, wxSizer* sizer, const wxString& label, int width = -1);
    wxTextCtrl* AddEdit(wxWindow* page, wxSizer* sizer, const wxString& label, const wxString& value,
                        int width, long style = 0);
    void AddText(wxWindow* page, wxSizer* sizer, const wxString& text);
    wxBoxSizer* AddRow(wxSizer* sizer);

    void BuildPlaybackPage(wxNotebook* book);
    void BuildRecordingPage(wxNotebook* book);
    void BuildDownloadsPage(wxNotebook* book);
    void BuildSpeechPage(wxNotebook* book);
    void BuildMovementPage(wxNotebook* book);
    void BuildHotkeysPage(wxNotebook* book);
    void BuildEffectsPage(wxNotebook* book);
    void BuildAdvancedPage(wxNotebook* book);
    void BuildYouTubePage(wxNotebook* book);
    void BuildYouTubeDownloadsPage(wxNotebook* book);
    void BuildSpeedyPage(wxNotebook* book);
    void BuildSignalsmithPage(wxNotebook* book);
    void BuildMidiPage(wxNotebook* book);
    void BuildLibraryPage(wxNotebook* book);
    void ShowLibraryFolders(int select);

    void OnOK(wxCommandEvent& event);
    void OnRecBrowse(wxCommandEvent& event);
    void OnDownloadBrowse(wxCommandEvent& event);
    void OnRecFormat(wxCommandEvent& event);
    void OnYtdlpBrowse(wxCommandEvent& event);
	YouTubeToolSettings ReadToolSettings() const;
	void UpdateToolControls();
	void OnTestTools(wxCommandEvent& event);
    void OnImportCookies(wxCommandEvent& event);
    void OnYtFolderBrowse(wxCommandEvent& event);
    void UpdateYtDownloadControls();
    void OnRemoveCookies(wxCommandEvent& event);
    void UpdateCookiesStatus();
    void OnMidiBrowse(wxCommandEvent& event);
    void OnConvBrowse(wxCommandEvent& event);
    void OnResetListOrder(wxCommandEvent& event);
    void OnHotkeyAdd(wxCommandEvent& event);
    void OnHotkeyEdit(wxCommandEvent& event);
    void OnHotkeyRemove(wxCommandEvent& event);
    void OnHotkeyEnabled(wxCommandEvent& event);

    wxNotebook* m_book = nullptr;

    // Playback
    wxChoice* m_soundcard = nullptr;
    std::vector<int> m_deviceIndexes;  // device number for each sound card entry
    wxCheckBox* m_allowAmplify = nullptr;
    wxCheckBox* m_rememberState = nullptr;
    wxChoice* m_rememberPos = nullptr;
    wxCheckBox* m_bringToFront = nullptr;
    wxCheckBox* m_loadFolder = nullptr;
    wxCheckBox* m_minimizeToTray = nullptr;
    wxCheckBox* m_showTitle = nullptr;
    wxCheckBox* m_autoAdvance = nullptr;
    wxCheckBox* m_playlistFollow = nullptr;
    wxCheckBox* m_checkUpdates = nullptr;
    wxCheckBox* m_multiInstance = nullptr;
    wxCheckBox* m_registerFileTypes = nullptr;
    wxTextCtrl* m_rewindOnPause = nullptr;
    wxChoice* m_volumeStep = nullptr;
    wxChoice* m_replayGainMode = nullptr;
    wxTextCtrl* m_replayGainPreamp = nullptr;
    wxCheckBox* m_replayGainClip = nullptr;

    // Recording
    wxTextCtrl* m_recPath = nullptr;
    wxTextCtrl* m_recTemplate = nullptr;
    wxChoice* m_recFormat = nullptr;
    wxChoice* m_recBitrate = nullptr;
    wxCheckBox* m_recEffects = nullptr;

    // Downloads
    wxTextCtrl* m_downloadPath = nullptr;
    wxCheckBox* m_downloadOrganize = nullptr;

    // Speech
    wxCheckBox* m_speechTrackChange = nullptr;
    wxCheckBox* m_speechVolume = nullptr;
    wxCheckBox* m_speechEffect = nullptr;

    // Movement
    std::vector<wxCheckBox*> m_seekChecks;
    wxCheckBox* m_chapterSeek = nullptr;

    // Global Hotkeys
    wxCheckBox* m_hotkeyEnabled = nullptr;
    wxListBox* m_hotkeyList = nullptr;

    // Effects
    wxCheckBox* m_effectVolume = nullptr;
    wxCheckBox* m_effectPitch = nullptr;
    wxCheckBox* m_effectTempo = nullptr;
    wxCheckBox* m_effectRate = nullptr;
    wxChoice* m_rateStepMode = nullptr;
    wxChoice* m_reverb = nullptr;
    wxCheckBox* m_dspEcho = nullptr;
    wxCheckBox* m_dspEQ = nullptr;
    wxCheckBox* m_dspCompressor = nullptr;
    wxCheckBox* m_dspNormalizer = nullptr;
    wxCheckBox* m_dspStereoWidth = nullptr;
    wxCheckBox* m_dspCenterCancel = nullptr;
    wxCheckBox* m_dspSpatial = nullptr;
    wxCheckBox* m_dspConvolution = nullptr;
    wxTextCtrl* m_convIR = nullptr;

    // Advanced
    wxChoice* m_bufferSize = nullptr;
    wxChoice* m_tempoAlgorithm = nullptr;
    wxTextCtrl* m_eqBassFreq = nullptr;
    wxTextCtrl* m_eqMidFreq = nullptr;
    wxTextCtrl* m_eqTrebleFreq = nullptr;
    wxCheckBox* m_disableBatch = nullptr;
    wxCheckBox* m_smoothSeek = nullptr;
    wxCheckBox* m_liveRewind = nullptr;
    wxChoice* m_liveRewindMinutes = nullptr;

    // YouTube
    wxTextCtrl* m_ytdlpPath = nullptr;
	wxChoice* m_toolSource = nullptr;
	wxTextCtrl* m_denoPath = nullptr;
	wxTextCtrl* m_ffmpegFolder = nullptr;
	wxButton* m_denoBrowse = nullptr;
	wxButton* m_ffmpegBrowse = nullptr;
	wxButton* m_ytdlpBrowse = nullptr;
	wxButton* m_testTools = nullptr;
	wxTextCtrl* m_toolResults = nullptr;
	bool m_testingTools = false;
	std::shared_ptr<bool> m_toolTestAlive = std::make_shared<bool>(true);
    wxStaticText* m_cookiesStatus = nullptr;
    wxChoice* m_ytAutoRefresh = nullptr;
    // YouTube downloads
    wxTextCtrl* m_ytFolder = nullptr;
    wxChoice* m_ytType = nullptr;
    wxChoice* m_ytAudioFormat = nullptr;
    wxChoice* m_ytAudioQuality = nullptr;
    wxChoice* m_ytVideoQuality = nullptr;
    wxChoice* m_ytVideoContainer = nullptr;
    wxChoice* m_ytVideoCodec = nullptr;
    wxChoice* m_ytNaming = nullptr;
    wxCheckBox* m_ytAddMetadata = nullptr;
    wxCheckBox* m_ytEmbedThumbnail = nullptr;
    wxCheckBox* m_ytWriteThumbnail = nullptr;
    wxCheckBox* m_ytWriteDescription = nullptr;
    wxCheckBox* m_ytWriteSubtitles = nullptr;
    wxCheckBox* m_ytEmbedSubtitles = nullptr;
    wxCheckBox* m_ytChannelFolder = nullptr;
    wxTextCtrl* m_ytExtraOptions = nullptr;
    wxButton* m_removeCookies = nullptr;
    wxTextCtrl* m_ytApiKey = nullptr;


    // Speedy
    wxCheckBox* m_speedyNonlinear = nullptr;

    // Signalsmith
    wxChoice* m_ssPreset = nullptr;
    wxTextCtrl* m_ssTonality = nullptr;

    // MIDI
    wxTextCtrl* m_midiSoundFont = nullptr;
    wxTextCtrl* m_midiVoices = nullptr;
    wxCheckBox* m_midiSinc = nullptr;

    // Library: the folders as edited here, saved on OK
    std::vector<LibraryFolder> m_libraryFolders;
    wxListBox* m_libraryList = nullptr;
    wxCheckBox* m_libraryTagged = nullptr;
};

OptionsDialog::OptionsDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, "Options") {
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // Tab order: the tabs, then the selected page's controls, then OK / Cancel.
    m_book = new wxNotebook(this, wxID_ANY);
    BuildPlaybackPage(m_book);
    BuildRecordingPage(m_book);
    BuildLibraryPage(m_book);
    BuildDownloadsPage(m_book);
    BuildSpeechPage(m_book);
    BuildMovementPage(m_book);
    BuildHotkeysPage(m_book);
    BuildEffectsPage(m_book);
    BuildAdvancedPage(m_book);
    BuildYouTubePage(m_book);
    BuildYouTubeDownloadsPage(m_book);
    BuildSpeedyPage(m_book);
    BuildSignalsmithPage(m_book);
    BuildMidiPage(m_book);
    sizer->Add(m_book, 1, wxEXPAND | wxALL, 10);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* ok = new wxButton(this, wxID_OK, "OK");
    ok->SetDefault();
    buttons->Add(ok, 0, wxRIGHT, 6);
    buttons->Add(new wxButton(this, wxID_CANCEL, "Cancel"));
    sizer->Add(buttons, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    Bind(wxEVT_BUTTON, &OptionsDialog::OnOK, this, wxID_OK);

    SetSizerAndFit(sizer);
    // No taller than the screen: the tabs scroll instead
    wxRect area = wxDisplay(this).GetClientArea();
    if (GetSize().y > area.height) SetSize(wxSize(GetSize().x, area.height));
    CentreOnParent();

    // Show the Playback tab first
    m_book->SetSelection(0);
    m_book->SetFocus();
}

wxCheckBox* OptionsDialog::AddCheck(wxWindow* page, wxBoxSizer* sizer, const wxString& label, bool checked) {
    auto* check = new wxCheckBox(page, wxID_ANY, label);
    check->SetValue(checked);
    if (sizer->GetOrientation() == wxHORIZONTAL) {
        sizer->Add(check, 0, wxALIGN_CENTER_VERTICAL);
    } else {
        sizer->Add(check, 0, wxTOP | wxBOTTOM, 3);
    }
    return check;
}

wxChoice* OptionsDialog::AddChoice(wxWindow* page, wxSizer* sizer, const wxString& label, int width) {
    if (!label.empty()) {
        sizer->Add(new wxStaticText(page, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    }
    auto* choice = new wxChoice(page, wxID_ANY, wxDefaultPosition, wxSize(width, -1));
    sizer->Add(choice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    return choice;
}

wxTextCtrl* OptionsDialog::AddEdit(wxWindow* page, wxSizer* sizer, const wxString& label, const wxString& value,
                                   int width, long style) {
    if (!label.empty()) {
        sizer->Add(new wxStaticText(page, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    }
    auto* edit = new wxTextCtrl(page, wxID_ANY, value, wxDefaultPosition, wxSize(width, -1), style);
    sizer->Add(edit, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    return edit;
}

void OptionsDialog::AddText(wxWindow* page, wxSizer* sizer, const wxString& text) {
    sizer->Add(new wxStaticText(page, wxID_ANY, text), 0, wxTOP | wxBOTTOM, 3);
}

wxBoxSizer* OptionsDialog::AddRow(wxSizer* sizer) {
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(row, 0, wxTOP | wxBOTTOM, 3);
    return row;
}

void OptionsDialog::BuildPlaybackPage(wxNotebook* book) {
    auto* page = NewPage(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // Populate sound card combo box
    m_soundcard = AddChoice(page, AddRow(sizer), "&Output device:", 330);
    int currentIndex = 0;
    for (const auto& dev : GetAudioDevices()) {
        int idx = m_soundcard->Append(WX(dev.name));
        m_deviceIndexes.push_back(dev.index);
        if (dev.current) {
            currentIndex = idx;
        }
    }
    if (!m_deviceIndexes.empty()) {
        m_soundcard->SetSelection(currentIndex);
    }

    m_allowAmplify = AddCheck(page, sizer, "&Allow volume above 100%", g_allowAmplify);
    m_rememberState = AddCheck(page, sizer, "&Remember playback state on exit", g_rememberState);

    // Populate remember position combo box
    {
        m_rememberPos = AddChoice(page, AddRow(sizer), "Remember p&osition if longer than:");
        const wchar_t* posLabels[] = {L"Off", L"5 minutes", L"10 minutes", L"20 minutes", L"30 minutes", L"45 minutes", L"60 minutes"};
        int posIndex = 0;
        for (int i = 0; i < g_posThresholdCount; i++) {
            m_rememberPos->Append(posLabels[i]);
            if (g_posThresholds[i] == g_rememberPosMinutes) {
                posIndex = i;
            }
        }
        m_rememberPos->SetSelection(posIndex);
    }

    m_bringToFront = AddCheck(page, sizer, "&Bring window to front when opening files", g_bringToFront);
    m_loadFolder = AddCheck(page, sizer, "&Load all files in folder when opening single file", g_loadFolder);
    m_minimizeToTray = AddCheck(page, sizer, "&Minimize to system tray", g_minimizeToTray);
    m_showTitle = AddCheck(page, sizer, "Show &track name in window title", g_showTitleInWindow);
    m_autoAdvance = AddCheck(page, sizer, "Auto-ad&vance to next playlist item", g_autoAdvance);
    m_playlistFollow = AddCheck(page, sizer, "&Follow playback in playlist dialog", g_playlistFollowPlayback);
    m_checkUpdates = AddCheck(page, sizer, "Check for &updates on startup", g_checkForUpdates);
    m_multiInstance = AddCheck(page, sizer, "Allow &multiple instances", g_allowMultipleInstances);
    m_registerFileTypes = AddCheck(page, sizer, "Register all supported &file types", g_registerFileTypes);

    m_rewindOnPause = AddEdit(page, AddRow(sizer), "Re&wind on pause (ms):",
                              wxString::Format("%u", static_cast<unsigned>(g_rewindOnPauseMs)), 60);
    SetDigitsOnly(m_rewindOnPause);

    // Populate volume step combo box
    {
        m_volumeStep = AddChoice(page, AddRow(sizer), "Volu&me step:");
        int stepIndex = 1;  // Default to 2%
        for (int i = 0; i < 7; i++) {
            m_volumeStep->Append(wxString::Format("%d%%", kVolumeSteps[i]));
            if (static_cast<int>(g_volumeStep * 100 + 0.5f) == kVolumeSteps[i]) {
                stepIndex = i;
            }
        }
        m_volumeStep->SetSelection(stepIndex);
    }

    // Populate ReplayGain controls
    {
        auto* row = AddRow(sizer);
        m_replayGainMode = AddChoice(page, row, "Replay&Gain:");
        m_replayGainMode->Append("Off");
        m_replayGainMode->Append("Track");
        m_replayGainMode->Append("Album");
        int rgMode = (g_replayGainMode >= 0 && g_replayGainMode <= 2) ? g_replayGainMode : 0;
        m_replayGainMode->SetSelection(rgMode);

        m_replayGainPreamp = AddEdit(page, row, "Pre&amp (dB):", wxString::Format("%g", g_replayGainPreamp), 50);

        m_replayGainClip = AddCheck(page, sizer, "Prevent clippin&g", g_replayGainPreventClip);
    }

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    page->FitInside();
    book->AddPage(page, "Playback");
}

void OptionsDialog::BuildRecordingPage(wxNotebook* book) {
    auto* page = NewPage(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // Set default recording path to user's Music folder if not set
    if (g_recordPath.empty()) {
        wxString musicPath = wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Music);
        if (!musicPath.empty()) {
            g_recordPath = WS(musicPath);
        }
    }

    AddText(page, sizer, "Record audio output to file. Press R to toggle recording.");

    auto* row = AddRow(sizer);
    m_recPath = AddEdit(page, row, "&Output folder:", WX(g_recordPath), 280);
    auto* browse = new wxButton(page, wxID_ANY, "&Browse...");
    row->Add(browse, 0, wxALIGN_CENTER_VERTICAL);
    browse->Bind(wxEVT_BUTTON, &OptionsDialog::OnRecBrowse, this);

    m_recTemplate = AddEdit(page, AddRow(sizer), "Filename &template:", WX(g_recordTemplate), 250);
    AddText(page, sizer, "(Uses strftime format: %Y=year, %m=month, %d=day, %H=hour, %M=min, %S=sec)");

    // Format combo: WAV, MP3, OGG, FLAC
    row = AddRow(sizer);
    m_recFormat = AddChoice(page, row, "&Format:");
    m_recFormat->Append("WAV (lossless)");
    m_recFormat->Append("MP3");
    m_recFormat->Append("OGG Vorbis");
    m_recFormat->Append("FLAC (lossless)");
    m_recFormat->SetSelection(g_recordFormat);
    m_recFormat->Bind(wxEVT_CHOICE, &OptionsDialog::OnRecFormat, this);

    // Bitrate combo (for MP3/OGG)
    m_recBitrate = AddChoice(page, row, "&Bitrate:");
    int bitrateIndex = 2;  // Default to 192
    for (int i = 0; i < 6; i++) {
        m_recBitrate->Append(wxString::Format("%d kbps", kBitrates[i]));
        if (kBitrates[i] == g_recordBitrate) {
            bitrateIndex = i;
        }
    }
    m_recBitrate->SetSelection(bitrateIndex);

    // Enable bitrate only for lossy formats (MP3=1, OGG=2)
    m_recBitrate->Enable(g_recordFormat == 1 || g_recordFormat == 2);

    AddText(page, sizer, "(Bitrate only applies to MP3 and OGG formats)");

    m_recEffects = AddCheck(page, sizer, "Record with &effects", g_recordEffects);
    AddText(page, sizer, "Off: recordings have the sound before the effects (tempo, pitch and rate still apply). "
                         "Volume never applies to recordings.");

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    page->FitInside();
    book->AddPage(page, "Recording");
}

void OptionsDialog::BuildDownloadsPage(wxNotebook* book) {
    auto* page = NewPage(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    AddText(page, sizer, "Configure podcast episode download settings.");
    AddText(page, sizer, "&Downloads folder:");

    auto* row = AddRow(sizer);
    m_downloadPath = new wxTextCtrl(page, wxID_ANY, WX(g_downloadPath), wxDefaultPosition, wxSize(380, -1),
                                    wxTE_READONLY);
    row->Add(m_downloadPath, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    auto* browse = new wxButton(page, wxID_ANY, "&Browse...");
    row->Add(browse, 0, wxALIGN_CENTER_VERTICAL);
    browse->Bind(wxEVT_BUTTON, &OptionsDialog::OnDownloadBrowse, this);

    m_downloadOrganize = AddCheck(page, sizer, "&Organize downloads into folders by feed title", g_downloadOrganizeByFeed);

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    page->FitInside();
    book->AddPage(page, "Downloads");
}

void OptionsDialog::BuildSpeechPage(wxNotebook* book) {
    auto* page = NewPage(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    AddText(page, sizer, "Configure speech feedback for various events.");
    m_speechTrackChange = AddCheck(page, sizer, "&Announce track changes", g_speechTrackChange);
    m_speechVolume = AddCheck(page, sizer, "Speak &volume when adjusted", g_speechVolume);
    m_speechEffect = AddCheck(page, sizer, "Speak &effect value when adjusted", g_speechEffect);

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    page->FitInside();
    book->AddPage(page, "Speech");
}

void OptionsDialog::BuildMovementPage(wxNotebook* book) {
    auto* page = NewPage(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    AddText(page, sizer, "Seek amounts (use , and . to cycle):");

    // Seek amount checkboxes
    const int labelCount = static_cast<int>(sizeof(kSeekLabels) / sizeof(kSeekLabels[0]));
    for (int i = 0; i < g_seekAmountCount; i++) {
        wxString label = (i < labelCount) ? wxString(kSeekLabels[i]) : wxString(g_seekAmounts[i].label);
        m_seekChecks.push_back(AddCheck(page, sizer, label, g_seekEnabled[i]));
    }
    m_chapterSeek = AddCheck(page, sizer, "1 c&hapter (if available)", g_chapterSeekEnabled);

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    page->FitInside();
    book->AddPage(page, "Movement");
}

void OptionsDialog::BuildHotkeysPage(wxNotebook* book) {
    auto* page = NewPage(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // Populate hotkey list and set enabled checkbox
    m_hotkeyEnabled = AddCheck(page, sizer, "&Enable global hotkeys", g_hotkeysEnabled);
    AddText(page, sizer, "Hotkeys that are not global work while FastPlay's main window has the focus.");
    m_hotkeyEnabled->Bind(wxEVT_CHECKBOX, &OptionsDialog::OnHotkeyEnabled, this);

    m_hotkeyList = new wxListBox(page, wxID_ANY, wxDefaultPosition, wxSize(430, 180), 0, nullptr, wxLB_SINGLE);
    for (const auto& hk : g_hotkeys) {
        m_hotkeyList->Append(WX(HotkeyListItem(hk)));
    }
    sizer->Add(m_hotkeyList, 1, wxEXPAND | wxTOP | wxBOTTOM, 3);

    auto* row = AddRow(sizer);
    auto* add = new wxButton(page, wxID_ANY, "&Add...");
    auto* edit = new wxButton(page, wxID_ANY, "&Edit...");
    auto* remove = new wxButton(page, wxID_ANY, "&Remove");
    row->Add(add, 0, wxRIGHT, 6);
    row->Add(edit, 0, wxRIGHT, 6);
    row->Add(remove);
    add->Bind(wxEVT_BUTTON, &OptionsDialog::OnHotkeyAdd, this);
    edit->Bind(wxEVT_BUTTON, &OptionsDialog::OnHotkeyEdit, this);
    remove->Bind(wxEVT_BUTTON, &OptionsDialog::OnHotkeyRemove, this);

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    page->FitInside();
    book->AddPage(page, "Hotkeys");
}

void OptionsDialog::BuildEffectsPage(wxNotebook* book) {
    auto* page = NewPage(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // Set effect checkboxes
    AddText(page, sizer, "Stream effects ([ ] to cycle, Up/Down to adjust):");
    m_effectVolume = AddCheck(page, sizer, "&Volume (0-400%)", g_effectEnabled[0]);
    m_effectPitch = AddCheck(page, sizer, "&Pitch (-12 to +12 semitones)", g_effectEnabled[1]);
    m_effectTempo = AddCheck(page, sizer, "&Tempo (-50% to +100%)", g_effectEnabled[2]);

    auto* row = AddRow(sizer);
    m_effectRate = AddCheck(page, row, "Playback &Rate (0.5x - 2x)", g_effectEnabled[3]);
    row->AddSpacer(12);

    // Set rate step mode combobox
    m_rateStepMode = AddChoice(page, row, "Step:");
    m_rateStepMode->Append("0.01x");
    m_rateStepMode->Append("Semitone");
    m_rateStepMode->SetSelection(g_rateStepMode);

    AddText(page, sizer, "DSP effects (enable to add their parameters to cycle list):");

    // Set reverb algorithm combobox
    m_reverb = AddChoice(page, AddRow(sizer), "Re&verb:", 200);
    m_reverb->Append("Off");
    m_reverb->Append("Simple");
    m_reverb->Append("Advanced (EFX)");
    m_reverb->SetSelection(g_reverbAlgorithm);

    // Set DSP effect checkboxes
    m_dspEcho = AddCheck(page, sizer, "&Echo", IsDSPEffectEnabled(DSPEffectType::Echo));
    m_dspEQ = AddCheck(page, sizer, "E&Q (Bass/Mid/Treble)", IsDSPEffectEnabled(DSPEffectType::EQ));
    m_dspCompressor = AddCheck(page, sizer, "&Compressor", IsDSPEffectEnabled(DSPEffectType::Compressor));
    m_dspNormalizer = AddCheck(page, sizer, "Normal&izer (a steady level, Ctrl+Shift+N)",
                               IsDSPEffectEnabled(DSPEffectType::Normalizer));
    m_dspStereoWidth = AddCheck(page, sizer, "&Stereo Width (0-200%)", IsDSPEffectEnabled(DSPEffectType::StereoWidth));
    m_dspCenterCancel = AddCheck(page, sizer, "Ce&nter Cancel (-100 to +100%)", IsDSPEffectEnabled(DSPEffectType::CenterCancel));
    m_dspSpatial = AddCheck(page, sizer, "&3D Audio (HRTF/Binaural)", IsDSPEffectEnabled(DSPEffectType::SpatialAudio));

    row = AddRow(sizer);
    m_dspConvolution = AddCheck(page, row, "Co&nvolution Reverb", IsDSPEffectEnabled(DSPEffectType::Convolution));
    row->AddSpacer(12);

    // Display current IR file path (just filename)
    m_convIR = AddEdit(page, row, "IR File:", g_convolutionIRPath.empty() ? wxString() : WX(FileNameOnly(g_convolutionIRPath)),
                       160, wxTE_READONLY);
    auto* browse = new wxButton(page, wxID_ANY, "...", wxDefaultPosition, wxSize(30, -1));
    row->Add(browse, 0, wxALIGN_CENTER_VERTICAL);
    browse->Bind(wxEVT_BUTTON, &OptionsDialog::OnConvBrowse, this);

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    page->FitInside();
    book->AddPage(page, "Effects");
}

void OptionsDialog::BuildAdvancedPage(wxNotebook* book) {
    auto* page = NewPage(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    AddText(page, sizer, "Audio buffer settings (changes apply on next file load):");

    // Populate buffer size combo box
    {
        m_bufferSize = AddChoice(page, AddRow(sizer), "&Buffer size:", 150);
        int bufferIndex = 3;  // Default to 500ms
        for (int i = 0; i < g_bufferSizeCount; i++) {
            m_bufferSize->Append(wxString::Format("%d ms", g_bufferSizes[i]));
            if (g_bufferSizes[i] == g_bufferSize) {
                bufferIndex = i;
            }
        }
        m_bufferSize->SetSelection(bufferIndex);
    }

    AddText(page, sizer, "How far ahead audio is prepared: lower answers effect changes sooner, higher is safer on a busy computer.");

    // Populate tempo algorithm combo box
    AddText(page, sizer, "Tempo/pitch &algorithm (changes apply on next file load):");
    m_tempoAlgorithm = new wxChoice(page, wxID_ANY, wxDefaultPosition, wxSize(330, -1));
    sizer->Add(m_tempoAlgorithm, 0, wxTOP | wxBOTTOM, 3);
    // In the order of TempoAlgorithm (Speedy is 1, Signalsmith 2)
    m_tempoAlgorithm->Append("Speedy (Google) - Nonlinear speech speedup");
    m_tempoAlgorithm->Append("Signalsmith Stretch - High quality time/pitch");
    m_tempoAlgorithm->SetSelection(g_tempoAlgorithm == static_cast<int>(TempoAlgorithm::Speedy) ? 0 : 1);

    // Initialize EQ frequency edit controls
    AddText(page, sizer, "EQ frequencies (Hz) - changes apply on next EQ enable:");
    auto* row = AddRow(sizer);
    m_eqBassFreq = AddEdit(page, row, "Bass (20-500):", wxString::Format("%.0f", g_eqBassFreq), 60);
    SetDigitsOnly(m_eqBassFreq);
    m_eqMidFreq = AddEdit(page, row, "Mid (200-5k):", wxString::Format("%.0f", g_eqMidFreq), 60);
    SetDigitsOnly(m_eqMidFreq);
    m_eqTrebleFreq = AddEdit(page, row, "Treble (2k-20k):", wxString::Format("%.0f", g_eqTrebleFreq), 60);
    SetDigitsOnly(m_eqTrebleFreq);

    // Disable batch delay checkbox
    m_disableBatch = AddCheck(page, sizer, "Disable &batch delay (only catches one file at a time)", g_disableBatchDelay);
    m_smoothSeek = AddCheck(page, sizer, "&Smooth seeking (short fades when seeking, pausing and changing tracks)", g_smoothSeek);

    // Live streams kept for rewinding
    m_liveRewind = AddCheck(page, sizer, "Allow &rewinding and pausing live streams (L goes back to live)", g_liveRewind);
    row = AddRow(sizer);
    m_liveRewindMinutes = AddChoice(page, row, "Rewind &length:");
    int minutesIndex = 3;
    for (int i = 0; i < g_liveRewindChoiceCount; i++) {
        m_liveRewindMinutes->Append(wxString::Format("%d minutes", g_liveRewindChoices[i]));
        if (g_liveRewindChoices[i] == g_liveRewindMinutes) minutesIndex = i;
    }
    m_liveRewindMinutes->SetSelection(minutesIndex);
    m_liveRewindMinutes->Enable(g_liveRewind);
    m_liveRewind->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { m_liveRewindMinutes->Enable(m_liveRewind->GetValue()); });
    AddText(page, sizer, "Kept as the stream comes, about 1 MB a minute at 128 kbps. Applies to streams opened after.");

    auto* reset = new wxButton(page, wxID_ANY, "Reset station/podcast &order to alphabetical");
    sizer->Add(reset, 0, wxTOP | wxBOTTOM, 3);
    reset->Bind(wxEVT_BUTTON, &OptionsDialog::OnResetListOrder, this);

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    page->FitInside();
    book->AddPage(page, "Advanced");
}

void OptionsDialog::BuildYouTubePage(wxNotebook* book) {
	auto* page = new wxScrolledWindow(book);
	page->SetScrollRate(0, 10);
	page->SetMinSize(wxSize(550, 500));
	auto* sizer = new wxBoxSizer(wxVERTICAL);
	const auto tools = GetYouTubeToolSettings();

	m_toolSource = AddChoice(page, sizer, "Tool &source:", 260);
	m_toolSource->Append("FastPlay managed");
	m_toolSource->Append("Installed tools");
	m_toolSource->SetSelection(tools.source == YouTubeToolSource::Installed ? 1 : 0);
	AddText(page, sizer, "Managed tools are downloaded and updated by FastPlay.\nInstalled tools are never downloaded or updated.");
	AddText(page, sizer, "Installed mode: leave paths empty to search PATH.\nOn macOS, standard Homebrew locations are also searched.");

	auto* row = AddRow(sizer);
	m_ytdlpPath = AddEdit(page, row, "&yt-dlp path:", WX(tools.ytdlpPath), 300);
	m_ytdlpBrowse = new wxButton(page, wxID_ANY, "Browse yt-dlp...");
	row->Add(m_ytdlpBrowse, 0, wxALIGN_CENTER_VERTICAL);
	m_ytdlpBrowse->Bind(wxEVT_BUTTON, &OptionsDialog::OnYtdlpBrowse, this);
	AddText(page, sizer, "In managed mode, an existing yt-dlp override is used;\nFastPlay manages Deno and FFmpeg.");

	row = AddRow(sizer);
	m_denoPath = AddEdit(page, row, "&Deno path:", WX(tools.denoPath), 300);
	m_denoBrowse = new wxButton(page, wxID_ANY, "Browse Deno...");
	row->Add(m_denoBrowse, 0, wxALIGN_CENTER_VERTICAL);
	m_denoBrowse->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
#ifdef __WXMSW__
		const char* filter = "Executables (*.exe)|*.exe|All Files (*.*)|*.*";
#else
		const char* filter = "All Files (*)|*";
#endif
		wxFileDialog dlg(this, "Select Deno executable", wxEmptyString, wxEmptyString, filter,
			wxFD_OPEN | wxFD_FILE_MUST_EXIST);
		if (dlg.ShowModal() == wxID_OK) m_denoPath->SetValue(dlg.GetPath());
	});
	row = AddRow(sizer);
	m_ffmpegFolder = AddEdit(page, row, "&FFmpeg folder:", WX(tools.ffmpegFolder), 300);
	m_ffmpegBrowse = new wxButton(page, wxID_ANY, "Browse FFmpeg...");
	row->Add(m_ffmpegBrowse, 0, wxALIGN_CENTER_VERTICAL);
	m_ffmpegBrowse->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
		wxDirDialog dlg(this, "Select folder containing FFmpeg and ffprobe", m_ffmpegFolder->GetValue());
		if (dlg.ShowModal() == wxID_OK) m_ffmpegFolder->SetValue(dlg.GetPath());
	});

	m_testTools = new wxButton(page, wxID_ANY, "&Test tools");
	sizer->Add(m_testTools, 0, wxTOP | wxBOTTOM, 5);
	m_testTools->Bind(wxEVT_BUTTON, &OptionsDialog::OnTestTools, this);
	AddText(page, sizer, "Tool test &results:");
	m_toolResults = new wxTextCtrl(page, wxID_ANY, "Test tools to check paths and versions offline.",
		wxDefaultPosition, wxSize(500, 115), wxTE_MULTILINE | wxTE_READONLY);
	m_toolResults->SetName("Tool test results");
	sizer->Add(m_toolResults, 0, wxEXPAND | wxBOTTOM, 8);
	m_toolSource->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
		UpdateToolControls();
		m_toolResults->ChangeValue("Tool source changed. Test tools to check this selection.");
	});
	for (auto* field : {m_ytdlpPath, m_denoPath, m_ffmpegFolder}) {
		field->Bind(wxEVT_TEXT, [this](wxCommandEvent& event) {
			m_toolResults->ChangeValue("Tool paths changed. Test tools to check this selection.");
			event.Skip();
		});
	}
	UpdateToolControls();

    AddText(page, sizer, "YouTube Data &API key (optional, enables search):");
    m_ytApiKey = new wxTextCtrl(page, wxID_ANY, WX(g_ytApiKey), wxDefaultPosition, wxSize(430, -1), wxTE_PASSWORD);
    sizer->Add(m_ytApiKey, 0, wxTOP | wxBOTTOM, 3);
    AddText(page, sizer, "Get an API key from: console.cloud.google.com");
    AddText(page, sizer, "Without API key, yt-dlp will be used for search (slower).");

    m_ytAutoRefresh = AddChoice(page, sizer, "Refresh &favorites:", 260);
    for (const char* item : {"Off", "At startup", "Every 30 minutes", "Every hour", "Every 2 hours", "Every 4 hours",
                             "Every 8 hours"}) {
        m_ytAutoRefresh->Append(item);
    }
    m_ytAutoRefresh->SetSelection(std::clamp(g_ytAutoRefresh, 0, 6));
    AddText(page, sizer, "Checks your favorite channels and playlists for new videos, and says when there are some.");

    // Cookies, for videos YouTube only shows to a signed-in account
    m_cookiesStatus = new wxStaticText(page, wxID_ANY, "");
    sizer->Add(m_cookiesStatus, 0, wxTOP, 10);
    auto* cookieRow = AddRow(sizer);
    auto* importCookies = new wxButton(page, wxID_ANY, "Import &cookies.txt...");
    cookieRow->Add(importCookies, 0, wxRIGHT, 6);
    m_removeCookies = new wxButton(page, wxID_ANY, "&Remove cookies");
    cookieRow->Add(m_removeCookies);
    importCookies->Bind(wxEVT_BUTTON, &OptionsDialog::OnImportCookies, this);
    m_removeCookies->Bind(wxEVT_BUTTON, &OptionsDialog::OnRemoveCookies, this);
    AddText(page, sizer, "Export your youtube.com cookies with a browser extension (cookies.txt format) while signed in.");
    UpdateCookiesStatus();

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
	page->FitInside();
    book->AddPage(page, "YouTube");
}

void OptionsDialog::BuildYouTubeDownloadsPage(wxNotebook* book) {
    auto* page = NewPage(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    const YouTubeDownloadSettings& s = g_ytDownload;

    AddText(page, sizer, "How the YouTube window's Download button saves videos.");
    AddText(page, sizer, "Download &folder:");
    auto* row = AddRow(sizer);
    m_ytFolder = new wxTextCtrl(page, wxID_ANY, WX(s.folder.empty() ? YouTubeDownloadFolder() : s.folder),
                                wxDefaultPosition, wxSize(380, -1));
    row->Add(m_ytFolder, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    auto* browse = new wxButton(page, wxID_ANY, "&Browse...");
    row->Add(browse, 0, wxALIGN_CENTER_VERTICAL);
    browse->Bind(wxEVT_BUTTON, &OptionsDialog::OnYtFolderBrowse, this);

    auto choice = [&](const char* label, std::initializer_list<const char*> items, int selection) {
        wxChoice* c = AddChoice(page, sizer, label, 260);
        for (const char* item : items) c->Append(item);
        c->SetSelection(selection);
        return c;
    };
    m_ytType = choice("Download &type:", {"Audio only", "Video"}, s.type);
    m_ytAudioFormat = choice("&Audio format:",
                             {"M4A (AAC)", "Best available, as YouTube has it", "MP3", "Opus", "FLAC", "WAV"},
                             s.audioFormat);
    m_ytAudioQuality = choice("Audio &quality (when converting):",
                              {"Best", "320 kbps", "256 kbps", "192 kbps", "128 kbps"}, s.audioQuality);
    m_ytVideoQuality = choice("&Video quality:", {"Best", "2160p (4K)", "1440p", "1080p", "720p", "480p", "360p"},
                              s.videoQuality);
    m_ytVideoContainer = choice("Video &container:", {"MP4", "MKV", "WebM"}, s.videoContainer);
    m_ytVideoCodec = choice("Video co&dec:", {"Any", "H.264", "VP9", "AV1"}, s.videoCodec);
    m_ytNaming = choice("File &naming:", {"Title", "Title [video ID]", "Channel - Title", "Upload date - Title"},
                        s.naming);

    m_ytAddMetadata = AddCheck(page, sizer, "Add &metadata (title, artist, date)", s.addMetadata);
    m_ytEmbedThumbnail = AddCheck(page, sizer, "&Embed thumbnail", s.embedThumbnail);
    m_ytWriteThumbnail = AddCheck(page, sizer, "Save t&humbnail as a file", s.writeThumbnail);
    m_ytWriteDescription = AddCheck(page, sizer, "Save descr&iption as a file", s.writeDescription);
    m_ytWriteSubtitles = AddCheck(page, sizer, "Download &subtitles", s.writeSubtitles);
    m_ytEmbedSubtitles = AddCheck(page, sizer, "Embed subtit&les in videos", s.embedSubtitles);
    m_ytChannelFolder = AddCheck(page, sizer, "Put each channel in its own f&older", s.channelFolder);

    m_ytExtraOptions = AddEdit(page, sizer, "E&xtra yt-dlp options:", WX(s.extraOptions), 380);
    AddText(page, sizer, "Converting, video, metadata and thumbnails need ffmpeg, which FastPlay downloads once if it is not installed.");

    m_ytType->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { UpdateYtDownloadControls(); });
    m_ytAudioFormat->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { UpdateYtDownloadControls(); });
    UpdateYtDownloadControls();

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    page->FitInside();
    book->AddPage(page, "YouTube Downloads");
}

// Only the settings for what is being downloaded can be changed.
void OptionsDialog::UpdateYtDownloadControls() {
    bool video = m_ytType->GetSelection() == 1;
    m_ytAudioFormat->Enable(!video);
    m_ytAudioQuality->Enable(!video && m_ytAudioFormat->GetSelection() >= 2 && m_ytAudioFormat->GetSelection() <= 3);
    m_ytVideoQuality->Enable(video);
    m_ytVideoContainer->Enable(video);
    m_ytVideoCodec->Enable(video);
    m_ytEmbedSubtitles->Enable(video);
}

void OptionsDialog::OnYtFolderBrowse(wxCommandEvent&) {
    wxDirDialog dlg(this, "Select YouTube download folder", m_ytFolder->GetValue(), wxDD_DEFAULT_STYLE);
    if (dlg.ShowModal() == wxID_OK) {
        m_ytFolder->SetValue(dlg.GetPath());
    }
}

void OptionsDialog::BuildSpeedyPage(wxNotebook* book) {
    auto* page = NewPage(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    AddText(page, sizer, "Google Speedy algorithm settings:");
    AddText(page, sizer, "Speedy uses nonlinear speedup optimized for speech.");
    AddText(page, sizer, "It compresses vowels more than consonants for clarity.");
    m_speedyNonlinear = AddCheck(page, sizer, "&Enable nonlinear speedup (recommended for speech)", g_speedyNonlinear);

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    page->FitInside();
    book->AddPage(page, "Speedy");
}

void OptionsDialog::BuildSignalsmithPage(wxNotebook* book) {
    auto* page = NewPage(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    AddText(page, sizer, "Signalsmith Stretch settings (changes apply on next file load):");

    m_ssPreset = AddChoice(page, AddRow(sizer), "&Quality preset:", 180);
    m_ssPreset->Append("Default (higher quality)");
    m_ssPreset->Append("Cheaper (lower CPU)");
    m_ssPreset->SetSelection(g_ssPreset);

    m_ssTonality = AddEdit(page, AddRow(sizer), "&Tonality limit (Hz, 0 = auto):",
                           wxString::Format("%d", g_ssTonalityLimit), 70);
    SetDigitsOnly(m_ssTonality);

    AddText(page, sizer, "Higher tonality limits preserve more harmonics during pitch shift.");
    AddText(page, sizer, "Use 0 for automatic, or 4000-8000 for speech, 8000-16000 for music.");

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    page->FitInside();
    book->AddPage(page, "Signalsmith");
}

void OptionsDialog::BuildMidiPage(wxNotebook* book) {
    auto* page = NewPage(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    AddText(page, sizer, "MIDI playback settings:");
    AddText(page, sizer, "&SoundFont (.sf2/.sf3):");

    auto* row = AddRow(sizer);
    m_midiSoundFont = AddEdit(page, row, wxString(), WX(g_midiSoundFont), 340);
    auto* browse = new wxButton(page, wxID_ANY, "&Browse...");
    row->Add(browse, 0, wxALIGN_CENTER_VERTICAL);
    browse->Bind(wxEVT_BUTTON, &OptionsDialog::OnMidiBrowse, this);

    row = AddRow(sizer);
    m_midiVoices = AddEdit(page, row, "Ma&x voices (polyphony):", wxString::Format("%d", g_midiMaxVoices), 60);
    SetDigitsOnly(m_midiVoices);
    row->Add(new wxStaticText(page, wxID_ANY, "(1-1000, default 128)"), 0, wxALIGN_CENTER_VERTICAL);

    m_midiSinc = AddCheck(page, sizer, "Use s&inc interpolation (higher quality, more CPU)", g_midiSincInterp);

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    page->FitInside();
    book->AddPage(page, "MIDI");
}

void OptionsDialog::BuildLibraryPage(wxNotebook* book) {
    auto* page = NewPage(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    m_libraryFolders = g_libraryFolders;

    AddText(page, sizer, "The folders in the library (Control+L). They are kept up to date as files change.");
    sizer->Add(new wxStaticText(page, wxID_ANY, "&Folders:"), 0, wxTOP, 6);
    m_libraryList = new wxListBox(page, wxID_ANY, wxDefaultPosition, wxSize(380, 120), 0, nullptr, wxLB_SINGLE);
    sizer->Add(m_libraryList, 1, wxEXPAND | wxTOP | wxBOTTOM, 3);
    m_libraryTagged = AddCheck(page, sizer, "&Include its songs in songs, artists, albums and genres", true);
    AddText(page, sizer, "Unchecked, the folder's files are only listed by name, in the folders view.");

    auto* row = AddRow(sizer);
    auto* add = new wxButton(page, wxID_ANY, "&Add Folder...");
    row->Add(add, 0, wxRIGHT, 6);
    auto* remove = new wxButton(page, wxID_ANY, "&Remove");
    row->Add(remove, 0, wxRIGHT, 6);
    auto* rescan = new wxButton(page, wxID_ANY, "Re&scan All");
    row->Add(rescan);

    m_libraryList->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { ShowLibraryFolders(m_libraryList->GetSelection()); });
    m_libraryTagged->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
        int sel = m_libraryList->GetSelection();
        if (sel < 0 || sel >= static_cast<int>(m_libraryFolders.size())) return;
        m_libraryFolders[sel].tagged = m_libraryTagged->GetValue();
        ShowLibraryFolders(sel);
    });
    add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxDirDialog dlg(this, "Add a folder to the library", "", wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dlg.ShowModal() != wxID_OK) return;
        LibraryFolder folder;
        folder.path = WS(dlg.GetPath());
        for (size_t i = 0; i < m_libraryFolders.size(); i++) {
            if (m_libraryFolders[i].path == folder.path) {
                ShowLibraryFolders(static_cast<int>(i));
                return;
            }
        }
        m_libraryFolders.push_back(folder);
        ShowLibraryFolders(static_cast<int>(m_libraryFolders.size()) - 1);
        m_libraryList->SetFocus();
    });
    remove->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        int sel = m_libraryList->GetSelection();
        if (sel < 0 || sel >= static_cast<int>(m_libraryFolders.size())) return;
        m_libraryFolders.erase(m_libraryFolders.begin() + sel);
        ShowLibraryFolders(std::min(sel, static_cast<int>(m_libraryFolders.size()) - 1));
        m_libraryList->SetFocus();
    });
    rescan->Bind(wxEVT_BUTTON, [](wxCommandEvent&) {
        RescanLibrary();
        Speak("Rescanning the library");
    });
    ShowLibraryFolders(m_libraryFolders.empty() ? -1 : 0);

    page->SetSizer(new wxBoxSizer(wxVERTICAL));
    page->GetSizer()->Add(sizer, 1, wxEXPAND | wxALL, 10);
    page->FitInside();
    book->AddPage(page, "Library");
}

// The folder list as edited, with `select` selected and its checkbox showing it
void OptionsDialog::ShowLibraryFolders(int select) {
    wxArrayString texts;
    for (const LibraryFolder& folder : m_libraryFolders) {
        texts.push_back(WX(folder.path) + (folder.tagged ? "" : ", folders view only"));
    }
    m_libraryList->Set(texts);
    if (select >= 0 && select < static_cast<int>(m_libraryFolders.size())) {
        m_libraryList->SetSelection(select);
        m_libraryTagged->SetValue(m_libraryFolders[select].tagged);
        m_libraryTagged->Enable(true);
    } else {
        m_libraryTagged->Enable(false);
    }
}

void OptionsDialog::OnOK(wxCommandEvent&) {
    // Get selected device
    int sel = m_soundcard->GetSelection();
    int newDevice = (sel >= 0 && sel < static_cast<int>(m_deviceIndexes.size())) ? m_deviceIndexes[sel] : -1;
    if (newDevice == 0) newDevice = -1;  // "Default": the system's, as it changes

    // Get amplify setting
    bool newAmplify = m_allowAmplify->GetValue();

    // Get remember playback state setting
    g_rememberState = m_rememberState->GetValue();

    // Get bring to front setting
    g_bringToFront = m_bringToFront->GetValue();
    g_loadFolder = m_loadFolder->GetValue();
    g_minimizeToTray = m_minimizeToTray->GetValue();
    g_showTitleInWindow = m_showTitle->GetValue();
    g_autoAdvance = m_autoAdvance->GetValue();
    g_playlistFollowPlayback = m_playlistFollow->GetValue();
    g_checkForUpdates = m_checkUpdates->GetValue();
    g_allowMultipleInstances = m_multiInstance->GetValue();
    UpdateWindowTitle();  // Apply immediately

    {
        unsigned int rewind = 0;
        if (!m_rewindOnPause->GetValue().ToUInt(&rewind)) rewind = 0;
        g_rewindOnPauseMs = static_cast<int>(rewind);
        if (g_rewindOnPauseMs < 0) g_rewindOnPauseMs = 0;
    }

    // Get download settings
    g_downloadPath = WS(m_downloadPath->GetValue());
    g_downloadOrganizeByFeed = m_downloadOrganize->GetValue();

    // Get volume step setting
    {
        int volStepSel = m_volumeStep->GetSelection();
        if (volStepSel >= 0 && volStepSel < 7) {
            g_volumeStep = kVolumeSteps[volStepSel] / 100.0f;
        }
    }

    // Get ReplayGain settings
    {
        int rgSel = m_replayGainMode->GetSelection();
        if (rgSel >= 0 && rgSel <= 2) g_replayGainMode = rgSel;

        float preamp = static_cast<float>(std::wcstod(WS(m_replayGainPreamp->GetValue()).c_str(), nullptr));
        if (preamp < -15.0f) preamp = -15.0f;
        if (preamp > 15.0f) preamp = 15.0f;
        g_replayGainPreamp = preamp;

        g_replayGainPreventClip = m_replayGainClip->GetValue();

        // Re-apply to the currently playing track so the change is audible immediately
        RefreshReplayGain();
    }

    // Get remember position threshold
    int posSel = m_rememberPos->GetSelection();
    if (posSel >= 0 && posSel < g_posThresholdCount) {
        g_rememberPosMinutes = g_posThresholds[posSel];
    }

    // Apply device change if needed
    if (newDevice != g_selectedDevice) {
        SwitchAudioDevice(newDevice);
    }

    // Apply amplify setting
    g_allowAmplify = newAmplify;

    // Clamp volume if amplify was disabled
    if (!g_allowAmplify && g_volume > MAX_VOLUME_NORMAL) {
        SetVolume(MAX_VOLUME_NORMAL);
    }

    // Get seek amount checkboxes
    for (int i = 0; i < g_seekAmountCount; i++) {
        g_seekEnabled[i] = m_seekChecks[i]->GetValue();
    }
    g_chapterSeekEnabled = m_chapterSeek->GetValue();

    // Validate current seek index - ensure it points to an enabled amount
    if (!g_seekEnabled[g_currentSeekIndex]) {
        for (int i = 0; i < g_seekAmountCount; i++) {
            if (g_seekEnabled[i]) {
                g_currentSeekIndex = i;
                break;
            }
        }
    }

    // Update file types registration setting
    {
        bool newRegister = m_registerFileTypes->GetValue();
        if (newRegister && !g_registerFileTypes) {
            // Just enabled - register all file types now
            RegisterAllFileTypes();
        } else if (!newRegister && g_registerFileTypes) {
            // Just disabled - unregister all file types now
            UnregisterAllFileTypes();
        }
        g_registerFileTypes = newRegister;
    }

    // Get effect checkboxes
    g_effectEnabled[0] = m_effectVolume->GetValue();
    g_effectEnabled[1] = m_effectPitch->GetValue();
    g_effectEnabled[2] = m_effectTempo->GetValue();
    g_effectEnabled[3] = m_effectRate->GetValue();

    // Get rate step mode
    {
        int rateStepSel = m_rateStepMode->GetSelection();
        if (rateStepSel >= 0 && rateStepSel <= 1) {
            g_rateStepMode = rateStepSel;
        }
    }

    // Validate current effect index - ensure it points to an enabled effect
    if (!g_effectEnabled[g_currentEffectIndex]) {
        for (int i = 0; i < 4; i++) {
            if (g_effectEnabled[i]) {
                g_currentEffectIndex = i;
                break;
            }
        }
    }

    // Get reverb algorithm from combobox
    {
        int reverbSel = m_reverb->GetSelection();
        if (reverbSel >= 0 && reverbSel < static_cast<int>(ReverbAlgorithm::COUNT)) {
            SetReverbAlgorithm(reverbSel);
        }
    }

    // Get DSP effect checkboxes and enable/disable effects
    EnableDSPEffect(DSPEffectType::Echo, m_dspEcho->GetValue());
    EnableDSPEffect(DSPEffectType::EQ, m_dspEQ->GetValue());
    EnableDSPEffect(DSPEffectType::Compressor, m_dspCompressor->GetValue());
    EnableDSPEffect(DSPEffectType::Normalizer, m_dspNormalizer->GetValue());
    EnableDSPEffect(DSPEffectType::StereoWidth, m_dspStereoWidth->GetValue());
    EnableDSPEffect(DSPEffectType::CenterCancel, m_dspCenterCancel->GetValue());
    EnableDSPEffect(DSPEffectType::Convolution, m_dspConvolution->GetValue());
    EnableDSPEffect(DSPEffectType::SpatialAudio, m_dspSpatial->GetValue());

    // Get buffer settings
    {
        int bufferSel = m_bufferSize->GetSelection();
        if (bufferSel >= 0 && bufferSel < g_bufferSizeCount && g_bufferSizes[bufferSel] != g_bufferSize) {
            // The output buffer is made with the device: open it again
            g_bufferSize = g_bufferSizes[bufferSel];
            SwitchAudioDevice(g_selectedDevice);
        }

        g_tempoAlgorithm = static_cast<int>(m_tempoAlgorithm->GetSelection() == 0 ? TempoAlgorithm::Speedy
                                                                                  : TempoAlgorithm::Signalsmith);

        // Get EQ frequencies
        float bassFreq = static_cast<float>(std::wcstod(WS(m_eqBassFreq->GetValue()).c_str(), nullptr));
        if (bassFreq >= 20.0f && bassFreq <= 500.0f) g_eqBassFreq = bassFreq;

        float midFreq = static_cast<float>(std::wcstod(WS(m_eqMidFreq->GetValue()).c_str(), nullptr));
        if (midFreq >= 200.0f && midFreq <= 5000.0f) g_eqMidFreq = midFreq;

        float trebleFreq = static_cast<float>(std::wcstod(WS(m_eqTrebleFreq->GetValue()).c_str(), nullptr));
        if (trebleFreq >= 2000.0f && trebleFreq <= 20000.0f) g_eqTrebleFreq = trebleFreq;

        // Get disable batch delay setting
        g_disableBatchDelay = m_disableBatch->GetValue();
        g_smoothSeek = m_smoothSeek->GetValue();
        audio::SetSmoothTransitions(g_smoothSeek);
        g_liveRewind = m_liveRewind->GetValue();
        int minutesSel = m_liveRewindMinutes->GetSelection();
        if (minutesSel >= 0 && minutesSel < g_liveRewindChoiceCount) g_liveRewindMinutes = g_liveRewindChoices[minutesSel];
        ApplyLiveRewindSetting();
    }

    // Get YouTube settings
	SetYouTubeToolSettings(ReadToolSettings());
    g_ytApiKey = WS(m_ytApiKey->GetValue());
    g_ytAutoRefresh = m_ytAutoRefresh->GetSelection();
    StartYouTubeAutoRefresh(false);

    // Get YouTube download settings. The folder is only stored when it is not
    // the default, so a moved Downloads folder is still followed.
    {
        YouTubeDownloadSettings& s = g_ytDownload;
        std::wstring folder = WS(m_ytFolder->GetValue());
        s.folder.clear();  // so YouTubeDownloadFolder() gives the default to compare with
        s.folder = folder == YouTubeDownloadFolder() ? L"" : folder;
        s.type = m_ytType->GetSelection();
        s.audioFormat = m_ytAudioFormat->GetSelection();
        s.audioQuality = m_ytAudioQuality->GetSelection();
        s.videoQuality = m_ytVideoQuality->GetSelection();
        s.videoContainer = m_ytVideoContainer->GetSelection();
        s.videoCodec = m_ytVideoCodec->GetSelection();
        s.naming = m_ytNaming->GetSelection();
        s.addMetadata = m_ytAddMetadata->GetValue();
        s.embedThumbnail = m_ytEmbedThumbnail->GetValue();
        s.writeThumbnail = m_ytWriteThumbnail->GetValue();
        s.writeDescription = m_ytWriteDescription->GetValue();
        s.writeSubtitles = m_ytWriteSubtitles->GetValue();
        s.embedSubtitles = m_ytEmbedSubtitles->GetValue();
        s.channelFolder = m_ytChannelFolder->GetValue();
        s.extraOptions = WS(m_ytExtraOptions->GetValue());
    }

    // Get Recording settings
    {
        g_recordPath = WS(m_recPath->GetValue());
        g_recordTemplate = WS(m_recTemplate->GetValue());

        int formatSel = m_recFormat->GetSelection();
        if (formatSel >= 0 && formatSel <= 3) g_recordFormat = formatSel;

        int bitrateSel = m_recBitrate->GetSelection();
        if (bitrateSel >= 0 && bitrateSel < 6) g_recordBitrate = kBitrates[bitrateSel];

        g_recordEffects = m_recEffects->GetValue();
        audio::SetTapBeforeEffects(!g_recordEffects);
    }

    // Get Speech settings
    g_speechTrackChange = m_speechTrackChange->GetValue();
    g_speechVolume = m_speechVolume->GetValue();
    g_speechEffect = m_speechEffect->GetValue();

    // Get Speedy settings
    g_speedyNonlinear = m_speedyNonlinear->GetValue();

    // Get Signalsmith settings
    {
        int presetSel = m_ssPreset->GetSelection();
        if (presetSel >= 0 && presetSel <= 1) g_ssPreset = presetSel;

        int tonality = static_cast<int>(std::wcstol(WS(m_ssTonality->GetValue()).c_str(), nullptr, 10));
        if (tonality >= 0 && tonality <= 20000) g_ssTonalityLimit = tonality;
    }

    // Get MIDI settings
    {
        g_midiSoundFont = WS(m_midiSoundFont->GetValue());

        int voices = static_cast<int>(std::wcstol(WS(m_midiVoices->GetValue()).c_str(), nullptr, 10));
        if (voices >= 1 && voices <= 1000) g_midiMaxVoices = voices;

        g_midiSincInterp = m_midiSinc->GetValue();
    }

    // Library folders: indexed again only if they changed
    bool libraryChanged = m_libraryFolders.size() != g_libraryFolders.size();
    for (size_t i = 0; !libraryChanged && i < m_libraryFolders.size(); i++) {
        libraryChanged = m_libraryFolders[i].path != g_libraryFolders[i].path ||
                         m_libraryFolders[i].tagged != g_libraryFolders[i].tagged;
    }
    if (libraryChanged) {
        g_libraryFolders = m_libraryFolders;
        LibraryFoldersChanged();
    }

    // Save settings
    SaveSettings();

    EndModal(wxID_OK);
}

void OptionsDialog::OnRecBrowse(wxCommandEvent&) {
    // Browse for recording output folder
    wxDirDialog dlg(this, "Select recording output folder", wxEmptyString, wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK) {
        m_recPath->SetValue(dlg.GetPath());
    }
}

void OptionsDialog::OnDownloadBrowse(wxCommandEvent&) {
    // Browse for downloads folder
    wxDirDialog dlg(this, "Select downloads folder", wxEmptyString, wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK) {
        m_downloadPath->SetValue(dlg.GetPath());
    }
}

void OptionsDialog::OnRecFormat(wxCommandEvent&) {
    // Enable bitrate only for lossy formats (MP3=1, OGG=2)
    int format = m_recFormat->GetSelection();
    m_recBitrate->Enable(format == 1 || format == 2);
}

void OptionsDialog::UpdateCookiesStatus() {
    bool has = YouTubeHasCookies();
    m_cookiesStatus->SetLabel(has ? "YouTube cookies: imported" : "YouTube cookies: none");
    m_removeCookies->Enable(has);
}

// Cookies are imported (or removed) at once, not when the options are saved.
void OptionsDialog::OnImportCookies(wxCommandEvent&) {
    wxFileDialog dlg(this, "Import cookies.txt", wxEmptyString, "cookies.txt",
                     "Cookie files (*.txt)|*.txt|All Files (*.*)|*.*", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;
    std::wstring error;
    if (YouTubeImportCookies(WS(dlg.GetPath()), error)) {
        UpdateCookiesStatus();
        wxMessageBox("Cookies imported. YouTube will now treat FastPlay as signed in to that account.",
                     "YouTube Cookies", wxOK | wxICON_INFORMATION, this);
    } else {
        wxMessageBox(WX(error), "YouTube Cookies", wxOK | wxICON_ERROR, this);
    }
}

void OptionsDialog::OnRemoveCookies(wxCommandEvent&) {
    YouTubeRemoveCookies();
    UpdateCookiesStatus();
    Speak("Cookies removed");
}

YouTubeToolSettings OptionsDialog::ReadToolSettings() const {
	YouTubeToolSettings tools;
	tools.source = m_toolSource->GetSelection() == 1 ? YouTubeToolSource::Installed : YouTubeToolSource::Managed;
	tools.ytdlpPath = WS(m_ytdlpPath->GetValue());
	tools.denoPath = WS(m_denoPath->GetValue());
	tools.ffmpegFolder = WS(m_ffmpegFolder->GetValue());
	return tools;
}

void OptionsDialog::UpdateToolControls() {
	bool installed = m_toolSource->GetSelection() == 1;
	m_toolSource->Enable(!m_testingTools);
	m_ytdlpPath->Enable(!m_testingTools);
	m_ytdlpBrowse->Enable(!m_testingTools);
	m_denoPath->Enable(installed && !m_testingTools);
	m_denoBrowse->Enable(installed && !m_testingTools);
	m_ffmpegFolder->Enable(installed && !m_testingTools);
	m_ffmpegBrowse->Enable(installed && !m_testingTools);
	m_testTools->Enable(!m_testingTools);
}

void OptionsDialog::OnTestTools(wxCommandEvent&) {
	if (m_testingTools) return;
	const auto tools = ReadToolSettings();
	m_testingTools = true;
	UpdateToolControls();
	m_toolResults->ChangeValue("Testing local tools...");
	auto alive = m_toolTestAlive;
	std::thread([this, alive, tools]() {
		std::wstring report;
		try {
			report = TestYouTubeTools(tools);
		} catch (const std::exception& error) {
			report = L"Could not test tools: " + Utf8ToWide(error.what());
		}
		RunOnUiThread([this, alive, report]() {
			// The flag is read and written only on the UI thread.
			if (!*alive) return;
			m_testingTools = false;
			UpdateToolControls();
			m_toolResults->ChangeValue(WX(report));
			m_toolResults->SetInsertionPoint(0);
			SpeakW(L"Tool test finished. Results are available in the YouTube settings tab.");
		});
	}).detach();
}

void OptionsDialog::OnYtdlpBrowse(wxCommandEvent&) {
#ifdef __WXMSW__
    const char* filter = "Executables (*.exe)|*.exe|All Files (*.*)|*.*";
#else
    const char* filter = "All Files (*)|*";
#endif
    wxFileDialog dlg(this, "Select yt-dlp executable", wxEmptyString, wxEmptyString, filter,
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK) {
        m_ytdlpPath->SetValue(dlg.GetPath());
    }
}

void OptionsDialog::OnMidiBrowse(wxCommandEvent&) {
    wxFileDialog dlg(this, "Select SoundFont file", wxEmptyString, wxEmptyString,
                     "SoundFont Files (*.sf2;*.sf3;*.sfz)|*.sf2;*.sf3;*.sfz|All Files (*.*)|*.*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK) {
        m_midiSoundFont->SetValue(dlg.GetPath());
    }
}

void OptionsDialog::OnConvBrowse(wxCommandEvent&) {
    wxFileDialog dlg(this, "Select Impulse Response file", wxEmptyString, wxEmptyString,
                     "IR Files (*.wav;*.flac;*.ogg;*.mp3)|*.wav;*.flac;*.ogg;*.mp3|"
                     "WAV Files (*.wav)|*.wav|"
                     "FLAC Files (*.flac)|*.flac|"
                     "All Files (*.*)|*.*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK) {
        std::wstring filePath = WS(dlg.GetPath());
        g_convolutionIRPath = filePath;
        // Display just the filename
        m_convIR->SetValue(WX(FileNameOnly(filePath)));
        // Load the IR file
        ConvolutionReverb* conv = GetConvolutionReverb();
        if (conv) {
            conv->LoadIR(filePath.c_str());
        }
    }
}

void OptionsDialog::OnResetListOrder(wxCommandEvent&) {
    ResetRadioSortOrder();
    ResetPodcastSortOrder();
    Speak("Order reset to alphabetical");
}

void OptionsDialog::OnHotkeyAdd(wxCommandEvent&) {
    HotkeyDlgData data = {0, 0, 0, false};
    if (ShowHotkeyDialog(this, data)) {
        // Add new hotkey
        GlobalHotkey hk;
        hk.id = g_nextHotkeyId++;
        hk.modifiers = data.modifiers;
        hk.vk = data.vk;
        hk.actionIdx = data.actionIdx;
        hk.global = data.global;
        MainFrame* frame = GetMainFrame();
        if (frame) frame->UnregisterGlobalHotkeys();
        g_hotkeys.push_back(hk);
        ApplyHotkeys();

        // Update list
        m_hotkeyList->Append(WX(HotkeyListItem(hk)));
        SaveHotkeys();
    }
}

void OptionsDialog::OnHotkeyEdit(wxCommandEvent&) {
    int sel = m_hotkeyList->GetSelection();
    if (sel >= 0 && sel < static_cast<int>(g_hotkeys.size())) {
        HotkeyDlgData data;
        data.actionIdx = g_hotkeys[sel].actionIdx;
        data.modifiers = g_hotkeys[sel].modifiers;
        data.vk = g_hotkeys[sel].vk;
        data.isEdit = true;
        data.global = g_hotkeys[sel].global;

        if (ShowHotkeyDialog(this, data)) {
            // Update hotkey, and put it to work
            MainFrame* frame = GetMainFrame();
            if (frame) frame->UnregisterGlobalHotkeys();
            g_hotkeys[sel].modifiers = data.modifiers;
            g_hotkeys[sel].vk = data.vk;
            g_hotkeys[sel].actionIdx = data.actionIdx;
            g_hotkeys[sel].global = data.global;
            ApplyHotkeys();

            // Update list item
            m_hotkeyList->SetString(sel, WX(HotkeyListItem(g_hotkeys[sel])));
            m_hotkeyList->SetSelection(sel);

            SaveHotkeys();
        }
    }
}

void OptionsDialog::OnHotkeyRemove(wxCommandEvent&) {
    int sel = m_hotkeyList->GetSelection();
    if (sel >= 0 && sel < static_cast<int>(g_hotkeys.size())) {
        // Unregister and remove
        MainFrame* frame = GetMainFrame();
        if (frame) frame->UnregisterGlobalHotkeys();
        g_hotkeys.erase(g_hotkeys.begin() + sel);
        ApplyHotkeys();
        m_hotkeyList->Delete(sel);

        // Select next item or previous
        if (sel >= static_cast<int>(g_hotkeys.size())) {
            sel = static_cast<int>(g_hotkeys.size()) - 1;
        }
        if (sel >= 0) {
            m_hotkeyList->SetSelection(sel);
        }

        SaveHotkeys();
    }
}

void OptionsDialog::OnHotkeyEnabled(wxCommandEvent&) {
    bool newEnabled = m_hotkeyEnabled->GetValue();
    if (newEnabled != g_hotkeysEnabled) {
        g_hotkeysEnabled = newEnabled;
        MainFrame* frame = GetMainFrame();
        if (frame) {
            if (g_hotkeysEnabled) {
                frame->UnregisterGlobalHotkeys();
                frame->RegisterGlobalHotkeys();
            } else {
                frame->UnregisterGlobalHotkeys();
            }
        }
        SaveHotkeys();
    }
}

}  // namespace

// Show options dialog
void ShowOptionsDialog() {
    OptionsDialog dlg(GetMainWindow());
    dlg.ShowModal();
}
