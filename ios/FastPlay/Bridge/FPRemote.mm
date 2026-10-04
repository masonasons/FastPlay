// Listing and copying over FTP, through FFmpeg: see FPRemote.h.

#import "FPRemote.h"

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

#include <cstdio>
#include <vector>

namespace {

// How long a server may say nothing before it is given up on, in microseconds
const char* kTimeout = "20000000";

NSError* ErrorFor(int code, NSString* what) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    NSString* message = [NSString stringWithFormat:@"%@ %s.", what, text];
    return [NSError errorWithDomain:@"FastPlay.Remote" code:code userInfo:@{NSLocalizedDescriptionKey: message}];
}

}  // namespace

@interface FPRemoteItem ()
- (instancetype)initWithName:(NSString*)name isFolder:(BOOL)isFolder size:(long long)size modified:(NSDate*)modified;
@end

@implementation FPRemoteItem
- (instancetype)initWithName:(NSString*)name isFolder:(BOOL)isFolder size:(long long)size modified:(NSDate*)modified {
    if ((self = [super init])) {
        _name = [name copy];
        _isFolder = isFolder;
        _size = size;
        _modified = modified;
    }
    return self;
}
@end

@implementation FPRemote

+ (NSArray<FPRemoteItem*>*)listFolderAtURL:(NSString*)url error:(NSError**)error {
    AVDictionary* options = nullptr;
    av_dict_set(&options, "timeout", kTimeout, 0);
    AVIODirContext* dir = nullptr;
    int rc = avio_open_dir(&dir, url.UTF8String, &options);
    av_dict_free(&options);
    if (rc < 0) {
        if (error) *error = ErrorFor(rc, @"Could not open the folder:");
        return nil;
    }
    NSMutableArray<FPRemoteItem*>* items = [NSMutableArray array];
    for (;;) {
        AVIODirEntry* entry = nullptr;
        rc = avio_read_dir(dir, &entry);
        if (rc < 0 || !entry) break;
        NSString* name = entry->name ? [NSString stringWithUTF8String:entry->name] : nil;
        if (name.length > 0 && ![name isEqualToString:@"."] && ![name isEqualToString:@".."]) {
            NSDate* modified = entry->modification_timestamp > 0
                ? [NSDate dateWithTimeIntervalSince1970:entry->modification_timestamp / 1000000.0] : nil;
            [items addObject:[[FPRemoteItem alloc] initWithName:name
                                                       isFolder:entry->type == AVIO_ENTRY_DIRECTORY
                                                           size:entry->size > 0 ? entry->size : 0
                                                       modified:modified]];
        }
        avio_free_directory_entry(&entry);
    }
    avio_close_dir(&dir);
    if (rc == AVERROR_EOF) rc = 0;  // how FTP says the listing has ended
    if (rc < 0) {
        if (error) *error = ErrorFor(rc, @"Could not read the folder:");
        return nil;
    }
    return items;
}

+ (BOOL)copyURL:(NSString*)url toFile:(NSString*)path shouldStop:(BOOL (^)(void))shouldStop error:(NSError**)error {
    AVDictionary* options = nullptr;
    av_dict_set(&options, "timeout", kTimeout, 0);
    AVIOContext* source = nullptr;
    int rc = avio_open2(&source, url.UTF8String, AVIO_FLAG_READ, nullptr, &options);
    av_dict_free(&options);
    if (rc < 0) {
        if (error) *error = ErrorFor(rc, @"Could not open the file:");
        return NO;
    }
    FILE* file = fopen(path.fileSystemRepresentation, "wb");
    if (!file) {
        avio_closep(&source);
        if (error) *error = ErrorFor(AVERROR(errno), @"Could not create the file here:");
        return NO;
    }
    std::vector<unsigned char> chunk(256 * 1024);
    BOOL ok = YES;
    for (;;) {
        if (shouldStop()) {
            ok = NO;
            if (error) *error = [NSError errorWithDomain:NSCocoaErrorDomain code:NSUserCancelledError userInfo:nil];
            break;
        }
        const int n = avio_read(source, chunk.data(), static_cast<int>(chunk.size()));
        if (n == AVERROR_EOF || n == 0) break;
        if (n < 0) {
            ok = NO;
            if (error) *error = ErrorFor(n, @"The download stopped:");
            break;
        }
        if (fwrite(chunk.data(), 1, static_cast<size_t>(n), file) != static_cast<size_t>(n)) {
            ok = NO;
            if (error) *error = ErrorFor(AVERROR(errno), @"Could not write the file here:");
            break;
        }
    }
    fclose(file);
    avio_closep(&source);
    if (!ok) remove(path.fileSystemRepresentation);
    return ok;
}

@end
