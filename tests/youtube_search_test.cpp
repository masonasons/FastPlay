// Standalone regression test, with HTTP mocked so no API key or network is needed.
// On macOS, run from the repository root:
// c++ -std=c++17 -O1 -ffunction-sections -fdata-sections -Iinclude -Iinclude/fastplay \
//   tests/youtube_search_test.cpp src/utils.cpp -Wl,-dead_strip -o /tmp/youtube_search_test
// /tmp/youtube_search_test
// On Linux, replace -Wl,-dead_strip with -Wl,--gc-sections.
// Include the implementation to exercise its private API parser; the linker
// discards the unrelated playback and download functions.
#include "../src/youtube.cpp"

#include <iostream>
#include <stdexcept>

std::wstring g_ytApiKey = L"test-key";

namespace {

std::string responseBody;
std::wstring requestedUrl;

void Check(bool ok, const char* message) {
	if (!ok) throw std::runtime_error(message);
}

std::string Entry(int number, bool snippetFirst, bool longSnippet) {
	std::string n = std::to_string(number);
	std::string id = R"("id":{"kind":"youtube#video","videoId":"video)" + n + R"("})";
	std::string snippet = R"("snippet":{"description":")";
	if (longSnippet) snippet += std::string(3000, 'x');
	snippet += R"(Braces {} and [] and escaped \"quotes\" and backslashes \\, )";
	snippet += R"(","thumbnails":{"default":{"url":"https://example.com/image.jpg"}},)";
	snippet += R"("title":"Title )" + n + R"(","channelTitle":"Channel )" + n;
	snippet += R"(","channelId":"channel)" + n + R"("})";
	return "{" + (snippetFirst ? snippet + "," + id : id + "," + snippet) + "}";
}

void CheckPage(int first, bool snippetFirst, bool longSnippet, const std::wstring& pageToken) {
	responseBody = R"({"nextPageToken":"next-page","items":[)";
	for (int i = 0; i < 25; i++) {
		if (i) responseBody += ",\n";
		responseBody += Entry(first + i, snippetFirst, longSnippet);
	}
	responseBody += "]}";
	std::vector<YouTubeResult> results;
	std::wstring next;
	Check(SearchWithAPI(L"test query", results, next, pageToken), "Search failed");
	Check(results.size() == 25, "A result was skipped or added");
	Check(next == L"next-page", "Next page token was lost");
	Check(requestedUrl.find(L"q=test%20query") != std::wstring::npos, "Query encoding changed");
	if (!pageToken.empty()) {
		Check(requestedUrl.find(L"&pageToken=" + pageToken) != std::wstring::npos, "Page token was not sent");
	}
	for (int i = 0; i < 25; i++) {
		std::wstring n = std::to_wstring(first + i);
		Check(results[i].id == L"video" + n, "Video ID does not match its row");
		Check(results[i].title == L"Title " + n, "Title belongs to a different video");
		Check(results[i].channel == L"Channel " + n, "Channel belongs to a different video");
		Check(results[i].channelId == L"channel" + n, "Channel ID belongs to a different video");
	}
}

void CheckIncompleteEntries() {
	responseBody = "{\"items\":[" + Entry(1, false, false);
	responseBody += R"(,{"id":{"videoId":"missing-title"},"snippet":{"channelTitle":"No title"}})";
	responseBody += R"(,{"snippet":{"title":"Missing ID"}},)" + Entry(3, false, false) + "]}";
	std::vector<YouTubeResult> results;
	std::wstring next;
	Check(SearchWithAPI(L"test", results, next, L""), "Valid entries were lost");
	Check(results.size() == 2, "An incomplete entry borrowed another entry's fields");
	Check(results[0].id == L"video1" && results[0].title == L"Title 1", "First entry mismatched");
	Check(results[1].id == L"video3" && results[1].title == L"Title 3", "Entry after incomplete rows mismatched");
}

void CheckMetadataOutsideResults() {
	responseBody = "{\"items\":[" + Entry(1, false, false);
	responseBody += R"(],"metadata":{"videoId":"outside","title":"Outside"}})";
	std::vector<YouTubeResult> results;
	std::wstring next;
	Check(SearchWithAPI(L"test", results, next, L""), "Result was lost");
	Check(results.size() == 1 && results[0].id == L"video1", "Metadata became an extra result");
}

void CheckEscapes() {
	responseBody = R"({"items":[{"id":{"videoId":"escaped"},"snippet":{"title":"A \"quote\" \\ {} [] \uD83D\uDE00","channelTitle":"Caf\u00e9","channelId":"channel"}}]})";
	std::vector<YouTubeResult> results;
	std::wstring next;
	Check(SearchWithAPI(L"test", results, next, L""), "Escaped title was lost");
	Check(results.size() == 1, "Escaped title changed the entry boundaries");
	Check(results[0].title == Utf8ToWide("A \"quote\" \\ {} [] \xF0\x9F\x98\x80"), "Title escapes changed");
	Check(results[0].channel == Utf8ToWide("Caf\xC3\xA9"), "Channel Unicode escapes changed");
}

void CheckHtmlEntities() {
	// The Data API HTML-escapes snippet text inside its JSON strings
	responseBody = R"({"items":[{"id":{"videoId":"entities"},"snippet":{"title":"Don&#39;t Stop &amp; &quot;Go&quot; &lt;3 &#x1F600; &bogus; AT&T","channelTitle":"Rock &amp; Roll","channelId":"channel"}}]})";
	std::vector<YouTubeResult> results;
	std::wstring next;
	Check(SearchWithAPI(L"test", results, next, L""), "Escaped title was lost");
	Check(results.size() == 1, "Escaped title changed the entry boundaries");
	Check(results[0].title == Utf8ToWide("Don't Stop & \"Go\" <3 \xF0\x9F\x98\x80 &bogus; AT&T"), "HTML entities in title were not decoded");
	Check(results[0].channel == L"Rock & Roll", "HTML entities in channel were not decoded");
}

void CheckEmptyAndTruncatedResponses() {
	for (const char* body : {"", "{}", "{\"items\":[]}", "{\"items\":[{\"id\":{\"videoId\":\"cut-off\"}"}) {
		responseBody = body;
		std::vector<YouTubeResult> results;
		std::wstring next;
		Check(!SearchWithAPI(L"test", results, next, L""), "Empty or truncated response was accepted");
		Check(results.empty(), "Empty or truncated response created results");
	}
}

}  // namespace

HttpResult HttpGet(const std::wstring& url, const HttpOptions&) {
	requestedUrl = url;
	HttpResult result;
	result.body = responseBody;
	return result;
}

int main() {
	try {
		CheckPage(1, false, false, L"");
		CheckPage(1, true, false, L"");
		CheckPage(1, false, true, L"");
		CheckPage(26, true, true, L"page-two");
		CheckIncompleteEntries();
		CheckMetadataOutsideResults();
		CheckEscapes();
		CheckHtmlEntities();
		CheckEmptyAndTruncatedResponses();
		std::cout << "YouTube search regression checks passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
