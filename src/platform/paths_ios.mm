// Where things are on iOS. Everything is inside the app's container: the Documents
// folder is the one the Files app shows as FastPlay's ("On My iPhone > FastPlay"),
// so it stands in for the Music and Downloads folders of a computer; settings and
// the database are in Application Support, out of sight.

#include "paths.h"
#include "utils.h"

#import <Foundation/Foundation.h>

#include <string>

const wchar_t kPathSeparator = L'/';

namespace {

std::wstring FolderFor(NSSearchPathDirectory directory) {
    NSString* path = NSSearchPathForDirectoriesInDomains(directory, NSUserDomainMask, YES).firstObject;
    return path ? Utf8ToWide(path.UTF8String) : std::wstring(L"/tmp");
}

}  // namespace

std::wstring GetExecutableDir() {
    NSString* path = NSBundle.mainBundle.bundlePath;
    return Utf8ToWide(path ? path.UTF8String : ".") + L"/";
}

bool IsInstalledMode() {
    return true;
}

std::wstring GetDataDirectory() {
    NSString* dir = [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject
        stringByAppendingPathComponent:@"FastPlay"];
    if (!dir) return L"/tmp/";
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    return Utf8ToWide(dir.UTF8String) + L"/";
}

std::wstring GetUserMusicDir() {
    return FolderFor(NSDocumentDirectory);
}

std::wstring GetUserDownloadsDir() {
    return FolderFor(NSDocumentDirectory);
}

std::wstring GetTempDir() {
    std::string dir = NSTemporaryDirectory().UTF8String ?: "/tmp/";
    if (dir.empty() || dir.back() != '/') dir += '/';
    return Utf8ToWide(dir);
}

// Everything is linked into the app.
std::wstring GetLibraryDir() {
    return L"";
}
