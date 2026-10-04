// HTTP through NSURLSession (iOS has no libcurl of its own).

#include "http.h"
#include "utils.h"

#import <Foundation/Foundation.h>

#include <cstdio>

namespace {
const int kMaxRedirects = 5;  // six requests in all, as on the other systems
}

// One request, run to its end on NSURLSession's queue while HttpGet() waits.
@interface FPHttpTransfer : NSObject <NSURLSessionDataDelegate>
@end

@implementation FPHttpTransfer {
@public
    const HttpOptions* _options;
    HttpResult* _result;
    FILE* _file;
    int _redirects;
    uint64_t _total;
    dispatch_semaphore_t _done;
}

- (void)URLSession:(NSURLSession*)session
                          task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response
                    newRequest:(NSURLRequest*)request
             completionHandler:(void (^)(NSURLRequest*))completionHandler {
    if (!_options->followRedirects || ++_redirects > kMaxRedirects) {
        completionHandler(nil);  // the redirect itself is the answer
        return;
    }
    // The headers go along (the user agent, and credentials for a protected feed,
    // which NSURLSession drops when the host changes)
    NSMutableURLRequest* next = [request mutableCopy];
    [task.originalRequest.allHTTPHeaderFields enumerateKeysAndObjectsUsingBlock:^(NSString* name, NSString* value, BOOL*) {
        if (![next valueForHTTPHeaderField:name]) [next setValue:value forHTTPHeaderField:name];
    }];
    completionHandler(next);
}

- (void)URLSession:(NSURLSession*)session
              dataTask:(NSURLSessionDataTask*)dataTask
    didReceiveResponse:(NSURLResponse*)response
     completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    long long expected = response.expectedContentLength;
    _total = expected > 0 ? static_cast<uint64_t>(expected) : 0;
    // Only the headers were wanted: stop here, with the answer complete.
    completionHandler(_options->readBody ? NSURLSessionResponseAllow : NSURLSessionResponseCancel);
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)dataTask didReceiveData:(NSData*)data {
    [data enumerateByteRangesUsingBlock:^(const void* bytes, NSRange range, BOOL* stop) {
        if (self->_file) {
            if (fwrite(bytes, 1, range.length, self->_file) != range.length) {
                [dataTask cancel];
                *stop = YES;
                return;
            }
        } else {
            self->_result->body.append(static_cast<const char*>(bytes), range.length);
        }
        self->_result->bytesReceived += range.length;
    }];
    if (_options->progress && !_options->progress(_result->bytesReceived, _total)) {
        _result->cancelled = true;
        [dataTask cancel];
    }
}

- (void)URLSession:(NSURLSession*)session
                    task:(NSURLSessionTask*)task
     didReceiveChallenge:(NSURLAuthenticationChallenge*)challenge
       completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition, NSURLCredential*))completionHandler {
    // Basic went up front; a server asking again (Digest, say) gets the credentials once.
    if (challenge.previousFailureCount == 0 && (!_options->username.empty() || !_options->password.empty())) {
        NSURLCredential* credential =
            [NSURLCredential credentialWithUser:[NSString stringWithUTF8String:WideToUtf8(_options->username).c_str()]
                                       password:[NSString stringWithUTF8String:WideToUtf8(_options->password).c_str()]
                                    persistence:NSURLCredentialPersistenceNone];
        completionHandler(NSURLSessionAuthChallengeUseCredential, credential);
        return;
    }
    completionHandler(NSURLSessionAuthChallengePerformDefaultHandling, nil);
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    NSHTTPURLResponse* response = [task.response isKindOfClass:[NSHTTPURLResponse class]]
        ? static_cast<NSHTTPURLResponse*>(task.response) : nil;
    // Stopping on purpose after the headers is still a complete answer.
    const bool stoppedAtHeaders = !_options->readBody && response != nil;
    if ((!error || stoppedAtHeaders) && response && !_result->cancelled) {
        _result->completed = true;
        _result->status = static_cast<unsigned long>(response.statusCode);
        NSString* finalUrl = response.URL.absoluteString;
        if (finalUrl) _result->finalUrl = Utf8ToWide(finalUrl.UTF8String);
    } else if (!_result->cancelled) {
        _result->systemError = static_cast<unsigned long>(error ? error.code : -1);
        _result->errorText = Utf8ToWide((error.localizedDescription ?: @"The request failed").UTF8String);
    }
    dispatch_semaphore_signal(_done);
}

@end

HttpResult HttpGet(const std::wstring& url, const HttpOptions& options) {
    HttpResult result;
    @autoreleasepool {
        NSString* text = [NSString stringWithUTF8String:WideToUtf8(url).c_str()];
        NSURL* target = text ? [NSURL URLWithString:text] : nil;
        if (!target) {
            result.errorText = L"The address is not valid";
            return result;
        }

        FPHttpTransfer* transfer = [FPHttpTransfer new];
        transfer->_options = &options;
        transfer->_result = &result;
        transfer->_file = nullptr;
        transfer->_redirects = 0;
        transfer->_total = 0;
        transfer->_done = dispatch_semaphore_create(0);
        if (!options.saveTo.empty() && options.readBody) {
            transfer->_file = FileOpen(options.saveTo, "wb");
            if (!transfer->_file) {
                result.errorText = L"Could not create the file";
                return result;
            }
        }

        NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:target];
        [request setValue:[NSString stringWithUTF8String:UserAgent().c_str()] forHTTPHeaderField:@"User-Agent"];
        auto addHeader = [&](const std::wstring& header) {
            NSString* line = [NSString stringWithUTF8String:WideToUtf8(header).c_str()];
            NSRange colon = [line rangeOfString:@":"];
            if (!line || colon.location == NSNotFound) return;
            NSString* name = [[line substringToIndex:colon.location]
                stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
            NSString* value = [[line substringFromIndex:colon.location + 1]
                stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
            [request setValue:value forHTTPHeaderField:name];
        };
        for (const auto& header : options.headers) addHeader(header);
        if (!options.username.empty() || !options.password.empty()) {
            addHeader(BuildBasicAuthHeader(options.username, options.password));
        }

        NSURLSessionConfiguration* config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
        if (options.timeoutMs) {
            // No data for this long abandons the request
            config.timeoutIntervalForRequest = options.timeoutMs / 1000.0;
        }
        NSURLSession* session = [NSURLSession sessionWithConfiguration:config delegate:transfer delegateQueue:nil];
        [[session dataTaskWithRequest:request] resume];
        dispatch_semaphore_wait(transfer->_done, DISPATCH_TIME_FOREVER);
        [session finishTasksAndInvalidate];

        if (transfer->_file) fclose(transfer->_file);
        if (result.completed && result.finalUrl.empty()) result.finalUrl = url;
    }
    return result;
}
