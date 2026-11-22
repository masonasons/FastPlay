// The media keys, and what the desktop shows of what is playing (GNOME's and KDE's
// media controls, the lock screen, a headset's buttons through the desktop),
// through MPRIS: FastPlay as org.mpris.MediaPlayer2.FastPlay on the session bus.
// The desktop sends the media keys to the player it last saw playing.
//
// GDBus calls these methods on the main loop of the thread that owns the name: the
// UI thread, whose loop GTK (and so wxWidgets) runs.

#include "system_keys.h"
#include "commands.h"
#include "utils.h"

#include <gio/gio.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <string>

namespace {

const char kObjectPath[] = "/org/mpris/MediaPlayer2";
const char kRootInterface[] = "org.mpris.MediaPlayer2";
const char kPlayerInterface[] = "org.mpris.MediaPlayer2.Player";

const char kIntrospection[] = R"(
<node>
  <interface name="org.mpris.MediaPlayer2">
    <method name="Raise"/>
    <method name="Quit"/>
    <property name="CanQuit" type="b" access="read"/>
    <property name="CanRaise" type="b" access="read"/>
    <property name="HasTrackList" type="b" access="read"/>
    <property name="Identity" type="s" access="read"/>
    <property name="DesktopEntry" type="s" access="read"/>
    <property name="SupportedUriSchemes" type="as" access="read"/>
    <property name="SupportedMimeTypes" type="as" access="read"/>
  </interface>
  <interface name="org.mpris.MediaPlayer2.Player">
    <method name="Next"/>
    <method name="Previous"/>
    <method name="Pause"/>
    <method name="PlayPause"/>
    <method name="Stop"/>
    <method name="Play"/>
    <method name="Seek"><arg direction="in" name="Offset" type="x"/></method>
    <method name="SetPosition">
      <arg direction="in" name="TrackId" type="o"/>
      <arg direction="in" name="Position" type="x"/>
    </method>
    <method name="OpenUri"><arg direction="in" name="Uri" type="s"/></method>
    <signal name="Seeked"><arg name="Position" type="x"/></signal>
    <property name="PlaybackStatus" type="s" access="read"/>
    <property name="Rate" type="d" access="readwrite"/>
    <property name="Metadata" type="a{sv}" access="read"/>
    <property name="Volume" type="d" access="readwrite"/>
    <property name="Position" type="x" access="read"/>
    <property name="MinimumRate" type="d" access="read"/>
    <property name="MaximumRate" type="d" access="read"/>
    <property name="CanGoNext" type="b" access="read"/>
    <property name="CanGoPrevious" type="b" access="read"/>
    <property name="CanPlay" type="b" access="read"/>
    <property name="CanPause" type="b" access="read"/>
    <property name="CanSeek" type="b" access="read"/>
    <property name="CanControl" type="b" access="read"/>
  </interface>
</node>)";

void (*g_mediaHandler)(int) = nullptr;
void (*g_positionHandler)(double) = nullptr;
bool g_mediaSeekControls = false;

GDBusNodeInfo* g_nodeInfo = nullptr;
GDBusConnection* g_connection = nullptr;
guint g_ownerId = 0;
guint g_registrations[2] = {0, 0};

// What the desktop was last told
struct NowPlayingState {
    std::wstring title;
    double duration = -1;
    double position = 0;
    bool playing = false;
    bool stopped = true;
    std::chrono::steady_clock::time_point when;
    unsigned track = 0;  // in the track's object path, new for each title
};
NowPlayingState g_state;

int64_t Microseconds(double seconds) {
    return static_cast<int64_t>(std::llround(seconds * 1e6));
}

double CurrentPosition() {
    double position = g_state.position;
    if (g_state.playing) {
        position += std::chrono::duration<double>(std::chrono::steady_clock::now() - g_state.when).count();
    }
    if (g_state.duration > 0 && position > g_state.duration) position = g_state.duration;
    return position;
}

std::string TrackPath() {
    return "/org/fastplay/track/" + std::to_string(g_state.track);
}

bool CanSeek() {
    return g_positionHandler != nullptr && g_state.duration > 0 && !g_state.stopped;
}

GVariant* Metadata() {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    if (!g_state.stopped) {
        g_variant_builder_add(&builder, "{sv}", "mpris:trackid",
                              g_variant_new_object_path(TrackPath().c_str()));
        g_variant_builder_add(&builder, "{sv}", "xesam:title",
                              g_variant_new_string(WideToUtf8(g_state.title).c_str()));
        if (g_state.duration > 0) {
            g_variant_builder_add(&builder, "{sv}", "mpris:length",
                                  g_variant_new_int64(Microseconds(g_state.duration)));
        }
    }
    return g_variant_builder_end(&builder);
}

const char* PlaybackStatus() {
    if (g_state.stopped) return "Stopped";
    return g_state.playing ? "Playing" : "Paused";
}

void RunCommand(int commandId) {
    if (g_mediaSeekControls) {
        if (commandId == IDM_PLAY_PREV) commandId = IDM_PLAY_SEEKBACK;
        else if (commandId == IDM_PLAY_NEXT) commandId = IDM_PLAY_SEEKFWD;
    }
    if (g_mediaHandler) g_mediaHandler(commandId);
}

void GoTo(double seconds) {
    if (!CanSeek()) return;
    if (seconds < 0) seconds = 0;
    if (seconds > g_state.duration) seconds = g_state.duration;
    g_positionHandler(seconds);
}

void OnMethodCall(GDBusConnection*, const gchar*, const gchar*, const gchar* interfaceName,
                  const gchar* method, GVariant* parameters, GDBusMethodInvocation* invocation, gpointer) {
    const std::string name = method;
    if (std::string(interfaceName) == kRootInterface) {
        // Neither raising the window nor quitting is offered (CanRaise, CanQuit)
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }
    if (name == "Next") RunCommand(IDM_PLAY_NEXT);
    else if (name == "Previous") RunCommand(IDM_PLAY_PREV);
    else if (name == "Pause") RunCommand(IDM_PLAY_PAUSE);
    else if (name == "PlayPause") RunCommand(IDM_PLAY_PLAYPAUSE);
    else if (name == "Stop") RunCommand(IDM_PLAY_STOP);
    else if (name == "Play") RunCommand(IDM_PLAY_PLAY);
    else if (name == "Seek") {
        gint64 offset = 0;
        g_variant_get(parameters, "(x)", &offset);
        GoTo(CurrentPosition() + offset / 1e6);
    } else if (name == "SetPosition") {
        const gchar* track = nullptr;
        gint64 position = 0;
        g_variant_get(parameters, "(&ox)", &track, &position);
        // Only for the track playing: one that has ended may still be on screen
        if (track && TrackPath() == track) GoTo(position / 1e6);
    }
    // OpenUri: not offered (SupportedUriSchemes is empty)
    g_dbus_method_invocation_return_value(invocation, nullptr);
}

GVariant* OnGetProperty(GDBusConnection*, const gchar*, const gchar*, const gchar* interfaceName,
                        const gchar* property, GError**, gpointer) {
    const std::string name = property;
    if (std::string(interfaceName) == kRootInterface) {
        if (name == "CanQuit" || name == "CanRaise" || name == "HasTrackList") return g_variant_new_boolean(FALSE);
        if (name == "Identity") return g_variant_new_string("FastPlay");
        if (name == "DesktopEntry") return g_variant_new_string("fastplay");
        if (name == "SupportedUriSchemes" || name == "SupportedMimeTypes") {
            return g_variant_new_strv(nullptr, 0);
        }
        return nullptr;
    }
    if (name == "PlaybackStatus") return g_variant_new_string(PlaybackStatus());
    if (name == "Rate" || name == "MinimumRate" || name == "MaximumRate") return g_variant_new_double(1.0);
    if (name == "Volume") return g_variant_new_double(1.0);
    if (name == "Metadata") return Metadata();
    if (name == "Position") return g_variant_new_int64(Microseconds(CurrentPosition()));
    if (name == "CanSeek") return g_variant_new_boolean(CanSeek());
    if (name == "CanGoNext" || name == "CanGoPrevious" || name == "CanPlay" || name == "CanPause" ||
        name == "CanControl") {
        return g_variant_new_boolean(g_mediaHandler != nullptr);
    }
    return nullptr;
}

gboolean OnSetProperty(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*,
                       GError**, gpointer) {
    return TRUE;  // Rate and Volume stay FastPlay's own; the desktop cannot set them
}

const GDBusInterfaceVTable kVTable = {OnMethodCall, OnGetProperty, OnSetProperty, {nullptr}};

// org.freedesktop.DBus.Properties.PropertiesChanged for the player's properties
void PlayerPropertiesChanged(GVariantBuilder* changed) {
    if (!g_connection) {
        g_variant_builder_clear(changed);
        return;
    }
    g_dbus_connection_emit_signal(g_connection, nullptr, kObjectPath, "org.freedesktop.DBus.Properties",
                                  "PropertiesChanged",
                                  g_variant_new("(sa{sv}as)", kPlayerInterface, changed, nullptr), nullptr);
}

void AnnounceAll() {
    GVariantBuilder changed;
    g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&changed, "{sv}", "PlaybackStatus", g_variant_new_string(PlaybackStatus()));
    g_variant_builder_add(&changed, "{sv}", "Metadata", Metadata());
    g_variant_builder_add(&changed, "{sv}", "CanSeek", g_variant_new_boolean(CanSeek()));
    PlayerPropertiesChanged(&changed);
}

void OnBusAcquired(GDBusConnection* connection, const gchar*, gpointer) {
    g_connection = connection;
    g_registrations[0] = g_dbus_connection_register_object(connection, kObjectPath, g_nodeInfo->interfaces[0],
                                                           &kVTable, nullptr, nullptr, nullptr);
    g_registrations[1] = g_dbus_connection_register_object(connection, kObjectPath, g_nodeInfo->interfaces[1],
                                                           &kVTable, nullptr, nullptr, nullptr);
}

void OnNameLost(GDBusConnection*, const gchar* name, gpointer) {
    // Another FastPlay has the name: this one goes by its process as well, as
    // the specification says a second instance should
    static bool retried = false;
    if (retried || !g_ownerId) return;
    retried = true;
    g_bus_unown_name(g_ownerId);
    std::string instance = std::string(name) + ".instance" + std::to_string(getpid());
    g_ownerId = g_bus_own_name(G_BUS_TYPE_SESSION, instance.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE,
                               OnBusAcquired, nullptr, nullptr, nullptr, nullptr);
}

void Unregister() {
    if (g_connection) {
        for (guint& id : g_registrations) {
            if (id) g_dbus_connection_unregister_object(g_connection, id);
            id = 0;
        }
    }
    if (g_ownerId) g_bus_unown_name(g_ownerId);
    g_ownerId = 0;
    g_connection = nullptr;
}

}  // namespace

void StartMediaKeys(void (*handler)(int commandId), void (*)(int direction, bool pressed)) {
    // (MPRIS has no seeking held down: the rewind and fast forward keys, where a
    // desktop passes them on at all, come as Seek, a step at a time.)
    g_mediaHandler = handler;
    if (g_ownerId) return;
    if (!g_nodeInfo) g_nodeInfo = g_dbus_node_info_new_for_xml(kIntrospection, nullptr);
    if (!g_nodeInfo) return;
    g_ownerId = g_bus_own_name(G_BUS_TYPE_SESSION, "org.mpris.MediaPlayer2.FastPlay", G_BUS_NAME_OWNER_FLAGS_NONE,
                               OnBusAcquired, nullptr, OnNameLost, nullptr, nullptr);
}

void StopMediaKeys() {
    ClearNowPlaying();
    Unregister();
    g_mediaHandler = nullptr;
    g_positionHandler = nullptr;
    g_mediaSeekControls = false;
}

void SetMediaPositionHandler(void (*handler)(double seconds)) {
    g_positionHandler = handler;
}

void SetMediaSeekControls(bool enabled, double) {
    // MPRIS has no skip buttons of their own: Next and Previous seek instead
    g_mediaSeekControls = enabled;
}

void UpdateNowPlaying(const std::wstring& title, double duration, double position, bool playing) {
    const auto now = std::chrono::steady_clock::now();
    const bool newTitle = g_state.stopped || g_state.title != title;
    const bool changed = newTitle || g_state.playing != playing || std::fabs(g_state.duration - duration) >= 0.5;
    // The desktop moves the position on by itself; only a jump needs telling
    const bool jumped = !changed && std::fabs(CurrentPosition() - position) >= 2.0;
    if (!changed && !jumped) return;

    if (newTitle) g_state.track++;
    g_state.title = title;
    g_state.duration = duration;
    g_state.position = position;
    g_state.playing = playing;
    g_state.stopped = false;
    g_state.when = now;

    if (changed) AnnounceAll();
    if ((jumped || newTitle) && g_connection) {
        g_dbus_connection_emit_signal(g_connection, nullptr, kObjectPath, kPlayerInterface, "Seeked",
                                      g_variant_new("(x)", Microseconds(position)), nullptr);
    }
}

void ClearNowPlaying() {
    if (g_state.stopped) return;
    g_state = NowPlayingState{std::wstring(), -1, 0, false, true, {}, g_state.track};
    AnnounceAll();
}
