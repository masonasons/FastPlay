// The arrow keys as the keyboard has them (system_keys.h): a low-level keyboard hook
// on a thread of its own, which only looks. It never holds up a key or keeps one
// from anyone, and comes off when FastPlay's window is no longer in front.

#include "system_keys.h"

#include <windows.h>

#include <atomic>
#include <future>
#include <thread>

namespace {

std::atomic<bool> g_down[2];
std::atomic<ULONGLONG> g_lastDown[2];  // GetTickCount64() at its last press or repeat
std::thread g_thread;
DWORD g_threadId = 0;

int ArrowIndex(DWORD vk) { return vk == VK_LEFT ? 0 : vk == VK_RIGHT ? 1 : -1; }

LRESULT CALLBACK ArrowHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        const int i = ArrowIndex(key->vkCode);
        // The keyboard's own, not what a screen reader sends on
        if (i >= 0 && !(key->flags & LLKHF_INJECTED)) {
            const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
            if (down) g_lastDown[i] = GetTickCount64();
            g_down[i] = down;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

}  // namespace

void StartWatchingArrows() {
    if (g_thread.joinable()) return;
    std::promise<DWORD> started;
    std::future<DWORD> threadId = started.get_future();
    g_thread = std::thread([&started] {
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);  // its queue, for WM_QUIT
        HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, ArrowHook, GetModuleHandleW(nullptr), 0);
        started.set_value(GetCurrentThreadId());
        // The hook is called on this thread, while it waits here
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        }
        if (hook) UnhookWindowsHookEx(hook);
    });
    g_threadId = threadId.get();
}

void StopWatchingArrows() {
    if (!g_thread.joinable()) return;
    PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
    g_thread.join();
    g_down[0] = false;
    g_down[1] = false;
}

bool ArrowHeld(unsigned vk) {
    const int i = ArrowIndex(vk);
    if (i < 0 || !g_down[i]) return false;
    // Held, the key repeats. Once it has gone longer than the keyboard waits before
    // repeating, it came up where the hook did not see it (a hook ahead of this one
    // kept it).
    UINT delay = 1;  // 0 to 3: 250 to 1000 ms
    SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &delay, 0);
    return GetTickCount64() - g_lastDown[i] < (delay + 1) * 250ULL + 250;
}
