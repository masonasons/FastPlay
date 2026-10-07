// The effects on one player: see effect_chain.h.

#include "effect_chain.h"

#include "audio.h"
#include "audio/basic_effects.h"
#include "center_cancel.h"
#include "convolution.h"
#include "reverb/efx_reverb.h"
#include "reverb/reverb.h"
#include "spatial_audio.h"
#include "speakers/presets.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

template <typename T> T clamp_val(T val, T minVal, T maxVal) {
    if (val < minVal) return minVal;
    if (val > maxVal) return maxVal;
    return val;
}

// Parameter definitions
const ParamDef g_paramDefs[] = {
    // Stream effects (DSPEffectType cast to -1 means not a DSP effect)
    {ParamId::Volume,      "Volume",       "%",          0.0f,   4.0f,   0.02f, 1.0f,  (DSPEffectType)-1},
    {ParamId::Pitch,       "Pitch",        " semitones", -12.0f, 12.0f,  1.0f,  0.0f,  (DSPEffectType)-1},
    {ParamId::Tempo,       "Tempo",        "%",          -75.0f, 200.0f, 5.0f,  0.0f,  (DSPEffectType)-1},
    {ParamId::Rate,        "Rate",         "x",          0.25f,  4.0f,   0.01f, 1.0f,  (DSPEffectType)-1},
    // Simple reverb parameters (algorithm 1). The preset comes first: choosing one sets the rest,
    // and presets and settings are loaded in this order.
    {ParamId::ReverbPreset,   "Reverb Room",      "",     0.0f,    21.0f,    1.0f,    0.0f,     DSPEffectType::Reverb},
    {ParamId::ReverbMix,      "Reverb Mix",       "%",    0.0f,    100.0f,   5.0f,    30.0f,    DSPEffectType::Reverb},
    {ParamId::ReverbRoom,     "Reverb Size",      "%",    0.0f,    100.0f,   5.0f,    50.0f,    DSPEffectType::Reverb},
    {ParamId::ReverbDamp,     "Reverb Damping",   "%",    0.0f,    100.0f,   5.0f,    25.0f,    DSPEffectType::Reverb},
    {ParamId::ReverbWidth,    "Reverb Width",     "%",    0.0f,    100.0f,   10.0f,   100.0f,   DSPEffectType::Reverb},
    {ParamId::ReverbPreDelay, "Reverb Pre-delay", " ms",  0.0f,    250.0f,   5.0f,    0.0f,     DSPEffectType::Reverb},
    {ParamId::ReverbLowCut,   "Reverb Low Cut",   " Hz",  0.0f,    1000.0f,  20.0f,   0.0f,     DSPEffectType::Reverb},
    {ParamId::ReverbHighCut,  "Reverb High Cut",  " Hz",  1000.0f, 20000.0f, 1000.0f, 20000.0f, DSPEffectType::Reverb},
    // Advanced reverb parameters (algorithm 2); defaults are the Generic environment
    {ParamId::AdvReverbPreset,      "Reverb Environment",       "",     0.0f,   25.0f,  1.0f,  0.0f,   DSPEffectType::Reverb},
    {ParamId::AdvReverbMix,         "Reverb Mix",               "%",    0.0f,   100.0f, 5.0f,  30.0f,  DSPEffectType::Reverb},
    {ParamId::AdvReverbDecay,       "Reverb Decay",             " s",   0.1f,   20.0f,  0.1f,  1.49f,  DSPEffectType::Reverb},
    {ParamId::AdvReverbHFRatio,     "Reverb High Decay",        "x",    0.1f,   2.0f,   0.05f, 0.83f,  DSPEffectType::Reverb},
    {ParamId::AdvReverbDensity,     "Reverb Density",           "%",    0.0f,   100.0f, 5.0f,  100.0f, DSPEffectType::Reverb},
    {ParamId::AdvReverbDiffusion,   "Reverb Diffusion",         "%",    0.0f,   100.0f, 5.0f,  100.0f, DSPEffectType::Reverb},
    {ParamId::AdvReverbReflections, "Reverb Reflections",       " dB",  -60.0f, 10.0f,  1.0f,  -26.0f, DSPEffectType::Reverb},
    {ParamId::AdvReverbLate,        "Reverb Tail",              " dB",  -60.0f, 20.0f,  1.0f,  2.0f,   DSPEffectType::Reverb},
    {ParamId::AdvReverbReflDelay,   "Reverb Reflections Delay", " ms",  0.0f,   300.0f, 5.0f,  7.0f,   DSPEffectType::Reverb},
    {ParamId::AdvReverbLateDelay,   "Reverb Tail Delay",        " ms",  0.0f,   100.0f, 5.0f,  11.0f,  DSPEffectType::Reverb},
    // Echo parameters
    {ParamId::EchoDelay,   "Echo Delay",   "ms",         10.0f,  2000.0f, 10.0f, 300.0f, DSPEffectType::Echo},
    {ParamId::EchoFeedback,"Echo Feedback","%",          0.0f,   90.0f,  5.0f,  40.0f, DSPEffectType::Echo},
    {ParamId::EchoMix,     "Echo Mix",     "%",          0.0f,   100.0f, 5.0f,  30.0f, DSPEffectType::Echo},
    // EQ parameters (in dB)
    {ParamId::EQPreamp,    "EQ Preamp",    "dB",         -15.0f, 0.0f,   1.0f,  0.0f,  DSPEffectType::EQ},
    {ParamId::EQBass,      "EQ Bass",      "dB",         -15.0f, 15.0f,  1.0f,  0.0f,  DSPEffectType::EQ},
    {ParamId::EQMid,       "EQ Mid",       "dB",         -15.0f, 15.0f,  1.0f,  0.0f,  DSPEffectType::EQ},
    {ParamId::EQTreble,    "EQ Treble",    "dB",         -15.0f, 15.0f,  1.0f,  0.0f,  DSPEffectType::EQ},
    // Compressor parameters
    {ParamId::CompThreshold, "Comp Threshold", "dB",     -60.0f, 0.0f,   3.0f,  -20.0f, DSPEffectType::Compressor},
    {ParamId::CompRatio,     "Comp Ratio",     ":1",     1.0f,   20.0f,  1.0f,  4.0f,   DSPEffectType::Compressor},
    {ParamId::CompAttack,    "Comp Attack",    "ms",     0.01f,  500.0f, 10.0f, 20.0f,  DSPEffectType::Compressor},
    {ParamId::CompRelease,   "Comp Release",   "ms",     10.0f,  2000.0f,50.0f, 200.0f, DSPEffectType::Compressor},
    {ParamId::CompGain,      "Comp Gain",      "dB",     -20.0f, 20.0f,  1.0f,  0.0f,   DSPEffectType::Compressor},
    // Stereo width parameter (0% = mono, 100% = normal, 200% = extra wide)
    {ParamId::StereoWidth,   "Stereo Width",   "%",      0.0f,   200.0f, 10.0f, 100.0f, DSPEffectType::StereoWidth},
    // Center cancel parameter (-100% = extract center, 0% = off, +100% = cancel center)
    {ParamId::CenterCancel,  "Center Cancel",  "%",      -100.0f, 100.0f, 10.0f, 0.0f, DSPEffectType::CenterCancel},
    // Convolution reverb parameters
    {ParamId::ConvolutionMix,  "Conv Mix",   "%",      0.0f, 100.0f, 5.0f, 50.0f, DSPEffectType::Convolution},
    {ParamId::ConvolutionGain, "Conv Gain",  "dB",    -20.0f, 20.0f, 1.0f, 0.0f, DSPEffectType::Convolution},
    // 3D audio parameters
    {ParamId::SpatialBlend,      "3D Blend",       "%",    0.0f,   100.0f,  5.0f,  100.0f, DSPEffectType::SpatialAudio},
    {ParamId::SpatialWidth,      "3D Width",       " deg", 15.0f,  90.0f,   5.0f,  45.0f,  DSPEffectType::SpatialAudio},
    {ParamId::SpatialRotation,   "3D Rotation",    " deg",-180.0f, 180.0f,  5.0f,  0.0f,   DSPEffectType::SpatialAudio},
    {ParamId::SpatialMode,       "3D Mode",        "",     0.0f,   1.0f + speakers::kRoomPresetCount, 1.0f, 0.0f, DSPEffectType::SpatialAudio},
    {ParamId::SpatialRearCenter, "3D Rear Speaker","",     0.0f,   1.0f,    1.0f,  1.0f,   DSPEffectType::SpatialAudio},
    {ParamId::SpatialX,          "3D Listener X",  "",    -50.0f,  50.0f,   1.0f,  0.0f,   DSPEffectType::SpatialAudio},
    {ParamId::SpatialY,          "3D Listener Y",  "",    -50.0f,  50.0f,   1.0f,  0.0f,   DSPEffectType::SpatialAudio},
    {ParamId::SpatialZ,          "3D Listener Z",  "",    -50.0f,  50.0f,   1.0f,  0.0f,   DSPEffectType::SpatialAudio},
    {ParamId::SpatialSub,        "3D Subwoofer",   "",     0.0f,   1.0f,    1.0f,  1.0f,   DSPEffectType::SpatialAudio},
    {ParamId::SpatialCrossover,  "3D Crossover",   " Hz",  40.0f,  160.0f,  10.0f, 80.0f,  DSPEffectType::SpatialAudio},
    {ParamId::SpatialConeNoise,  "3D Cone Noise",  "%",    0.0f,   1000.0f, 25.0f, 100.0f, DSPEffectType::SpatialAudio},
    {ParamId::SpatialBass,       "3D Bass",        " dB", -15.0f,  15.0f,   1.0f,  0.0f,   DSPEffectType::SpatialAudio},
    // Normalizer parameters
    {ParamId::NormTarget,    "Normalizer Target",    " dB", -40.0f, 0.0f,    1.0f,  -3.0f,   DSPEffectType::Normalizer},
    {ParamId::NormLookahead, "Normalizer Lookahead", " ms", 0.0f,   200.0f,  10.0f, 50.0f,   DSPEffectType::Normalizer},
    {ParamId::NormMaxGain,   "Normalizer Max Gain",  " dB", 0.0f,   40.0f,   1.0f,  20.0f,   DSPEffectType::Normalizer},
    {ParamId::NormRelease,   "Normalizer Release",   " ms", 50.0f,  5000.0f, 50.0f, 1000.0f, DSPEffectType::Normalizer},
};
const int g_paramDefCount = sizeof(g_paramDefs) / sizeof(g_paramDefs[0]);

// Stable names: what presets and the library call each parameter
struct ParamName {
    ParamId id;
    const char* key;
    const char* ini;  // FastPlay.ini's name for it ([DSPParams], or [Playback] for the stream controls)
};
const ParamName g_paramKeys[] = {
    {ParamId::Volume, "volume", "Volume"},
    {ParamId::Pitch, "pitch", "Pitch"},
    {ParamId::Tempo, "tempo", "Tempo"},
    {ParamId::Rate, "rate", "Rate"},
    {ParamId::ReverbPreset, "reverb_room", "ReverbPreset"},
    {ParamId::ReverbMix, "reverb_mix", "ReverbMix"},
    {ParamId::ReverbRoom, "reverb_size", "ReverbRoom"},
    {ParamId::ReverbDamp, "reverb_damping", "ReverbDamp"},
    {ParamId::ReverbWidth, "reverb_width", "ReverbWidth"},
    {ParamId::ReverbPreDelay, "reverb_predelay", "ReverbPreDelay"},
    {ParamId::ReverbLowCut, "reverb_low_cut", "ReverbLowCut"},
    {ParamId::ReverbHighCut, "reverb_high_cut", "ReverbHighCut"},
    {ParamId::AdvReverbPreset, "reverb_environment", "AdvReverbPreset"},
    {ParamId::AdvReverbMix, "advanced_reverb_mix", "AdvReverbMix"},
    {ParamId::AdvReverbDecay, "advanced_reverb_decay", "AdvReverbDecay"},
    {ParamId::AdvReverbHFRatio, "advanced_reverb_high_decay", "AdvReverbHFRatio"},
    {ParamId::AdvReverbDensity, "advanced_reverb_density", "AdvReverbDensity"},
    {ParamId::AdvReverbDiffusion, "advanced_reverb_diffusion", "AdvReverbDiffusion"},
    {ParamId::AdvReverbReflections, "advanced_reverb_reflections", "AdvReverbReflections"},
    {ParamId::AdvReverbLate, "advanced_reverb_tail", "AdvReverbLate"},
    {ParamId::AdvReverbReflDelay, "advanced_reverb_reflections_delay", "AdvReverbReflDelay"},
    {ParamId::AdvReverbLateDelay, "advanced_reverb_tail_delay", "AdvReverbLateDelay"},
    {ParamId::EchoDelay, "echo_delay", "EchoDelay"},
    {ParamId::EchoFeedback, "echo_feedback", "EchoFeedback"},
    {ParamId::EchoMix, "echo_mix", "EchoMix"},
    {ParamId::EQPreamp, "eq_preamp", "EQPreamp"},
    {ParamId::EQBass, "eq_bass", "EQBass"},
    {ParamId::EQMid, "eq_mid", "EQMid"},
    {ParamId::EQTreble, "eq_treble", "EQTreble"},
    {ParamId::CompThreshold, "compressor_threshold", "CompThreshold"},
    {ParamId::CompRatio, "compressor_ratio", "CompRatio"},
    {ParamId::CompAttack, "compressor_attack", "CompAttack"},
    {ParamId::CompRelease, "compressor_release", "CompRelease"},
    {ParamId::CompGain, "compressor_gain", "CompGain"},
    {ParamId::StereoWidth, "stereo_width", "StereoWidth"},
    {ParamId::CenterCancel, "center_cancel", "CenterCancel"},
    {ParamId::ConvolutionMix, "convolution_mix", "ConvolutionMix"},
    {ParamId::ConvolutionGain, "convolution_gain", "ConvolutionGain"},
    {ParamId::SpatialBlend, "3d_blend", "SpatialBlend"},
    {ParamId::SpatialWidth, "3d_width", "SpatialWidth"},
    {ParamId::SpatialRotation, "3d_rotation", "SpatialRotation"},
    {ParamId::SpatialMode, "3d_mode", "SpatialMode"},
    {ParamId::SpatialRearCenter, "3d_rear_speaker", "SpatialRearCenter"},
    {ParamId::SpatialX, "3d_listener_x", "SpatialX"},
    {ParamId::SpatialY, "3d_listener_y", "SpatialY"},
    {ParamId::SpatialZ, "3d_listener_z", "SpatialZ"},
    {ParamId::SpatialSub, "3d_subwoofer", "SpatialSub"},
    {ParamId::SpatialCrossover, "3d_crossover", "SpatialCrossover"},
    {ParamId::SpatialConeNoise, "3d_cone_noise", "SpatialConeNoise"},
    {ParamId::SpatialBass, "3d_bass", "SpatialBass"},
    {ParamId::NormTarget, "normalizer_target", "NormTarget"},
    {ParamId::NormLookahead, "normalizer_lookahead", "NormLookahead"},
    {ParamId::NormMaxGain, "normalizer_max_gain", "NormMaxGain"},
    {ParamId::NormRelease, "normalizer_release", "NormRelease"},
};

const char* const g_effectKeys[] = {"reverb",        "echo",        "eq",      "compressor", "stereo_width",
                                    "center_cancel", "convolution", "3d_audio", "normalizer"};
static_assert(sizeof(g_effectKeys) / sizeof(g_effectKeys[0]) == static_cast<size_t>(DSPEffectType::COUNT),
              "an effect without a name");

// Rooms for the simple reverb.
struct SimpleReverbPreset {
    const char* name;
    float room, damping, preDelayMs, width, lowCutHz, highCutHz;
    float level;  // the preset's wet level relative to the default (1 = default)
};
const SimpleReverbPreset g_simpleReverbPresets[] = {
    {"Generic",          0.5f,  0.25f, 0,   1.0f, 0,   0,    1.0f},
    {"Small room",       0.0f,  0.6f,  4,   1.0f, 0,   0,    1.0f},
    {"Medium room",      0.39f, 0.5f,  8,   1.0f, 0,   0,    1.0f},
    {"Large room",       0.67f, 0.45f, 14,  1.0f, 0,   0,    1.0f},
    {"Bathroom",         0.57f, 0.1f,  3,   1.0f, 0,   0,    1.0f},
    {"Living room",      0.0f,  0.8f,  6,   1.0f, 0,   6000, 0.75f},
    {"Stone room",       0.74f, 0.2f,  10,  1.0f, 0,   0,    1.0f},
    {"Hallway",          0.6f,  0.4f,  7,   0.7f, 0,   0,    1.0f},
    {"Carpeted hallway", 0.0f,  0.9f,  7,   1.0f, 0,   4000, 0.6f},
    {"Auditorium",       0.84f, 0.4f,  20,  1.0f, 0,   0,    1.0f},
    {"Concert hall",     0.88f, 0.35f, 24,  1.0f, 0,   0,    1.0f},
    {"Cave",             0.82f, 0.15f, 15,  1.0f, 0,   0,    1.0f},
    {"Arena",            0.97f, 0.3f,  30,  1.0f, 0,   0,    1.0f},
    {"Hangar",           1.0f,  0.35f, 30,  1.0f, 0,   0,    1.0f},
    {"Forest",           0.6f,  0.8f,  40,  1.0f, 0,   5000, 0.45f},
    {"City",             0.6f,  0.7f,  20,  1.0f, 0,   6000, 0.45f},
    {"Mountains",        0.6f,  0.7f,  120, 1.0f, 0,   0,    0.3f},
    {"Parking lot",      0.64f, 0.5f,  25,  1.0f, 0,   0,    0.6f},
    {"Sewer pipe",       0.81f, 0.2f,  10,  0.4f, 150, 5000, 1.0f},
    {"Underwater",       0.6f,  1.0f,  5,   1.0f, 0,   1200, 1.0f},
    {"Cathedral",        0.95f, 0.3f,  35,  1.0f, 0,   0,    1.0f},
    {"Plate",            0.71f, 0.2f,  0,   1.0f, 0,   0,    1.0f},
};
const int g_simpleReverbPresetCount = sizeof(g_simpleReverbPresets) / sizeof(g_simpleReverbPresets[0]);

// Environments for the advanced reverb: the EFX default presets (OpenAL Soft's efx-presets.h),
// field for field in EfxReverbParams order.
struct AdvancedReverbPreset {
    const char* name;
    fastplay::audio::EfxReverbParams params;
};
const AdvancedReverbPreset g_advancedReverbPresets[] = {
    {"Generic", { 1.0000f, 1.0000f, 0.3162f, 0.8913f, 1.0000f, 1.4900f, 0.8300f, 1.0000f, 0.0500f, 0.0070f, { 0.0000f, 0.0000f, 0.0000f }, 1.2589f, 0.0110f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Padded cell", { 0.1715f, 1.0000f, 0.3162f, 0.0010f, 1.0000f, 0.1700f, 0.1000f, 1.0000f, 0.2500f, 0.0010f, { 0.0000f, 0.0000f, 0.0000f }, 1.2691f, 0.0020f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Room", { 0.4287f, 1.0000f, 0.3162f, 0.5929f, 1.0000f, 0.4000f, 0.8300f, 1.0000f, 0.1503f, 0.0020f, { 0.0000f, 0.0000f, 0.0000f }, 1.0629f, 0.0030f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Bathroom", { 0.1715f, 1.0000f, 0.3162f, 0.2512f, 1.0000f, 1.4900f, 0.5400f, 1.0000f, 0.6531f, 0.0070f, { 0.0000f, 0.0000f, 0.0000f }, 3.2734f, 0.0110f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Living room", { 0.9766f, 1.0000f, 0.3162f, 0.0010f, 1.0000f, 0.5000f, 0.1000f, 1.0000f, 0.2051f, 0.0030f, { 0.0000f, 0.0000f, 0.0000f }, 0.2805f, 0.0040f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Stone room", { 1.0000f, 1.0000f, 0.3162f, 0.7079f, 1.0000f, 2.3100f, 0.6400f, 1.0000f, 0.4411f, 0.0120f, { 0.0000f, 0.0000f, 0.0000f }, 1.1003f, 0.0170f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Auditorium", { 1.0000f, 1.0000f, 0.3162f, 0.5781f, 1.0000f, 4.3200f, 0.5900f, 1.0000f, 0.4032f, 0.0200f, { 0.0000f, 0.0000f, 0.0000f }, 0.7170f, 0.0300f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Concert hall", { 1.0000f, 1.0000f, 0.3162f, 0.5623f, 1.0000f, 3.9200f, 0.7000f, 1.0000f, 0.2427f, 0.0200f, { 0.0000f, 0.0000f, 0.0000f }, 0.9977f, 0.0290f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Cave", { 1.0000f, 1.0000f, 0.3162f, 1.0000f, 1.0000f, 2.9100f, 1.3000f, 1.0000f, 0.5000f, 0.0150f, { 0.0000f, 0.0000f, 0.0000f }, 0.7063f, 0.0220f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, false }},
    {"Arena", { 1.0000f, 1.0000f, 0.3162f, 0.4477f, 1.0000f, 7.2400f, 0.3300f, 1.0000f, 0.2612f, 0.0200f, { 0.0000f, 0.0000f, 0.0000f }, 1.0186f, 0.0300f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Hangar", { 1.0000f, 1.0000f, 0.3162f, 0.3162f, 1.0000f, 10.0500f, 0.2300f, 1.0000f, 0.5000f, 0.0200f, { 0.0000f, 0.0000f, 0.0000f }, 1.2560f, 0.0300f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Carpeted hallway", { 0.4287f, 1.0000f, 0.3162f, 0.0100f, 1.0000f, 0.3000f, 0.1000f, 1.0000f, 0.1215f, 0.0020f, { 0.0000f, 0.0000f, 0.0000f }, 0.1531f, 0.0300f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Hallway", { 0.3645f, 1.0000f, 0.3162f, 0.7079f, 1.0000f, 1.4900f, 0.5900f, 1.0000f, 0.2458f, 0.0070f, { 0.0000f, 0.0000f, 0.0000f }, 1.6615f, 0.0110f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Stone corridor", { 1.0000f, 1.0000f, 0.3162f, 0.7612f, 1.0000f, 2.7000f, 0.7900f, 1.0000f, 0.2472f, 0.0130f, { 0.0000f, 0.0000f, 0.0000f }, 1.5758f, 0.0200f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Alley", { 1.0000f, 0.3000f, 0.3162f, 0.7328f, 1.0000f, 1.4900f, 0.8600f, 1.0000f, 0.2500f, 0.0070f, { 0.0000f, 0.0000f, 0.0000f }, 0.9954f, 0.0110f, { 0.0000f, 0.0000f, 0.0000f }, 0.1250f, 0.9500f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Forest", { 1.0000f, 0.3000f, 0.3162f, 0.0224f, 1.0000f, 1.4900f, 0.5400f, 1.0000f, 0.0525f, 0.1620f, { 0.0000f, 0.0000f, 0.0000f }, 0.7682f, 0.0880f, { 0.0000f, 0.0000f, 0.0000f }, 0.1250f, 1.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"City", { 1.0000f, 0.5000f, 0.3162f, 0.3981f, 1.0000f, 1.4900f, 0.6700f, 1.0000f, 0.0730f, 0.0070f, { 0.0000f, 0.0000f, 0.0000f }, 0.1427f, 0.0110f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Mountains", { 1.0000f, 0.2700f, 0.3162f, 0.0562f, 1.0000f, 1.4900f, 0.2100f, 1.0000f, 0.0407f, 0.3000f, { 0.0000f, 0.0000f, 0.0000f }, 0.1919f, 0.1000f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 1.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, false }},
    {"Quarry", { 1.0000f, 1.0000f, 0.3162f, 0.3162f, 1.0000f, 1.4900f, 0.8300f, 1.0000f, 0.0000f, 0.0610f, { 0.0000f, 0.0000f, 0.0000f }, 1.7783f, 0.0250f, { 0.0000f, 0.0000f, 0.0000f }, 0.1250f, 0.7000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Plain", { 1.0000f, 0.2100f, 0.3162f, 0.1000f, 1.0000f, 1.4900f, 0.5000f, 1.0000f, 0.0585f, 0.1790f, { 0.0000f, 0.0000f, 0.0000f }, 0.1089f, 0.1000f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 1.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Parking lot", { 1.0000f, 1.0000f, 0.3162f, 1.0000f, 1.0000f, 1.6500f, 1.5000f, 1.0000f, 0.2082f, 0.0080f, { 0.0000f, 0.0000f, 0.0000f }, 0.2652f, 0.0120f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, false }},
    {"Sewer pipe", { 0.3071f, 0.8000f, 0.3162f, 0.3162f, 1.0000f, 2.8100f, 0.1400f, 1.0000f, 1.6387f, 0.0140f, { 0.0000f, 0.0000f, 0.0000f }, 3.2471f, 0.0210f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Underwater", { 0.3645f, 1.0000f, 0.3162f, 0.0100f, 1.0000f, 1.4900f, 0.1000f, 1.0000f, 0.5963f, 0.0070f, { 0.0000f, 0.0000f, 0.0000f }, 7.0795f, 0.0110f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 1.1800f, 0.3480f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Drugged", { 0.4287f, 0.5000f, 0.3162f, 1.0000f, 1.0000f, 8.3900f, 1.3900f, 1.0000f, 0.8760f, 0.0020f, { 0.0000f, 0.0000f, 0.0000f }, 3.1081f, 0.0300f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 1.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, false }},
    {"Dizzy", { 0.3645f, 0.6000f, 0.3162f, 0.6310f, 1.0000f, 17.2300f, 0.5600f, 1.0000f, 0.1392f, 0.0200f, { 0.0000f, 0.0000f, 0.0000f }, 0.4937f, 0.0300f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 1.0000f, 0.8100f, 0.3100f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, false }},
    {"Psychotic", { 0.0625f, 0.5000f, 0.3162f, 0.8404f, 1.0000f, 7.5600f, 0.9100f, 1.0000f, 0.4864f, 0.0200f, { 0.0000f, 0.0000f, 0.0000f }, 2.4378f, 0.0300f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 4.0000f, 1.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, false }},
};
const int g_advancedReverbPresetCount = sizeof(g_advancedReverbPresets) / sizeof(g_advancedReverbPresets[0]);

// High cut at its maximum means off.
const float kReverbHighCutOff = 20000.0f;

// Mix: 0% is dry only, 50% is the dry signal at full level with the reverb at its natural level,
// 100% is the reverb only.
void ReverbMixGains(float mixPercent, float& dry, float& wet) {
    float m = clamp_val(mixPercent / 100.0f, 0.0f, 1.0f);
    dry = std::min(1.0f, 2.0f * (1.0f - m));
    wet = std::min(1.0f, 2.0f * m);
}

float GainToDb(float gain, float minDb, float maxDb) {
    float db = gain > 0.0f ? 20.0f * std::log10(gain) : minDb;
    return clamp_val(std::round(db), minDb, maxDb);
}

}  // namespace

namespace audio {

const std::vector<ParamDef>& ParamDefs() {
    static const std::vector<ParamDef> defs(std::begin(g_paramDefs), std::end(g_paramDefs));
    return defs;
}

const ParamDef* FindParamDef(ParamId id) {
    for (int i = 0; i < g_paramDefCount; i++) {
        if (g_paramDefs[i].id == id) return &g_paramDefs[i];
    }
    return nullptr;
}

const char* ParamKey(ParamId id) {
    for (const auto& p : g_paramKeys) {
        if (p.id == id) return p.key;
    }
    return "";
}

const char* ParamIniName(ParamId id) {
    for (const auto& p : g_paramKeys) {
        if (p.id == id) return p.ini;
    }
    return "";
}

bool ParamFromKey(const std::string& key, ParamId& id) {
    for (const auto& p : g_paramKeys) {
        if (key == p.key) {
            id = p.id;
            return true;
        }
    }
    return false;
}

const char* EffectKey(DSPEffectType type) {
    int i = static_cast<int>(type);
    return i >= 0 && i < static_cast<int>(DSPEffectType::COUNT) ? g_effectKeys[i] : "";
}

bool EffectFromKey(const std::string& key, DSPEffectType& type) {
    for (int i = 0; i < static_cast<int>(DSPEffectType::COUNT); i++) {
        if (key == g_effectKeys[i]) {
            type = static_cast<DSPEffectType>(i);
            return true;
        }
    }
    return false;
}

std::vector<std::string> ParamChoices(ParamId id) {
    std::vector<std::string> names;
    if (id == ParamId::ReverbPreset) {
        for (const auto& p : g_simpleReverbPresets) names.push_back(p.name);
    } else if (id == ParamId::AdvReverbPreset) {
        for (const auto& p : g_advancedReverbPresets) names.push_back(p.name);
    } else if (id == ParamId::SpatialMode) {
        names.push_back("Binaural");
        names.push_back("5.1 Surround");
        for (int i = 0; i < speakers::kRoomPresetCount; i++) names.push_back(speakers::RoomPresetName(i));
    } else if (id == ParamId::SpatialRearCenter || id == ParamId::SpatialSub) {
        names.push_back("Off");
        names.push_back("On");
    }
    return names;
}

// ---------------------------------------------------------------------------
// The chain
// ---------------------------------------------------------------------------

struct EffectChain::Impl {
    Player* player = nullptr;
    float values[static_cast<int>(ParamId::COUNT)] = {};
    bool enabled[static_cast<int>(DSPEffectType::COUNT)] = {};
    int reverbAlgorithm = 0;
    float eqBassHz = 50.0f, eqMidHz = 1000.0f, eqTrebleHz = 12000.0f;

    // Each effect's place in the player's chain (0: not in it)
    int dsp[static_cast<int>(DSPEffectType::COUNT)] = {};

    Echo echo;
    Gain eqPreamp;
    PeakingEq eqBass, eqMid, eqTreble;
    Compressor compressor;
    Normalizer normalizer;
    CenterCancelProcessor centerCancel;
    std::vector<float> centerOut;

    // The reverbs: the simple FDN and the EFX model, run as one effect
    struct Reverb {
        std::mutex mutex;  // params are set from the control thread, processing runs on the audio thread
        fastplay::audio::Reverb simple;
        fastplay::audio::EfxReverb advanced;
        fastplay::audio::ReverbParams simpleParams;
        fastplay::audio::EfxReverbParams advancedParams;
        int algorithm = 0;
        int sampleRate = 0;
        float dry = 1.0f;          // target dry gain
        float dryCurrent = 1.0f;   // ramped towards dry once per block
        std::vector<float> inL, inR, outL, outR;
    } reverb;

    // 3D audio and the convolution reverb: the chain's own, or the app's
    std::unique_ptr<SpatialAudio> ownSpatial;
    SpatialAudio* spatial = nullptr;
    std::unique_ptr<ConvolutionReverb> ownConvolution;
    ConvolutionReverb* convolution = nullptr;

    float V(ParamId id) const { return values[static_cast<int>(id)]; }

    SpatialAudio* Spatial() {
        if (!spatial) {
            ownSpatial = std::make_unique<SpatialAudio>();
            spatial = ownSpatial.get();
            ApplySpatialSettings();
        }
        return spatial;
    }

    ConvolutionReverb* Convolution() {
        if (!convolution) {
            ownConvolution = std::make_unique<ConvolutionReverb>();
            convolution = ownConvolution.get();
        }
        return convolution;
    }

    int ReverbPresetIndex(ParamId id, int count) const {
        int i = static_cast<int>(std::lround(V(id)));
        return clamp_val(i, 0, count - 1);
    }

    // Copy a preset's values into the parameters that can be adjusted.
    void ApplySimpleReverbPreset(int index) {
        const SimpleReverbPreset& p = g_simpleReverbPresets[clamp_val(index, 0, g_simpleReverbPresetCount - 1)];
        values[(int)ParamId::ReverbRoom] = p.room * 100.0f;
        values[(int)ParamId::ReverbDamp] = p.damping * 100.0f;
        values[(int)ParamId::ReverbWidth] = p.width * 100.0f;
        values[(int)ParamId::ReverbPreDelay] = p.preDelayMs;
        values[(int)ParamId::ReverbLowCut] = p.lowCutHz;
        values[(int)ParamId::ReverbHighCut] = p.highCutHz > 0 ? p.highCutHz : kReverbHighCutOff;
    }

    void ApplyAdvancedReverbPreset(int index) {
        const fastplay::audio::EfxReverbParams& p =
            g_advancedReverbPresets[clamp_val(index, 0, g_advancedReverbPresetCount - 1)].params;
        values[(int)ParamId::AdvReverbDecay] = p.decay_time;
        values[(int)ParamId::AdvReverbHFRatio] = p.decay_hf_ratio;
        values[(int)ParamId::AdvReverbDensity] = p.density * 100.0f;
        values[(int)ParamId::AdvReverbDiffusion] = p.diffusion * 100.0f;
        values[(int)ParamId::AdvReverbReflections] = GainToDb(p.reflections_gain, -60.0f, 10.0f);
        values[(int)ParamId::AdvReverbLate] = GainToDb(p.late_reverb_gain, -60.0f, 20.0f);
        values[(int)ParamId::AdvReverbReflDelay] = p.reflections_delay * 1000.0f;
        values[(int)ParamId::AdvReverbLateDelay] = p.late_reverb_delay * 1000.0f;
    }

    // Rebuild both reverbs' settings from the parameter values.
    void UpdateReverb() {
        float dry, wet;

        fastplay::audio::ReverbParams sp;
        const SimpleReverbPreset& room =
            g_simpleReverbPresets[ReverbPresetIndex(ParamId::ReverbPreset, g_simpleReverbPresetCount)];
        ReverbMixGains(V(ParamId::ReverbMix), dry, wet);
        float simpleDry = dry;
        sp.room_size = V(ParamId::ReverbRoom) / 100.0f;
        sp.damping = V(ParamId::ReverbDamp) / 100.0f;
        sp.width = V(ParamId::ReverbWidth) / 100.0f;
        sp.pre_delay_ms = V(ParamId::ReverbPreDelay);
        sp.low_cut_hz = V(ParamId::ReverbLowCut);
        sp.high_cut_hz = V(ParamId::ReverbHighCut) >= kReverbHighCutOff ? 0.0f : V(ParamId::ReverbHighCut);
        sp.wet = wet * room.level / 3.0f;  // the reverb's wet is scaled so 1/3 is unity
        sp.dry = 0.0f;                     // the dry signal is mixed here, not by the reverb

        fastplay::audio::EfxReverbParams ap =
            g_advancedReverbPresets[ReverbPresetIndex(ParamId::AdvReverbPreset, g_advancedReverbPresetCount)].params;
        ReverbMixGains(V(ParamId::AdvReverbMix), dry, wet);
        float advancedDry = dry;
        ap.decay_time = V(ParamId::AdvReverbDecay);
        ap.decay_hf_ratio = V(ParamId::AdvReverbHFRatio);
        ap.density = V(ParamId::AdvReverbDensity) / 100.0f;
        ap.diffusion = V(ParamId::AdvReverbDiffusion) / 100.0f;
        ap.reflections_gain = std::pow(10.0f, V(ParamId::AdvReverbReflections) / 20.0f);
        ap.late_reverb_gain = std::pow(10.0f, V(ParamId::AdvReverbLate) / 20.0f);
        ap.reflections_delay = V(ParamId::AdvReverbReflDelay) / 1000.0f;
        ap.late_reverb_delay = V(ParamId::AdvReverbLateDelay) / 1000.0f;
        ap.gain *= wet;

        std::lock_guard<std::mutex> lock(reverb.mutex);
        reverb.algorithm = reverbAlgorithm;
        reverb.simpleParams = sp;
        reverb.advancedParams = ap;
        reverb.dry = reverbAlgorithm == 2 ? advancedDry : simpleDry;
        if (reverb.sampleRate > 0) {
            reverb.simple.set_params(sp);
            reverb.advanced.set_params(ap);
        }
    }

    // Start the reverbs fresh at a stream's sample rate (no tail carried over from the last track).
    void ResetReverb(int sampleRate) {
        std::lock_guard<std::mutex> lock(reverb.mutex);
        if (sampleRate != reverb.sampleRate) {
            reverb.simple.init(sampleRate);
            reverb.advanced.init(sampleRate);
            reverb.sampleRate = sampleRate;
        }
        reverb.simple.set_params(reverb.simpleParams);
        reverb.advanced.set_params(reverb.advancedParams);
        reverb.simple.reset();
        reverb.advanced.reset();
        reverb.dryCurrent = reverb.dry;
    }

    void UpdateEcho() {
        Echo::Params p;
        p.dry = 1.0f - (V(ParamId::EchoMix) / 100.0f);
        p.wet = V(ParamId::EchoMix) / 100.0f;
        p.feedback = V(ParamId::EchoFeedback) / 100.0f;
        p.delay = V(ParamId::EchoDelay) / 1000.0f;  // ms to seconds
        p.stereo = true;
        echo.Set(p);
    }

    void UpdateEQ() {
        eqPreamp.Set(powf(10.0f, V(ParamId::EQPreamp) / 20.0f));
        eqBass.Set(eqBassHz, 2.5f, V(ParamId::EQBass));
        eqMid.Set(eqMidHz, 2.5f, V(ParamId::EQMid));
        eqTreble.Set(eqTrebleHz, 2.5f, V(ParamId::EQTreble));
    }

    void UpdateCompressor() {
        Compressor::Params p;
        p.gainDb = V(ParamId::CompGain);
        p.thresholdDb = V(ParamId::CompThreshold);
        p.ratio = V(ParamId::CompRatio);
        p.attackMs = V(ParamId::CompAttack);
        p.releaseMs = V(ParamId::CompRelease);
        compressor.Set(p);
    }

    void UpdateNormalizer() {
        Normalizer::Params p;
        p.targetDb = V(ParamId::NormTarget);
        p.lookaheadMs = V(ParamId::NormLookahead);
        p.maxGainDb = V(ParamId::NormMaxGain);
        p.releaseMs = V(ParamId::NormRelease);
        normalizer.Set(p);
    }

    // Everything the 3D audio is set to, onto it
    void ApplySpatialSettings() {
        if (!spatial) return;
        int mode = static_cast<int>(V(ParamId::SpatialMode) + 0.5f);
        if (mode >= 2) {
            spatial->SetRoomPreset(mode - 2);
            spatial->SetMode(SpatialMode::Speakers);
        } else {
            spatial->SetMode(mode == 1 ? SpatialMode::Surround51 : SpatialMode::Binaural);
        }
        spatial->SetRearCenter(V(ParamId::SpatialRearCenter) >= 0.5f);
        spatial->SetSubwoofer(V(ParamId::SpatialSub) >= 0.5f);
        spatial->SetCrossover(V(ParamId::SpatialCrossover));
        spatial->SetConeNoise(V(ParamId::SpatialConeNoise) / 100.0f);
        spatial->SetBass(V(ParamId::SpatialBass));
        spatial->SetWidth(V(ParamId::SpatialWidth));
        spatial->SetRotation(V(ParamId::SpatialRotation));
        spatial->SetListenerOffset(V(ParamId::SpatialX), V(ParamId::SpatialY), V(ParamId::SpatialZ));
    }

    // ---- on the device thread ----

    static void ReverbProc(float* samples, int frames, int chans, int, void* user) {
        Reverb& e = static_cast<Impl*>(user)->reverb;
        std::lock_guard<std::mutex> lock(e.mutex);
        if (e.algorithm == 0 || e.sampleRate <= 0 || frames <= 0) return;

        e.inL.resize(frames);
        e.inR.resize(frames);
        e.outL.assign(frames, 0.0f);
        e.outR.assign(frames, 0.0f);
        for (int i = 0; i < frames; i++) {
            e.inL[i] = samples[i * chans];
            e.inR[i] = samples[i * chans + 1];
        }

        if (e.algorithm == 2) {
            e.advanced.process(e.inL.data(), e.inR.data(), e.outL.data(), e.outR.data(), frames);
        } else {
            e.simple.process(e.inL.data(), e.inR.data(), e.outL.data(), e.outR.data(), frames);
        }

        // Dry gain ramps across the block so moving the mix does not click.
        const float dry0 = e.dryCurrent, dryStep = (e.dry - e.dryCurrent) / frames;
        for (int i = 0; i < frames; i++) {
            const float dry = dry0 + dryStep * (i + 1);
            float* frame = samples + static_cast<size_t>(i) * chans;
            frame[0] = frame[0] * dry + e.outL[i];
            frame[1] = frame[1] * dry + e.outR[i];
        }
        e.dryCurrent = e.dry;
    }

    static void EchoProc(float* samples, int frames, int, int sampleRate, void* user) {
        static_cast<Impl*>(user)->echo.Process(samples, frames, sampleRate);
    }

    static void EQProc(float* samples, int frames, int, int sampleRate, void* user) {
        Impl* m = static_cast<Impl*>(user);
        m->eqPreamp.Process(samples, frames);
        m->eqBass.Process(samples, frames, sampleRate);
        m->eqMid.Process(samples, frames, sampleRate);
        m->eqTreble.Process(samples, frames, sampleRate);
    }

    static void CompressorProc(float* samples, int frames, int, int sampleRate, void* user) {
        static_cast<Impl*>(user)->compressor.Process(samples, frames, sampleRate);
    }

    static void NormalizerProc(float* samples, int frames, int, int sampleRate, void* user) {
        static_cast<Impl*>(user)->normalizer.Process(samples, frames, sampleRate);
    }

    // Mid/side: 0% = mono, 100% = normal stereo, 200% = extra wide
    static void StereoWidthProc(float* samples, int frames, int, int, void* user) {
        float width = static_cast<Impl*>(user)->V(ParamId::StereoWidth) / 100.0f;
        for (int i = 0; i < frames; i++) {
            float left = samples[i * 2];
            float right = samples[i * 2 + 1];
            float mid = (left + right) * 0.5f;
            float side = (left - right) * 0.5f * width;
            samples[i * 2] = mid + side;
            samples[i * 2 + 1] = mid - side;
        }
    }

    // -100% = extract the center (isolate vocals), 0% = nothing, +100% = cancel it (remove vocals)
    static void CenterCancelProc(float* samples, int frames, int, int sampleRate, void* user) {
        Impl* m = static_cast<Impl*>(user);
        if (!m->centerCancel.IsInitialized()) m->centerCancel.Init(sampleRate);
        if (!m->centerCancel.IsInitialized()) return;
        m->centerCancel.SetAmount(m->V(ParamId::CenterCancel) / 100.0f);
        m->centerOut.resize(static_cast<size_t>(frames) * 2);
        int outputFrames = 0;
        m->centerCancel.ProcessFloat(samples, frames, m->centerOut.data(), outputFrames);
        // outputFrames equals frames in the steady state
        std::copy(m->centerOut.begin(), m->centerOut.begin() + static_cast<size_t>(outputFrames) * 2, samples);
    }

    static void ConvolutionProc(float* samples, int frames, int, int sampleRate, void* user) {
        Impl* m = static_cast<Impl*>(user);
        ConvolutionReverb* conv = m->convolution;
        if (!conv) return;
        // Loaded since it went in: ready it now
        if (conv->IsLoaded() && !conv->IsInitialized()) conv->Init(sampleRate);
        conv->SetMix(m->V(ParamId::ConvolutionMix));
        conv->SetGain(m->V(ParamId::ConvolutionGain));
        conv->Process(samples, frames);
    }

    static void SpatialProc(float* samples, int frames, int, int, void* user) {
        Impl* m = static_cast<Impl*>(user);
        SpatialAudio* spatial = m->spatial;
        if (!spatial || !spatial->IsInitialized()) return;
        float blend = m->V(ParamId::SpatialBlend) / 100.0f;
        if (blend <= 0.0f) return;
        spatial->Process(samples, frames, blend);
    }

    // ---- in and out of the player's chain ----

    void RemoveOne(DSPEffectType type) {
        int& id = dsp[static_cast<int>(type)];
        if (id && player) player->RemoveDsp(id);
        id = 0;
    }

    bool InChain(DSPEffectType type) const { return dsp[static_cast<int>(type)] != 0; }

    void Add(DSPEffectType type, DspProc proc, int priority = 0) {
        dsp[static_cast<int>(type)] = player->AddDsp(proc, this, priority);
    }
};

EffectChain::EffectChain() : m(std::make_unique<Impl>()) {
    for (const auto& def : g_paramDefs) m->values[static_cast<int>(def.id)] = def.defaultValue;
    m->UpdateReverb();
    m->UpdateEcho();
    m->UpdateEQ();
    m->UpdateCompressor();
    m->UpdateNormalizer();
}

EffectChain::~EffectChain() { Remove(); }

void EffectChain::Attach(Player* player) {
    if (player == m->player) return;
    Remove();
    m->player = player;
}

bool EffectChain::Apply(std::wstring* error) {
    Player* player = m->player;
    if (!player || !player->IsLoaded()) return true;
    const int sampleRate = player->MixSampleRate();
    bool ok = true;

    // They run in the order added, all at the same priority but the normalizer.
    if (m->reverbAlgorithm > 0 && !m->InChain(DSPEffectType::Reverb)) {
        m->ResetReverb(sampleRate);
        m->Add(DSPEffectType::Reverb, &Impl::ReverbProc);
    }
    if (m->enabled[(int)DSPEffectType::Echo] && !m->InChain(DSPEffectType::Echo)) {
        m->UpdateEcho();
        m->Add(DSPEffectType::Echo, &Impl::EchoProc);
    }
    if (m->enabled[(int)DSPEffectType::EQ] && !m->InChain(DSPEffectType::EQ)) {
        m->UpdateEQ();
        m->Add(DSPEffectType::EQ, &Impl::EQProc);
    }
    if (m->enabled[(int)DSPEffectType::Compressor] && !m->InChain(DSPEffectType::Compressor)) {
        m->UpdateCompressor();
        m->Add(DSPEffectType::Compressor, &Impl::CompressorProc);
    }
    if (m->enabled[(int)DSPEffectType::StereoWidth] && !m->InChain(DSPEffectType::StereoWidth)) {
        m->Add(DSPEffectType::StereoWidth, &Impl::StereoWidthProc);
    }
    if (m->enabled[(int)DSPEffectType::CenterCancel] && !m->InChain(DSPEffectType::CenterCancel)) {
        m->Add(DSPEffectType::CenterCancel, &Impl::CenterCancelProc);
    }
    if (m->enabled[(int)DSPEffectType::Convolution] && !m->InChain(DSPEffectType::Convolution)) {
        ConvolutionReverb* conv = m->Convolution();
        if (conv->IsLoaded()) conv->Init(sampleRate);
        m->Add(DSPEffectType::Convolution, &Impl::ConvolutionProc);
    }
    if (m->enabled[(int)DSPEffectType::SpatialAudio] && !m->InChain(DSPEffectType::SpatialAudio)) {
        SpatialAudio* spatial = m->Spatial();
        if (spatial->Initialize(sampleRate)) {
            m->ApplySpatialSettings();
            m->Add(DSPEffectType::SpatialAudio, &Impl::SpatialProc);
        } else {
            if (error) *error = spatial->GetLastError();
            m->enabled[(int)DSPEffectType::SpatialAudio] = false;
            ok = false;
        }
    }
    // The normalizer last, so it holds the level of everything before it
    if (m->enabled[(int)DSPEffectType::Normalizer] && !m->InChain(DSPEffectType::Normalizer)) {
        m->UpdateNormalizer();
        m->Add(DSPEffectType::Normalizer, &Impl::NormalizerProc, -10);
    }
    return ok;
}

void EffectChain::Remove() {
    for (int i = 0; i < static_cast<int>(DSPEffectType::COUNT); i++) m->RemoveOne(static_cast<DSPEffectType>(i));
}

float EffectChain::Get(ParamId id) const {
    int i = static_cast<int>(id);
    return i >= 0 && i < static_cast<int>(ParamId::COUNT) ? m->values[i] : 0.0f;
}

void EffectChain::Set(ParamId id, float value) {
    const ParamDef* def = FindParamDef(id);
    if (!def) return;
    m->values[static_cast<int>(id)] = clamp_val(value, def->minValue, def->maxValue);

    switch (id) {
        // A preset sets the others, then both reverbs are updated
        case ParamId::ReverbPreset:
            m->ApplySimpleReverbPreset(m->ReverbPresetIndex(id, g_simpleReverbPresetCount));
            m->UpdateReverb();
            break;
        case ParamId::AdvReverbPreset:
            m->ApplyAdvancedReverbPreset(m->ReverbPresetIndex(id, g_advancedReverbPresetCount));
            m->UpdateReverb();
            break;
        case ParamId::ReverbMix:
        case ParamId::ReverbRoom:
        case ParamId::ReverbDamp:
        case ParamId::ReverbWidth:
        case ParamId::ReverbPreDelay:
        case ParamId::ReverbLowCut:
        case ParamId::ReverbHighCut:
        case ParamId::AdvReverbMix:
        case ParamId::AdvReverbDecay:
        case ParamId::AdvReverbHFRatio:
        case ParamId::AdvReverbDensity:
        case ParamId::AdvReverbDiffusion:
        case ParamId::AdvReverbReflections:
        case ParamId::AdvReverbLate:
        case ParamId::AdvReverbReflDelay:
        case ParamId::AdvReverbLateDelay:
            m->UpdateReverb();
            break;
        case ParamId::EchoDelay:
        case ParamId::EchoFeedback:
        case ParamId::EchoMix:
            m->UpdateEcho();
            break;
        case ParamId::EQPreamp:
        case ParamId::EQBass:
        case ParamId::EQMid:
        case ParamId::EQTreble:
            m->UpdateEQ();
            break;
        case ParamId::CompThreshold:
        case ParamId::CompRatio:
        case ParamId::CompAttack:
        case ParamId::CompRelease:
        case ParamId::CompGain:
            m->UpdateCompressor();
            break;
        case ParamId::NormTarget:
        case ParamId::NormLookahead:
        case ParamId::NormMaxGain:
        case ParamId::NormRelease:
            m->UpdateNormalizer();
            break;
        case ParamId::SpatialMode:
            if (m->spatial) {
                int mode = static_cast<int>(value + 0.5f);
                if (mode >= 2) {
                    m->spatial->SetRoomPreset(mode - 2);
                    m->spatial->SetMode(SpatialMode::Speakers);
                } else {
                    m->spatial->SetMode(mode == 1 ? SpatialMode::Surround51 : SpatialMode::Binaural);
                }
            }
            break;
        case ParamId::SpatialSub:
            if (m->spatial) m->spatial->SetSubwoofer(m->V(id) >= 0.5f);
            break;
        case ParamId::SpatialCrossover:
            if (m->spatial) m->spatial->SetCrossover(m->V(id));
            break;
        case ParamId::SpatialConeNoise:
            if (m->spatial) m->spatial->SetConeNoise(m->V(id) / 100.0f);
            break;
        case ParamId::SpatialBass:
            if (m->spatial) m->spatial->SetBass(m->V(id));
            break;
        case ParamId::SpatialRearCenter:
            if (m->spatial) m->spatial->SetRearCenter(m->V(id) >= 0.5f);
            break;
        case ParamId::SpatialWidth:
            if (m->spatial) m->spatial->SetWidth(m->V(id));
            break;
        case ParamId::SpatialRotation:
            if (m->spatial) m->spatial->SetRotation(m->V(id));
            break;
        case ParamId::SpatialX:
        case ParamId::SpatialY:
        case ParamId::SpatialZ:
            if (m->spatial)
                m->spatial->SetListenerOffset(m->V(ParamId::SpatialX), m->V(ParamId::SpatialY), m->V(ParamId::SpatialZ));
            break;
        default:
            break;
    }
}

void EffectChain::Enable(DSPEffectType type, bool on) {
    int i = static_cast<int>(type);
    if (i < 0 || i >= static_cast<int>(DSPEffectType::COUNT)) return;
    if (type == DSPEffectType::Reverb) {
        if (on != (m->reverbAlgorithm > 0)) SetReverbAlgorithm(on ? 1 : 0);
        return;
    }
    bool was = m->enabled[i];
    m->enabled[i] = on;
    if (on && !was) {
        Apply();  // in at once, if something is playing
    } else if (!on && was) {
        m->RemoveOne(type);
    }
}

bool EffectChain::Enabled(DSPEffectType type) const {
    int i = static_cast<int>(type);
    if (i < 0 || i >= static_cast<int>(DSPEffectType::COUNT)) return false;
    if (type == DSPEffectType::Reverb) return m->reverbAlgorithm > 0;
    return m->enabled[i];
}

void EffectChain::SetReverbAlgorithm(int algorithm) {
    if (algorithm < 0 || algorithm >= static_cast<int>(::ReverbAlgorithm::COUNT)) return;
    m->RemoveOne(DSPEffectType::Reverb);
    m->reverbAlgorithm = algorithm;
    m->UpdateReverb();
    if (algorithm > 0) Apply();
}

int EffectChain::ReverbAlgorithm() const { return m->reverbAlgorithm; }

void EffectChain::SetEqFrequencies(float bass, float mid, float treble) {
    m->eqBassHz = bass;
    m->eqMidHz = mid;
    m->eqTrebleHz = treble;
    m->UpdateEQ();
}

bool EffectChain::LoadImpulseResponse(const std::wstring& path, std::wstring& error) {
    ConvolutionReverb* conv = m->Convolution();
    // Out of the chain while it changes, back in (initialised at the device's rate) after
    const bool wasIn = m->InChain(DSPEffectType::Convolution);
    m->RemoveOne(DSPEffectType::Convolution);
    bool ok = conv->LoadIR(path.c_str());
    if (!ok) error = L"The impulse response could not be loaded.";
    if (wasIn) Apply();
    return ok;
}

std::wstring EffectChain::ImpulseResponsePath() const {
    return m->convolution ? m->convolution->GetIRPath() : std::wstring();
}

void EffectChain::UseSpatialAudio(SpatialAudio* spatial) {
    m->spatial = spatial;
    m->ApplySpatialSettings();
}

void EffectChain::UseConvolution(ConvolutionReverb* convolution) { m->convolution = convolution; }

SpatialAudio* EffectChain::Spatial() { return m->Spatial(); }

void EffectChain::ClearTails() {
    if (m->spatial) m->spatial->ClearTails();
}

}  // namespace audio
