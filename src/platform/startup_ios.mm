#include "platform.h"

#import <UIKit/UIKit.h>

void PlatformStartup() {
    // Nothing to do: everything FastPlay uses is linked into the app.
}

std::string GetSystemDescription() {
    // "iOS 17.5; arm64" ("iPadOS" on an iPad)
    std::string name = UIDevice.currentDevice.systemName.UTF8String ?: "iOS";
    if (NSString* version = UIDevice.currentDevice.systemVersion) {
        name += " ";
        name += version.UTF8String;
    }
#if defined(__arm64__) || defined(__aarch64__)
    return name + "; arm64";
#else
    return name + "; x86_64";
#endif
}
