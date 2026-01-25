#include "tray.h"
#include "globals.h"
#include "resource.h"
#include <shellapi.h>

void CreateTrayIcon(HWND hwnd) {
    if (g_trayIconVisible) return;

    g_trayIcon.cbSize = sizeof(NOTIFYICONDATAW);
    g_trayIcon.hWnd = hwnd;
    g_trayIcon.uID = 1;
    g_trayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_trayIcon.uCallbackMessage = WM_TRAYICON;
    g_trayIcon.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(g_trayIcon.szTip, APP_NAME);

    Shell_NotifyIconW(NIM_ADD, &g_trayIcon);
    g_trayIconVisible = true;
}

void RemoveTrayIcon() {
    if (!g_trayIconVisible) return;

    Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
    g_trayIconVisible = false;
}

void ShowTrayMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_RESTORE, L"&Restore");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_PLAY_PLAYPAUSE, L"&Play/Pause");
    AppendMenuW(hMenu, MF_STRING, IDM_PLAY_STOP, L"&Stop");
    AppendMenuW(hMenu, MF_STRING, IDM_PLAY_PREV, L"P&revious");
    AppendMenuW(hMenu, MF_STRING, IDM_PLAY_NEXT, L"&Next");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, L"E&xit");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessage(hwnd, WM_NULL, 0, 0);

    DestroyMenu(hMenu);
}

void HideToTray(HWND hwnd) {
    CreateTrayIcon(hwnd);
    ShowWindow(hwnd, SW_HIDE);
}

void RestoreFromTray(HWND hwnd) {
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }
}

void ToggleWindow(HWND hwnd) {
    if (IsWindowVisible(hwnd)) {
        HideToTray(hwnd);
    } else {
        RestoreFromTray(hwnd);
    }
}
