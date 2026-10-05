#include "presets.h"

#include "model/system.h"

namespace speakers {

namespace {

// One speaker, or a left/right pair when `pair` is set (on the named mount and
// its opposite number).
struct Install {
    const char* specId;
    const char* mountId;
    bool pair;
};

struct RoomPreset {
    const char* name;
    const char* roomId;
    // How much to turn it up (or down) to play about as loud as the music does
    // without it. The simulation is in real sound pressure, so a PA in a field
    // and a bookshelf pair across a room differ by twenty-odd decibels. Each is
    // the K-weighted (BS.1770) loudness of pink noise through the preset,
    // matched to the noise itself less 3 dB of headroom for the room's bass,
    // heard through the HRTF.
    float levelDb;
    Install speakers[4];
    int count;
};

const RoomPreset kPresets[] = {
    {"Car with factory speakers", "car_saloon", 9.3f,
     {{"car_door_65", "door_front_left", true}, {"car_door_65", "door_rear_left", true}}, 2},
    {"Car with a subwoofer", "car_saloon", 10.3f,
     {{"car_comp_65", "door_front_left", true},
      {"car_door_65", "door_rear_left", true},
      {"car_sub_12_ported", "boot_center", false}},
     3},
    {"Hatchback with an under-seat sub", "car_hatchback", 11.9f,
     {{"car_door_65", "door_front_left", true}, {"car_sub_8_sealed", "under_seat_right", false}}, 2},
    {"SUV with two 15 inch subs", "car_suv", 4.5f,
     {{"car_comp_65", "door_front_left", true},
      {"car_door_65", "door_rear_left", true},
      {"car_sub_15_ported", "boot_left", true}},
     3},
    {"Living room, bookshelf speakers and a sub", "living_room", 20.4f,
     {{"home_book_65", "main_left", true}, {"home_sub_10_sealed", "sub_front_left", false}}, 2},
    {"Living room, floorstanding speakers", "living_room", 11.4f, {{"home_tower_65", "main_left", true}}, 1},
    {"Bedroom, satellites and a sub", "bedroom", 28.1f,
     {{"home_sat_3", "main_left", true}, {"home_sub_10_sealed", "sub_front_right", false}}, 2},
    {"Studio monitors", "bedroom", 14.2f, {{"home_monitor_8", "main_left", true}}, 1},
    {"Home theatre", "home_theatre", 20.3f,
     {{"home_tower_65", "main_left", true},
      {"home_center_525", "center_front", false},
      {"home_book_525", "surround_left", true},
      {"home_sub_12_ported", "sub_front_left", false}},
     4},
    {"Garage party", "garage", 7.5f,
     {{"home_pa_15", "main_left", true}, {"home_sub_18_ported", "sub_front_left", false}}, 2},
    {"Hall with a PA", "hall", 16.5f,
     {{"home_pa_15", "main_left", true}, {"home_sub_18_ported", "sub_front_left", true}}, 2},
    {"Open air PA", "outdoors", 13.7f,
     {{"home_pa_15", "main_left", true}, {"home_sub_18_ported", "sub_front_left", true}}, 2},
};

static_assert(sizeof(kPresets) / sizeof(kPresets[0]) == kRoomPresetCount,
              "kRoomPresetCount must match the preset table");

const RoomPreset* Find(int preset) {
    return preset >= 0 && preset < kRoomPresetCount ? &kPresets[preset] : nullptr;
}

}  // namespace

const char* RoomPresetName(int preset) {
    const RoomPreset* p = Find(preset);
    return p ? p->name : "";
}

bool RoomPresetHasSub(int preset) {
    const RoomPreset* p = Find(preset);
    if (!p) return false;
    for (int i = 0; i < p->count; i++) {
        const SpeakerSpec* spec = FindSpeakerSpec(p->speakers[i].specId);
        if (spec && spec->kind == SpeakerKind::Subwoofer) return true;
    }
    return false;
}

bool BuildRoomPreset(int preset, SpeakerSystem& system) {
    const RoomPreset* p = Find(preset);
    if (!p) return false;
    system = SpeakerSystem();
    if (system.SetRoom(p->roomId) < 0) return false;
    system.Settings().levelTrimDb = p->levelDb;
    for (int i = 0; i < p->count; i++) {
        const Install& install = p->speakers[i];
        if (install.pair) {
            system.AddPair(install.specId, install.mountId);
        } else {
            system.AddSpeaker(install.specId, install.mountId);
        }
    }
    system.ResetListener();
    return !system.Speakers().empty();
}

}  // namespace speakers
