#pragma once
#ifndef FASTPLAY_VERSION_H
#define FASTPLAY_VERSION_H

#define APP_VERSION "0.5.1"
#define APP_VERSION_MAJOR 0
#define APP_VERSION_MINOR 5
#define APP_VERSION_PATCH 1

// This will be set during build from git commit
#ifndef BUILD_COMMIT
#define BUILD_COMMIT ""
#endif

#define GITHUB_REPO "masonasons/FastPlay"
#define GITHUB_API_URL "https://api.github.com/repos/masonasons/FastPlay/releases"

#endif // FASTPLAY_VERSION_H
