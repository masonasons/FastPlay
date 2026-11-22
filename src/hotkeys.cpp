#include "hotkeys.h"
#include "ini.h"
#include "globals.h"
#include "keycodes.h"
#include "types.h"
#include <cstdio>

#ifndef _WIN32
// The name of a key, by its Windows virtual key code.
static std::wstring KeyName(unsigned vk) {
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return L"Num " + std::to_wstring(vk - VK_NUMPAD0);
    switch (vk) {
#ifdef __APPLE__
        // As the Mac's keys are labelled
        case VK_BACK: return L"Delete";
        case VK_INSERT: return L"Help";
        case VK_DELETE: return L"Forward Delete";
#else
        case VK_BACK: return L"Backspace";
        case VK_INSERT: return L"Insert";
        case VK_DELETE: return L"Delete";
#endif
        case VK_TAB: return L"Tab";
        case VK_CLEAR: return L"Clear";
        case VK_RETURN: return L"Return";
        case VK_ESCAPE: return L"Escape";
        case VK_SPACE: return L"Space";
        case VK_PRIOR: return L"Page Up";
        case VK_NEXT: return L"Page Down";
        case VK_END: return L"End";
        case VK_HOME: return L"Home";
        case VK_LEFT: return L"Left";
        case VK_UP: return L"Up";
        case VK_RIGHT: return L"Right";
        case VK_DOWN: return L"Down";
        case VK_MULTIPLY: return L"Num *";
        case VK_ADD: return L"Num +";
        case VK_SUBTRACT: return L"Num -";
        case VK_DECIMAL: return L"Num .";
        case VK_DIVIDE: return L"Num /";
        case VK_OEM_1: return L";";
        case VK_OEM_PLUS: return L"=";
        case VK_OEM_COMMA: return L",";
        case VK_OEM_MINUS: return L"-";
        case VK_OEM_PERIOD: return L".";
        case VK_OEM_2: return L"/";
        case VK_OEM_3: return L"`";
        case VK_OEM_4: return L"[";
        case VK_OEM_5: return L"\\";
        case VK_OEM_6: return L"]";
        case VK_OEM_7: return L"'";
        case VK_OEM_102: return L"\u00a7";
    }
    wchar_t buf[16];
    swprintf(buf, 16, L"0x%02X", vk);
    return buf;
}
#endif

// Format hotkey for display (e.g., "Ctrl+Shift+P")
std::wstring FormatHotkey(unsigned modifiers, unsigned vk) {
    std::wstring result;
#ifdef __APPLE__
    // In the order macOS shows them. Stored Ctrl is Command, and Win is Control.
    if (modifiers & MOD_WIN) result += L"Ctrl+";
    if (modifiers & MOD_ALT) result += L"Option+";
    if (modifiers & MOD_SHIFT) result += L"Shift+";
    if (modifiers & MOD_CONTROL) result += L"Cmd+";
#else
    if (modifiers & MOD_CONTROL) result += L"Ctrl+";
    if (modifiers & MOD_ALT) result += L"Alt+";
    if (modifiers & MOD_SHIFT) result += L"Shift+";
#ifdef _WIN32
    if (modifiers & MOD_WIN) result += L"Win+";
#else
    if (modifiers & MOD_WIN) result += L"Super+";
#endif
#endif

#ifdef _WIN32
    // Get key name
    UINT scanCode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG keyParam = static_cast<LONG>(scanCode << 16);
    // Keys on the extended part of the keyboard share scan codes with the numeric
    // keypad; without the extended bit, Up would be named "Num 8".
    switch (vk) {
        case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
        case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
        case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
        case VK_RCONTROL: case VK_RMENU: case VK_LWIN: case VK_RWIN: case VK_APPS:
            keyParam |= 1 << 24;
            break;
    }
    wchar_t keyName[64] = {0};
    GetKeyNameTextW(keyParam, keyName, 64);
    if (keyName[0]) {
        result += keyName;
    } else {
        wchar_t buf[16];
        swprintf(buf, 16, L"0x%02X", vk);
        result += buf;
    }
#else
    result += KeyName(vk);
#endif
    return result;
}

// The action a saved hotkey names: by its key, or in hotkeys saved before keys, by
// its place in the list as it was then. -1 if there is no such action.
static int FindAction(const wchar_t* name) {
    std::wstring key = name;
    if (!key.empty() && key.find_first_not_of(L"0123456789") == std::wstring::npos) {
        int legacy = std::stoi(key);
        if (legacy < 0 || legacy >= g_legacyHotkeyActionCount) return -1;
        key = g_legacyHotkeyActions[legacy];
    }
    for (int i = 0; i < g_hotkeyActionCount; i++) {
        if (key == g_hotkeyActions[i].key) return i;
    }
    return -1;
}

// Load hotkeys from INI file
void LoadHotkeys() {
    g_hotkeysEnabled = IniGetInt(L"Hotkeys", L"Enabled", 1, g_configPath.c_str()) != 0;
    g_hotkeys.clear();
    int count = IniGetInt(L"Hotkeys", L"Count", 0, g_configPath.c_str());

    for (int i = 0; i < count; i++) {
        wchar_t key[32];
        wchar_t value[64] = {0};

        swprintf(key, 32, L"Hotkey%d", i);
        IniGetString(L"Hotkeys", key, L"", value, 64, g_configPath.c_str());

        // Parse "modifiers,vk,action[,global]" (hotkeys saved before local ones
        // existed are all global)
        unsigned mods = 0, vk = 0;
        wchar_t action[64] = {0};
        int global = 1;
		// The l modifier makes the scanset write wchar_t on every platform.
		if (swscanf(value, L"%u,%u,%63l[^,],%d", &mods, &vk, action, &global) >= 3) {
            int actionIdx = FindAction(action);
            if (actionIdx >= 0) {
                GlobalHotkey hk;
                hk.id = g_nextHotkeyId++;
                hk.modifiers = mods;
                hk.vk = vk;
                hk.actionIdx = actionIdx;
                hk.global = global != 0;
                g_hotkeys.push_back(hk);
            }
        }
    }
}

// Save hotkeys to INI file
void SaveHotkeys() {
    wchar_t buf[64];

    IniWriteString(L"Hotkeys", L"Enabled", g_hotkeysEnabled ? L"1" : L"0", g_configPath.c_str());

    swprintf(buf, 64, L"%d", static_cast<int>(g_hotkeys.size()));
    IniWriteString(L"Hotkeys", L"Count", buf, g_configPath.c_str());

    for (size_t i = 0; i < g_hotkeys.size(); i++) {
        wchar_t key[32];
        swprintf(key, 32, L"Hotkey%zu", i);
        swprintf(buf, 64, L"%u,%u,%ls,%d", g_hotkeys[i].modifiers, g_hotkeys[i].vk,
                 g_hotkeyActions[g_hotkeys[i].actionIdx].key, g_hotkeys[i].global ? 1 : 0);
        IniWriteString(L"Hotkeys", key, buf, g_configPath.c_str());
    }
}

