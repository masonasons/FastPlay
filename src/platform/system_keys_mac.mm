// System-wide keys on macOS: global hotkeys through Carbon's RegisterEventHotKey
// (which needs no accessibility permission). The media keys and Control Center are
// in now_playing_apple.mm, which the iPhone shares.

#include "system_keys.h"
#include "commands.h"
#include "keycodes.h"
#include "utils.h"

#import <Carbon/Carbon.h>
#import <Foundation/Foundation.h>

#include <map>

namespace {

// Windows virtual key codes and the macOS key in the same place on the keyboard.
struct KeyPair {
    unsigned vk;
    unsigned mac;
};

const KeyPair kKeys[] = {
    {'A', kVK_ANSI_A}, {'B', kVK_ANSI_B}, {'C', kVK_ANSI_C}, {'D', kVK_ANSI_D},
    {'E', kVK_ANSI_E}, {'F', kVK_ANSI_F}, {'G', kVK_ANSI_G}, {'H', kVK_ANSI_H},
    {'I', kVK_ANSI_I}, {'J', kVK_ANSI_J}, {'K', kVK_ANSI_K}, {'L', kVK_ANSI_L},
    {'M', kVK_ANSI_M}, {'N', kVK_ANSI_N}, {'O', kVK_ANSI_O}, {'P', kVK_ANSI_P},
    {'Q', kVK_ANSI_Q}, {'R', kVK_ANSI_R}, {'S', kVK_ANSI_S}, {'T', kVK_ANSI_T},
    {'U', kVK_ANSI_U}, {'V', kVK_ANSI_V}, {'W', kVK_ANSI_W}, {'X', kVK_ANSI_X},
    {'Y', kVK_ANSI_Y}, {'Z', kVK_ANSI_Z},
    {'0', kVK_ANSI_0}, {'1', kVK_ANSI_1}, {'2', kVK_ANSI_2}, {'3', kVK_ANSI_3},
    {'4', kVK_ANSI_4}, {'5', kVK_ANSI_5}, {'6', kVK_ANSI_6}, {'7', kVK_ANSI_7},
    {'8', kVK_ANSI_8}, {'9', kVK_ANSI_9},
    {VK_OEM_1, kVK_ANSI_Semicolon}, {VK_OEM_PLUS, kVK_ANSI_Equal},
    {VK_OEM_COMMA, kVK_ANSI_Comma}, {VK_OEM_MINUS, kVK_ANSI_Minus},
    {VK_OEM_PERIOD, kVK_ANSI_Period}, {VK_OEM_2, kVK_ANSI_Slash},
    {VK_OEM_3, kVK_ANSI_Grave}, {VK_OEM_4, kVK_ANSI_LeftBracket},
    {VK_OEM_5, kVK_ANSI_Backslash}, {VK_OEM_6, kVK_ANSI_RightBracket},
    {VK_OEM_7, kVK_ANSI_Quote}, {VK_OEM_102, kVK_ISO_Section},
    {VK_RETURN, kVK_Return}, {VK_TAB, kVK_Tab}, {VK_SPACE, kVK_Space},
    {VK_BACK, kVK_Delete}, {VK_ESCAPE, kVK_Escape}, {VK_DELETE, kVK_ForwardDelete},
    {VK_INSERT, kVK_Help}, {VK_HOME, kVK_Home}, {VK_END, kVK_End},
    {VK_PRIOR, kVK_PageUp}, {VK_NEXT, kVK_PageDown},
    {VK_LEFT, kVK_LeftArrow}, {VK_RIGHT, kVK_RightArrow},
    {VK_UP, kVK_UpArrow}, {VK_DOWN, kVK_DownArrow},
    {VK_F1, kVK_F1}, {VK_F2, kVK_F2}, {VK_F3, kVK_F3}, {VK_F4, kVK_F4},
    {VK_F5, kVK_F5}, {VK_F6, kVK_F6}, {VK_F7, kVK_F7}, {VK_F8, kVK_F8},
    {VK_F9, kVK_F9}, {VK_F10, kVK_F10}, {VK_F11, kVK_F11}, {VK_F12, kVK_F12},
    {VK_F13, kVK_F13}, {VK_F14, kVK_F14}, {VK_F15, kVK_F15}, {VK_F16, kVK_F16},
    {VK_F17, kVK_F17}, {VK_F18, kVK_F18}, {VK_F19, kVK_F19}, {VK_F20, kVK_F20},
    {VK_NUMPAD0, kVK_ANSI_Keypad0}, {VK_NUMPAD1, kVK_ANSI_Keypad1},
    {VK_NUMPAD2, kVK_ANSI_Keypad2}, {VK_NUMPAD3, kVK_ANSI_Keypad3},
    {VK_NUMPAD4, kVK_ANSI_Keypad4}, {VK_NUMPAD5, kVK_ANSI_Keypad5},
    {VK_NUMPAD6, kVK_ANSI_Keypad6}, {VK_NUMPAD7, kVK_ANSI_Keypad7},
    {VK_NUMPAD8, kVK_ANSI_Keypad8}, {VK_NUMPAD9, kVK_ANSI_Keypad9},
    {VK_MULTIPLY, kVK_ANSI_KeypadMultiply}, {VK_ADD, kVK_ANSI_KeypadPlus},
    {VK_SUBTRACT, kVK_ANSI_KeypadMinus}, {VK_DECIMAL, kVK_ANSI_KeypadDecimal},
    {VK_DIVIDE, kVK_ANSI_KeypadDivide}, {VK_CLEAR, kVK_ANSI_KeypadClear},
};

bool VirtualKeyToMac(unsigned vk, unsigned& mac) {
    for (const auto& key : kKeys) {
        if (key.vk == vk) {
            mac = key.mac;
            return true;
        }
    }
    return false;
}

// Hotkeys registered by FastPlay carry this signature.
const OSType kHotkeySignature = 'FPHK';

void (*g_hotkeyHandler)(int, bool) = nullptr;
std::map<int, EventHotKeyRef> g_hotkeys;
bool g_handlerInstalled = false;

OSStatus OnHotkeyPressed(EventHandlerCallRef, EventRef event, void*) {
    EventHotKeyID hotkeyId;
    if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, nullptr,
                          sizeof(hotkeyId), nullptr, &hotkeyId) != noErr ||
        hotkeyId.signature != kHotkeySignature) {
        return eventNotHandledErr;
    }
    // Carbon delivers hotkeys on the main thread.
    if (g_hotkeyHandler) g_hotkeyHandler(static_cast<int>(hotkeyId.id), GetEventKind(event) == kEventHotKeyPressed);
    return noErr;
}

}  // namespace

unsigned MacKeyCodeToVirtualKey(unsigned macKeyCode) {
    for (const auto& key : kKeys) {
        if (key.mac == macKeyCode) return key.vk;
    }
    return 0;
}

void SetSystemHotkeyHandler(void (*handler)(int id, bool pressed)) {
    g_hotkeyHandler = handler;
}

bool RegisterSystemHotkey(int id, unsigned modifiers, unsigned vk) {
    unsigned macKey;
    if (!VirtualKeyToMac(vk, macKey)) return false;

    if (!g_handlerInstalled) {
        EventTypeSpec eventTypes[] = {{kEventClassKeyboard, kEventHotKeyPressed},
                                      {kEventClassKeyboard, kEventHotKeyReleased}};
        InstallApplicationEventHandler(&OnHotkeyPressed, 2, eventTypes, nullptr, nullptr);
        g_handlerInstalled = true;
    }

    UInt32 macModifiers = 0;
    if (modifiers & MOD_CONTROL) macModifiers |= cmdKey;
    if (modifiers & MOD_ALT) macModifiers |= optionKey;
    if (modifiers & MOD_SHIFT) macModifiers |= shiftKey;
    if (modifiers & MOD_WIN) macModifiers |= controlKey;

    UnregisterSystemHotkey(id);
    EventHotKeyID hotkeyId = {kHotkeySignature, static_cast<UInt32>(id)};
    EventHotKeyRef ref = nullptr;
    if (RegisterEventHotKey(macKey, macModifiers, hotkeyId, GetApplicationEventTarget(), 0, &ref) != noErr) {
        return false;
    }
    g_hotkeys[id] = ref;
    return true;
}

void UnregisterSystemHotkey(int id) {
    auto it = g_hotkeys.find(id);
    if (it == g_hotkeys.end()) return;
    UnregisterEventHotKey(it->second);
    g_hotkeys.erase(it);
}
