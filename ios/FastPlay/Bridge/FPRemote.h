// Files on a server FFmpeg can reach by an address (FTP): listing a folder and
// copying a file down. Both block until done, so they belong off the main thread.

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface FPRemoteItem : NSObject
@property (nonatomic, readonly, copy) NSString* name;
@property (nonatomic, readonly) BOOL isFolder;
@property (nonatomic, readonly) long long size;
@property (nonatomic, readonly, nullable) NSDate* modified;
@end

@interface FPRemote : NSObject

// What is in the folder at `url` (which ends with a slash), or nil with `error` set.
+ (nullable NSArray<FPRemoteItem*>*)listFolderAtURL:(NSString*)url error:(NSError**)error
    NS_SWIFT_NAME(listFolder(atURL:));

// Copies the file at `url` to `path`. `shouldStop` is asked now and then; true
// gives up, leaving nothing behind.
+ (BOOL)copyURL:(NSString*)url toFile:(NSString*)path shouldStop:(BOOL (^)(void))shouldStop error:(NSError**)error
    NS_SWIFT_NAME(copy(url:toFile:shouldStop:));

@end

NS_ASSUME_NONNULL_END
