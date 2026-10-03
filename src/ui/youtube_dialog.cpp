// The YouTube window, with three tabs:
// - Search: search YouTube or paste a video, playlist or channel URL. Enter plays a
//   video or opens a channel or playlist (Backspace goes back); a channel or
//   playlist, or a video's channel, can be added to the favorites.
// - Favorites: the favorite channels and playlists, newest upload first or by name.
//   Enter lists a favorite's videos below it; Delete removes it. Import adds the
//   channels listed in a text file, one URL, @handle or ID per line.
// - Recent: the searches, channels and playlists viewed lately, newest first (not
//   the videos played). Enter searches or opens one again on the Search tab;
//   Delete removes one.
// Download, on either tab, saves a video (or a whole playlist) to keep, as
// Options > YouTube Downloads says.
// It is modeless, so the main window stays usable while it is open. Searching,
// listing and loading take seconds, so they run on worker threads; what they find
// is used only if the window is still open and nothing newer was asked for.

#include "ui/dialogs.h"
#include "ui/ui_common.h"

#include "youtube.h"
#include "player.h"
#include "playlist_io.h"
#include "database.h"
#include "globals.h"
#include "accessibility.h"
#include "app_ui.h"
#include "utils.h"

#include <wx/filedlg.h>
#include <wx/notebook.h>
#include <algorithm>
#include <atomic>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

class YouTubeDialog;

// The open YouTube window, if any.
YouTubeDialog* g_youTubeDialog = nullptr;

// Each window gets a new number, so work started for a closed one is dropped.
std::atomic<int> g_windowNumber{0};
// The newest search/listing on each tab and the newest video to play; older ones
// are dropped when they finish.
std::atomic<int> g_searchRequest{0};
std::atomic<int> g_videosRequest{0};
std::atomic<int> g_playRequest{0};

// Progress from a worker thread, said on the UI thread.
void SpeakStatus(const std::wstring& message) {
    RunOnUiThread([message]() { SpeakW(message); });
}

void RunInBackground(std::function<void()> work) {
    std::thread(std::move(work)).detach();
}

// Get a video ready on a worker thread, then play it (whether or not the window is
// still open).
void StartPlaying(const std::wstring& videoId) {
	int request = ++g_playRequest;
	Speak("Loading");
	RunInBackground([videoId, request]() {
		YouTubeMedia media;
		std::wstring error;
		bool ok = YouTubePrepare(videoId, media, error, SpeakStatus);
		RunOnUiThread([ok, media, error, request]() {
			if (request != g_playRequest) return;  // another video was chosen meanwhile
			if (!ok) {
				Speak("Could not play the video");
				ShowMessage(L"Could not play the video.\n\n" + error, L"YouTube", MessageIcon::Error);
				return;
			}
			// Played like an opened file or stream: it becomes the playlist.
			std::wstring path = media.url.empty() ? media.file : media.url;
			if (!media.title.empty()) {
				SetTrackName(path, media.channel.empty() ? media.title : media.channel + L" - " + media.title);
			}
			SetTrackMetadata(path, media.metadata);
			g_playlist.clear();
			g_playlist.push_back(path);
			PlayTrack(0);
		});
	});
}

// "3 days ago", for when a favorite last uploaded.
std::wstring Ago(int64_t when) {
    int64_t seconds = static_cast<int64_t>(time(nullptr)) - when;
    if (seconds < 3600) return L"less than an hour ago";
    struct Unit {
        int64_t seconds;
        const wchar_t* name;
    };
    const Unit units[] = {{365 * 86400, L"year"}, {30 * 86400, L"month"}, {7 * 86400, L"week"},
                          {86400, L"day"}, {3600, L"hour"}};
    for (const auto& unit : units) {
        int64_t count = seconds / unit.seconds;
        if (count >= 1) {
            return std::to_wstring(count) + L" " + unit.name + (count == 1 ? L"" : L"s") + L" ago";
        }
    }
    return L"";
}

std::wstring ResultText(const YouTubeResult& result) {
    switch (result.kind) {
        case YouTubeKind::Channel:
            return L"Channel: " + result.title;
        case YouTubeKind::Playlist:
            return L"Playlist: " + result.title + (result.channel.empty() ? L"" : L" - " + result.channel);
        default: {
            std::wstring text = result.title;
            if (!result.channel.empty()) text += L" - " + result.channel;
            if (!result.duration.empty()) text += L" [" + result.duration + L"]";
            return text;
        }
    }
}

// What a results list shows: a search, or a channel's or playlist's videos.
struct ResultsView {
    std::wstring query;         // a search, or
    std::wstring listUrl;       // a channel's or playlist's page
    YouTubeListInfo list;       // which one, when known (list.id empty otherwise)
    std::vector<YouTubeResult> results;
    std::wstring nextPageToken;
    int selection = -1;
};

using Worker = std::function<bool(std::vector<YouTubeResult>&, std::wstring&, std::wstring&)>;

// Looking up the favorites' latest uploads: quietly (the window opening, a
// favorite added), because the Refresh button was pressed (says what it found),
// or on the automatic schedule (speaks only of new videos).
enum class RefreshMode { Quiet, Manual, Auto };
void RefreshFavorites(RefreshMode mode, int onlyId = -1);

class YouTubeDialog : public wxDialog {
public:
    explicit YouTubeDialog(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, "YouTube", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          m_window(++g_windowNumber) {
        g_youTubeDialog = this;

        auto* sizer = new wxBoxSizer(wxVERTICAL);
        m_book = new wxNotebook(this, wxID_ANY);
        BuildSearchPage();
        BuildFavoritesPage();
        BuildRecentPage();
        sizer->Add(m_book, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        buttons->AddStretchSpacer();
        buttons->Add(new wxButton(this, wxID_CANCEL, "Close"));
        sizer->Add(buttons, 0, wxEXPAND | wxALL, 10);

        SetSizerAndFit(sizer);
        CentreOnParent();

        // Close and Escape destroy the window rather than hiding it
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Destroy(); }, wxID_CANCEL);
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { Destroy(); });
        Bind(wxEVT_CHAR_HOOK, &YouTubeDialog::OnCharHook, this);

        LoadFavorites();
        LoadRecent();
        RefreshFavorites(RefreshMode::Quiet);
        m_search->SetFocus();
    }

    ~YouTubeDialog() override {
        if (g_youTubeDialog == this) g_youTubeDialog = nullptr;
    }

    // Whether work started for window number `window` still belongs to this one.
    static YouTubeDialog* For(int window) {
        return g_youTubeDialog && g_youTubeDialog->m_window == window ? g_youTubeDialog : nullptr;
    }

private:
    // ------------------------------------------------------------------
    // Layout
    // ------------------------------------------------------------------

    void BuildSearchPage() {
        auto* page = new wxPanel(m_book);
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(new wxStaticText(page, wxID_ANY, "&Search or paste URL:"), 0, wxLEFT | wxRIGHT | wxTOP, 10);
        m_search = new wxTextCtrl(page, wxID_ANY, "");
        sizer->Add(m_search, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        sizer->Add(new wxStaticText(page, wxID_ANY, "&Results:"), 0, wxLEFT | wxRIGHT | wxTOP, 10);
        m_results = new wxListBox(page, wxID_ANY, wxDefaultPosition, ConvertDialogToPixels(wxSize(336, 180)), 0,
                                  nullptr, wxLB_SINGLE);
        sizer->Add(m_results, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        m_loadMore = new wxButton(page, wxID_ANY, "&Load More");
        m_loadMore->Enable(false);
        buttons->Add(m_loadMore, 0, wxRIGHT, 6);
        m_addFavorite = new wxButton(page, wxID_ANY, "Add to &Favorites");
        buttons->Add(m_addFavorite, 0, wxRIGHT, 6);
        auto* download = new wxButton(page, wxID_ANY, "Do&wnload");
        buttons->Add(download);
        sizer->Add(buttons, 0, wxALL, 10);
        page->SetSizer(sizer);
        m_book->AddPage(page, "Search");

        m_results->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) { OpenResult(); });
        m_loadMore->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { LoadMoreResults(); });
        m_addFavorite->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { AddFavoriteFromResults(); });
        download->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { DownloadResult(); });
    }

    void BuildFavoritesPage() {
        auto* page = new wxPanel(m_book);
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        auto* sortRow = new wxBoxSizer(wxHORIZONTAL);
        sortRow->Add(new wxStaticText(page, wxID_ANY, "S&ort by:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        m_sort = new wxChoice(page, wxID_ANY);
        m_sort->Append("Latest upload");
        m_sort->Append("Name");
        m_sort->SetSelection(g_ytFavoritesSort == 1 ? 1 : 0);
        sortRow->Add(m_sort);
        sizer->Add(sortRow, 0, wxLEFT | wxRIGHT | wxTOP, 10);

        sizer->Add(new wxStaticText(page, wxID_ANY, "&Favorites:"), 0, wxLEFT | wxRIGHT | wxTOP, 10);
        m_favoritesList = new wxListBox(page, wxID_ANY, wxDefaultPosition, ConvertDialogToPixels(wxSize(336, 90)),
                                        0, nullptr, wxLB_SINGLE);
        sizer->Add(m_favoritesList, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        sizer->Add(new wxStaticText(page, wxID_ANY, "&Videos:"), 0, wxLEFT | wxRIGHT | wxTOP, 10);
        m_videos = new wxListBox(page, wxID_ANY, wxDefaultPosition, ConvertDialogToPixels(wxSize(336, 90)), 0,
                                 nullptr, wxLB_SINGLE);
        sizer->Add(m_videos, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* remove = new wxButton(page, wxID_ANY, "&Remove");
        buttons->Add(remove, 0, wxRIGHT, 6);
        auto* refresh = new wxButton(page, wxID_ANY, "Refres&h");
        buttons->Add(refresh, 0, wxRIGHT, 6);
        m_moreVideos = new wxButton(page, wxID_ANY, "Load &More");
        m_moreVideos->Enable(false);
        buttons->Add(m_moreVideos, 0, wxRIGHT, 6);
        auto* importButton = new wxButton(page, wxID_ANY, "&Import...");
        buttons->Add(importButton, 0, wxRIGHT, 6);
        auto* download = new wxButton(page, wxID_ANY, "Do&wnload");
        buttons->Add(download);
        sizer->Add(buttons, 0, wxALL, 10);
        page->SetSizer(sizer);
        m_book->AddPage(page, "Favorites");

        m_sort->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            g_ytFavoritesSort = m_sort->GetSelection() == 1 ? 1 : 0;
            ShowFavorites(SelectedFavoriteId());
        });
        m_favoritesList->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) { OpenFavorite(); });
        m_videos->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) { PlayVideo(); });
        remove->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RemoveFavorite(); });
        refresh->Bind(wxEVT_BUTTON, [](wxCommandEvent&) { RefreshFavorites(RefreshMode::Manual); });
        m_moreVideos->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { LoadMoreVideos(); });
        importButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ImportFavorites(); });
        download->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { DownloadVideo(); });
    }

    void BuildRecentPage() {
        auto* page = new wxPanel(m_book);
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(new wxStaticText(page, wxID_ANY, "&Recently viewed:"), 0, wxLEFT | wxRIGHT | wxTOP, 10);
        m_recentList = new wxListBox(page, wxID_ANY, wxDefaultPosition, ConvertDialogToPixels(wxSize(336, 180)), 0,
                                     nullptr, wxLB_SINGLE);
        sizer->Add(m_recentList, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* open = new wxButton(page, wxID_ANY, "&Open");
        buttons->Add(open, 0, wxRIGHT, 6);
        auto* remove = new wxButton(page, wxID_ANY, "Re&move");
        buttons->Add(remove, 0, wxRIGHT, 6);
        auto* clear = new wxButton(page, wxID_ANY, "&Clear All");
        buttons->Add(clear);
        sizer->Add(buttons, 0, wxALL, 10);
        page->SetSizer(sizer);
        m_book->AddPage(page, "Recent");

        m_recentList->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) { OpenRecent(); });
        open->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OpenRecent(); });
        remove->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RemoveRecent(); });
        clear->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ClearRecent(); });
    }

    // Enter acts on the focused control: search from the search box, open or play
    // from a list. Backspace in the results goes back; Delete removes a favorite.
    void OnCharHook(wxKeyEvent& event) {
        int key = event.GetKeyCode();
        wxWindow* focus = FindFocus();
        if (key == WXK_RETURN || key == WXK_NUMPAD_ENTER) {
            if (focus == m_search) return DoSearch();
            if (focus == m_results) return OpenResult();
            if (focus == m_favoritesList) return OpenFavorite();
            if (focus == m_videos) return PlayVideo();
            if (focus == m_recentList) return OpenRecent();
        } else if (key == WXK_BACK && focus == m_results && !event.HasAnyModifiers()) {
            return GoBack();
        } else if ((key == WXK_DELETE || key == WXK_NUMPAD_DELETE) && focus == m_favoritesList) {
            return RemoveFavorite();
        } else if ((key == WXK_DELETE || key == WXK_NUMPAD_DELETE) && focus == m_recentList) {
            return RemoveRecent();
        }
        event.Skip();
    }

    // ------------------------------------------------------------------
    // Search tab
    // ------------------------------------------------------------------

    // Run a search or listing for the results list on a worker thread.
    void StartResults(Worker work, bool append, const char* doneMessage) {
        int request = ++g_searchRequest, window = m_window;
        RunInBackground([work, request, window, append, doneMessage]() {
            std::vector<YouTubeResult> results;
            std::wstring nextPageToken, error;
            bool ok = work(results, nextPageToken, error);
            RunOnUiThread([=]() {
                YouTubeDialog* dialog = For(window);
                if (!dialog || request != g_searchRequest) return;
                dialog->ResultsArrived(ok, results, nextPageToken, error, append, doneMessage);
            });
        });
    }

    void ResultsArrived(bool ok, const std::vector<YouTubeResult>& results, const std::wstring& nextPageToken,
                        const std::wstring& error, bool append, const char* doneMessage) {
        if (!append) m_view.results.clear();
        m_view.results.insert(m_view.results.end(), results.begin(), results.end());
        m_view.nextPageToken = nextPageToken;
        ShowResults(append ? -1 : 0);

        if (!ok && results.empty()) {
            Speak("Search failed");
            if (!error.empty()) ShowMessage(L"The search failed.\n\n" + error, L"YouTube", MessageIcon::Error);
        } else if (results.empty()) {
            Speak(append ? "No more results" : "No results");
        } else {
            Speak(std::to_string(results.size()) + doneMessage);
        }
    }

    void ShowResults(int select) {
        wxArrayString items;
        for (const auto& result : m_view.results) items.Add(WX(ResultText(result)));
        int keep = m_results->GetSelection();
        m_results->Set(items);
        int index = select >= 0 ? select : keep;
        if (index >= 0 && index < static_cast<int>(items.size())) m_results->SetSelection(index);
        m_loadMore->Enable(!m_view.nextPageToken.empty());
    }

    // Show a channel's or playlist's videos in the results, keeping what was there
    // for Backspace.
    void OpenList(const std::wstring& listUrl, const YouTubeListInfo& info, const char* loading) {
        AddRecentList(listUrl, info);
        m_view.selection = m_results->GetSelection();
        m_history.push_back(m_view);
        m_view = ResultsView();
        m_view.listUrl = listUrl;
        m_view.list = info;
        ShowResults(-1);
        Speak(loading);
        StartResults(
            [listUrl](std::vector<YouTubeResult>& results, std::wstring& next, std::wstring& error) {
                return YouTubeGetListContents(listUrl, results, next, L"", error, SpeakStatus);
            },
            false, " videos");
    }

    void DoSearch() {
        std::wstring query = WS(m_search->GetValue());
        if (query.empty()) return;

        // A YouTube URL: list a playlist or channel, or play a video
        std::wstring id;
        bool isPlaylist, isChannel;
        if (IsYouTubeURL(query) && ParseYouTubeURL(query, id, isPlaylist, isChannel)) {
            if (isPlaylist || isChannel) {
                YouTubeListInfo info;
                if (isPlaylist) {
                    info.kind = YouTubeKind::Playlist;
                    info.id = id;
                }  // a channel may be a @handle: identified if it is added
                m_history.clear();
                OpenList(isPlaylist ? YouTubePlaylistUrl(id) : YouTubeChannelUrl(id), info,
                         isPlaylist ? "Loading playlist" : "Loading channel");
                m_history.clear();
            } else {
                StartPlaying(id);
            }
            return;
        }

        AddYouTubeRecent(YouTubeRecentKind::Search, query, L"", query, L"");
        LoadRecent();
        m_history.clear();
        m_view = ResultsView();
        m_view.query = query;
        ShowResults(-1);
        Speak("Searching");
        StartResults(
            [query](std::vector<YouTubeResult>& results, std::wstring& next, std::wstring& error) {
                return YouTubeSearch(query, results, next, L"", error, SpeakStatus);
            },
            false, " results");
    }

    void LoadMoreResults() {
        if (m_view.nextPageToken.empty()) return;
        Speak("Loading more");
        std::wstring query = m_view.query, listUrl = m_view.listUrl, token = m_view.nextPageToken;
        StartResults(
            [query, listUrl, token](std::vector<YouTubeResult>& results, std::wstring& next, std::wstring& error) {
                if (!listUrl.empty()) return YouTubeGetListContents(listUrl, results, next, token, error, SpeakStatus);
                return YouTubeSearch(query, results, next, token, error, SpeakStatus);
            },
            true, " more loaded");
    }

    // Enter on a result: play a video, open a channel or playlist
    void OpenResult() {
        int sel = m_results->GetSelection();
        if (sel < 0 || sel >= static_cast<int>(m_view.results.size())) return;
        const YouTubeResult& result = m_view.results[sel];
        if (result.kind == YouTubeKind::Video) {
            StartPlaying(result.id);
            return;
        }
        YouTubeListInfo info;
        info.kind = result.kind;
        info.id = result.id;
        info.name = result.title;
        info.channel = result.kind == YouTubeKind::Playlist ? result.channel : L"";
        OpenList(YouTubeListUrl(result.kind, result.id), info,
                 result.kind == YouTubeKind::Channel ? "Loading channel" : "Loading playlist");
    }

    // Download: a video, or a whole playlist
    void DownloadResult() {
        int sel = m_results->GetSelection();
        if (sel < 0 || sel >= static_cast<int>(m_view.results.size())) {
            Speak("Select a video or playlist first");
            return;
        }
        const YouTubeResult& result = m_view.results[sel];
        switch (result.kind) {
            case YouTubeKind::Video:
                YouTubeDownload(L"https://www.youtube.com/watch?v=" + result.id, result.title);
                break;
            case YouTubeKind::Playlist:
                YouTubeDownload(YouTubePlaylistUrl(result.id), L"playlist " + result.title);
                break;
            case YouTubeKind::Channel:
                Speak("Open the channel to download its videos");
                break;
        }
    }

    void DownloadVideo() {
        int sel = m_videos->GetSelection();
        if (sel < 0 || sel >= static_cast<int>(m_videoResults.size())) {
            Speak("Select a video first");
            return;
        }
        const YouTubeResult& result = m_videoResults[sel];
        YouTubeDownload(L"https://www.youtube.com/watch?v=" + result.id, result.title);
    }

    void GoBack() {
        if (m_history.empty()) return;
        ++g_searchRequest;  // drop anything still loading for this view
        m_view = m_history.back();
        m_history.pop_back();
        ShowResults(m_view.selection);
        Speak("Back");
    }

    // Add to Favorites: the selected channel or playlist, a selected video's
    // channel, or with nothing selected, the channel or playlist being listed.
    void AddFavoriteFromResults() {
        int sel = m_results->GetSelection();
        const YouTubeResult* result =
            sel >= 0 && sel < static_cast<int>(m_view.results.size()) ? &m_view.results[sel] : nullptr;

        YouTubeListInfo info;
        std::wstring identifyUrl, videoId;
        if (result && result->kind != YouTubeKind::Video) {
            info.kind = result->kind;
            info.id = result->id;
            info.name = result->title;
            info.channel = result->kind == YouTubeKind::Playlist ? result->channel : L"";
        } else if (result && !result->channelId.empty()) {
            info.kind = YouTubeKind::Channel;
            info.id = result->channelId;
            info.name = result->channel;
        } else if (result && m_view.list.kind == YouTubeKind::Channel && !m_view.list.id.empty()) {
            info = m_view.list;  // a video of the channel being listed
        } else if (result) {
            videoId = result->id;
        } else if (!m_view.listUrl.empty()) {
            if (!m_view.list.id.empty() && !m_view.list.name.empty()) {
                info = m_view.list;
            } else {
                identifyUrl = m_view.listUrl;
            }
        } else {
            Speak("Select a channel, playlist or video first");
            return;
        }

        if (!info.id.empty()) {
            AddFavorite(info);
            return;
        }
        // Not known yet: ask yt-dlp
        Speak("Adding");
        int window = m_window;
        RunInBackground([identifyUrl, videoId, window]() {
            YouTubeListInfo found;
            std::wstring error;
            bool ok = videoId.empty() ? YouTubeIdentify(identifyUrl, found, error, SpeakStatus)
                                      : YouTubeVideoChannel(videoId, found, error, SpeakStatus);
            RunOnUiThread([=]() {
                if (!ok) {
                    Speak("Could not add to favorites");
                    ShowMessage(L"Could not add to favorites.\n\n" + error, L"YouTube", MessageIcon::Error);
                    return;
                }
                if (YouTubeDialog* dialog = For(window)) dialog->AddFavorite(found);
            });
        });
    }

    // ------------------------------------------------------------------
    // Favorites tab
    // ------------------------------------------------------------------

    void AddFavorite(const YouTubeListInfo& info) {
        YouTubeFavoriteKind kind =
            info.kind == YouTubeKind::Playlist ? YouTubeFavoriteKind::Playlist : YouTubeFavoriteKind::Channel;
        bool existed = std::any_of(m_favorites.begin(), m_favorites.end(),
                                   [&](const YouTubeFavorite& f) { return f.youtubeId == info.id; });
        int id = AddYouTubeFavorite(kind, info.id, info.name, info.channel);
        if (id < 0) {
            Speak("Could not add to favorites");
            return;
        }
        std::wstring what = (kind == YouTubeFavoriteKind::Channel ? L"channel " : L"playlist ") + info.name;
        SpeakW(existed ? what + L" is already in favorites" : L"Added " + what + L" to favorites");
        LoadFavorites(id);
        if (!existed) RefreshFavorites(RefreshMode::Quiet, id);
    }

    void LoadFavorites(int selectId = -1) {
        m_favorites = GetYouTubeFavorites();
        ShowFavorites(selectId >= 0 ? selectId : SelectedFavoriteId());
    }

    int SelectedFavoriteId() const {
        int sel = m_favoritesList->GetSelection();
        return sel >= 0 && sel < static_cast<int>(m_favorites.size()) ? m_favorites[sel].id : -1;
    }

    // Sort and show the favorites, keeping `selectId` selected.
    void ShowFavorites(int selectId) {
        if (g_ytFavoritesSort == 1) {
            std::stable_sort(m_favorites.begin(), m_favorites.end(), [](const YouTubeFavorite& a, const YouTubeFavorite& b) {
                return WStrICmp(a.name.c_str(), b.name.c_str()) < 0;
            });
        } else {
            // Newest upload first; not yet known last
            std::stable_sort(m_favorites.begin(), m_favorites.end(), [](const YouTubeFavorite& a, const YouTubeFavorite& b) {
                return a.lastUpload > b.lastUpload;
            });
        }
        wxArrayString items;
        int select = -1;
        for (size_t i = 0; i < m_favorites.size(); i++) {
            items.Add(WX(FavoriteText(m_favorites[i])));
            if (m_favorites[i].id == selectId) select = static_cast<int>(i);
        }
        m_favoritesList->Set(items);
        if (select >= 0) m_favoritesList->SetSelection(select);
    }

    std::wstring FavoriteText(const YouTubeFavorite& favorite) const {
        std::wstring text = favorite.name;
        if (favorite.kind == YouTubeFavoriteKind::Playlist) {
            text += favorite.channel.empty() ? L" (playlist)" : L" (playlist by " + favorite.channel + L")";
        }
        if (favorite.lastUpload > 0) {
            text += L", latest " + Ago(favorite.lastUpload);
        }
        return text;
    }

public:
    // A favorite's latest upload arrived: update its text in place (the order is
    // left alone while the user may be reading the list).
    void UploadArrived(int id, int64_t published) {
        for (size_t i = 0; i < m_favorites.size(); i++) {
            if (m_favorites[i].id != id) continue;
            m_favorites[i].lastUpload = published;
            m_favoritesList->SetString(static_cast<unsigned>(i), WX(FavoriteText(m_favorites[i])));
        }
    }

    // All arrived: put them in order, keeping the selection.
    void RefreshDone() { ShowFavorites(SelectedFavoriteId()); }

private:
    void OpenFavorite() {
        int sel = m_favoritesList->GetSelection();
        if (sel < 0 || sel >= static_cast<int>(m_favorites.size())) return;
        const YouTubeFavorite& favorite = m_favorites[sel];
        YouTubeKind kind =
            favorite.kind == YouTubeFavoriteKind::Playlist ? YouTubeKind::Playlist : YouTubeKind::Channel;
        m_videosUrl = YouTubeListUrl(kind, favorite.youtubeId);
        YouTubeListInfo info;
        info.kind = kind;
        info.id = favorite.youtubeId;
        info.name = favorite.name;
        info.channel = favorite.channel;
        AddRecentList(m_videosUrl, info);
        m_videoResults.clear();
        m_videosToken.clear();
        m_videos->Clear();
        m_moreVideos->Enable(false);
        SpeakW(L"Loading " + favorite.name);
        StartVideos(L"", false);
    }

    void LoadMoreVideos() {
        if (m_videosToken.empty()) return;
        Speak("Loading more");
        StartVideos(m_videosToken, true);
    }

    void StartVideos(const std::wstring& pageToken, bool append) {
        int request = ++g_videosRequest, window = m_window;
        std::wstring listUrl = m_videosUrl;
        RunInBackground([listUrl, pageToken, request, window, append]() {
            std::vector<YouTubeResult> results;
            std::wstring next, error;
            bool ok = YouTubeGetListContents(listUrl, results, next, pageToken, error, SpeakStatus);
            RunOnUiThread([=]() {
                YouTubeDialog* dialog = For(window);
                if (!dialog || request != g_videosRequest) return;
                dialog->VideosArrived(ok, results, next, error, append);
            });
        });
    }

    void VideosArrived(bool ok, const std::vector<YouTubeResult>& results, const std::wstring& next,
                       const std::wstring& error, bool append) {
        size_t first = m_videoResults.size();
        m_videoResults.insert(m_videoResults.end(), results.begin(), results.end());
        m_videosToken = next;
        for (const auto& result : results) m_videos->Append(WX(ResultText(result)));
        m_moreVideos->Enable(!next.empty());

        if (!ok && results.empty()) {
            Speak("Could not load the videos");
            if (!error.empty()) ShowMessage(L"Could not load the videos.\n\n" + error, L"YouTube", MessageIcon::Error);
            return;
        }
        Speak(std::to_string(results.size()) + (append ? " more loaded" : " videos"));
        if (!append && !results.empty()) {
            m_videos->SetSelection(0);
            m_videos->SetFocus();
        } else if (append && first < m_videoResults.size()) {
            m_videos->SetSelection(static_cast<int>(first));
        }
    }

    void PlayVideo() {
        int sel = m_videos->GetSelection();
        if (sel < 0 || sel >= static_cast<int>(m_videoResults.size())) return;
        if (m_videoResults[sel].kind == YouTubeKind::Video) StartPlaying(m_videoResults[sel].id);
    }

    void RemoveFavorite() {
        int sel = m_favoritesList->GetSelection();
        if (sel < 0 || sel >= static_cast<int>(m_favorites.size())) return;
        YouTubeFavorite favorite = m_favorites[sel];
        if (wxMessageBox(WX(L"Remove " + favorite.name + L" from favorites?"), "Remove Favorite",
                         wxYES_NO | wxICON_QUESTION, this) != wxYES) {
            return;
        }
        RemoveYouTubeFavorite(favorite.id);
        LoadFavorites();
        int count = static_cast<int>(m_favorites.size());
        if (count > 0) m_favoritesList->SetSelection(std::min(sel, count - 1));
        m_favoritesList->SetFocus();
        SpeakW(L"Removed " + favorite.name);
    }

    // Import: add the channels (or playlists) in a text file, one per line, a few
    // at a time. It finishes even if the window is closed meanwhile.
    void ImportFavorites() {
        wxFileDialog dlg(this, "Import YouTube Channels", wxEmptyString, wxEmptyString,
                         "Text files (*.txt)|*.txt|All Files (*.*)|*.*", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() != wxID_OK) return;

        auto lines = std::make_shared<std::vector<std::wstring>>();
        if (FILE* f = FileOpen(WS(dlg.GetPath()), "rb")) {
            std::string text;
            char buffer[65536];
            size_t n;
            while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) text.append(buffer, n);
            fclose(f);
            if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
            size_t start = 0;
            while (start <= text.size()) {
                size_t end = text.find('\n', start);
                std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
                size_t first = line.find_first_not_of(" \t\r");
                if (first != std::string::npos && line[first] != '#') lines->push_back(PlaylistLineToWide(line.c_str()));
                if (end == std::string::npos) break;
                start = end + 1;
            }
        }
        if (lines->empty()) {
            Speak("No channels in the file");
            return;
        }
        SpeakW(L"Importing " + std::to_wstring(lines->size()) + (lines->size() == 1 ? L" channel" : L" channels"));

        struct Found {
            bool ok = false;
            YouTubeListInfo info;
            int64_t published = 0;
            std::wstring error;
        };
        auto found = std::make_shared<std::vector<Found>>(lines->size());
        auto next = std::make_shared<std::atomic<size_t>>(0);
        auto done = std::make_shared<std::atomic<size_t>>(0);
        auto running = std::make_shared<std::atomic<int>>(0);
        int window = m_window;
        const int threads = static_cast<int>(std::min<size_t>(4, lines->size()));
        *running = threads;
        for (int t = 0; t < threads; t++) {
            RunInBackground([lines, found, next, done, running, window]() {
                for (size_t i = (*next)++; i < lines->size(); i = (*next)++) {
                    Found& result = (*found)[i];
                    result.ok = YouTubeResolveFavorite((*lines)[i], result.info, result.published, result.error);
                    size_t count = ++*done;
                    if (count % 10 == 0 && count < lines->size()) {
                        std::wstring progress = std::to_wstring(count) + L" of " + std::to_wstring(lines->size());
                        RunOnUiThread([progress]() { SpeakW(progress); });
                    }
                }
                if (--*running == 0) {
                    RunOnUiThread([lines, found, window]() { FinishImport(*lines, *found, window); });
                }
            });
        }
    }

    template <typename FoundList>
    static void FinishImport(const std::vector<std::wstring>& lines, const FoundList& found, int window) {
        std::vector<YouTubeFavorite> before = GetYouTubeFavorites();
        int added = 0, existing = 0;
        std::wstring failures;
        int failed = 0;
        for (size_t i = 0; i < found.size(); i++) {
            const auto& result = found[i];
            if (!result.ok) {
                if (++failed <= 20) failures += L"\n" + lines[i] + L": " + result.error;
                continue;
            }
            bool had = std::any_of(before.begin(), before.end(),
                                   [&](const YouTubeFavorite& f) { return f.youtubeId == result.info.id; });
            YouTubeFavoriteKind kind = result.info.kind == YouTubeKind::Playlist ? YouTubeFavoriteKind::Playlist
                                                                                 : YouTubeFavoriteKind::Channel;
            int id = AddYouTubeFavorite(kind, result.info.id, result.info.name, result.info.channel);
            if (id < 0) {
                if (++failed <= 20) failures += L"\n" + lines[i] + L": could not be saved";
                continue;
            }
            if (result.published > 0) UpdateYouTubeFavoriteUpload(id, result.published);
            if (had) {
                existing++;
            } else {
                added++;
                before.push_back(YouTubeFavorite{id, kind, result.info.id, result.info.name, result.info.channel, 0});
            }
        }

        std::wstring summary = L"Imported " + std::to_wstring(added) + (added == 1 ? L" channel" : L" channels");
        if (existing) summary += L", " + std::to_wstring(existing) + L" already in favorites";
        if (failed) summary += L", " + std::to_wstring(failed) + L" not found";
        SpeakW(summary);
        if (YouTubeDialog* dialog = For(window)) dialog->LoadFavorites();
        if (failed) {
            if (failed > 20) failures += L"\n...";
            ShowMessage(summary + L".\n\nThese lines could not be imported:" + failures, L"Import YouTube Channels",
                        MessageIcon::Warning);
        }
    }

    // ------------------------------------------------------------------
    // Recent tab
    // ------------------------------------------------------------------

    // A channel or playlist was opened: remember it (a pasted channel URL has no
    // name yet, and is listed by its @handle, or else its address)
    void AddRecentList(const std::wstring& listUrl, const YouTubeListInfo& info) {
        bool playlist = info.kind == YouTubeKind::Playlist;
        std::wstring name = info.name;
        size_t handle = listUrl.find(L"/@");
        if (name.empty() && handle != std::wstring::npos) {
            size_t end = listUrl.find_first_of(L"/?", handle + 1);
            name = listUrl.substr(handle + 1, end == std::wstring::npos ? std::wstring::npos : end - handle - 1);
        }
        AddYouTubeRecent(playlist ? YouTubeRecentKind::Playlist : YouTubeRecentKind::Channel, listUrl, info.id,
                         name, playlist ? info.channel : L"");
        LoadRecent();
    }

    void LoadRecent() {
        int keep = m_recentList->GetSelection();
        m_recent = GetYouTubeRecent();
        wxArrayString items;
        for (const auto& item : m_recent) {
            switch (item.kind) {
                case YouTubeRecentKind::Search:
                    items.Add(WX(L"Search: " + item.name));
                    break;
                case YouTubeRecentKind::Channel:
                    items.Add(WX(L"Channel: " + item.name));
                    break;
                case YouTubeRecentKind::Playlist:
                    items.Add(WX(L"Playlist: " + item.name + (item.channel.empty() ? L"" : L" - " + item.channel)));
                    break;
            }
        }
        m_recentList->Set(items);
        int count = static_cast<int>(items.size());
        if (count > 0) m_recentList->SetSelection(keep >= 0 ? std::min(keep, count - 1) : 0);
    }

    // Search or open the selected one again, on the Search tab
    void OpenRecent() {
        int sel = m_recentList->GetSelection();
        if (sel < 0 || sel >= static_cast<int>(m_recent.size())) return;
        YouTubeRecent item = m_recent[sel];
        m_book->SetSelection(0);
        if (item.kind == YouTubeRecentKind::Search) {
            m_search->ChangeValue(WX(item.target));
            DoSearch();
        } else {
            YouTubeListInfo info;
            info.kind = item.kind == YouTubeRecentKind::Playlist ? YouTubeKind::Playlist : YouTubeKind::Channel;
            info.id = item.youtubeId;
            info.name = item.name == item.target ? L"" : item.name;
            info.channel = item.channel;
            m_history.clear();
            OpenList(item.target, info,
                     info.kind == YouTubeKind::Playlist ? "Loading playlist" : "Loading channel");
            m_history.clear();
        }
        m_recentList->SetSelection(0);  // it is the most recent now
        m_results->SetFocus();
    }

    void RemoveRecent() {
        int sel = m_recentList->GetSelection();
        if (sel < 0 || sel >= static_cast<int>(m_recent.size())) return;
        std::wstring name = m_recent[sel].name;
        RemoveYouTubeRecent(m_recent[sel].id);
        LoadRecent();
        int count = static_cast<int>(m_recent.size());
        if (count > 0) m_recentList->SetSelection(std::min(sel, count - 1));
        SpeakW(L"Removed " + name);
    }

    void ClearRecent() {
        if (m_recent.empty()) return;
        if (wxMessageBox("Clear the recently viewed list?", "Clear Recent", wxYES_NO | wxICON_QUESTION, this) != wxYES) {
            return;
        }
        ClearYouTubeRecent();
        LoadRecent();
        Speak("Recent list cleared");
    }

    const int m_window;
    wxNotebook* m_book;

    // Search tab
    wxTextCtrl* m_search;
    wxListBox* m_results;
    wxButton* m_loadMore;
    wxButton* m_addFavorite;
    ResultsView m_view;
    std::vector<ResultsView> m_history;  // for Backspace

    // Favorites tab
    wxChoice* m_sort;
    wxListBox* m_favoritesList;
    wxListBox* m_videos;
    wxButton* m_moreVideos;
    std::vector<YouTubeFavorite> m_favorites;  // in the list's order
    std::vector<YouTubeResult> m_videoResults;
    std::wstring m_videosUrl;
    std::wstring m_videosToken;

    // Recent tab
    wxListBox* m_recentList;
    std::vector<YouTubeRecent> m_recent;  // in the list's order
};

std::atomic<bool> g_refreshing{false};

struct RefreshJob {
    int id;
    YouTubeKind kind;
    std::wstring youtubeId;
    std::wstring name;
    int64_t previous;  // latest upload known before this refresh
    // Filled in by the refresh
    bool ok = false;
    int64_t published = 0;
    std::wstring newestTitle;
};

// What the refresh found, said as the mode asks.
void FinishRefresh(RefreshMode mode, const std::vector<RefreshJob>& jobs) {
    g_refreshing = false;
    if (g_youTubeDialog) g_youTubeDialog->RefreshDone();

    std::vector<const RefreshJob*> fresh;  // uploaded since the last look
    int failed = 0;
    for (const auto& job : jobs) {
        if (!job.ok) failed++;
        else if (job.previous > 0 && job.published > job.previous) fresh.push_back(&job);
    }
    std::wstring found;
    for (size_t i = 0; i < fresh.size() && i < 5; i++) {
        found += L" " + fresh[i]->name + L": " + fresh[i]->newestTitle + L".";
    }
    if (fresh.size() > 5) found += L" And " + std::to_wstring(fresh.size() - 5) + L" more.";

    if (mode == RefreshMode::Manual) {
        std::wstring text = L"Favorites refreshed.";
        if (fresh.empty()) text += L" No new videos.";
        else if (fresh.size() == 1) text += L" 1 new video:" + found;
        else text += L" " + std::to_wstring(fresh.size()) + L" new videos:" + found;
        if (failed) text += L" " + std::to_wstring(failed) + L" could not be checked.";
        SpeakW(text);
    } else if (mode == RefreshMode::Auto && !fresh.empty()) {
        if (fresh.size() == 1) {
            SpeakW(L"New video from " + fresh[0]->name + L": " + fresh[0]->newestTitle);
        } else {
            SpeakW(std::to_wstring(fresh.size()) + L" new YouTube videos:" + found);
        }
    }
}

void RefreshFavorites(RefreshMode mode, int onlyId) {
    if (g_refreshing.exchange(true)) {
        if (mode == RefreshMode::Manual) Speak("Already refreshing");
        return;
    }
    auto jobs = std::make_shared<std::vector<RefreshJob>>();
    for (const auto& f : GetYouTubeFavorites()) {
        if (onlyId >= 0 && f.id != onlyId) continue;
        jobs->push_back({f.id, f.kind == YouTubeFavoriteKind::Playlist ? YouTubeKind::Playlist : YouTubeKind::Channel,
                         f.youtubeId, f.name, f.lastUpload});
    }
    if (jobs->empty()) {
        g_refreshing = false;
        if (mode == RefreshMode::Manual) Speak("No favorites to refresh");
        return;
    }
    if (mode == RefreshMode::Manual) {
        SpeakW(L"Refreshing " + std::to_wstring(jobs->size()) + (jobs->size() == 1 ? L" favorite" : L" favorites"));
    }

    // A few at a time; each result goes into the database (and the window, if
    // open) as it arrives.
    auto next = std::make_shared<std::atomic<size_t>>(0);
    auto running = std::make_shared<std::atomic<int>>(0);
    const int threads = static_cast<int>(std::min<size_t>(4, jobs->size()));
    *running = threads;
    for (int t = 0; t < threads; t++) {
        RunInBackground([jobs, next, running, mode]() {
            for (size_t i = (*next)++; i < jobs->size(); i = (*next)++) {
                RefreshJob& job = (*jobs)[i];
                YouTubeListInfo info;
                job.ok = YouTubeReadFeed(job.kind, job.youtubeId, info, job.published, &job.newestTitle);
                if (job.ok && job.published > 0) {
                    int id = job.id;
                    int64_t published = job.published;
                    RunOnUiThread([id, published]() {
                        UpdateYouTubeFavoriteUpload(id, published);
                        if (g_youTubeDialog) g_youTubeDialog->UploadArrived(id, published);
                    });
                }
            }
            if (--*running == 0) {
                RunOnUiThread([jobs, mode]() { FinishRefresh(mode, *jobs); });
            }
        });
    }
}

// The automatic schedule: minutes between refreshes for each choice in Options
// (Off, At startup, then every 30 minutes up to 8 hours). All but Off also
// refresh shortly after startup.
const int kAutoRefreshMinutes[] = {0, 0, 30, 60, 120, 240, 480};
wxTimer* g_autoRefreshTimer = nullptr;
wxTimer* g_startupRefreshTimer = nullptr;

}  // namespace

void StartYouTubeAutoRefresh(bool atStartup) {
    if (!g_autoRefreshTimer) {
        g_autoRefreshTimer = new wxTimer();
        g_autoRefreshTimer->Bind(wxEVT_TIMER, [](wxTimerEvent&) { RefreshFavorites(RefreshMode::Auto); });
        g_startupRefreshTimer = new wxTimer();
        g_startupRefreshTimer->Bind(wxEVT_TIMER, [](wxTimerEvent&) { RefreshFavorites(RefreshMode::Auto); });
    }
    int choice = std::clamp(g_ytAutoRefresh, 0, 6);
    g_autoRefreshTimer->Stop();
    if (kAutoRefreshMinutes[choice] > 0) g_autoRefreshTimer->Start(kAutoRefreshMinutes[choice] * 60 * 1000);
    // Not at once: startup has enough to do, and the favorites can wait a moment.
    if (atStartup && choice > 0) g_startupRefreshTimer->StartOnce(15000);
}

void StopYouTubeAutoRefresh() {
    delete g_autoRefreshTimer;
    delete g_startupRefreshTimer;
    g_autoRefreshTimer = g_startupRefreshTimer = nullptr;
}

void ShowYouTubeDialog() {
    if (g_youTubeDialog && !g_youTubeDialog->IsBeingDeleted()) {
        // Already open, bring to front
        g_youTubeDialog->Raise();
        return;
    }

    // Owned by the main window, so it is destroyed with it. yt-dlp is fetched on
    // first use if there is none.
    auto* dialog = new YouTubeDialog(GetMainWindow());
    dialog->Show();
}
