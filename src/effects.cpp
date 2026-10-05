#include "effects.h"
#include "ini.h"
#include "globals.h"
#include "accessibility.h"
#include "app_ui.h"
#include "audio.h"
#include "audio/basic_effects.h"
#include "player.h"
#include "center_cancel.h"
#include "convolution.h"
#include "reverb/reverb.h"
#include "reverb/efx_reverb.h"
#include "spatial_audio.h"
#include "speakers/presets.h"
#include <cwchar>
#include <cstdio>
#include <vector>
#include <cmath>
#include <mutex>
#include <algorithm>

// Clamp helper
template<typename T> T clamp_val(T val, T minVal, T maxVal) {
    if (val < minVal) return minVal;
    if (val > maxVal) return maxVal;
    return val;
}

// Parameter definitions
static const ParamDef g_paramDefs[] = {
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
    {ParamId::SpatialSubLevel,   "3D Sub Level",   " dB", -15.0f,  15.0f,   1.0f,  0.0f,   DSPEffectType::SpatialAudio},
    {ParamId::SpatialCrossover,  "3D Crossover",   " Hz",  40.0f,  160.0f,  10.0f, 80.0f,  DSPEffectType::SpatialAudio},
    {ParamId::SpatialBassFeel,   "3D Bass Feel",   "%",    0.0f,   200.0f,  10.0f, 100.0f, DSPEffectType::SpatialAudio},
    {ParamId::SpatialConeNoise,  "3D Cone Noise",  "%",    0.0f,   1000.0f, 25.0f, 100.0f, DSPEffectType::SpatialAudio},
    {ParamId::SpatialBass,       "3D Bass",        " dB", -15.0f,  15.0f,   1.0f,  0.0f,   DSPEffectType::SpatialAudio},
    // Normalizer parameters
    {ParamId::NormTarget,    "Normalizer Target",    " dB", -40.0f, 0.0f,    1.0f,  -3.0f,   DSPEffectType::Normalizer},
    {ParamId::NormLookahead, "Normalizer Lookahead", " ms", 0.0f,   200.0f,  10.0f, 50.0f,   DSPEffectType::Normalizer},
    {ParamId::NormMaxGain,   "Normalizer Max Gain",  " dB", 0.0f,   40.0f,   1.0f,  20.0f,   DSPEffectType::Normalizer},
    {ParamId::NormRelease,   "Normalizer Release",   " ms", 50.0f,  5000.0f, 50.0f, 1000.0f, DSPEffectType::Normalizer},
};
static const int g_paramDefCount = sizeof(g_paramDefs) / sizeof(g_paramDefs[0]);

// The effects in the audio engine's chain (0 when not in it)
static int g_dspEcho = 0;
static int g_dspEQ = 0;             // preamp and the three bands, in one
static int g_dspCompressor = 0;
static int g_dspReverb = 0;         // the reverbs
static int g_dspStereoWidth = 0;
static int g_dspCenterCancel = 0;   // center cancel/extract
static int g_dspConvolution = 0;    // convolution reverb
static int g_dspSpatialAudio = 0;   // 3D audio
static int g_dspNormalizer = 0;     // normalizer

// Echo, EQ and compressor (src/audio/basic_effects.h)
static audio::Echo g_echo;
static audio::Gain g_eqPreamp;
static audio::PeakingEq g_eqBass, g_eqMid, g_eqTreble;
static audio::Compressor g_compressor;
static audio::Normalizer g_normalizer;

// DSP effect enabled states
static bool g_dspEnabled[(int)DSPEffectType::COUNT] = {};

// Parameter values
static float g_paramValues[(int)ParamId::COUNT];

// Current parameter index for cycling
static int g_currentParamIndex = 0;

// ---------------------------------------------------------------------------
// Reverb: the two reverbs in src/reverb, run as one custom DSP on the stream.
// Simple is a 16 line FDN reverb; Advanced is an EFX model reverb with the EFX environments.
// ---------------------------------------------------------------------------

// Rooms for the simple reverb.
struct SimpleReverbPreset {
    const char* name;
    float room, damping, preDelayMs, width, lowCutHz, highCutHz;
    float level;  // the preset's wet level relative to the default (1 = default)
};
static const SimpleReverbPreset g_simpleReverbPresets[] = {
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
static const int g_simpleReverbPresetCount = sizeof(g_simpleReverbPresets) / sizeof(g_simpleReverbPresets[0]);

// Environments for the advanced reverb: the EFX default presets (OpenAL Soft's efx-presets.h),
// field for field in EfxReverbParams order.
struct AdvancedReverbPreset {
    const char* name;
    fastplay::audio::EfxReverbParams params;
};
static const AdvancedReverbPreset g_advancedReverbPresets[] = {
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
static const int g_advancedReverbPresetCount = sizeof(g_advancedReverbPresets) / sizeof(g_advancedReverbPresets[0]);

// High cut at its maximum means off.
static const float kReverbHighCutOff = 20000.0f;

struct ReverbEngine {
    std::mutex mutex;  // params are set from the UI thread, processing runs on the audio thread
    fastplay::audio::Reverb simple;
    fastplay::audio::EfxReverb advanced;
    fastplay::audio::ReverbParams simpleParams;
    fastplay::audio::EfxReverbParams advancedParams;
    int algorithm = 0;
    int sampleRate = 0;
    float dry = 1.0f;          // target dry gain
    float dryCurrent = 1.0f;   // ramped towards dry once per block
    std::vector<float> inL, inR, outL, outR;
};
static ReverbEngine g_reverbEngine;

// Mix: 0% is dry only, 50% is the dry signal at full level with the reverb at its natural level,
// 100% is the reverb only.
static void ReverbMixGains(float mixPercent, float& dry, float& wet) {
    float m = clamp_val(mixPercent / 100.0f, 0.0f, 1.0f);
    dry = std::min(1.0f, 2.0f * (1.0f - m));
    wet = std::min(1.0f, 2.0f * m);
}

static int ReverbPresetIndex(ParamId id, int count) {
    int i = static_cast<int>(std::lround(g_paramValues[(int)id]));
    return clamp_val(i, 0, count - 1);
}

// Copy a preset's values into the parameters the user can adjust.
static void ApplySimpleReverbPreset(int index) {
    const SimpleReverbPreset& p = g_simpleReverbPresets[clamp_val(index, 0, g_simpleReverbPresetCount - 1)];
    g_paramValues[(int)ParamId::ReverbRoom] = p.room * 100.0f;
    g_paramValues[(int)ParamId::ReverbDamp] = p.damping * 100.0f;
    g_paramValues[(int)ParamId::ReverbWidth] = p.width * 100.0f;
    g_paramValues[(int)ParamId::ReverbPreDelay] = p.preDelayMs;
    g_paramValues[(int)ParamId::ReverbLowCut] = p.lowCutHz;
    g_paramValues[(int)ParamId::ReverbHighCut] = p.highCutHz > 0 ? p.highCutHz : kReverbHighCutOff;
}

static float GainToDb(float gain, float minDb, float maxDb) {
    float db = gain > 0.0f ? 20.0f * std::log10(gain) : minDb;
    return clamp_val(std::round(db), minDb, maxDb);
}

static void ApplyAdvancedReverbPreset(int index) {
    const fastplay::audio::EfxReverbParams& p = g_advancedReverbPresets[clamp_val(index, 0, g_advancedReverbPresetCount - 1)].params;
    g_paramValues[(int)ParamId::AdvReverbDecay] = p.decay_time;
    g_paramValues[(int)ParamId::AdvReverbHFRatio] = p.decay_hf_ratio;
    g_paramValues[(int)ParamId::AdvReverbDensity] = p.density * 100.0f;
    g_paramValues[(int)ParamId::AdvReverbDiffusion] = p.diffusion * 100.0f;
    g_paramValues[(int)ParamId::AdvReverbReflections] = GainToDb(p.reflections_gain, -60.0f, 10.0f);
    g_paramValues[(int)ParamId::AdvReverbLate] = GainToDb(p.late_reverb_gain, -60.0f, 20.0f);
    g_paramValues[(int)ParamId::AdvReverbReflDelay] = p.reflections_delay * 1000.0f;
    g_paramValues[(int)ParamId::AdvReverbLateDelay] = p.late_reverb_delay * 1000.0f;
}

// Rebuild both reverbs' settings from the parameter values.
static void UpdateReverbParams() {
    const float* v = g_paramValues;
    float dry, wet;

    fastplay::audio::ReverbParams sp;
    const SimpleReverbPreset& room = g_simpleReverbPresets[ReverbPresetIndex(ParamId::ReverbPreset, g_simpleReverbPresetCount)];
    ReverbMixGains(v[(int)ParamId::ReverbMix], dry, wet);
    float simpleDry = dry;
    sp.room_size = v[(int)ParamId::ReverbRoom] / 100.0f;
    sp.damping = v[(int)ParamId::ReverbDamp] / 100.0f;
    sp.width = v[(int)ParamId::ReverbWidth] / 100.0f;
    sp.pre_delay_ms = v[(int)ParamId::ReverbPreDelay];
    sp.low_cut_hz = v[(int)ParamId::ReverbLowCut];
    sp.high_cut_hz = v[(int)ParamId::ReverbHighCut] >= kReverbHighCutOff ? 0.0f : v[(int)ParamId::ReverbHighCut];
    sp.wet = wet * room.level / 3.0f;  // the reverb's wet is scaled so 1/3 is unity
    sp.dry = 0.0f;                     // the dry signal is mixed here, not by the reverb

    fastplay::audio::EfxReverbParams ap = g_advancedReverbPresets[ReverbPresetIndex(ParamId::AdvReverbPreset, g_advancedReverbPresetCount)].params;
    ReverbMixGains(v[(int)ParamId::AdvReverbMix], dry, wet);
    float advancedDry = dry;
    ap.decay_time = v[(int)ParamId::AdvReverbDecay];
    ap.decay_hf_ratio = v[(int)ParamId::AdvReverbHFRatio];
    ap.density = v[(int)ParamId::AdvReverbDensity] / 100.0f;
    ap.diffusion = v[(int)ParamId::AdvReverbDiffusion] / 100.0f;
    ap.reflections_gain = std::pow(10.0f, v[(int)ParamId::AdvReverbReflections] / 20.0f);
    ap.late_reverb_gain = std::pow(10.0f, v[(int)ParamId::AdvReverbLate] / 20.0f);
    ap.reflections_delay = v[(int)ParamId::AdvReverbReflDelay] / 1000.0f;
    ap.late_reverb_delay = v[(int)ParamId::AdvReverbLateDelay] / 1000.0f;
    ap.gain *= wet;

    std::lock_guard<std::mutex> lock(g_reverbEngine.mutex);
    g_reverbEngine.algorithm = g_reverbAlgorithm;
    g_reverbEngine.simpleParams = sp;
    g_reverbEngine.advancedParams = ap;
    g_reverbEngine.dry = g_reverbAlgorithm == 2 ? advancedDry : simpleDry;
    if (g_reverbEngine.sampleRate > 0) {
        g_reverbEngine.simple.set_params(sp);
        g_reverbEngine.advanced.set_params(ap);
    }
}

// Start the reverbs fresh at a stream's sample rate (no tail carried over from the last track).
static void ResetReverbEngine(int sampleRate) {
    std::lock_guard<std::mutex> lock(g_reverbEngine.mutex);
    if (sampleRate != g_reverbEngine.sampleRate) {
        g_reverbEngine.simple.init(sampleRate);
        g_reverbEngine.advanced.init(sampleRate);
        g_reverbEngine.sampleRate = sampleRate;
    }
    g_reverbEngine.simple.set_params(g_reverbEngine.simpleParams);
    g_reverbEngine.advanced.set_params(g_reverbEngine.advancedParams);
    g_reverbEngine.simple.reset();
    g_reverbEngine.advanced.reset();
    g_reverbEngine.dryCurrent = g_reverbEngine.dry;
}

static void ReverbDSPProc(float* samples, int frames, int chans, int, void*) {
    ReverbEngine& e = g_reverbEngine;
    std::lock_guard<std::mutex> lock(e.mutex);
    if (e.algorithm == 0 || e.sampleRate <= 0 || frames <= 0) return;

    e.inL.resize(frames); e.inR.resize(frames);
    e.outL.assign(frames, 0.0f); e.outR.assign(frames, 0.0f);
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

// Initialize parameter values to defaults
bool InitEffects() {
    for (int i = 0; i < g_paramDefCount; i++) {
        g_paramValues[(int)g_paramDefs[i].id] = g_paramDefs[i].defaultValue;
    }
    // Note: g_tempo, g_pitch, g_rate are loaded from settings in LoadSettings()
    // Only set defaults if they haven't been loaded yet (all zero means uninitialized)
    // Actually, these are loaded before InitEffects, so don't overwrite them
    UpdateReverbParams();
    return true;
}

void FreeEffects() {
    RemoveDSPEffects();
    FreeCenterCancelProcessor();
    FreeSpatialAudio();
}

// Helper to check if a reverb param matches current algorithm
static bool IsReverbParamForCurrentAlgorithm(ParamId id) {
    switch (id) {
        // Simple reverb params (algorithm 1)
        case ParamId::ReverbPreset:
        case ParamId::ReverbMix:
        case ParamId::ReverbRoom:
        case ParamId::ReverbDamp:
        case ParamId::ReverbWidth:
        case ParamId::ReverbPreDelay:
        case ParamId::ReverbLowCut:
        case ParamId::ReverbHighCut:
            return g_reverbAlgorithm == 1;
        // Advanced reverb params (algorithm 2)
        case ParamId::AdvReverbPreset:
        case ParamId::AdvReverbMix:
        case ParamId::AdvReverbDecay:
        case ParamId::AdvReverbHFRatio:
        case ParamId::AdvReverbDensity:
        case ParamId::AdvReverbDiffusion:
        case ParamId::AdvReverbReflections:
        case ParamId::AdvReverbLate:
        case ParamId::AdvReverbReflDelay:
        case ParamId::AdvReverbLateDelay:
            return g_reverbAlgorithm == 2;
        default:
            return true;  // Not a reverb param
    }
}

// Build list of available parameters based on enabled stream effects and DSP effects
// The room preset 3D Mode is on (0 and up), or -1 for Binaural and 5.1.
static int CurrentRoomPreset() {
    int mode = static_cast<int>(g_paramValues[(int)ParamId::SpatialMode] + 0.5f);
    return mode >= 2 ? mode - 2 : -1;
}

// Whether a 3D Audio parameter means anything in the current 3D mode: width and
// the rear speaker belong to the virtual speakers of Binaural and 5.1, the
// subwoofer controls to room presets that have subs.
static bool IsSpatialParamInUse(ParamId id) {
    int preset = CurrentRoomPreset();
    switch (id) {
        case ParamId::SpatialWidth:
        case ParamId::SpatialRearCenter:
            return preset < 0;
        case ParamId::SpatialBassFeel:
        case ParamId::SpatialConeNoise:
        case ParamId::SpatialBass:
            return preset >= 0;
        case ParamId::SpatialSub:
        case ParamId::SpatialSubLevel:
        case ParamId::SpatialCrossover:
            return preset >= 0 && speakers::RoomPresetHasSub(preset);
        default:
            return true;
    }
}

static std::vector<ParamId> GetAvailableParams() {
    std::vector<ParamId> params;

    for (int i = 0; i < g_paramDefCount; i++) {
        const ParamDef& def = g_paramDefs[i];

        // Check if this is a stream effect parameter
        if ((int)def.dspEffect == -1) {
            // Stream effect - check if enabled in g_effectEnabled
            int effectIdx = (int)def.id;
            if (effectIdx < 4 && g_effectEnabled[effectIdx]) {
                params.push_back(def.id);
            }
        } else if (def.dspEffect == DSPEffectType::Reverb) {
            // Reverb parameter - check algorithm and matching params
            if (g_reverbAlgorithm > 0 && IsReverbParamForCurrentAlgorithm(def.id)) {
                params.push_back(def.id);
            }
        } else {
            // Other DSP effect parameter - check if the DSP effect is enabled
            if (g_dspEnabled[(int)def.dspEffect] &&
                (def.dspEffect != DSPEffectType::SpatialAudio || IsSpatialParamInUse(def.id))) {
                params.push_back(def.id);
            }
        }
    }

    return params;
}

int GetAvailableParamCount() {
    return (int)GetAvailableParams().size();
}

std::vector<ParamId> GetAvailableParamIds() {
    return GetAvailableParams();
}

ParamId GetCurrentParam() {
    return (ParamId)g_currentParamIndex;
}

void SetCurrentParam(ParamId id) {
    if (GetParamDef(id)) g_currentParamIndex = (int)id;
}

// Toggle stream effect (Volume=0, Pitch=1, Tempo=2, Rate=3)
void ToggleStreamEffect(int effectIndex) {
    if (effectIndex < 0 || effectIndex >= 4) return;

    g_effectEnabled[effectIndex] = !g_effectEnabled[effectIndex];

    const char* names[] = {"Volume", "Pitch", "Tempo", "Rate"};
    std::string msg = std::string(names[effectIndex]) +
                      (g_effectEnabled[effectIndex] ? " enabled" : " disabled");
    Speak(msg);
}

bool IsStreamEffectEnabled(int effectIndex) {
    if (effectIndex < 0 || effectIndex >= 4) return false;
    return g_effectEnabled[effectIndex];
}

// Toggle DSP effect
void ToggleDSPEffect(DSPEffectType type) {
    if ((int)type < 0 || (int)type >= (int)DSPEffectType::COUNT) return;

    // Reverb uses algorithm selection, not simple toggle
    if (type == DSPEffectType::Reverb) {
        // Cycle through reverb algorithms: Off -> Simple -> Advanced -> Off
        int newAlgo = (g_reverbAlgorithm + 1) % (int)ReverbAlgorithm::COUNT;
        SetReverbAlgorithm(newAlgo);
        const char* algoNames[] = {"Off", "Simple", "Advanced"};
        Speak(std::string("Reverb: ") + algoNames[newAlgo]);
        return;
    }

    bool newState = !g_dspEnabled[(int)type];
    EnableDSPEffect(type, newState);

    const char* names[] = {"Reverb", "Echo", "EQ", "Compressor", "Stereo Width", "Center Cancel", "Convolution", "3D Audio",
                           "Normalizer"};
    std::string msg = std::string(names[(int)type]) +
                      (newState ? " enabled" : " disabled");
    Speak(msg);
}

// Set reverb algorithm (0=Off, 1=Simple, 2=Advanced)
void SetReverbAlgorithm(int algorithm) {
    if (algorithm < 0 || algorithm >= (int)ReverbAlgorithm::COUNT) return;

    // Remove existing reverb effect if any
    if (g_dspReverb) {
        audio::RemoveDsp(g_dspReverb);
        g_dspReverb = 0;
    }

    g_reverbAlgorithm = algorithm;
    UpdateReverbParams();

    // Apply new reverb if enabled and something is playing
    if (algorithm > 0 && audio::IsLoaded()) {
        ApplyDSPEffects();
    }
}

// Take one effect out of the chain
static void RemoveDsp(int& id) {
    if (id) audio::RemoveDsp(id);
    id = 0;
}

// Enable or disable a DSP effect
void EnableDSPEffect(DSPEffectType type, bool enable) {
    if ((int)type < 0 || (int)type >= (int)DSPEffectType::COUNT) return;

    bool wasEnabled = g_dspEnabled[(int)type];
    g_dspEnabled[(int)type] = enable;

    // If something is playing, apply/remove the effect immediately
    if (audio::IsLoaded()) {
        if (enable && !wasEnabled) {
            ApplyDSPEffects();
        } else if (!enable && wasEnabled) {
            switch (type) {
                case DSPEffectType::Reverb: RemoveDsp(g_dspReverb); break;
                case DSPEffectType::Echo: RemoveDsp(g_dspEcho); break;
                case DSPEffectType::EQ: RemoveDsp(g_dspEQ); break;
                case DSPEffectType::Compressor: RemoveDsp(g_dspCompressor); break;
                case DSPEffectType::StereoWidth: RemoveDsp(g_dspStereoWidth); break;
                case DSPEffectType::CenterCancel: RemoveDsp(g_dspCenterCancel); break;
                case DSPEffectType::Convolution: RemoveDsp(g_dspConvolution); break;
                case DSPEffectType::SpatialAudio: RemoveDsp(g_dspSpatialAudio); break;
                case DSPEffectType::Normalizer: RemoveDsp(g_dspNormalizer); break;
                default: break;
            }
        }
    }
}

// Echo, EQ and compressor, set from the parameter values
static void UpdateEcho() {
    audio::Echo::Params p;
    p.dry = 1.0f - (g_paramValues[(int)ParamId::EchoMix] / 100.0f);
    p.wet = g_paramValues[(int)ParamId::EchoMix] / 100.0f;
    p.feedback = g_paramValues[(int)ParamId::EchoFeedback] / 100.0f;
    p.delay = g_paramValues[(int)ParamId::EchoDelay] / 1000.0f;  // ms to seconds
    p.stereo = true;
    g_echo.Set(p);
}

static void UpdateEQ() {
    g_eqPreamp.Set(powf(10.0f, g_paramValues[(int)ParamId::EQPreamp] / 20.0f));
    g_eqBass.Set(g_eqBassFreq, 2.5f, g_paramValues[(int)ParamId::EQBass]);
    g_eqMid.Set(g_eqMidFreq, 2.5f, g_paramValues[(int)ParamId::EQMid]);
    g_eqTreble.Set(g_eqTrebleFreq, 2.5f, g_paramValues[(int)ParamId::EQTreble]);
}

static void UpdateCompressor() {
    audio::Compressor::Params p;
    p.gainDb = g_paramValues[(int)ParamId::CompGain];
    p.thresholdDb = g_paramValues[(int)ParamId::CompThreshold];
    p.ratio = g_paramValues[(int)ParamId::CompRatio];
    p.attackMs = g_paramValues[(int)ParamId::CompAttack];
    p.releaseMs = g_paramValues[(int)ParamId::CompRelease];
    g_compressor.Set(p);
}

static void UpdateNormalizer() {
    audio::Normalizer::Params p;
    p.targetDb = g_paramValues[(int)ParamId::NormTarget];
    p.lookaheadMs = g_paramValues[(int)ParamId::NormLookahead];
    p.maxGainDb = g_paramValues[(int)ParamId::NormMaxGain];
    p.releaseMs = g_paramValues[(int)ParamId::NormRelease];
    g_normalizer.Set(p);
}

static void NormalizerDSPProc(float* samples, int frames, int, int sampleRate, void*) {
    g_normalizer.Process(samples, frames, sampleRate);
}

static void EchoDSPProc(float* samples, int frames, int, int sampleRate, void*) {
    g_echo.Process(samples, frames, sampleRate);
}

static void EQDSPProc(float* samples, int frames, int, int sampleRate, void*) {
    g_eqPreamp.Process(samples, frames);
    g_eqBass.Process(samples, frames, sampleRate);
    g_eqMid.Process(samples, frames, sampleRate);
    g_eqTreble.Process(samples, frames, sampleRate);
}

static void CompressorDSPProc(float* samples, int frames, int, int sampleRate, void*) {
    g_compressor.Process(samples, frames, sampleRate);
}

// Stereo width - uses Mid/Side processing
// Width 0% = mono, 100% = normal stereo, 200% = extra wide
static void StereoWidthDSPProc(float* samples, int frames, int, int, void*) {
    float width = g_paramValues[(int)ParamId::StereoWidth] / 100.0f;
    for (int i = 0; i < frames; i++) {
        float left = samples[i * 2];
        float right = samples[i * 2 + 1];
        float mid = (left + right) * 0.5f;
        float side = (left - right) * 0.5f * width;
        samples[i * 2] = mid + side;
        samples[i * 2 + 1] = mid - side;
    }
}

// Center cancel/extract - FFT-based spectral processing
// -100% = extract center (isolate vocals), 0% = no effect, +100% = cancel center (remove vocals)
static void CenterCancelDSPProc(float* samples, int frames, int, int sampleRate, void*) {
    float amount = g_paramValues[(int)ParamId::CenterCancel] / 100.0f;

    CenterCancelProcessor* processor = GetCenterCancelProcessor();
    if (!processor) {
        InitCenterCancelProcessor(sampleRate);
        processor = GetCenterCancelProcessor();
    }
    if (!processor || !processor->IsInitialized()) return;
    processor->SetAmount(amount);

    // If amount is 0, the processor passes the audio through
    static std::vector<float> tempOut;
    tempOut.resize(static_cast<size_t>(frames) * 2);
    int outputFrames = 0;
    processor->ProcessFloat(samples, frames, tempOut.data(), outputFrames);
    // outputFrames equals frames in the steady state
    std::copy(tempOut.begin(), tempOut.begin() + static_cast<size_t>(outputFrames) * 2, samples);
}

// Convolution reverb
static void ConvolutionDSPProc(float* samples, int frames, int, int sampleRate, void*) {
    ConvolutionReverb* conv = GetConvolutionReverb();
    if (!conv) return;

    // Initialize if IR is loaded but not yet initialized
    if (conv->IsLoaded() && !conv->IsInitialized()) {
        conv->Init(sampleRate);
    }

    conv->SetMix(g_paramValues[(int)ParamId::ConvolutionMix]);
    conv->SetGain(g_paramValues[(int)ParamId::ConvolutionGain]);
    conv->Process(samples, frames);
}

// 3D Audio - HRTF binaural rendering and the room presets
static void SpatialAudioDSPProc(float* samples, int frames, int, int, void*) {
    SpatialAudio* spatial = GetSpatialAudio();
    if (!spatial || !spatial->IsInitialized()) return;

    float blend = g_paramValues[(int)ParamId::SpatialBlend] / 100.0f;
    if (blend <= 0.0f) return;
    spatial->Process(samples, frames, blend);
}

bool IsDSPEffectEnabled(DSPEffectType type) {
    if ((int)type < 0 || (int)type >= (int)DSPEffectType::COUNT) return false;
    // Reverb uses g_reverbAlgorithm instead of g_dspEnabled
    if (type == DSPEffectType::Reverb) {
        return g_reverbAlgorithm > 0;
    }
    return g_dspEnabled[(int)type];
}

// Put the enabled effects into the audio engine's chain, fresh, for what was just
// loaded. They run in the order added; all have the same priority.
void ApplyDSPEffects() {
    if (!audio::IsLoaded()) return;
    const int sampleRate = audio::MixSampleRate();

    // Reverb (based on selected algorithm)
    if (g_reverbAlgorithm > 0 && !g_dspReverb) {
        ResetReverbEngine(sampleRate);
        g_dspReverb = audio::AddDsp(ReverbDSPProc, nullptr, 0);
    }

    // Echo
    if (g_dspEnabled[(int)DSPEffectType::Echo] && !g_dspEcho) {
        UpdateEcho();
        g_dspEcho = audio::AddDsp(EchoDSPProc, nullptr, 0);
    }

    // EQ (preamp and three peaking bands)
    if (g_dspEnabled[(int)DSPEffectType::EQ] && !g_dspEQ) {
        UpdateEQ();
        g_dspEQ = audio::AddDsp(EQDSPProc, nullptr, 0);
    }

    // Compressor
    if (g_dspEnabled[(int)DSPEffectType::Compressor] && !g_dspCompressor) {
        UpdateCompressor();
        g_dspCompressor = audio::AddDsp(CompressorDSPProc, nullptr, 0);
    }

    // Stereo Width
    if (g_dspEnabled[(int)DSPEffectType::StereoWidth] && !g_dspStereoWidth) {
        g_dspStereoWidth = audio::AddDsp(StereoWidthDSPProc, nullptr, 0);
    }

    // Center Cancel/Extract
    if (g_dspEnabled[(int)DSPEffectType::CenterCancel] && !g_dspCenterCancel) {
        g_dspCenterCancel = audio::AddDsp(CenterCancelDSPProc, nullptr, 0);
    }

    // Convolution Reverb
    if (g_dspEnabled[(int)DSPEffectType::Convolution] && !g_dspConvolution) {
        ConvolutionReverb* conv = GetConvolutionReverb();
        if (conv && conv->IsLoaded()) conv->Init(sampleRate);
        g_dspConvolution = audio::AddDsp(ConvolutionDSPProc, nullptr, 0);
    }

    // 3D Audio
    if (g_dspEnabled[(int)DSPEffectType::SpatialAudio] && !g_dspSpatialAudio) {
        bool initOk = false;
        SpatialAudio* spatial = GetSpatialAudio();
        if (spatial) {
            initOk = spatial->Initialize(sampleRate);
            if (!initOk) {
                const wchar_t* err = spatial->GetLastError();
                if (err && err[0]) {
                    ShowMessage(err, L"3D Audio Error", MessageIcon::Error);
                }
                g_dspEnabled[(int)DSPEffectType::SpatialAudio] = false;
            }
        }
        if (initOk) {
            g_dspSpatialAudio = audio::AddDsp(SpatialAudioDSPProc, nullptr, 0);
        }
    }

    // Normalizer: last, so it holds the level of everything before it
    if (g_dspEnabled[(int)DSPEffectType::Normalizer] && !g_dspNormalizer) {
        UpdateNormalizer();
        g_dspNormalizer = audio::AddDsp(NormalizerDSPProc, nullptr, -10);
    }
}

void RemoveDSPEffects() {
    RemoveDsp(g_dspReverb);
    RemoveDsp(g_dspEcho);
    RemoveDsp(g_dspEQ);
    RemoveDsp(g_dspCompressor);
    RemoveDsp(g_dspStereoWidth);
    RemoveDsp(g_dspCenterCancel);
    RemoveDsp(g_dspConvolution);
    RemoveDsp(g_dspSpatialAudio);
    RemoveDsp(g_dspNormalizer);
}

const ParamDef* GetParamDef(ParamId id) {
    for (int i = 0; i < g_paramDefCount; i++) {
        if (g_paramDefs[i].id == id) return &g_paramDefs[i];
    }
    return nullptr;
}

float GetParamValue(ParamId id) {
    // For stream effects, return the actual global values
    switch (id) {
        case ParamId::Volume: return g_volume;
        case ParamId::Pitch: return g_pitch;
        case ParamId::Tempo: return g_tempo;
        case ParamId::Rate: return g_rate;
        default: break;
    }
    // For DSP effect parameters, use the stored values
    if ((int)id < 0 || (int)id >= (int)ParamId::COUNT) return 0.0f;
    return g_paramValues[(int)id];
}

const char* GetParamName(ParamId id) {
    const ParamDef* def = GetParamDef(id);
    return def ? def->name : "Unknown";
}

const char* GetParamUnit(ParamId id) {
    const ParamDef* def = GetParamDef(id);
    return def ? def->unit : "";
}

void SetParamValue(ParamId id, float value) {
    const ParamDef* def = GetParamDef(id);
    if (!def) return;

    // For Volume, respect the allow amplify setting
    float maxVal = def->maxValue;
    if (id == ParamId::Volume) {
        maxVal = g_allowAmplify ? MAX_VOLUME_AMPLIFY : MAX_VOLUME_NORMAL;
    }

    value = clamp_val(value, def->minValue, maxVal);
    g_paramValues[(int)id] = value;

    // Apply the change
    switch (id) {
        case ParamId::Volume:
            g_volume = value;
            UpdateOutputGain();
            break;
        case ParamId::Pitch:
            g_pitch = value;
            audio::SetPitch(g_pitch);
            break;
        case ParamId::Tempo:
            g_tempo = value;
            audio::SetTempo(g_tempo);  // live streams keep their speed
            break;
        case ParamId::Rate:
            g_rate = value;
            audio::SetRate(g_rate);  // speed and pitch together; not for live streams
            break;
        // Reverb parameters: a preset sets the others, then both reverbs are updated
        case ParamId::ReverbPreset:
            ApplySimpleReverbPreset(ReverbPresetIndex(id, g_simpleReverbPresetCount));
            UpdateReverbParams();
            break;
        case ParamId::AdvReverbPreset:
            ApplyAdvancedReverbPreset(ReverbPresetIndex(id, g_advancedReverbPresetCount));
            UpdateReverbParams();
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
            UpdateReverbParams();
            break;
        case ParamId::EchoDelay:
        case ParamId::EchoFeedback:
        case ParamId::EchoMix:
            UpdateEcho();
            break;
        case ParamId::EQPreamp:
        case ParamId::EQBass:
        case ParamId::EQMid:
        case ParamId::EQTreble:
            UpdateEQ();
            break;
        case ParamId::CompThreshold:
        case ParamId::CompRatio:
        case ParamId::CompAttack:
        case ParamId::CompRelease:
        case ParamId::CompGain:
            UpdateCompressor();
            break;
        case ParamId::NormTarget:
        case ParamId::NormLookahead:
        case ParamId::NormMaxGain:
        case ParamId::NormRelease:
            UpdateNormalizer();
            break;
        case ParamId::SpatialMode: {
            SpatialAudio* spatial = GetSpatialAudio();
            int mode = static_cast<int>(value + 0.5f);
            if (spatial && mode >= 2) {
                spatial->SetRoomPreset(mode - 2);
                spatial->SetMode(SpatialMode::Speakers);
            } else if (spatial) {
                spatial->SetMode(mode == 1 ? SpatialMode::Surround51 : SpatialMode::Binaural);
            }
            break;
        }
        case ParamId::SpatialSub:
            if (SpatialAudio* spatial = GetSpatialAudio()) spatial->SetSubwoofer(value >= 0.5f);
            break;
        case ParamId::SpatialSubLevel:
            if (SpatialAudio* spatial = GetSpatialAudio()) spatial->SetSubLevel(value);
            break;
        case ParamId::SpatialCrossover:
            if (SpatialAudio* spatial = GetSpatialAudio()) spatial->SetCrossover(value);
            break;
        case ParamId::SpatialBassFeel:
            if (SpatialAudio* spatial = GetSpatialAudio()) spatial->SetBassFeel(value / 100.0f);
            break;
        case ParamId::SpatialConeNoise:
            if (SpatialAudio* spatial = GetSpatialAudio()) spatial->SetConeNoise(value / 100.0f);
            break;
        case ParamId::SpatialBass:
            if (SpatialAudio* spatial = GetSpatialAudio()) spatial->SetBass(value);
            break;
        case ParamId::SpatialRearCenter: {
            SpatialAudio* spatial = GetSpatialAudio();
            if (spatial) spatial->SetRearCenter(value >= 0.5f);
            break;
        }
        default:
            break;
    }
}

void CycleParam(int direction) {
    std::vector<ParamId> params = GetAvailableParams();

    if (params.empty()) {
        Speak("No parameters available");
        return;
    }

    // Find current param in available list
    int currentIdx = -1;
    for (int i = 0; i < (int)params.size(); i++) {
        if ((int)params[i] == g_currentParamIndex) {
            currentIdx = i;
            break;
        }
    }

    // If current not found, start at beginning
    if (currentIdx < 0) {
        currentIdx = 0;
    } else {
        currentIdx += direction;
        // Clamp to bounds (no wrapping)
        if (currentIdx < 0) currentIdx = 0;
        if (currentIdx >= (int)params.size()) currentIdx = (int)params.size() - 1;
    }

    g_currentParamIndex = (int)params[currentIdx];
    AnnounceCurrentParam();
}

void AdjustCurrentParam(int direction) {
    std::vector<ParamId> params = GetAvailableParams();
    if (params.empty()) return;

    // Check if current param is available
    bool found = false;
    for (const auto& p : params) {
        if ((int)p == g_currentParamIndex) {
            found = true;
            break;
        }
    }

    if (!found) {
        g_currentParamIndex = (int)params[0];
    }

    ParamId id = (ParamId)g_currentParamIndex;
    const ParamDef* def = GetParamDef(id);
    if (!def) return;

    // Block tempo and rate adjustments for live streams
    if (g_isLiveStream && (id == ParamId::Tempo || id == ParamId::Rate)) {
        Speak("Not available for live streams");
        return;
    }

    // For Volume, respect the allow amplify setting and use g_volumeStep
    float maxVal = def->maxValue;
    float step = def->step;
    if (id == ParamId::Volume) {
        maxVal = g_allowAmplify ? MAX_VOLUME_AMPLIFY : MAX_VOLUME_NORMAL;
        step = g_volumeStep;  // Use configurable volume step
    }

    float currentVal = GetParamValue(id);
    float newVal;

    // Handle Rate with semitone stepping if enabled
    if (id == ParamId::Rate && g_rateStepMode == 1) {
        // Semitone ratio = 2^(1/12) ≈ 1.0594630943592953
        const float semitoneRatio = 1.0594630943592953f;
        if (direction > 0) {
            newVal = currentVal * semitoneRatio;
        } else {
            newVal = currentVal / semitoneRatio;
        }
    } else {
        newVal = currentVal + (direction * step);
    }

    // 3D Rotation, Mode, and Rear Speaker wrap around instead of clamping
    // so the user can cycle through modes or rotate continuously.
    if (id == ParamId::SpatialRotation) {
        // Angular: ±180° meet, so full range = max - min
        float range = def->maxValue - def->minValue;
        while (newVal > def->maxValue) newVal -= range;
        while (newVal < def->minValue) newVal += range;
    } else if (id == ParamId::SpatialMode || id == ParamId::SpatialRearCenter || id == ParamId::SpatialSub ||
               id == ParamId::ReverbPreset || id == ParamId::AdvReverbPreset) {
        // Discrete choice: add step so past-max wraps to min
        float range = def->maxValue - def->minValue + def->step;
        while (newVal > def->maxValue) newVal -= range;
        while (newVal < def->minValue) newVal += range;
    } else {
        newVal = clamp_val(newVal, def->minValue, maxVal);
    }

    SetParamValue(id, newVal);
    AnnounceCurrentParam();
}

void ResetCurrentParam() {
    std::vector<ParamId> params = GetAvailableParams();
    if (params.empty()) return;

    // Check if current param is available
    bool found = false;
    for (const auto& p : params) {
        if ((int)p == g_currentParamIndex) {
            found = true;
            break;
        }
    }

    if (!found) {
        g_currentParamIndex = (int)params[0];
    }

    ParamId id = (ParamId)g_currentParamIndex;
    const ParamDef* def = GetParamDef(id);
    if (!def) return;

    // Block tempo and rate reset for live streams
    if (g_isLiveStream && (id == ParamId::Tempo || id == ParamId::Rate)) {
        Speak("Not available for live streams");
        return;
    }

    SetParamValue(id, def->defaultValue);
    AnnounceCurrentParam();
}

void SetCurrentParamToMin() {
    std::vector<ParamId> params = GetAvailableParams();
    if (params.empty()) return;

    // Check if current param is available
    bool found = false;
    for (const auto& p : params) {
        if ((int)p == g_currentParamIndex) {
            found = true;
            break;
        }
    }

    if (!found) {
        g_currentParamIndex = (int)params[0];
    }

    ParamId id = (ParamId)g_currentParamIndex;
    const ParamDef* def = GetParamDef(id);
    if (!def) return;

    // Block tempo and rate min for live streams
    if (g_isLiveStream && (id == ParamId::Tempo || id == ParamId::Rate)) {
        Speak("Not available for live streams");
        return;
    }

    SetParamValue(id, def->minValue);
    AnnounceCurrentParam();
}

void SetCurrentParamToMax() {
    std::vector<ParamId> params = GetAvailableParams();
    if (params.empty()) return;

    // Check if current param is available
    bool found = false;
    for (const auto& p : params) {
        if ((int)p == g_currentParamIndex) {
            found = true;
            break;
        }
    }

    if (!found) {
        g_currentParamIndex = (int)params[0];
    }

    ParamId id = (ParamId)g_currentParamIndex;
    const ParamDef* def = GetParamDef(id);
    if (!def) return;

    // Block tempo and rate max for live streams
    if (g_isLiveStream && (id == ParamId::Tempo || id == ParamId::Rate)) {
        Speak("Not available for live streams");
        return;
    }

    // Use maxValue, but respect g_allowAmplify for volume
    float maxVal = def->maxValue;
    if (id == ParamId::Volume && !g_allowAmplify && maxVal > 1.0f) {
        maxVal = 1.0f;
    }

    SetParamValue(id, maxVal);
    AnnounceCurrentParam();
}

void AnnounceCurrentParam() {
    if (!g_speechEffect) return;
    std::string text = DescribeParam((ParamId)g_currentParamIndex);
    if (!text.empty()) Speak(text);
}

std::string DescribeParam(ParamId id) {
    const ParamDef* def = GetParamDef(id);
    if (!def) return std::string();

    float val = GetParamValue(id);
    char buf[64];

    // Format based on parameter type
    if (id == ParamId::SpatialMode) {
        int preset = CurrentRoomPreset();
        snprintf(buf, sizeof(buf), "3D Mode: %s",
                 preset >= 0 ? speakers::RoomPresetName(preset) : val >= 0.5f ? "5.1 Surround" : "Binaural");
    } else if (id == ParamId::SpatialRearCenter) {
        snprintf(buf, sizeof(buf), "3D Rear Speaker: %s", val >= 0.5f ? "On" : "Off");
    } else if (id == ParamId::SpatialSub) {
        snprintf(buf, sizeof(buf), "3D Subwoofer: %s", val >= 0.5f ? "On" : "Off");
    } else if (id == ParamId::SpatialSubLevel || id == ParamId::SpatialBass) {
        snprintf(buf, sizeof(buf), "%s %+.0f%s", def->name, val, def->unit);
    } else if ((id == ParamId::SpatialX || id == ParamId::SpatialY || id == ParamId::SpatialZ) &&
               CurrentRoomPreset() >= 0) {
        // In a room the listener moves in tenths of a metre from the seat
        snprintf(buf, sizeof(buf), "%s %.1f metres", def->name, val * 0.1f);
    } else if (id == ParamId::ReverbPreset) {
        snprintf(buf, sizeof(buf), "%s: %s", def->name,
                 g_simpleReverbPresets[ReverbPresetIndex(id, g_simpleReverbPresetCount)].name);
    } else if (id == ParamId::AdvReverbPreset) {
        snprintf(buf, sizeof(buf), "%s: %s", def->name,
                 g_advancedReverbPresets[ReverbPresetIndex(id, g_advancedReverbPresetCount)].name);
    } else if ((id == ParamId::ReverbLowCut && val <= 0.0f) ||
               (id == ParamId::ReverbHighCut && val >= kReverbHighCutOff)) {
        snprintf(buf, sizeof(buf), "%s Off", def->name);
    } else if (id == ParamId::AdvReverbDecay) {
        snprintf(buf, sizeof(buf), "%s %.1f%s", def->name, val, def->unit);
    } else if (id == ParamId::AdvReverbHFRatio) {
        snprintf(buf, sizeof(buf), "%s %.2f%s", def->name, val, def->unit);
    } else if (id == ParamId::AdvReverbReflections || id == ParamId::AdvReverbLate) {
        snprintf(buf, sizeof(buf), "%s %+.0f%s", def->name, val, def->unit);
    } else if (id == ParamId::Volume) {
        snprintf(buf, sizeof(buf), "%s %d%s", def->name, (int)(val * 100 + 0.5f), def->unit);
    } else if (id == ParamId::Rate) {
        snprintf(buf, sizeof(buf), "%s %.2f%s", def->name, val, def->unit);
    } else if (id == ParamId::Pitch || id == ParamId::EQBass || id == ParamId::EQMid || id == ParamId::EQTreble) {
        snprintf(buf, sizeof(buf), "%s %+.0f%s", def->name, val, def->unit);
    } else if (id == ParamId::EchoDelay) {
        snprintf(buf, sizeof(buf), "%s %.0f%s", def->name, val, def->unit);
    } else {
        snprintf(buf, sizeof(buf), "%s %.0f%s", def->name, val, def->unit);
    }

    return buf;
}

void ResetEffects() {
    for (int i = 0; i < g_paramDefCount; i++) {
        SetParamValue(g_paramDefs[i].id, g_paramDefs[i].defaultValue);
    }
}

// Legacy compatibility functions
float GetEffectValue(EffectType type) {
    switch (type) {
        case EffectType::Volume: return g_volume;
        case EffectType::Pitch: return g_pitch;
        case EffectType::Tempo: return g_tempo;
        case EffectType::Rate: return g_rate;
        default: return 0.0f;
    }
}

const char* GetEffectName(EffectType type) {
    switch (type) {
        case EffectType::Volume: return "Volume";
        case EffectType::Pitch: return "Pitch";
        case EffectType::Tempo: return "Tempo";
        case EffectType::Rate: return "Rate";
        default: return "Unknown";
    }
}

const char* GetEffectUnit(EffectType type) {
    switch (type) {
        case EffectType::Volume: return "%";
        case EffectType::Pitch: return " semitones";
        case EffectType::Tempo: return "%";
        case EffectType::Rate: return "x";
        default: return "";
    }
}

void SetEffectValue(EffectType type, float value) {
    switch (type) {
        case EffectType::Volume: SetParamValue(ParamId::Volume, value); break;
        case EffectType::Pitch: SetParamValue(ParamId::Pitch, value); break;
        case EffectType::Tempo: SetParamValue(ParamId::Tempo, value); break;
        case EffectType::Rate: SetParamValue(ParamId::Rate, value); break;
        default: break;
    }
}

void CycleEffect(int direction) {
    CycleParam(direction);
}

void AdjustCurrentEffect(int direction) {
    AdjustCurrentParam(direction);
}

// ---------------------------------------------------------------------------
// Effect presets: save/load/delete named sets of all enabled effects + params.
// Stored in FastPlay.ini under [Presets] (index) and [Preset_<name>] (values).
// ---------------------------------------------------------------------------

static std::wstring PresetSectionName(const std::wstring& name) {
    return L"Preset_" + name;
}

static float IniGetFloatPreset(const wchar_t* section, const wchar_t* key, float defaultVal) {
    wchar_t buf[64] = {0};
    IniGetString(section, key, L"", buf, 64, g_configPath.c_str());
    if (buf[0] == L'\0') return defaultVal;
    return (float)std::wcstod(buf, nullptr);
}

std::vector<std::wstring> GetEffectPresetNames() {
    std::vector<std::wstring> names;
    int count = IniGetInt(L"Presets", L"Count", 0, g_configPath.c_str());
    for (int i = 0; i < count; i++) {
        wchar_t key[32];
        swprintf(key, 32, L"Name%d", i);
        wchar_t buf[128] = {0};
        IniGetString(L"Presets", key, L"", buf, 128, g_configPath.c_str());
        if (buf[0] != L'\0') names.push_back(buf);
    }
    return names;
}

static void WritePresetNameList(const std::vector<std::wstring>& names) {
    // Clear old name entries first
    int oldCount = IniGetInt(L"Presets", L"Count", 0, g_configPath.c_str());
    for (int i = 0; i < oldCount; i++) {
        wchar_t key[32];
        swprintf(key, 32, L"Name%d", i);
        IniWriteString(L"Presets", key, nullptr, g_configPath.c_str());
    }
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", (int)names.size());
    IniWriteString(L"Presets", L"Count", buf, g_configPath.c_str());
    for (size_t i = 0; i < names.size(); i++) {
        wchar_t key[32];
        swprintf(key, 32, L"Name%zu", i);
        IniWriteString(L"Presets", key, names[i].c_str(), g_configPath.c_str());
    }
}

bool SaveEffectPreset(const std::wstring& name) {
    if (name.empty()) return false;

    std::wstring section = PresetSectionName(name);
    wchar_t buf[64];

    // Stream effect enabled flags
    for (int i = 0; i < 4; i++) {
        wchar_t key[32];
        swprintf(key, 32, L"StreamEnabled%d", i);
        IniWriteString(section.c_str(), key, g_effectEnabled[i] ? L"1" : L"0", g_configPath.c_str());
    }

    // Stream effect values (pitch, tempo, rate — volume intentionally excluded)
    swprintf(buf, 64, L"%.4f", g_pitch);
    IniWriteString(section.c_str(), L"Pitch", buf, g_configPath.c_str());
    swprintf(buf, 64, L"%.4f", g_tempo);
    IniWriteString(section.c_str(), L"Tempo", buf, g_configPath.c_str());
    swprintf(buf, 64, L"%.4f", g_rate);
    IniWriteString(section.c_str(), L"Rate", buf, g_configPath.c_str());

    // Reverb algorithm
    swprintf(buf, 64, L"%d", g_reverbAlgorithm);
    IniWriteString(section.c_str(), L"ReverbAlgorithm", buf, g_configPath.c_str());

    // DSP effect enabled flags
    for (int i = 0; i < (int)DSPEffectType::COUNT; i++) {
        wchar_t key[32];
        swprintf(key, 32, L"DSPEnabled%d", i);
        IniWriteString(section.c_str(), key,
            g_dspEnabled[i] ? L"1" : L"0", g_configPath.c_str());
    }

    // All param values (DSP params plus stream effect params)
    for (int i = 0; i < g_paramDefCount; i++) {
        const ParamDef& def = g_paramDefs[i];
        // Skip volume - presets shouldn't hijack playback volume
        if (def.id == ParamId::Volume) continue;
        wchar_t key[64];
        swprintf(key, 64, L"Param%d", (int)def.id);
        swprintf(buf, 64, L"%.6f", g_paramValues[(int)def.id]);
        IniWriteString(section.c_str(), key, buf, g_configPath.c_str());
    }

    // Add to name list if not already present
    auto names = GetEffectPresetNames();
    bool found = false;
    for (auto& n : names) { if (n == name) { found = true; break; } }
    if (!found) {
        names.push_back(name);
        WritePresetNameList(names);
    }
    return true;
}

bool LoadEffectPreset(const std::wstring& name) {
    if (name.empty()) return false;
    std::wstring section = PresetSectionName(name);

    // Quick existence check
    wchar_t test[8] = {0};
    IniGetString(section.c_str(), L"Pitch", L"__MISSING__", test, 8, g_configPath.c_str());
    if (wcscmp(test, L"__MISSING__") == 0) return false;

    // Stream effect values
    wchar_t buf[64] = {0};
    IniGetString(section.c_str(), L"Pitch", L"0", buf, 64, g_configPath.c_str());
    g_pitch = (float)std::wcstod(buf, nullptr);
    IniGetString(section.c_str(), L"Tempo", L"0", buf, 64, g_configPath.c_str());
    g_tempo = (float)std::wcstod(buf, nullptr);
    IniGetString(section.c_str(), L"Rate", L"1", buf, 64, g_configPath.c_str());
    g_rate = (float)std::wcstod(buf, nullptr);

    // Stream effect enabled flags
    for (int i = 0; i < 4; i++) {
        wchar_t key[32];
        swprintf(key, 32, L"StreamEnabled%d", i);
        g_effectEnabled[i] = IniGetInt(section.c_str(), key,
            g_effectEnabled[i] ? 1 : 0, g_configPath.c_str()) != 0;
    }

    // Reverb algorithm
    int ra = IniGetInt(section.c_str(), L"ReverbAlgorithm", g_reverbAlgorithm, g_configPath.c_str());
    if (ra < 0) ra = 0;
    if (ra >= (int)ReverbAlgorithm::COUNT) ra = (int)ReverbAlgorithm::Advanced;  // old DX8 / I3DL2
    SetReverbAlgorithm(ra);

    // DSP effect enabled flags (apply via EnableDSPEffect so handlers hook up properly)
    for (int i = 0; i < (int)DSPEffectType::COUNT; i++) {
        if ((DSPEffectType)i == DSPEffectType::Reverb) continue;  // controlled by algorithm
        wchar_t key[32];
        swprintf(key, 32, L"DSPEnabled%d", i);
        bool en = IniGetInt(section.c_str(), key,
            g_dspEnabled[i] ? 1 : 0, g_configPath.c_str()) != 0;
        EnableDSPEffect((DSPEffectType)i, en);
    }

    // All param values (via SetParamValue so effects update live)
    for (int i = 0; i < g_paramDefCount; i++) {
        const ParamDef& def = g_paramDefs[i];
        if (def.id == ParamId::Volume) continue;
        wchar_t key[64];
        swprintf(key, 64, L"Param%d", (int)def.id);
        float val = IniGetFloatPreset(section.c_str(), key, g_paramValues[(int)def.id]);
        SetParamValue(def.id, val);
    }

    return true;
}

bool DeleteEffectPreset(const std::wstring& name) {
    if (name.empty()) return false;
    std::wstring section = PresetSectionName(name);
    // Wipe the preset's entire section
    IniWriteString(section.c_str(), nullptr, nullptr, g_configPath.c_str());

    auto names = GetEffectPresetNames();
    bool found = false;
    for (auto it = names.begin(); it != names.end(); ) {
        if (*it == name) { it = names.erase(it); found = true; } else { ++it; }
    }
    if (found) WritePresetNameList(names);
    return found;
}
