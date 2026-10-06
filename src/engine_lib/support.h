#pragma once

#include <functional>
#include <string>

// The library's side of what the app usually provides (support.cpp).
namespace fpe {

// The thread events are delivered on: everything posted runs there in order.
void StartEventThread();
void StopEventThread();
void Post(std::function<void()> fn);
// Waits until everything posted so far has run (at once on the event thread itself).
void Flush();

// Where yt-dlp, deno and cookies are kept (GetDataDirectory()).
void SetDataDirectory(const std::wstring& dir);

}  // namespace fpe
