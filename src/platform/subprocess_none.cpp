// Running helper programs where a program may not start another (iOS): nothing
// runs, and what needs a helper (yt-dlp, for YouTube) is not offered there.

#include "subprocess.h"

bool RunProcessCapture(const std::wstring&, const std::vector<std::wstring>&, std::string& output,
                       std::string* errors, int* exitCode, const ProcessOptions&) {
    output.clear();
    if (errors) errors->clear();
    if (exitCode) *exitCode = -1;
    return false;
}
