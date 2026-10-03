// Standalone regression test with a fake decoder; no network or audio device.
// On macOS, run from the repository root:
// c++ -std=c++17 -O1 -ffunction-sections -fdata-sections -Isrc -Iinclude -Iinclude/fastplay \
//   tests/youtube_metadata_test.cpp src/playlist_io.cpp src/utils.cpp \
//   -Wl,-dead_strip -o /tmp/youtube_metadata_test
// /tmp/youtube_metadata_test
// On Linux, replace -Wl,-dead_strip with -Wl,--gc-sections.
#include "../src/youtube.cpp"
#include "../src/player.cpp"

#include <fstream>
#include <iostream>
#include <stdexcept>

std::vector<std::wstring> g_playlist;
int g_currentTrack = -1;
bool g_isLiveStream = false;
int g_currentBitrate = 0;

namespace {

void Check(bool ok, const char* message) {
	if (!ok) throw std::runtime_error(message);
}

class FakeDecoder : public audio::Decoder {
public:
	std::map<std::string, std::string> tags;
	std::string streamTitle;
	double length = 0;
	int bitrate = 0;
	int SampleRate() const override { return 48000; }
	int Read(float*, int) override { return 0; }
	bool Seek(double) override { return true; }
	void Abort() override {}
	double Length() const override { return length; }
	bool IsLive() const override { return g_isLiveStream; }
	std::string Tag(const std::string& name) const override {
		auto it = tags.find(name);
		return it == tags.end() ? "" : it->second;
	}
	std::string StreamTitle() const override { return streamTitle; }
	std::vector<Chapter> Chapters() const override { return {}; }
	int Bitrate() const override { return bitrate; }
	bool IsVbr() const override { return false; }
	int SourceChannels() const override { return 2; }
	int SourceSampleRate() const override { return 48000; }
	int SourceBits() const override { return 0; }
	std::string CodecName() const override { return "aac"; }
};

FakeDecoder decoder;
bool loaded = true;
std::wstring spoken;

void CheckMetadata() {
	YouTubeMedia media;
	Check(ReadPreparedMedia("[download] progress\n" + std::string(R"({"title":"Caf\u00e9 \"quotes\" \uD83D\uDE00","url":"https://example.com/audio.m3u8","channel":"Channel","artist":"Artist","album":"Album","year":"2026","track":"2","genre":"Jazz","description":"First line\nSecond\tline with {} [] and \\","duration":"125.5","bitrate":"129.6","live_status":"not_live"})"), media), "Metadata was not parsed");
	Check(media.title == Utf8ToWide("Caf\xc3\xa9 \"quotes\" \xf0\x9f\x98\x80"), "Unicode title was damaged");
	Check(media.channel == L"Channel" && media.url == L"https://example.com/audio.m3u8", "Playback fields were lost");
	media.metadata.sourceUrl = L"https://www.youtube.com/watch?v=test";
	SetTrackMetadata(media.url, media.metadata);
	g_loadedMetadata = GetTrackMetadata(media.url);
	g_playlist = {media.url};
	g_currentTrack = 0;
	decoder.tags["TITLE"] = "Transport title";
	Check(GetTagTitle() == L"Artist - " + media.title, "Title shortcuts did not use YouTube metadata");
	Check(GetTagArtist() == L"Artist", "Artist shortcut failed");
	Check(GetTagAlbum() == L"Album", "Album shortcut failed");
	Check(GetTagYear() == L"2026", "Year shortcut failed");
	Check(GetTagTrack() == L"2", "Track shortcut failed");
	Check(GetTagGenre() == L"Jazz", "Genre shortcut failed");
	Check(GetTagComment() == std::wstring(L"First line Second\tline with {} [] and ") + wchar_t(92), "Description escapes were damaged");
	Check(GetTagBitrate() == L"130 kbps, 48000 Hz, Stereo", "Bitrate fallback failed");
	Check(GetTagDuration() == FormatTime(125.5), "Duration fallback failed");
	Check(GetTagFilename() == media.metadata.sourceUrl, "URL shortcut exposed the transport URL");
	SpeakTagArtist();
	Check(spoken == L"Artist: Artist", "Spoken artist differs from the dialog");
	SpeakTagFilename();
	Check(spoken == L"URL: " + media.metadata.sourceUrl, "Spoken URL differs from the dialog");
	decoder.length = 250;
	decoder.bitrate = 192;
	Check(GetTagDuration() == FormatTime(250), "Decoder duration no longer takes precedence");
	Check(GetCurrentBitrate() == 192, "Decoder bitrate no longer takes precedence");
	decoder.length = 0;
	decoder.bitrate = 0;
	// An HLS decoder may classify unknown-length media as live; known video
	// duration from YouTube should still be readable.
	g_isLiveStream = true;
	Check(GetTagDuration() == FormatTime(125.5), "Known video duration was called a live stream");
	g_isLiveStream = false;
	UnloadCurrent();
	Check(g_loadedMetadata.tags.empty() && g_loadedMetadata.sourceUrl.empty(), "Metadata leaked after unload");
	loaded = true;
	Check(GetTagTitle() == L"Transport title" && GetTagArtist() == L"No artist", "Next track inherited YouTube metadata");
	Check(GetTrackMetadata(media.url).tags.at("ARTIST") == "Artist", "Replay lost the metadata");
	Check(GetTrackMetadata(L"unrelated.m4a").tags.empty(), "Another path inherited metadata");
	decoder.tags.clear();
}

void CheckMissingAndLiveFields() {
	YouTubeMedia media;
	Check(ReadPreparedMedia(R"({"title":"Video","channel":"Uploader","artist":"Uploader","album":null,"year":"","track":"","genre":null,"description":null,"duration":"","bitrate":""})", media), "Missing fields rejected a playable video");
	g_loadedMetadata = media.metadata;
	Check(GetTagArtist() == L"Uploader", "Channel fallback was lost");
	Check(GetTagAlbum() == L"No album" && GetTagYear() == L"No year" && GetTagTrack() == L"No track" &&
		GetTagGenre() == L"No genre" && GetTagComment() == L"No comment", "Absent fields became invented tags");
	Check(ReadPreparedMedia(R"({"title":"Live","duration":"9999","live_status":"is_live"})", media), "Live video rejected");
	Check(media.metadata.duration == 0 && media.metadata.bitrate == 0, "Live or missing numeric values were reused");
	g_loadedMetadata = media.metadata;
	g_isLiveStream = true;
	Check(GetTagDuration() == L"Live stream", "Live duration was invented");
	g_isLiveStream = false;
	for (const char* body : {"", "{}", "{\"title\":\"truncated\"", "{\"title\":null}"}) {
		Check(!ReadPreparedMedia(body, media), "Incomplete metadata was accepted");
	}
	g_loadedMetadata = TrackMetadata();
	decoder.streamTitle = "Radio artist - Radio song";
	Check(GetTagArtist() == L"Radio artist" && GetTagTitle() == L"Radio artist - Radio song", "Radio metadata regressed");
	decoder.streamTitle.clear();
	loaded = false;
	Check(GetTagArtist() == L"Nothing playing", "Unloaded playback still exposed metadata");
}

}  // namespace

namespace audio {
const Decoder* Current() { return loaded ? &decoder : nullptr; }
bool IsLoaded() { return loaded; }
double Length() { return decoder.length; }
void Unload() { loaded = false; }
}

void Speak(const char* text, bool) { spoken = Utf8ToWide(text); }
void SpeakW(const std::wstring& text, bool) { spoken = text; }
void SaveFilePosition(const std::wstring&) {}
void RemoveDSPEffects() {}

int main(int argc, char** argv) {
	try {
		CheckMetadata();
		CheckMissingAndLiveFields();
		// Optional: parse real yt-dlp template output generated from local fixtures.
		if (argc > 1) {
			std::ifstream input(argv[1]);
			Check(input.good(), "Could not open template fixture");
			std::string line;
			int count = 0;
			while (std::getline(input, line)) {
				YouTubeMedia media;
				Check(ReadPreparedMedia(line, media), "Real yt-dlp template output failed to parse");
				Check(media.metadata.tags.at("ARTIST") == "Expected artist", "Template artist fallback failed");
				Check(media.metadata.tags.at("DATE") == "2026", "Template year fallback failed");
				count++;
			}
			Check(count == 3, "Template fixtures were missing");
		}
		std::cout << "YouTube metadata regression checks passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
