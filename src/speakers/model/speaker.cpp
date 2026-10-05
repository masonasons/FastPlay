#include "speaker.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace speakers {

namespace {

constexpr float kSpeedOfSound = 343.0f; // m/s at 20 C
constexpr float kPi = 3.14159265358979f;

float Log2f(float x) { return std::log(x) / std::log(2.0f); }

bool HasTweeter(SpeakerKind k) {
    return k == SpeakerKind::Coaxial || k == SpeakerKind::Component || k == SpeakerKind::Center;
}

bool EqualsIgnoreCase(const std::string &a, const char *b) {
#ifdef _MSC_VER
    return _stricmp(a.c_str(), b) == 0;
#else
    return strcasecmp(a.c_str(), b) == 0;
#endif
}

} // namespace

const char *EnclosureName(Enclosure e) {
    switch (e) {
    case Enclosure::Sealed: return "sealed";
    case Enclosure::Ported: return "ported";
    case Enclosure::InfiniteBaffle: return "infinite baffle";
    case Enclosure::Horn: return "horn";
    }
    return "sealed";
}

const char *SpeakerKindName(SpeakerKind k) {
    switch (k) {
    case SpeakerKind::FullRange: return "full range";
    case SpeakerKind::Coaxial: return "coaxial";
    case SpeakerKind::Component: return "component";
    case SpeakerKind::Midbass: return "midbass";
    case SpeakerKind::Subwoofer: return "subwoofer";
    case SpeakerKind::Center: return "center";
    }
    return "coaxial";
}

const char *ChannelFeedName(ChannelFeed c) {
    switch (c) {
    case ChannelFeed::Left: return "left";
    case ChannelFeed::Right: return "right";
    case ChannelFeed::Mono: return "mono";
    }
    return "mono";
}

bool ParseEnclosure(const std::string &s, Enclosure &out) {
    if (EqualsIgnoreCase(s, "sealed")) { out = Enclosure::Sealed; return true; }
    if (EqualsIgnoreCase(s, "ported")) { out = Enclosure::Ported; return true; }
    if (EqualsIgnoreCase(s, "infinite baffle") || EqualsIgnoreCase(s, "infinite_baffle")) {
        out = Enclosure::InfiniteBaffle;
        return true;
    }
    if (EqualsIgnoreCase(s, "horn")) { out = Enclosure::Horn; return true; }
    return false;
}

bool ParseSpeakerKind(const std::string &s, SpeakerKind &out) {
    if (EqualsIgnoreCase(s, "full range") || EqualsIgnoreCase(s, "full_range")) {
        out = SpeakerKind::FullRange;
        return true;
    }
    if (EqualsIgnoreCase(s, "coaxial")) { out = SpeakerKind::Coaxial; return true; }
    if (EqualsIgnoreCase(s, "component")) { out = SpeakerKind::Component; return true; }
    if (EqualsIgnoreCase(s, "midbass")) { out = SpeakerKind::Midbass; return true; }
    if (EqualsIgnoreCase(s, "subwoofer")) { out = SpeakerKind::Subwoofer; return true; }
    if (EqualsIgnoreCase(s, "center")) { out = SpeakerKind::Center; return true; }
    return false;
}

bool ParseChannelFeed(const std::string &s, ChannelFeed &out) {
    if (EqualsIgnoreCase(s, "left")) { out = ChannelFeed::Left; return true; }
    if (EqualsIgnoreCase(s, "right")) { out = ChannelFeed::Right; return true; }
    if (EqualsIgnoreCase(s, "mono")) { out = ChannelFeed::Mono; return true; }
    return false;
}

// ---------------------------------------------------------------------------
// Derive
//
// The numbers here are deliberately simple closed forms rather than a real
// Thiele-Small solve: they only have to rank correctly against each other and
// land in the right octave, so that a 15 inch ported sub audibly digs deeper
// than a 10 inch sealed one and a 3.5 inch dash speaker audibly has no bass.
// ---------------------------------------------------------------------------
void SpeakerSpec::Derive() {
    float d = coneInches > 0.5f ? coneInches : 6.5f;
    float dMetres = d * 0.0254f;

    // Corner frequency a driver of this size reaches in a well behaved sealed
    // box. Inversely proportional to diameter, which matches the usual ladder:
    // 3.5 in near 120 Hz, 6.5 in near 65 Hz, 12 in near 35 Hz, 18 in near 23 Hz.
    float sealedCorner = 420.0f / d;

    if (enclosure == Enclosure::Ported && portTuningHz <= 0.0f)
        portTuningHz = sealedCorner * 0.78f;

    if (lowCornerHz <= 0.0f) {
        switch (enclosure) {
        case Enclosure::Sealed: lowCornerHz = sealedCorner; break;
        case Enclosure::Ported: lowCornerHz = portTuningHz; break;
        // A door card or a rear deck is not a box. It loses the bottom octave
        // and gains an uncontrolled bump just above the corner.
        case Enclosure::InfiniteBaffle: lowCornerHz = sealedCorner * 1.20f; break;
        case Enclosure::Horn: lowCornerHz = sealedCorner * 1.40f; break;
        }
    }
    if (lowOrder <= 0) {
        // A vented box unloads below tuning and falls off twice as fast.
        lowOrder = (enclosure == Enclosure::Ported || enclosure == Enclosure::Horn) ? 4 : 2;
    }
    if (lowQ <= 0.0f) {
        switch (enclosure) {
        case Enclosure::Sealed: lowQ = 0.707f; break;        // maximally flat
        case Enclosure::Ported: lowQ = 0.80f; break;         // a little lift at tuning
        case Enclosure::InfiniteBaffle: lowQ = 0.95f; break; // boomy, underdamped
        case Enclosure::Horn: lowQ = 0.90f; break;
        }
    }

    // ---- Thiele-Small ----------------------------------------------------
    // The surround does not radiate as well as the cone does, so the piston
    // that actually pushes air is a little smaller than the nominal size.
    if (sdCm2 <= 0.0f) {
        float effective = dMetres * 0.85f;
        sdCm2 = kPi * (effective * 0.5f) * (effective * 0.5f) * 10000.0f;
    }
    // Free air resonance falls with size on much the same ladder the corner
    // does: a 12 inch sits near 32 Hz, a 6.5 near 58, a 3.5 near 109.
    if (fsHz <= 0.0f) fsHz = 380.0f / d;
    if (qts <= 0.0f) {
        // A driver meant for a box is built with more damping than one meant
        // to be screwed to a door, where there is nothing behind it to help.
        qts = (enclosure == Enclosure::InfiniteBaffle) ? 0.70f
              : (kind == SpeakerKind::Subwoofer)       ? 0.40f
                                                       : 0.50f;
    }
    if (vasLitres <= 0.0f) vasLitres = 0.05f * d * d * d;
    // Linear travel one way. A subwoofer is built to move; a midrange is built
    // to stop, and runs out of room long before it runs out of power.
    if (xmaxMm <= 0.0f) xmaxMm = (kind == SpeakerKind::Subwoofer) ? 1.0f * d : 0.55f * d;

    // Where the cone stops behaving as a piston: the wavelength has shrunk to
    // the cone circumference, and output starts to narrow onto the axis.
    if (beamingHz <= 0.0f) beamingHz = kSpeedOfSound / (kPi * dMetres);

    if (topHz <= 0.0f) {
        if (HasTweeter(kind)) {
            topHz = 20000.0f;
        } else if (kind == SpeakerKind::Subwoofer) {
            // Voice coil inductance kills a sub well before cone breakup does.
            topHz = std::min(beamingHz * 3.0f, 800.0f);
        } else {
            topHz = std::min(beamingHz * 4.0f, 16000.0f);
        }
    }
    if (topOrder <= 0) topOrder = 2;

    // Efficiency climbs with radiating area; a horn buys a lot more, and a door
    // with no box behind it loses a little.
    if (sensitivityDb <= 0.0f) {
        sensitivityDb = 85.0f + 4.0f * Log2f(d / 6.5f);
        if (enclosure == Enclosure::Ported) sensitivityDb += 2.0f;
        if (enclosure == Enclosure::InfiniteBaffle) sensitivityDb -= 1.0f;
        if (enclosure == Enclosure::Horn) sensitivityDb += 6.0f;
    }

    if (powerWatts <= 0.0f) {
        powerWatts = (kind == SpeakerKind::Subwoofer) ? 300.0f * (d / 12.0f) * (d / 12.0f)
                                                     : 60.0f * (d / 6.5f) * (d / 6.5f);
    }

    if (boxLitres <= 0.0f) {
        boxLitres = 0.35f * d * d;
        if (enclosure == Enclosure::Ported) boxLitres *= 1.8f;
        if (enclosure == Enclosure::InfiniteBaffle) boxLitres = 0.0f; // no box at all
    }
}

float SpeakerSpec::MaxSplDb() const {
    float w = powerWatts > 0.0f ? powerWatts : 1.0f;
    return sensitivityDb + 10.0f * std::log10(w);
}

// ---------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------

namespace {

SpeakerSpec Make(const char *id, const char *name, const char *description, float cone,
                 Enclosure enc, SpeakerKind kind, ChannelFeed feed) {
    SpeakerSpec s;
    s.id = id;
    s.name = name;
    s.description = description;
    s.coneInches = cone;
    s.enclosure = enc;
    s.kind = kind;
    s.defaultFeed = feed;
    s.Derive();
    return s;
}

std::vector<SpeakerSpec> BuildCatalog() {
    std::vector<SpeakerSpec> c;

    // ---- car: mains ----
    {
        // The generic derivation is unkind to a dash pair on all three counts.
        // It gives them a corner at 144 Hz with an underdamped lift just above
        // it -- a 3.5 inch cone firing into a dash cavity has no business
        // making anything at 150 Hz, and what it does make is a drone rather
        // than a bump. It also reads their efficiency off cone area alone and
        // lands near 81 dB, where a real small coaxial is rated closer to 89:
        // the light cone and small voice coil buy back most of what the area
        // loses. They end up quiet and thick, which is the opposite of what
        // they are for.
        SpeakerSpec dash =
            Make("car_dash_35", "3.5 inch dash pair",
                 "Small dash or sail panel speakers. Clear up top, no bass at all.", 3.5f,
                 Enclosure::InfiniteBaffle, SpeakerKind::Coaxial, ChannelFeed::Left);
        dash.lowCornerHz = 320.0f;
        dash.lowQ = 0.70f;
        dash.sensitivityDb = 88.5f;
        dash.Derive();
        c.push_back(dash);
    }
    c.push_back(Make("car_door_525", "5.25 inch coaxial door pair",
                     "Common factory door size. Some midbass, rolls off early.", 5.25f,
                     Enclosure::InfiniteBaffle, SpeakerKind::Coaxial, ChannelFeed::Left));
    c.push_back(Make("car_door_65", "6.5 inch coaxial door pair",
                     "The usual aftermarket door upgrade. Useful midbass.", 6.5f,
                     Enclosure::InfiniteBaffle, SpeakerKind::Coaxial, ChannelFeed::Left));
    c.push_back(Make("car_comp_65", "6.5 inch component set",
                     "Door woofer with a separate tweeter up on the pillar.", 6.5f,
                     Enclosure::InfiniteBaffle, SpeakerKind::Component, ChannelFeed::Left));
    c.push_back(Make("car_deck_69", "6 by 9 rear deck pair",
                     "Big oval rear deck speakers. Loud, with real low end for a main.", 7.5f,
                     Enclosure::InfiniteBaffle, SpeakerKind::Coaxial, ChannelFeed::Left));
    c.push_back(Make("car_midbass_8", "8 inch door midbass pair",
                     "Dedicated midbass, no tweeter. Wants a crossover above it.", 8.0f,
                     Enclosure::InfiniteBaffle, SpeakerKind::Midbass, ChannelFeed::Left));

    // ---- car: subs ----
    c.push_back(Make("car_sub_8_sealed", "8 inch under seat sub, sealed",
                     "Shallow sealed sub that hides under a seat. Tight, not loud.", 8.0f,
                     Enclosure::Sealed, SpeakerKind::Subwoofer, ChannelFeed::Mono));
    c.push_back(Make("car_sub_10_sealed", "10 inch sub, sealed",
                     "Sealed ten. Accurate and quick, gives up the bottom octave.", 10.0f,
                     Enclosure::Sealed, SpeakerKind::Subwoofer, ChannelFeed::Mono));
    c.push_back(Make("car_sub_12_ported", "12 inch sub, ported",
                     "The classic boot build. Loud, and digs low.", 12.0f,
                     Enclosure::Ported, SpeakerKind::Subwoofer, ChannelFeed::Mono));
    c.push_back(Make("car_sub_15_ported", "15 inch sub, ported",
                     "Serious output. Takes half the boot and shakes the panels.", 15.0f,
                     Enclosure::Ported, SpeakerKind::Subwoofer, ChannelFeed::Mono));

    // ---- home: mains ----
    c.push_back(Make("home_sat_3", "3 inch satellite pair",
                     "Tiny satellites from a boxed surround set. Needs a sub.", 3.0f,
                     Enclosure::Sealed, SpeakerKind::Coaxial, ChannelFeed::Left));
    c.push_back(Make("home_book_525", "5.25 inch bookshelf pair",
                     "Compact two way bookshelf. Polite bass, easy to place.", 5.25f,
                     Enclosure::Sealed, SpeakerKind::Coaxial, ChannelFeed::Left));
    c.push_back(Make("home_book_65", "6.5 inch bookshelf pair",
                     "Ported two way bookshelf. Surprising low end for the size.", 6.5f,
                     Enclosure::Ported, SpeakerKind::Coaxial, ChannelFeed::Left));
    c.push_back(Make("home_monitor_8", "8 inch studio monitor pair",
                     "Nearfield monitors. Flat and unforgiving.", 8.0f, Enclosure::Ported,
                     SpeakerKind::Component, ChannelFeed::Left));
    {
        // Derived from the cone size alone, a tower is the bookshelf's single
        // 6.5 inch woofer in a box tuned to 50 Hz, made to play full range on
        // its own: it ran out of travel on any bass at all and distorted by
        // twenty per cent. A real three way tower has two woofers (twice the
        // cone area, three decibels more efficient) and a big box tuned low.
        SpeakerSpec tower = Make("home_tower_65", "6.5 inch floorstanding tower pair",
                                 "Three way towers. Full range on their own.", 6.5f,
                                 Enclosure::Ported, SpeakerKind::Component, ChannelFeed::Left);
        tower.sdCm2 *= 2.0f;
        tower.sensitivityDb += 3.0f;
        tower.portTuningHz = 36.0f;
        tower.lowCornerHz = 36.0f;
        tower.Derive();
        c.push_back(tower);
    }
    c.push_back(Make("home_center_525", "5.25 inch center channel",
                     "Horizontal centre for dialogue. Fed the mono sum.", 5.25f, Enclosure::Sealed,
                     SpeakerKind::Center, ChannelFeed::Mono));
    c.push_back(Make("home_pa_15", "15 inch PA top",
                     "Horn loaded PA cabinet. Very efficient, very loud.", 15.0f, Enclosure::Horn,
                     SpeakerKind::Component, ChannelFeed::Left));

    // ---- home: subs ----
    c.push_back(Make("home_sub_10_sealed", "10 inch home sub, sealed",
                     "Sealed sub for a small room. Musical rather than thunderous.", 10.0f,
                     Enclosure::Sealed, SpeakerKind::Subwoofer, ChannelFeed::Mono));
    c.push_back(Make("home_sub_12_ported", "12 inch home sub, ported",
                     "The all rounder. Room filling and cheerfully loud.", 12.0f, Enclosure::Ported,
                     SpeakerKind::Subwoofer, ChannelFeed::Mono));
    c.push_back(Make("home_sub_18_ported", "18 inch home sub, ported",
                     "A wardrobe that plays 20 hertz. You feel this one.", 18.0f, Enclosure::Ported,
                     SpeakerKind::Subwoofer, ChannelFeed::Mono));

    return c;
}

} // namespace

const std::vector<SpeakerSpec> &SpeakerCatalog() {
    static const std::vector<SpeakerSpec> catalog = BuildCatalog();
    return catalog;
}

const SpeakerSpec *FindSpeakerSpec(const std::string &id) {
    for (const auto &s : SpeakerCatalog())
        if (s.id == id) return &s;
    return nullptr;
}

}  // namespace speakers
