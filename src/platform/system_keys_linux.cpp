// Global hotkeys on Linux: key grabs on the X server's root window, seen by a GDK
// event filter on the UI thread. Wayland lets no program take keys from the
// others, so there a hotkey cannot be registered (and the main window's own keys
// are all FastPlay has); the media keys come through MPRIS (now_playing_linux.cpp)
// either way.
//
// Hotkeys are stored as Windows virtual key codes, which name keys by where they
// are; here a key is the X keysym its key gives without Shift, on the first layout.

#include "system_keys.h"
#include "keycodes.h"

#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <map>

namespace {

struct KeyMapping {
    unsigned vk;
    unsigned keysym;
};

const KeyMapping kKeys[] = {
    {VK_BACK, XK_BackSpace}, {VK_TAB, XK_Tab}, {VK_CLEAR, XK_Clear}, {VK_RETURN, XK_Return},
    {VK_PAUSE, XK_Pause}, {VK_ESCAPE, XK_Escape}, {VK_SPACE, XK_space}, {VK_PRIOR, XK_Prior},
    {VK_NEXT, XK_Next}, {VK_END, XK_End}, {VK_HOME, XK_Home}, {VK_LEFT, XK_Left}, {VK_UP, XK_Up},
    {VK_RIGHT, XK_Right}, {VK_DOWN, XK_Down}, {VK_SNAPSHOT, XK_Print}, {VK_INSERT, XK_Insert},
    {VK_DELETE, XK_Delete}, {VK_HELP, XK_Help},
    {VK_MULTIPLY, XK_KP_Multiply}, {VK_ADD, XK_KP_Add}, {VK_SUBTRACT, XK_KP_Subtract},
    {VK_DECIMAL, XK_KP_Decimal}, {VK_DIVIDE, XK_KP_Divide},
    {VK_OEM_1, XK_semicolon}, {VK_OEM_PLUS, XK_equal}, {VK_OEM_COMMA, XK_comma}, {VK_OEM_MINUS, XK_minus},
    {VK_OEM_PERIOD, XK_period}, {VK_OEM_2, XK_slash}, {VK_OEM_3, XK_grave}, {VK_OEM_4, XK_bracketleft},
    {VK_OEM_5, XK_backslash}, {VK_OEM_6, XK_bracketright}, {VK_OEM_7, XK_apostrophe}, {VK_OEM_102, XK_less},
};

unsigned VirtualKeyToKeysym(unsigned vk) {
    if (vk >= 'A' && vk <= 'Z') return XK_a + (vk - 'A');
    if (vk >= '0' && vk <= '9') return XK_0 + (vk - '0');
    if (vk >= VK_F1 && vk <= VK_F24) return XK_F1 + (vk - VK_F1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return XK_KP_0 + (vk - VK_NUMPAD0);
    for (const auto& key : kKeys) {
        if (key.vk == vk) return key.keysym;
    }
    return 0;
}

unsigned KeysymToVirtualKey(unsigned keysym) {
    if (keysym >= XK_a && keysym <= XK_z) return 'A' + (keysym - XK_a);
    if (keysym >= XK_A && keysym <= XK_Z) return 'A' + (keysym - XK_A);
    if (keysym >= XK_0 && keysym <= XK_9) return '0' + (keysym - XK_0);
    if (keysym >= XK_F1 && keysym <= XK_F24) return VK_F1 + (keysym - XK_F1);
    if (keysym >= XK_KP_0 && keysym <= XK_KP_9) return VK_NUMPAD0 + (keysym - XK_KP_0);
    for (const auto& key : kKeys) {
        if (key.keysym == keysym) return key.vk;
    }
    return 0;
}

struct Grab {
    KeyCode keycode;
    unsigned modifiers;  // X modifier mask
};

void (*g_hotkeyHandler)(int, bool) = nullptr;
std::map<int, Grab> g_grabs;
bool g_filterInstalled = false;

// The X display, or null when GTK is not on X (Wayland)
Display* XDisplay() {
    GdkDisplay* display = gdk_display_get_default();
    if (!display || !GDK_IS_X11_DISPLAY(display)) return nullptr;
    return GDK_DISPLAY_XDISPLAY(display);
}

// Num Lock and Caps Lock change the modifier state but not what the key is: each
// grab is made with and without them
unsigned NumLockMask(Display* display) {
    unsigned mask = 0;
    XModifierKeymap* map = XGetModifierMapping(display);
    KeyCode numLock = XKeysymToKeycode(display, XK_Num_Lock);
    for (int m = 0; m < 8 && numLock; m++) {
        for (int k = 0; k < map->max_keypermod; k++) {
            if (map->modifiermap[m * map->max_keypermod + k] == numLock) mask = 1u << m;
        }
    }
    XFreeModifiermap(map);
    return mask;
}

template <typename Each>
void ForEachLockState(Display* display, unsigned modifiers, Each each) {
    const unsigned numLock = NumLockMask(display);
    const unsigned locks[] = {0, LockMask, numLock, LockMask | numLock};
    for (int i = 0; i < (numLock ? 4 : 2); i++) each(modifiers | locks[i]);
}

const unsigned kModifierMask = ControlMask | ShiftMask | Mod1Mask | Mod4Mask;

GdkFilterReturn OnXEvent(GdkXEvent* native, GdkEvent*, gpointer) {
    XEvent* event = static_cast<XEvent*>(native);
    if (event->type != KeyPress && event->type != KeyRelease) return GDK_FILTER_CONTINUE;
    const unsigned state = event->xkey.state & kModifierMask;
    for (const auto& [id, grab] : g_grabs) {
        // A release may come with the modifiers already let go
        if (grab.keycode != event->xkey.keycode) continue;
        if (event->type == KeyPress && grab.modifiers != state) continue;
        if (g_hotkeyHandler) g_hotkeyHandler(id, event->type == KeyPress);
        return GDK_FILTER_REMOVE;
    }
    return GDK_FILTER_CONTINUE;
}

}  // namespace

unsigned LinuxKeyCodeToVirtualKey(unsigned hardwareKeycode) {
    GdkDisplay* display = gdk_display_get_default();
    if (!display) return 0;
    GdkKeymapKey* keys = nullptr;
    guint* keyvals = nullptr;
    gint count = 0;
    unsigned vk = 0;
    if (gdk_keymap_get_entries_for_keycode(gdk_keymap_get_for_display(display), hardwareKeycode, &keys,
                                           &keyvals, &count)) {
        // The key's first layout, without Shift
        for (gint i = 0; i < count && !vk; i++) {
            if (keys[i].group == 0 && keys[i].level == 0) vk = KeysymToVirtualKey(keyvals[i]);
        }
        g_free(keys);
        g_free(keyvals);
    }
    return vk;
}

void SetSystemHotkeyHandler(void (*handler)(int id, bool pressed)) {
    g_hotkeyHandler = handler;
}

bool RegisterSystemHotkey(int id, unsigned modifiers, unsigned vk) {
    Display* display = XDisplay();
    if (!display) return false;
    const unsigned keysym = VirtualKeyToKeysym(vk);
    const KeyCode keycode = keysym ? XKeysymToKeycode(display, keysym) : 0;
    if (!keycode) return false;

    unsigned mask = 0;
    if (modifiers & MOD_CONTROL) mask |= ControlMask;
    if (modifiers & MOD_SHIFT) mask |= ShiftMask;
    if (modifiers & MOD_ALT) mask |= Mod1Mask;
    if (modifiers & MOD_WIN) mask |= Mod4Mask;

    if (!g_filterInstalled) {
        gdk_window_add_filter(nullptr, OnXEvent, nullptr);
        // Holding a key down then gives presses, and one release when it comes up
        XkbSetDetectableAutoRepeat(display, True, nullptr);
        g_filterInstalled = true;
    }

    UnregisterSystemHotkey(id);
    // Another program's grab of the same key is an error from the server, which
    // GDK would otherwise treat as fatal: caught here, it fails the registration
    GdkDisplay* gdkDisplay = gdk_display_get_default();
    gdk_x11_display_error_trap_push(gdkDisplay);
    const Window root = DefaultRootWindow(display);
    ForEachLockState(display, mask, [&](unsigned m) {
        XGrabKey(display, keycode, m, root, False, GrabModeAsync, GrabModeAsync);
    });
    XSync(display, False);
    if (gdk_x11_display_error_trap_pop(gdkDisplay) != 0) {
        ForEachLockState(display, mask, [&](unsigned m) { XUngrabKey(display, keycode, m, root); });
        XFlush(display);
        return false;
    }
    g_grabs[id] = Grab{keycode, mask};
    return true;
}

void UnregisterSystemHotkey(int id) {
    auto it = g_grabs.find(id);
    if (it == g_grabs.end()) return;
    if (Display* display = XDisplay()) {
        GdkDisplay* gdkDisplay = gdk_display_get_default();
        gdk_x11_display_error_trap_push(gdkDisplay);
        const Grab grab = it->second;
        ForEachLockState(display, grab.modifiers, [&](unsigned m) {
            XUngrabKey(display, grab.keycode, m, DefaultRootWindow(display));
        });
        XFlush(display);
        gdk_x11_display_error_trap_pop_ignored(gdkDisplay);
    }
    g_grabs.erase(it);
}
