#include "room.h"

#include <algorithm>
#include <cmath>

namespace speakers {

namespace {

constexpr float kSpeedOfSound = 343.0f;

float Log2f(float x) { return std::log(x) / std::log(2.0f); }

MountPoint Mount(const char *id, const char *name, Vec3 pos, float yaw, float pitch,
                 MountSide side, const char *pairId, float maxCone, bool subOnly = false) {
    MountPoint m;
    m.id = id;
    m.name = name;
    m.position = pos;
    m.aimYawDeg = yaw;
    m.aimPitchDeg = pitch;
    m.side = side;
    m.pairId = pairId ? pairId : "";
    m.maxConeInches = maxCone;
    m.subOnly = subOnly;
    return m;
}

// ---------------------------------------------------------------------------
// Car cabins
//
// Axes: +x right, +y forward, +z up, origin at the centre of the cabin floor.
// A saloon is about 1.5 m across, 2.6 m of cabin, and 1.2 m from floor to
// headlining, with the boot hanging off the back beyond the rear seats.
// ---------------------------------------------------------------------------

void AddCarMounts(RoomSpec &r, float halfWidth, float frontY, float rearY, float deckY,
                  float bootY, float roofZ) {
    // Aimed nearer the listener than straight up at the glass. A dash pair
    // does fire into the windscreen and you hear a good deal of them by
    // reflection -- but that reflection arrives within a millisecond of the
    // direct sound, which is inside the window this engine deliberately fades
    // out to keep close walls from comb filtering the direct sound. Pointing
    // them twenty degrees at the roof and then removing the bounce that comes
    // back leaves them dull for a reason that is not in the model.
    r.mounts.push_back(Mount("dash_left", "left dash", {-halfWidth * 0.75f, frontY + 0.55f, 0.72f},
                             25.0f, 8.0f, MountSide::Left, "dash_right", 4.0f));
    r.mounts.push_back(Mount("dash_right", "right dash", {halfWidth * 0.75f, frontY + 0.55f, 0.72f},
                             -25.0f, 8.0f, MountSide::Right, "dash_left", 4.0f));
    r.mounts.push_back(Mount("pillar_left", "left A pillar",
                             {-halfWidth * 0.92f, frontY + 0.42f, roofZ - 0.22f}, 35.0f, -10.0f,
                             MountSide::Left, "pillar_right", 1.5f));
    r.mounts.push_back(Mount("pillar_right", "right A pillar",
                             {halfWidth * 0.92f, frontY + 0.42f, roofZ - 0.22f}, -35.0f, -10.0f,
                             MountSide::Right, "pillar_left", 1.5f));
    r.mounts.push_back(Mount("door_front_left", "front left door",
                             {-halfWidth * 0.95f, frontY, 0.30f}, 70.0f, 25.0f, MountSide::Left,
                             "door_front_right", 7.0f));
    r.mounts.push_back(Mount("door_front_right", "front right door",
                             {halfWidth * 0.95f, frontY, 0.30f}, -70.0f, 25.0f, MountSide::Right,
                             "door_front_left", 7.0f));
    r.mounts.push_back(Mount("door_rear_left", "rear left door",
                             {-halfWidth * 0.95f, rearY, 0.30f}, 70.0f, 25.0f, MountSide::Left,
                             "door_rear_right", 6.5f));
    r.mounts.push_back(Mount("door_rear_right", "rear right door",
                             {halfWidth * 0.95f, rearY, 0.30f}, -70.0f, 25.0f, MountSide::Right,
                             "door_rear_left", 6.5f));
    r.mounts.push_back(Mount("kick_left", "left kick panel",
                             {-halfWidth * 0.8f, frontY + 0.45f, 0.12f}, 45.0f, 35.0f,
                             MountSide::Left, "kick_right", 6.5f));
    r.mounts.push_back(Mount("kick_right", "right kick panel",
                             {halfWidth * 0.8f, frontY + 0.45f, 0.12f}, -45.0f, 35.0f,
                             MountSide::Right, "kick_left", 6.5f));
    r.mounts.push_back(Mount("deck_left", "left rear deck", {-halfWidth * 0.6f, deckY, 0.70f}, 0.0f,
                             80.0f, MountSide::Left, "deck_right", 9.0f));
    r.mounts.push_back(Mount("deck_right", "right rear deck", {halfWidth * 0.6f, deckY, 0.70f},
                             0.0f, 80.0f, MountSide::Right, "deck_left", 9.0f));
    r.mounts.push_back(Mount("under_seat_left", "under the left front seat",
                             {-halfWidth * 0.55f, frontY - 0.15f, 0.10f}, 0.0f, 0.0f,
                             MountSide::Left, "under_seat_right", 8.0f, true));
    r.mounts.push_back(Mount("under_seat_right", "under the right front seat",
                             {halfWidth * 0.55f, frontY - 0.15f, 0.10f}, 0.0f, 0.0f,
                             MountSide::Right, "under_seat_left", 8.0f, true));
    r.mounts.push_back(Mount("boot_center", "centre of the boot", {0.0f, bootY, 0.32f}, 180.0f,
                             0.0f, MountSide::Center, "", 18.0f, true));
    r.mounts.push_back(Mount("boot_left", "left side of the boot", {-halfWidth * 0.55f, bootY, 0.32f},
                             180.0f, 0.0f, MountSide::Left, "boot_right", 15.0f, true));
    r.mounts.push_back(Mount("boot_right", "right side of the boot", {halfWidth * 0.55f, bootY, 0.32f},
                             180.0f, 0.0f, MountSide::Right, "boot_left", 15.0f, true));
}

RoomSpec MakeSaloon() {
    RoomSpec r;
    r.id = "car_saloon";
    r.name = "saloon car";
    r.description =
        "A four door saloon with a separate boot. Dead, tiny, and enormous cabin gain below "
        "seventy hertz.";
    r.kind = RoomKind::Vehicle;
    r.width = 1.50f;
    r.depth = 2.60f;
    r.height = 1.20f;
    r.absorption = 0.45f; // seats, carpet, headlining
    // In a seat, not on the centre line.
    //
    // Sitting exactly between the two front speakers looks like the fair place
    // to judge a system and is the worst one. Both sides arrive at the same
    // instant, so everything the two channels share adds to itself coherently
    // -- twenty decibels of it at 250 Hz -- and then interferes with the rear
    // pair at a fixed path difference, cutting a ten decibel hole at 500 Hz.
    // Neither is a car; both are an artefact of being exactly equidistant. Off
    // to one side by the width of a seat, the same system measures smooth.
    //
    // Nobody sits there either. The reference recordings this room is matched
    // against were made from the front passenger seat, which is where this
    // puts you.
    r.defaultListener = {0.45f, 0.35f, 0.72f};
    r.defaultYawDeg = 0.0f;
    r.listenerHeight = 0.72f; // seated
    r.wallMargin = 0.18f;
    AddCarMounts(r, 0.75f, 0.55f, -0.55f, -1.15f, -1.55f, 1.20f);
    return r;
}

RoomSpec MakeHatchback() {
    RoomSpec r;
    r.id = "car_hatchback";
    r.name = "hatchback";
    r.description =
        "A small hatchback. The boot opens straight into the cabin, so the sub is right behind "
        "your head and the tailgate never stops buzzing.";
    r.kind = RoomKind::Vehicle;
    r.width = 1.42f;
    r.depth = 2.30f;
    r.height = 1.22f;
    r.absorption = 0.40f;
    r.defaultListener = {0.43f, 0.30f, 0.72f}; // in a seat; see the saloon
    r.listenerHeight = 0.72f;
    r.wallMargin = 0.16f;
    AddCarMounts(r, 0.71f, 0.50f, -0.50f, -1.00f, -1.20f, 1.22f);
    return r;
}

RoomSpec MakeSuv() {
    RoomSpec r;
    r.id = "car_suv";
    r.name = "SUV";
    r.description =
        "A large SUV. More cabin to fill and a little less cabin gain, but room for a boot full "
        "of subwoofer.";
    r.kind = RoomKind::Vehicle;
    r.width = 1.62f;
    r.depth = 3.05f;
    r.height = 1.35f;
    r.absorption = 0.44f;
    r.defaultListener = {0.48f, 0.55f, 0.80f}; // in a seat; see the saloon
    r.listenerHeight = 0.80f;
    r.wallMargin = 0.18f;
    AddCarMounts(r, 0.81f, 0.70f, -0.45f, -1.25f, -1.70f, 1.35f);
    return r;
}

// ---------------------------------------------------------------------------
// Rooms
// ---------------------------------------------------------------------------

// Yaw 0 faces the front wall (+y), 180 the back. The front speakers sit ahead of
// the seat, so they face back into the room, toed in towards it: 180 less the
// toe in. (They faced the front wall, the listener 126 to 137 degrees off their
// axis, which took ten decibels and more off everything above the low midrange.)
void AddStereoRoomMounts(RoomSpec &r, float spread, float frontY, float rearY, float standZ,
                         float surroundZ) {
    float hw = r.width * 0.5f;
    r.mounts.push_back(Mount("main_left", "front left", {-spread, frontY, standZ}, 160.0f, 0.0f,
                             MountSide::Left, "main_right", 0.0f));
    r.mounts.push_back(Mount("main_right", "front right", {spread, frontY, standZ}, -160.0f, 0.0f,
                             MountSide::Right, "main_left", 0.0f));
    r.mounts.push_back(Mount("center_front", "front centre", {0.0f, frontY, standZ * 0.55f}, 180.0f,
                             0.0f, MountSide::Center, "", 0.0f));
    r.mounts.push_back(Mount("shelf_left", "left shelf", {-hw + 0.25f, frontY - 0.6f, standZ + 0.6f},
                             145.0f, -15.0f, MountSide::Left, "shelf_right", 8.0f));
    r.mounts.push_back(Mount("shelf_right", "right shelf", {hw - 0.25f, frontY - 0.6f, standZ + 0.6f},
                             -145.0f, -15.0f, MountSide::Right, "shelf_left", 8.0f));
    r.mounts.push_back(Mount("surround_left", "left surround", {-hw + 0.2f, rearY, surroundZ},
                             75.0f, -20.0f, MountSide::Left, "surround_right", 0.0f));
    r.mounts.push_back(Mount("surround_right", "right surround", {hw - 0.2f, rearY, surroundZ},
                             -75.0f, -20.0f, MountSide::Right, "surround_left", 0.0f));
    r.mounts.push_back(Mount("sub_front_left", "front left corner", {-hw + 0.3f, frontY + 0.3f, 0.25f},
                             180.0f, 0.0f, MountSide::Left, "sub_front_right", 0.0f, true));
    r.mounts.push_back(Mount("sub_front_right", "front right corner", {hw - 0.3f, frontY + 0.3f, 0.25f},
                             180.0f, 0.0f, MountSide::Right, "sub_front_left", 0.0f, true));
    r.mounts.push_back(Mount("sub_rear_center", "back wall, centred", {0.0f, rearY - 0.2f, 0.25f},
                             0.0f, 0.0f, MountSide::Center, "", 0.0f, true));
}

RoomSpec MakeLivingRoom() {
    RoomSpec r;
    r.id = "living_room";
    r.name = "living room";
    r.description = "A furnished living room, about five metres by four. The default room.";
    r.kind = RoomKind::Indoor;
    r.width = 5.2f;
    r.depth = 4.2f;
    r.height = 2.5f;
    r.absorption = 0.18f;
    r.defaultListener = {0.0f, -0.6f, 1.15f};
    r.listenerHeight = 1.15f; // seated on a sofa
    AddStereoRoomMounts(r, 1.6f, 1.75f, -1.75f, 1.05f, 1.85f);
    return r;
}

RoomSpec MakeBedroom() {
    RoomSpec r;
    r.id = "bedroom";
    r.name = "bedroom";
    r.description =
        "A small bedroom with a bed and curtains. Dead for its size, and close walls put the room "
        "modes up where you hear them.";
    r.kind = RoomKind::Indoor;
    r.width = 3.6f;
    r.depth = 3.0f;
    r.height = 2.4f;
    r.absorption = 0.26f;
    r.defaultListener = {0.0f, -0.4f, 1.2f};
    r.listenerHeight = 1.2f;
    AddStereoRoomMounts(r, 1.05f, 1.15f, -1.15f, 0.95f, 1.75f);
    return r;
}

RoomSpec MakeHomeTheatre() {
    RoomSpec r;
    r.id = "home_theatre";
    r.name = "home theatre";
    r.description =
        "A treated room built for this. Soft walls, low reverberation, and nothing left loose "
        "enough to buzz.";
    r.kind = RoomKind::Indoor;
    r.width = 6.0f;
    r.depth = 4.8f;
    r.height = 2.7f;
    r.absorption = 0.34f;
    r.defaultListener = {0.0f, -0.8f, 1.15f};
    r.listenerHeight = 1.15f;
    AddStereoRoomMounts(r, 1.9f, 2.0f, -2.0f, 1.1f, 1.95f);
    return r;
}

RoomSpec MakeGarage() {
    RoomSpec r;
    r.id = "garage";
    r.name = "garage";
    r.description =
        "Bare concrete and a metal roller door. Wildly reverberant, and the door answers every "
        "bass note.";
    r.kind = RoomKind::Indoor;
    r.width = 6.0f;
    r.depth = 6.0f;
    r.height = 3.0f;
    r.absorption = 0.06f;
    r.defaultListener = {0.0f, -1.0f, 1.6f};
    r.listenerHeight = 1.6f; // standing
    AddStereoRoomMounts(r, 2.0f, 2.4f, -2.4f, 1.1f, 2.2f);
    return r;
}

RoomSpec MakeHall() {
    RoomSpec r;
    r.id = "hall";
    r.name = "small hall";
    r.description = "A hall you could put a band in. Long reverberation and a lot of walking room.";
    r.kind = RoomKind::Indoor;
    r.width = 14.0f;
    r.depth = 20.0f;
    r.height = 6.5f;
    r.absorption = 0.11f;
    r.defaultListener = {0.0f, -3.0f, 1.6f};
    r.listenerHeight = 1.6f;
    AddStereoRoomMounts(r, 4.0f, 8.0f, -8.5f, 1.6f, 3.0f);
    return r;
}

RoomSpec MakeOutdoors() {
    RoomSpec r;
    r.id = "outdoors";
    r.name = "open air";
    r.description =
        "A field. No walls, no reflections, no cabin gain. Everything sounds "
        "thin, which is the point.";
    r.kind = RoomKind::Outdoor;
    r.width = 60.0f;
    r.depth = 60.0f;
    r.height = 20.0f;
    r.absorption = 0.95f;
    r.defaultListener = {0.0f, -3.0f, 1.6f};
    r.listenerHeight = 1.6f;
    r.wallMargin = 1.0f;
    AddStereoRoomMounts(r, 3.0f, 4.0f, -6.0f, 1.2f, 1.8f);
    return r;
}

std::vector<RoomSpec> BuildCatalog() {
    std::vector<RoomSpec> rooms;
    rooms.push_back(MakeSaloon());
    rooms.push_back(MakeHatchback());
    rooms.push_back(MakeSuv());
    rooms.push_back(MakeLivingRoom());
    rooms.push_back(MakeBedroom());
    rooms.push_back(MakeHomeTheatre());
    rooms.push_back(MakeGarage());
    rooms.push_back(MakeHall());
    rooms.push_back(MakeOutdoors());
    for (auto &r : rooms) r.Derive();
    return rooms;
}

} // namespace

const char *RoomKindName(RoomKind k) {
    switch (k) {
    case RoomKind::Vehicle: return "vehicle";
    case RoomKind::Indoor: return "indoor";
    case RoomKind::Outdoor: return "outdoor";
    }
    return "indoor";
}

// What a real car does to a recording, on the way to the seat.
//
// Fitted to measurement, not drawn: eight songs were played in a car and
// recorded from the front passenger seat, each against the same passage of
// the source file. Aligned by cross correlation, averaged in third octave
// bands over the whole clip, and normalised at a kilohertz, the car came back
// +20 dB at 50 Hz, dipping to +8 at 125, rising again to +12 around 200 to
// 250, and back to level by 500. The spread across the eight songs is 2 to 4
// dB through most of that, so the shape is solid.
//
// Against the same system rendered without any cabin response at all -- a
// component pair in the front doors, a coaxial pair in the rear, and a ported
// twelve in the boot -- the difference is what these three sections
// reproduce, to within 1.1 dB rms -- fitted at a twelfth of an octave from
// 100 to 900 Hz, where the shape actually has detail, rather than in third
// octave bands.
//
// The caveat worth keeping in mind: this is one car, one system, one phone,
// and whatever the head unit was set to. It is a far better starting point
// than a guess, but it is not every car.
const RoomSpec::CabinBand kCabinShape[] = {
    {73.7f, 0.597f, -1.5f, false},
    {74.1f, 0.931f, 13.2f, true},
    {264.7f, 1.114f, 15.2f, true},
    // The upper edge of the plateau, which is a cliff rather than a slope.
    // The car sits at +13 dB all the way from 170 to 280 Hz and is down to
    // +5 by 320 -- nine decibels in a sixth of an octave. A single peak's
    // skirt cannot do that, and fitting this in third octave bands hid it:
    // the bands are wider than the edge, so they smeared it into a gentle
    // roll-off and left the model an octave too wide through the low mids.
    {389.0f, 3.960f, 5.6f, true},
    {430.0f, 1.294f, -8.2f, true},
};

void RoomSpec::Derive() {
    float longest = std::max(width, std::max(depth, height));

    if (cabinGainHz <= 0.0f) {
        // Below the frequency whose half wavelength spans the longest
        // dimension, the space stops behaving as a room and starts behaving as
        // a sealed box being pressurised.
        cabinGainHz = (kind == RoomKind::Outdoor) ? 0.0f : kSpeedOfSound / (2.0f * longest);
    }
    if (cabinGainDb <= 0.0f) {
        switch (kind) {
        case RoomKind::Outdoor: cabinGainDb = 0.0f; break;
        case RoomKind::Vehicle: cabinGainDb = 12.0f; break;
        case RoomKind::Indoor:
            // Small rooms pressurise noticeably; big ones barely at all, but
            // every enclosed space keeps a couple of dB of it.
            cabinGainDb = std::max(2.0f, std::min(12.0f, 14.0f - 5.0f * Log2f(Volume() / 5.0f)));
            break;
        }
    }
    if (listenerHeight <= 0.0f) listenerHeight = 1.2f;
    defaultListener.z = listenerHeight;
    if (wallMargin <= 0.0f) wallMargin = 0.35f;

    // Vehicles carry the measured shape; other spaces keep the pressure shelf
    // on its own, which is all the evidence there is for them.
    if (cabinShape.empty() && kind == RoomKind::Vehicle)
        cabinShape.assign(std::begin(kCabinShape), std::end(kCabinShape));
}

float RoomSpec::Volume() const { return width * depth * height; }

float RoomSpec::SurfaceArea() const {
    return 2.0f * (width * depth + width * height + depth * height);
}

float RoomSpec::Rt60() const {
    if (kind == RoomKind::Outdoor) return 0.0f;
    float absorbed = SurfaceArea() * std::max(absorption, 0.01f);
    return std::max(0.02f, 0.161f * Volume() / absorbed);
}

const MountPoint *RoomSpec::FindMount(const std::string &mountId) const {
    for (const auto &m : mounts)
        if (m.id == mountId) return &m;
    return nullptr;
}

const std::vector<RoomSpec> &RoomCatalog() {
    static const std::vector<RoomSpec> rooms = BuildCatalog();
    return rooms;
}

const RoomSpec *FindRoomSpec(const std::string &id) {
    for (const auto &r : RoomCatalog())
        if (r.id == id) return &r;
    return nullptr;
}

}  // namespace speakers
