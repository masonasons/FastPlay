#pragma once
#ifndef FASTPLAY_VERSION_H
#define FASTPLAY_VERSION_H

#define APP_VERSION "0.7.0"
#define APP_VERSION_MAJOR 0
#define APP_VERSION_MINOR 7
#define APP_VERSION_PATCH 0

// This will be set during build from git commit
#ifndef BUILD_COMMIT
#define BUILD_COMMIT ""
#endif

#define GITHUB_REPO "masonasons/FastPlay"
// FastPlay's own release, by its tag: the repository has others (the engine's,
// engine-latest), and the newest of all is not always FastPlay's
#define GITHUB_API_URL "https://api.github.com/repos/masonasons/FastPlay/releases/tags/latest"

#endif // FASTPLAY_VERSION_H
