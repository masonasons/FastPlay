#include "hotkeys.h"
#include "globals.h"
#include "types.h"
#include "resource.h"
#include <commctrl.h>
#include <cstdio>

// Format hotkey for display (e.g., "Ctrl+Shift+P")
std::wstring FormatHotkey(UINT modifiers, UINT vk) {
    std::wstring result;
    if (modifiers & MOD_CONTROL) result += L"Ctrl+";
    if (modifiers & MOD_ALT) result += L"Alt+";
    if (modifiers & MOD_SHIFT) result += L"Shift+";
    if (modifiers & MOD_WIN) result += L"Win+";

    // Get key name
    UINT scanCode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    wchar_t keyName[64] = {0};
    GetKeyNameTextW(scanCode << 16, keyName, 64);
    if (keyName[0]) {
        result += keyName;
    } else {
        wchar_t buf[16];
        swprintf(buf, 16, L"0x%02X", vk);
        result += buf;
    }
    return result;
}

// Media key hotkey IDs (use high values to avoid conflicts)
#define HOTKEY_ID_MEDIA_PLAYPAUSE   0x7F00
#define HOTKEY_ID_MEDIA_STOP        0x7F01
#define HOTKEY_ID_MEDIA_PREV        0x7F02
#define HOTKEY_ID_MEDIA_NEXT        0x7F03

// Register all global hotkeys
void RegisterGlobalHotkeys() {
    if (!g_hwnd) return;

    // Always register media keys (no modifiers needed)
    RegisterHotKey(g_hwnd, HOTKEY_ID_MEDIA_PLAYPAUSE, 0, VK_MEDIA_PLAY_PAUSE);
    RegisterHotKey(g_hwnd, HOTKEY_ID_MEDIA_STOP, 0, VK_MEDIA_STOP);
    RegisterHotKey(g_hwnd, HOTKEY_ID_MEDIA_PREV, 0, VK_MEDIA_PREV_TRACK);
    RegisterHotKey(g_hwnd, HOTKEY_ID_MEDIA_NEXT, 0, VK_MEDIA_NEXT_TRACK);

    // Register user-defined hotkeys
    if (!g_hotkeysEnabled) return;
    for (const auto& hk : g_hotkeys) {
        RegisterHotKey(g_hwnd, hk.id, hk.modifiers, hk.vk);
    }
}

// Unregister all global hotkeys
void UnregisterGlobalHotkeys() {
    if (!g_hwnd) return;

    // Unregister media keys
    UnregisterHotKey(g_hwnd, HOTKEY_ID_MEDIA_PLAYPAUSE);
    UnregisterHotKey(g_hwnd, HOTKEY_ID_MEDIA_STOP);
    UnregisterHotKey(g_hwnd, HOTKEY_ID_MEDIA_PREV);
    UnregisterHotKey(g_hwnd, HOTKEY_ID_MEDIA_NEXT);

    // Unregister user-defined hotkeys
    for (const auto& hk : g_hotkeys) {
        UnregisterHotKey(g_hwnd, hk.id);
    }
}

// Load hotkeys from INI file
void LoadHotkeys() {
    g_hotkeysEnabled = GetPrivateProfileIntW(L"Hotkeys", L"Enabled", 1, g_configPath.c_str()) != 0;
    g_hotkeys.clear();
    int count = GetPrivateProfileIntW(L"Hotkeys", L"Count", 0, g_configPath.c_str());

    for (int i = 0; i < count; i++) {
        wchar_t key[32];
        wchar_t value[64] = {0};

        swprintf(key, 32, L"Hotkey%d", i);
        GetPrivateProfileStringW(L"Hotkeys", key, L"", value, 64, g_configPath.c_str());

        // Parse "modifiers,vk,actionIdx"
        UINT mods = 0, vk = 0;
        int actionIdx = 0;
        if (swscanf(value, L"%u,%u,%d", &mods, &vk, &actionIdx) == 3) {
            if (actionIdx >= 0 && actionIdx < g_hotkeyActionCount) {
                GlobalHotkey hk;
                hk.id = g_nextHotkeyId++;
                hk.modifiers = mods;
                hk.vk = vk;
                hk.actionIdx = actionIdx;
                g_hotkeys.push_back(hk);
            }
        }
    }
}

// Save hotkeys to INI file
void SaveHotkeys() {
    wchar_t buf[64];

    WritePrivateProfileStringW(L"Hotkeys", L"Enabled", g_hotkeysEnabled ? L"1" : L"0", g_configPath.c_str());

    swprintf(buf, 64, L"%d", static_cast<int>(g_hotkeys.size()));
    WritePrivateProfileStringW(L"Hotkeys", L"Count", buf, g_configPath.c_str());

    for (size_t i = 0; i < g_hotkeys.size(); i++) {
        wchar_t key[32];
        swprintf(key, 32, L"Hotkey%zu", i);
        swprintf(buf, 64, L"%u,%u,%d", g_hotkeys[i].modifiers, g_hotkeys[i].vk, g_hotkeys[i].actionIdx);
        WritePrivateProfileStringW(L"Hotkeys", key, buf, g_configPath.c_str());
    }
}

// Hotkey assignment dialog procedure
INT_PTR CALLBACK HotkeyDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HotkeyDlgData* data = nullptr;

    switch (msg) {
        case WM_INITDIALOG: {
            data = reinterpret_cast<HotkeyDlgData*>(lParam);

            // Populate action combo box
            HWND hCombo = GetDlgItem(hwnd, IDC_HOTKEY_ACTION);
            for (int i = 0; i < g_hotkeyActionCount; i++) {
                SendMessageW(hCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(g_hotkeyActions[i].name));
            }
            SendMessageW(hCombo, CB_SETCURSEL, data->actionIdx, 0);

            // Set hotkey control
            if (data->vk != 0) {
                WORD hkCode = static_cast<WORD>(data->vk);
                if (data->modifiers & MOD_SHIFT) hkCode |= HOTKEYF_SHIFT << 8;
                if (data->modifiers & MOD_CONTROL) hkCode |= HOTKEYF_CONTROL << 8;
                if (data->modifiers & MOD_ALT) hkCode |= HOTKEYF_ALT << 8;
                SendDlgItemMessageW(hwnd, IDC_HOTKEY_KEY, HKM_SETHOTKEY, hkCode, 0);
            }

            // Set dialog title
            SetWindowTextW(hwnd, data->isEdit ? L"Edit Global Hotkey" : L"Add Global Hotkey");

            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK: {
                    // Get selected action
                    data->actionIdx = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_HOTKEY_ACTION, CB_GETCURSEL, 0, 0));

                    // Get hotkey
                    WORD hk = static_cast<WORD>(SendDlgItemMessageW(hwnd, IDC_HOTKEY_KEY, HKM_GETHOTKEY, 0, 0));
                    data->vk = LOBYTE(hk);
                    BYTE mods = HIBYTE(hk);

                    data->modifiers = 0;
                    if (mods & HOTKEYF_SHIFT) data->modifiers |= MOD_SHIFT;
                    if (mods & HOTKEYF_CONTROL) data->modifiers |= MOD_CONTROL;
                    if (mods & HOTKEYF_ALT) data->modifiers |= MOD_ALT;

                    // Require at least one modifier for global hotkeys
                    if (data->vk == 0) {
                        MessageBoxW(hwnd, L"Please enter a hotkey.", L"Error", MB_ICONWARNING);
                        return TRUE;
                    }
                    if (data->modifiers == 0) {
                        MessageBoxW(hwnd, L"Global hotkeys require at least one modifier key (Ctrl, Alt, or Shift).", L"Error", MB_ICONWARNING);
                        return TRUE;
                    }

                    EndDialog(hwnd, IDOK);
                    return TRUE;
                }

                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;
    }

    return FALSE;
}
