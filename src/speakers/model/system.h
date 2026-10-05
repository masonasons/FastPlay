#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "room.h"
#include "speaker.h"

namespace speakers {

// A speaker system is a room, a list of speakers placed in it, the settings
// that tie them together (crossover, levels), and where you are standing.
//
// This layer is pure data and geometry. It does no audio: the engine reads it
// and renders it, and the interface reads it and speaks it.

// One speaker installed somewhere in the room. The spec is held by value, not
// by reference into the catalog, so that editing a placed speaker never
// disturbs the catalog entry or any other copy of it.
struct PlacedSpeaker {
    int id = 0;        // unique within the system; never reused
    int pairId = 0;    // speakers added as a stereo pair share this; 0 = single

    SpeakerSpec spec;      // an editable copy of the catalog entry
    std::string mountId;   // empty when placed at a free position
    Vec3 position;
    float aimYawDeg = 0.0f;
    float aimPitchDeg = 0.0f;

    // A component set is a woofer in the door and a tweeter somewhere else
    // entirely -- up on the pillar, at about ear height. That matters more
    // than it sounds like it should: you place a sound by its top end, so
    // radiating the treble from down beside your knee with the woofer makes
    // the whole speaker seem to come from the floor. When the room has a
    // tweeter mount on the same side, the top end comes from there instead.
    bool separateTweeter = false;
    std::string tweeterMountId;
    Vec3 tweeterPosition;

    ChannelFeed feed = ChannelFeed::Mono;
    float gainDb = 0.0f;      // per speaker trim
    float delayMs = 0.0f;     // time alignment
    // What a subwoofer amplifier's phase knob does: nought to a hundred and
    // eighty degrees, continuous rather than a switch. Nought and a hundred
    // and eighty are the two a polarity switch gives you; everything between
    // is what the knob is for, and what actually lines a boot sub up with the
    // doors when the distance is not a whole number of wavelengths.
    float phaseDeg = 0.0f;
    bool invertPolarity = false;
    bool muted = false;
    bool soloed = false;

    // Human readable name for this installation, e.g.
    // "front left door, 6.5 inch coaxial door pair".
    std::string Label(const RoomSpec &room) const;

    bool IsSub() const { return spec.kind == SpeakerKind::Subwoofer; }
};

// Settings that apply to the whole system rather than to one speaker.
struct SystemSettings {
    float crossoverHz = 80.0f;   // subs below, mains above
    int crossoverOrder = 4;      // Linkwitz-Riley order: 2, 4 or 8
    float subGainDb = 0.0f;      // level of the sub bus relative to the mains
    float masterGainDb = -2.0f;  // output trim
    // A level trim that only sets how loud the result is, applied with the
    // master level but never read as the system being driven harder -- which
    // the master level is, by what an ear would make of the bass. Used to play
    // a quiet room and a loud car back at the same loudness.
    float levelTrimDb = 0.0f;
    // How hard the amplifiers are driven. Past about 0 dB of headroom the
    // clipping model starts to bite, which is audible and deliberate.
    //
    // Six decibels short of it by default. Music is mastered to sit at full
    // scale, so at 0 every speaker spent every song at the very limit of what
    // it can do -- which no one plays a system at -- and the studio monitors and
    // towers audibly broke up. The level trim makes the loudness back up.
    float driveDb = -6.0f;
    // The subwoofer level the installer set, on top of subGainDb (which is
    // yours): set per preset so the subs sit where a system like it is
    // usually tuned, above the midrange.
    float subTrimDb = 0.0f;
    // How much bass, in dB. With subwoofers playing it is their level, on top
    // of subGainDb; without, a bass control as on the amplifier: a shelf on the
    // signal before it reaches any of the speakers, so turning it up drives
    // them harder.
    float bassDb = 0.0f;
    // How much of the bass you would feel in a system this loud is put back.
    //
    // A car playing hard is up around 110 dB, and at that level an ear hears
    // far more bass than it does at the seventy-odd decibels this comes out of
    // a pair of headphones at. That is not a matter of taste: the standardised
    // weightings differ by 36 dB at 31.5 Hz between quiet and loud. Rendering
    // the sound pressure faithfully and then playing it quietly throws all of
    // that away, and the bottom end stops being something you feel.
    //
    // 0 is off and the output is the bare sound pressure. 1 puts back what the
    // level difference costs.
    float bassFeel = 1.0f;

    // The cone itself, heard rather than the air it moves: surround, spider
    // and coil, scraping and flexing in time with the note. Loud enough to
    // hear with your head at the boot of a car and gone from the driver's
    // seat, which is where it was measured. 0 is off, 1 is what was measured,
    // and up to 10 turns it up for hearing it from further away.
    float coneNoise = 1.0f;

    // How much of the room's own character is applied, 0 to 1.5.
    //
    // 1 is the car as it was measured, and the car as it was measured has a
    // fourteen decibel hump from 170 to 280 Hz. That is real -- it is most of
    // why a car sounds like a car -- but it is also the loudest colour in the
    // whole simulation, and whether it reads as "a car" or as "a peak" is a
    // judgement no measurement settles. This is the knob for it. It leaves the
    // cabin's pressure gain, below that, alone: that is physics, not colour.
    //
    // Half by default. At the full fourteen decibels the low mids sat on top of
    // everything, and every car preset sounded boxed in rather than in a car.
    float cabinCharacter = 0.5f;

    // How hard the subwoofer bus is squeezed, 0 to 1. A little of this is what
    // every real installation has, whether the amplifier admits to it or not.
    float subCompression = 0.15f;
    // Whether the cone's own limits are modelled: how far it has to move,
    // the motor giving up and the suspension hardening when it moves too far,
    // the coil heating over seconds, and a port chuffing. Off, a speaker is a
    // filter and stays clean to any level, which is what an EQ would do.
    bool driverLimitsEnabled = true;
    bool roomEnabled = true;     // reflections and reverberation
    bool cabinGainEnabled = true;
};

struct Listener {
    Vec3 position;
    float yawDeg = 0.0f; // 0 faces +y (forward); increases turning right
};

class SpeakerSystem {
public:
    SpeakerSystem();

    // ---- room ----------------------------------------------------------
    // Replaces the room. Speakers on named mounts are moved to the mount of
    // the same id in the new room; any speaker whose mount does not exist
    // there is dropped, and the count of those is returned.
    int SetRoom(const std::string &roomId);
    const RoomSpec &Room() const { return m_room; }
    RoomSpec &MutableRoom() { return m_room; }

    // ---- speakers ------------------------------------------------------
    // Places one speaker. `mountId` may be empty, in which case `position` is
    // used as given. Returns the new speaker id, or 0 if the spec is unknown.
    int AddSpeaker(const std::string &specId, const std::string &mountId,
                   ChannelFeed feed);
    int AddSpeaker(const std::string &specId, const std::string &mountId);

    // Places a stereo pair on a mount and its opposite number, feeding the
    // left and right channels respectively. Returns the two speaker ids, or
    // an empty vector when the mount has no pair.
    std::vector<int> AddPair(const std::string &specId, const std::string &mountId);

    // Replaces the whole speaker list, as when loading a project. Ids and
    // pair ids are taken as given, and the counters are moved past them so
    // that anything added afterwards still gets a fresh id.
    void AdoptSpeakers(std::vector<PlacedSpeaker> speakers);

    bool RemoveSpeaker(int id);
    // Removes a speaker and anything paired with it. Returns how many went.
    int RemovePair(int id);

    PlacedSpeaker *Find(int id);
    const PlacedSpeaker *Find(int id) const;

    std::vector<PlacedSpeaker> &Speakers() { return m_speakers; }
    const std::vector<PlacedSpeaker> &Speakers() const { return m_speakers; }

    // True when at least one speaker is soloed, in which case only soloed
    // speakers are heard.
    bool AnySoloed() const;
    // Whether this speaker should be producing sound right now.
    bool IsAudible(const PlacedSpeaker &s) const;

    // Moves a placed speaker onto a named mount, or to a free position.
    bool MoveToMount(int id, const std::string &mountId);
    bool MoveToPosition(int id, Vec3 position);

    // ---- listener ------------------------------------------------------
    const Listener &GetListener() const { return m_listener; }
    void ResetListener();
    // Steps in the listener's own frame: +forward is where you are facing,
    // +right is to your right. Metres. Clamped inside the room.
    void Walk(float forwardMetres, float rightMetres);
    // Absolute placement, clamped inside the room.
    void SetListenerPosition(Vec3 p);
    void Turn(float degrees);
    void SetYaw(float degrees);

    // ---- settings ------------------------------------------------------
    SystemSettings &Settings() { return m_settings; }
    const SystemSettings &Settings() const { return m_settings; }

    // Bumped whenever anything the engine cares about changes, so the engine
    // can rebuild its filters only when it has to.
    uint64_t Revision() const { return m_revision; }
    void Touch() { ++m_revision; }

private:
    void ApplyMount(PlacedSpeaker &s, const MountPoint &m);
    // Finds where this speaker radiates its top end from, if not from itself.
    void ResolveTweeter(PlacedSpeaker &s) const;
    Vec3 ClampToRoom(Vec3 p) const;

    RoomSpec m_room;
    std::vector<PlacedSpeaker> m_speakers;
    SystemSettings m_settings;
    Listener m_listener;
    int m_nextSpeakerId = 1;
    int m_nextPairId = 1;
    uint64_t m_revision = 1;
};

}  // namespace speakers
