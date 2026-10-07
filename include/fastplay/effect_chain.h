#pragma once
#ifndef FASTPLAY_EFFECT_CHAIN_H
#define FASTPLAY_EFFECT_CHAIN_H

// The effects on one player: reverb (simple or advanced), echo, EQ, compressor,
// stereo width, center cancel, convolution reverb, 3D audio and the normalizer,
// each with its parameters. FastPlay has one, on its player (effects.cpp, which
// adds the effect slider, presets and settings around it); each player of the
// engine library has its own.
//
// Control calls come from one thread at a time (the player's); the effects run
// on its device thread.

#include "types.h"

#include <memory>
#include <string>
#include <vector>

class SpatialAudio;
class ConvolutionReverb;

namespace audio {

class Player;

// Every parameter, in the order the effect slider goes through them (the tempo
// controls first), with its range and default.
const std::vector<ParamDef>& ParamDefs();
const ParamDef* FindParamDef(ParamId id);
// Stable names for parameters and effects ("eq_bass", "reverb"), for presets
// and the library: unlike ParamId's numbers, they never move.
const char* ParamKey(ParamId id);
bool ParamFromKey(const std::string& key, ParamId& id);
// What FastPlay.ini calls it: "EQBass" ([DSPParams]; "Tempo" and the like, [Playback]).
const char* ParamIniName(ParamId id);
const char* EffectKey(DSPEffectType type);
bool EffectFromKey(const std::string& key, DSPEffectType& type);
// The names of a choice parameter's values (the simple reverb's rooms, the
// advanced reverb's environments, the 3D modes), by value; empty for others.
std::vector<std::string> ParamChoices(ParamId id);

class EffectChain {
public:
    EffectChain();
    ~EffectChain();  // out of the player's chain
    EffectChain(const EffectChain&) = delete;
    EffectChain& operator=(const EffectChain&) = delete;

    // The player whose effects these are (null: none).
    void Attach(Player* player);
    // Something new was loaded (the device's rate is known): the enabled effects
    // go into the player's chain, fresh. False if one could not start (3D audio
    // without its HRTF): it is turned off, and `error` says why.
    bool Apply(std::wstring* error = nullptr);
    // Out of the player's chain (before what is loaded goes).
    void Remove();

    // Parameter values, clamped to their ranges and heard at once. The tempo
    // controls (Volume, Pitch, Tempo, Rate) are only kept: the player does those.
    float Get(ParamId id) const;
    void Set(ParamId id, float value);

    // Turning an effect on or off. The reverb is on when it has an algorithm.
    void Enable(DSPEffectType type, bool on);
    bool Enabled(DSPEffectType type) const;
    // 0 off, 1 simple, 2 advanced
    void SetReverbAlgorithm(int algorithm);
    int ReverbAlgorithm() const;

    // The EQ's band centres, in Hz.
    void SetEqFrequencies(float bass, float mid, float treble);

    // The convolution reverb's impulse response (a WAV file).
    bool LoadImpulseResponse(const std::wstring& path, std::wstring& error);
    std::wstring ImpulseResponsePath() const;

    // FastPlay's own 3D audio and convolution reverb, which its settings dialogs
    // reach directly, instead of the chain's own. Before Apply().
    void UseSpatialAudio(SpatialAudio* spatial);
    void UseConvolution(ConvolutionReverb* convolution);
    SpatialAudio* Spatial();

    // A seek: the 3D rooms drop what they were still sounding.
    void ClearTails();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

}  // namespace audio

#endif  // FASTPLAY_EFFECT_CHAIN_H
